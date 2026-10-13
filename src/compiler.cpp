#include "compiler.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "parser.h"
#include "vm.h"

namespace astra {

namespace fs = std::filesystem;

// The standard library past the prelude: src/std/*.astra, packed into the executable by the build.
extern const unsigned char kStdSource[];
extern const size_t kStdSourceSize;

namespace {

// Compiled into every program as module 0. Runtime errors are instances of Error.
const char* const kPrelude = R"(class Error {
  var message
  fun init(message) {
    this.message = message
  }
  fun toString() {
    return type(this) + ": " + str(this.message)
  }
}

)";

enum class FunctionKind { Script, Function, Method, Initializer, StaticMethod, Setter };

// The declared type of a variable: a TypeId, optionally with kTypeNullable. 0 means untyped.
struct TypeSpec {
    uint8_t flags = 0;
    std::string className;  // for kTypeInstance

    bool typed() const { return flags != 0; }
    uint8_t id() const { return flags & kTypeMask; }
    bool nullable() const { return (flags & kTypeNullable) != 0; }
    std::string text() const { return typeText(flags, className); }
};

struct VarInfo {
    bool isVal = false;
    TypeSpec type;

    // The variable is still being given its initial value, so reading it needs a check.
    bool pending = false;
};

// A local's `depth` is its scope depth once it can be used. Before that it is one of these:
const int kDeclaring = -1;    // its initial value is being compiled
const int kUnavailable = -2;  // a parameter that may not have been given a value yet
const int kPending = -3;      // as kDeclaring, compiled again with the slot set aside (see VarStmt)

struct Local {
    std::string name;
    int depth;
    VarInfo info;
    bool isCaptured = false;

    // A function inside this variable's own initial value refers to it.
    bool selfCaptured = false;

    // Where its value lives, counted from the frame base. Usually its position among the
    // function's locals; further up when it is declared in a block that is part of an
    // expression, with the half-built values of that expression underneath it.
    int slot = 0;
};

// What an initializer is certain to evaluate to: a TypeId, kStaticNull, or 0 if only
// running it will tell.
const uint8_t kStaticNull = 0xFF;

uint8_t staticTypeOf(const Expr& expr) {
    switch (expr.kind) {
        case ExprKind::Literal:
            switch (static_cast<const LiteralExpr&>(expr).type) {
                case LiteralExpr::Type::Null: return kStaticNull;
                case LiteralExpr::Type::Bool: return kTypeBool;
                case LiteralExpr::Type::Int: return kTypeInt;
                case LiteralExpr::Type::Float: return kTypeFloat;
                case LiteralExpr::Type::String: return kTypeString;
            }
            return 0;
        case ExprKind::Interp: return kTypeString;
        case ExprKind::List: return kTypeList;
        case ExprKind::Map: return kTypeMap;
        case ExprKind::Range: return kTypeRange;
        case ExprKind::Function: return kTypeFunction;
        case ExprKind::Unary: {
            const auto& unary = static_cast<const UnaryExpr&>(expr);
            if (unary.op == TokenType::Bang) return kTypeBool;
            uint8_t operand = staticTypeOf(*unary.operand);
            if (unary.op == TokenType::Tilde) return operand == kTypeInt ? kTypeInt : 0;
            return operand == kTypeInt || operand == kTypeFloat ? operand : 0;
        }
        default: return 0;
    }
}

struct LoopState {
    LoopState* enclosing;
    int start;       // address that `continue` jumps to
    int scopeDepth;
    int tryDepth;
    size_t localCount;  // the function's locals when the body starts; break/continue discard the rest
    int baseSlot;       // and the stack slot the body's first variable gets
    std::vector<int> breakJumps;
    std::string label;               // `outer@ for ...`
    bool continueForward = false;    // do-while: `continue` goes on to the test, further down
    std::vector<int> continueJumps;
};

// A `finally` block that the code being compiled is inside of. Leaving it early, by return,
// break or continue, has to run the block on the way out.
struct FinallyState {
    FinallyState* enclosing;
    const BlockStmt* block;
    int tryDepth;      // try blocks open in the function before this one was entered
    LoopState* loop;   // the loop it was entered in, if any
};

struct FunctionState {
    FunctionState* enclosing = nullptr;
    FunctionKind kind = FunctionKind::Script;
    FunctionProto* proto = nullptr;

    std::vector<Local> locals;
    int slotTop = 0;  // the slot the next local will get
    std::map<std::pair<int, uint64_t>, int> constantIndex;

    void add(Local local) {
        local.slot = slotTop++;
        locals.push_back(std::move(local));
    }

    int scopeDepth = 0;
    LoopState* loop = nullptr;
    int tryDepth = 0;  // try blocks currently open in this function
    FinallyState* finally = nullptr;  // innermost `finally` block the code is inside of
    TypeSpec returnType;  // untyped unless the function declares one

    // A lambda literal: a name it cannot find among its variables may be a member of the
    // object it is run on, which is only known when it runs.
    bool isLambda = false;
    std::string label, calleeLabel;  // what `return@name` may call it

    // An extension function: it runs on a value, as a lambda may, so a name it cannot find
    // among its variables may be a member of that value.
    bool isExtension = false;
    std::string receiverType;  // the type an extension function is for, as written

    // An accessor written under a property: the property's name, and the name its stored value
    // is kept under, which is what `field` means there. Empty when the property stores nothing.
    std::string property, backingField;
};

// The body of `value.let { ... }`, `also` or `takeIf`, compiled in place as part of the
// function it is written in rather than as a function of its own.
struct InlineBlock {
    InlineBlock* enclosing;
    std::string name, label;       // let / also / takeIf, and a label written before the lambda
    FunctionState* function;
    int valueSlot;                 // the slot of the value the block works on
    FinallyState* finallyAtEntry;
    int tryDepth;
    std::vector<int> exits;        // `return@let value` jumps, to be pointed at the end
};

struct ClassState {
    ClassState* enclosing;
    bool hasSuper = false;

    // For names written bare inside the class: what it declares itself, how to reach the
    // class object, and the function the declaration is written in.
    std::unordered_set<std::string> members;  // fields, methods and properties of each object
    std::unordered_set<std::string> statics;
    std::unordered_map<std::string, TypeSpec> fieldTypes;   // declared types of its fields, static ones included
    std::string storage;
    std::string qualified;                    // `Outer.Inner` for a class declared inside another
    const ClassState* outer = nullptr;        // the class this one is declared inside, if any
    FunctionState* definedIn = nullptr;
    bool inStaticInit = false;  // the value of a static field is being compiled
    bool isEnum = false;

    // Fields whose initial values the generated initializer assigns, after the header's
    // entries, so that such a value can be worked out from them. The value may be null.
    std::vector<std::pair<const VarStmt*, const Expr*>> initializedByInit;
    std::vector<size_t> initializedIndex;  // where each of those stands among the class's fields

    // Set when the class declares fields and has no superclass: its fields are then exactly
    // the declared ones, so assigning to any other name through `this` is a compile error.
    // (With a superclass the full set is only known when the program runs.)
    const ClassStmt* closed = nullptr;
};

}  // namespace

bool readFile(const std::string& path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    if (file.bad()) return false;
    out = buffer.str();
    if (out.compare(0, 3, "\xEF\xBB\xBF") == 0) out.erase(0, 3);
    return true;
}

// Turns the AST of one source file into bytecode.
class ModuleCompiler {
public:
    ModuleCompiler(CompilerSession& session, std::string file, std::string directory, Namespace* ns, std::string fileKey)
        : session_(session), file_(std::move(file)), directory_(std::move(directory)), ns_(ns), fileKey_(std::move(fileKey)) {}

    // A file on its own. Before anything else of it runs, its imports are loaded, its functions
    // defined and the structure of its classes set up, so that the order they are written in
    // does not matter; the rest then runs from top to bottom.
    FunctionProto* compile(const std::vector<StmtPtr>& statements, bool repl) {
        FunctionState state;
        beginScript(state, file_);
        noteEnums(statements);
        hoistImports(statements);
        hoistFunctions(statements);
        std::vector<std::pair<size_t, const std::vector<StmtPtr>*>> files{{0, &statements}};
        std::vector<std::string> names{file_};
        for (const UnitClass& cls : orderClasses(files, names)) hoistClass(*cls.stmt);
        hoistExtensions(statements);
        compileRemainder(statements, repl);
        endScript(state);
        return state.proto;
    }

    // For a folder, whose files are compiled together: these are called on each file's compiler
    // in turn, the hoisted parts all going into one function that runs first.
    void beginScript(FunctionState& state, const std::string& source) {
        state.kind = FunctionKind::Script;
        state.proto = session_.program.newProto();
        state.proto->name = "<script>";
        state.proto->source = source;
        state.add(Local{"", 0, VarInfo{}});
        fs_ = &state;
        shared_ = false;
    }

    void continueScript(FunctionState& state) {
        fs_ = &state;
        shared_ = true;
    }

    void endScript(FunctionState& state) {
        fs_ = &state;
        emit(Op::Null);
        emit(Op::Return);
        finish(state);
        fs_ = nullptr;
    }

    // The names the file declares at its top level are known already (the session noted them
    // before compiling any of it). The entries of its enums are noted here.
    void noteEnums(const std::vector<StmtPtr>& statements) {
        for (const StmtPtr& stmt : statements)
            if (stmt->kind == StmtKind::Class) noteEnums(static_cast<const ClassStmt&>(*stmt), static_cast<const ClassStmt&>(*stmt).name);
    }

    void noteEnums(const ClassStmt& cls, const std::string& qualified) {
        if (cls.isEnum) registerEnum(cls, qualified);
        for (const auto& inner : cls.nested) noteEnums(*inner, qualified + "." + inner->name);
    }

    void hoistImports(const std::vector<StmtPtr>& statements) {
        for (const StmtPtr& stmt : statements) {
            if (stmt->kind != StmtKind::Import) continue;
            compileStmt(*stmt);
            hoisted_.insert(stmt.get());
        }
    }

    void hoistFunctions(const std::vector<StmtPtr>& statements) {
        for (const StmtPtr& stmt : statements) {
            if (stmt->kind != StmtKind::Function || !static_cast<const FunctionStmt&>(*stmt).receiver.empty()) continue;
            compileStmt(*stmt);
            hoisted_.insert(stmt.get());
        }
    }

    // Extension functions are added once the classes they are for exist.
    void hoistExtensions(const std::vector<StmtPtr>& statements) {
        for (const StmtPtr& stmt : statements) {
            if (stmt->kind != StmtKind::Function || static_cast<const FunctionStmt&>(*stmt).receiver.empty()) continue;
            compileStmt(*stmt);
            hoisted_.insert(stmt.get());
        }
    }

    void hoistClass(const ClassStmt& cls) {
        line_ = cls.line;
        hoisted_.insert(&cls);
        if (!shared_) {
            compileClass(cls, "", nullptr, ClassPart::Structure);
            return;
        }

        // In the script a folder's files share, a class is set up by a function of its own,
        // so that an error in doing so is reported against the file the class is written in.
        FunctionState state;
        state.enclosing = fs_;
        state.kind = FunctionKind::Function;
        state.proto = session_.program.newProto();
        state.proto->name = "<class " + cls.name + ">";
        state.proto->source = file_;
        state.add(Local{"", 0, VarInfo{}});
        fs_ = &state;
        compileClass(cls, "", nullptr, ClassPart::Structure);
        line_ = cls.line;
        emit(Op::Null);
        emit(Op::Return);
        finish(state);
        fs_ = state.enclosing;

        int index = static_cast<int>(fs_->proto->protos.size());
        fs_->proto->protos.push_back(state.proto);
        emit(Op::Closure, index);
        emit(Op::Call);
        emitByte(0);
        emit(Op::Pop);
    }

    // Everything that was not hoisted, in the order written. A class whose structure was set
    // up ahead gets its values here.
    void compileRemainder(const std::vector<StmtPtr>& statements, bool repl) {
        for (size_t i = 0; i < statements.size(); i++) {
            const Stmt& stmt = *statements[i];
            if (hoisted_.count(&stmt) != 0) {
                if (stmt.kind == StmtKind::Class) {
                    line_ = stmt.line;
                    compileClass(static_cast<const ClassStmt&>(stmt), "", nullptr, ClassPart::Values);
                }
                continue;
            }
            if (repl && i + 1 == statements.size() && stmt.kind == StmtKind::Expr &&
                static_cast<const ExprStmt&>(stmt).expr->kind != ExprKind::IncDec) {
                compileExpr(*static_cast<const ExprStmt&>(stmt).expr);
                emit(Op::Return);
            } else {
                compileStmt(stmt);
            }
        }
    }

    // A top-level class of the files being compiled together, and the file it is in.
    struct UnitClass {
        const ClassStmt* stmt;
        size_t file;
    };

    // The top-level classes of a unit in an order in which each comes after those of the unit
    // that it extends or implements. A class that extends something held in a variable cannot
    // be set up ahead, and neither can one that extends such a class; they are left out, to be
    // compiled where they are written.
    static std::vector<UnitClass> orderClasses(const std::vector<std::pair<size_t, const std::vector<StmtPtr>*>>& files,
                                               const std::vector<std::string>& fileNames) {
        std::vector<UnitClass> classes;
        std::unordered_map<std::string, std::vector<size_t>> byName;
        std::unordered_set<std::string> variables;
        for (const auto& [file, statements] : files) {
            for (const StmtPtr& stmt : *statements) {
                if (stmt->kind == StmtKind::Var) {
                    const auto& var = static_cast<const VarStmt&>(*stmt);
                    if (var.pattern.empty()) variables.insert(var.name);
                    for (const std::string& name : var.pattern) variables.insert(name);
                } else if (stmt->kind == StmtKind::Class) {
                    const auto& cls = static_cast<const ClassStmt&>(*stmt);
                    byName[cls.name].push_back(classes.size());
                    classes.push_back(UnitClass{&cls, file});
                }
            }
        }

        // A name as one file means it: its own class of that name first, then another file's
        // unless that one is private.
        auto resolve = [&](const std::string& name, size_t file) -> int {
            auto found = byName.find(name);
            if (found == byName.end()) return -1;
            for (size_t index : found->second)
                if (classes[index].file == file) return static_cast<int>(index);
            for (size_t index : found->second)
                if (classes[index].stmt->visibility != GlobalName::Private) return static_cast<int>(index);
            return -1;
        };

        enum State : char { Fresh, Visiting, Placed, LeftOut };
        std::vector<char> state(classes.size(), Fresh);
        std::vector<UnitClass> order;
        std::function<void(size_t)> visit = [&](size_t i) {
            state[i] = Visiting;
            bool leftOut = false;
            for (const std::string& supertype : classes[i].stmt->supertypes) {
                const std::string first = supertype.substr(0, supertype.find('.'));
                int j = resolve(first, classes[i].file);
                if (j < 0) {
                    leftOut = leftOut || variables.count(first) != 0;
                    continue;
                }
                if (state[static_cast<size_t>(j)] == Visiting) {
                    const ClassStmt& cls = *classes[i].stmt;
                    throw CompileError(fileNames[classes[i].file], cls.line, cls.col,
                                       "'" + cls.name + "' and '" + classes[static_cast<size_t>(j)].stmt->name +
                                           "' each inherit from the other");
                }
                if (state[static_cast<size_t>(j)] == Fresh) visit(static_cast<size_t>(j));
                leftOut = leftOut || state[static_cast<size_t>(j)] == LeftOut;
            }
            state[i] = leftOut ? LeftOut : Placed;
            if (!leftOut) order.push_back(classes[i]);
        };
        for (size_t i = 0; i < classes.size(); i++)
            if (state[i] == Fresh) visit(i);
        return order;
    }

private:
    CompilerSession& session_;
    std::string file_;
    std::string directory_;

    FunctionState* fs_ = nullptr;
    ClassState* class_ = nullptr;
    int line_ = 1;
    InlineBlock* inline_ = nullptr;  // innermost block being compiled in place
    std::string nextProperty_, nextBackingField_;  // for the accessor about to be compiled
    bool nextIsExtension_ = false;                 // for the function about to be compiled
    std::string nextReceiverType_;
    int anonymousClasses_ = 0;
    const TryStmt* valueTry_ = nullptr;  // the try being compiled for its value, and where that goes
    int valueTryLocal_ = -1;
    bool shared_ = false;  // compiling into the script that a folder's files share
    std::unordered_set<const Stmt*> hoisted_;  // top-level statements compiled ahead of their place

    // Where the file's top-level names live, and whose names it can see besides.
    Namespace* ns_;
    std::string fileKey_;
    struct Import {
        Namespace* ns;
        std::string alias;  // what its names are written after: jade.Name, or ui.Name after `as ui`
        std::string path;   // as written, for messages
        bool all;           // `import jade.*`: its names can be written bare
    };
    std::vector<Import> imports_;
    bool jumpInExpression_ = false;  // the statement about to be compiled is a jump inside an expression

    CompileError error(const Node& at, const std::string& message) const {
        return CompileError(file_, at.line, at.col, message);
    }

    void finish(FunctionState& state) {
        try {
            verifyFunction(*state.proto, session_.program);
        } catch (const std::runtime_error& e) {
            throw CompileError(file_, line_, 1, std::string("internal compiler error: ") + e.what());
        }
    }

    // ---- Emission ----

    std::vector<uint8_t>& code() { return fs_->proto->code; }
    int here() { return static_cast<int>(code().size()); }

    void emitByte(int b) {
        fs_->proto->code.push_back(static_cast<uint8_t>(b));
        fs_->proto->lines.push_back(line_);
    }

    void emitU16(int value) {
        emitByte(value & 0xFF);
        emitByte((value >> 8) & 0xFF);
    }

    void emitI32(int value) {
        emitByte(value & 0xFF);
        emitByte((value >> 8) & 0xFF);
        emitByte((value >> 16) & 0xFF);
        emitByte((value >> 24) & 0xFF);
    }

    void emit(Op op) { emitByte(static_cast<int>(op)); }

    void emit(Op op, int u16) {
        if (u16 < 0 || u16 > 0xFFFF) throw CompileError(file_, line_, 1, "function is too large (operand overflow)");
        emit(op);
        emitU16(u16);
    }

    // Emits a jump with a placeholder target and returns where to patch it.
    int emitJump(Op op) {
        emit(op);
        int at = here();
        emitI32(0);
        return at;
    }

    void emitLoop(int target) {
        emit(Op::Loop);
        emitI32(target);
    }

    void patchJump(int at) { patchJump(at, here()); }

    void patchJump(int at, int target) {
        std::vector<uint8_t>& c = code();
        size_t i = static_cast<size_t>(at);
        c[i] = static_cast<uint8_t>(target & 0xFF);
        c[i + 1] = static_cast<uint8_t>((target >> 8) & 0xFF);
        c[i + 2] = static_cast<uint8_t>((target >> 16) & 0xFF);
        c[i + 3] = static_cast<uint8_t>((target >> 24) & 0xFF);
    }

    int addConstant(Value value, int tag, uint64_t bits) {
        auto key = std::make_pair(tag, bits);
        auto it = fs_->constantIndex.find(key);
        if (it != fs_->constantIndex.end()) return it->second;

        int index = static_cast<int>(fs_->proto->constants.size());
        if (index > 0xFFFF) throw CompileError(file_, line_, 1, "too many constants in one function");
        fs_->proto->constants.push_back(value);
        fs_->constantIndex[key] = index;
        return index;
    }

    int intConstant(int64_t i) { return addConstant(Value::integer(i), 0, static_cast<uint64_t>(i)); }

    int floatConstant(double d) {
        uint64_t bits;
        std::memcpy(&bits, &d, sizeof bits);
        return addConstant(Value::number(d), 1, bits);
    }

    int stringConstant(const std::string& s) {
        ObjString* str = session_.vm_.intern(s);
        return addConstant(Value::object(str), 2, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(str)));
    }

    // ---- Scopes and variables ----

    bool isGlobalScope() const { return fs_->scopeDepth == 0; }

    void beginScope() { fs_->scopeDepth++; }

    void endScope() {
        fs_->scopeDepth--;
        std::vector<Local>& locals = fs_->locals;
        while (!locals.empty() && locals.back().depth > fs_->scopeDepth) {
            emit(locals.back().isCaptured ? Op::CloseUpvalue : Op::Pop);
            locals.pop_back();
            fs_->slotTop--;
        }
    }

    int slotOf(int local) const { return fs_->locals[static_cast<size_t>(local)].slot; }

    // Adds a local whose value is (or is about to be) on top of the stack. Returns its place
    // among the function's locals; slotOf() gives the stack slot.
    int addLocal(const std::string& name, bool isVal, const Node& at, bool initialized, TypeSpec type = {}) {
        std::vector<Local>& locals = fs_->locals;
        bool hidden = name.empty() || name[0] == '(';
        if (!hidden) {
            for (size_t i = locals.size(); i-- > 0;) {
                const Local& existing = locals[i];
                if (existing.depth >= 0 && existing.depth < fs_->scopeDepth) break;
                if (existing.name == name) throw error(at, "variable '" + name + "' is already declared in this scope");
            }
        }
        if (locals.size() >= 0xFFFF) throw error(at, "too many local variables in one function");

        fs_->add(Local{name, initialized ? fs_->scopeDepth : kDeclaring, VarInfo{isVal, std::move(type), false}});
        return static_cast<int>(locals.size()) - 1;
    }

    // `fromInner` is true when the name is being looked up on behalf of a nested function.
    int resolveLocal(FunctionState* state, const std::string& name, const Node& at, VarInfo& info, bool fromInner = false) {
        for (size_t i = state->locals.size(); i-- > 0;) {
            Local& local = state->locals[i];
            if (local.name != name) continue;
            info = local.info;

            if (local.depth < 0) {
                // A variable cannot be used directly in its own initial value (val x = x + 1).
                // A function written there may refer to it, since it normally runs later:
                // the first time round that is only noted, and VarStmt compiles the value again.
                if (fromInner && local.depth == kDeclaring) {
                    local.selfCaptured = true;
                } else if (fromInner && local.depth == kPending) {
                    info.pending = true;
                } else {
                    throw error(at, "cannot use '" + name + "' here: it does not have a value yet");
                }
            }
            return static_cast<int>(i);
        }
        info = VarInfo{};
        return -1;
    }

    int resolveUpvalue(FunctionState* state, const std::string& name, const Node& at, VarInfo& info) {
        info = VarInfo{};
        if (state->enclosing == nullptr) return -1;

        int local = resolveLocal(state->enclosing, name, at, info, true);
        if (local != -1) {
            Local& captured = state->enclosing->locals[static_cast<size_t>(local)];
            captured.isCaptured = true;
            return addUpvalue(state, true, captured.slot);
        }

        int upvalue = resolveUpvalue(state->enclosing, name, at, info);
        if (upvalue != -1) return addUpvalue(state, false, upvalue);
        return -1;
    }

    int addUpvalue(FunctionState* state, bool isLocal, int index) {
        std::vector<UpvalueDesc>& upvalues = state->proto->upvalues;
        for (size_t i = 0; i < upvalues.size(); i++)
            if (upvalues[i].isLocal == isLocal && upvalues[i].index == index) return static_cast<int>(i);
        upvalues.push_back(UpvalueDesc{isLocal, static_cast<uint16_t>(index)});
        return static_cast<int>(upvalues.size()) - 1;
    }

    // Gives `state` access to slot 0 (the receiver, if there is one) of the function `levels`
    // steps out from it, as an upvalue.
    int captureReceiver(FunctionState* state, int levels) {
        if (levels == 1) {
            state->enclosing->locals[0].isCaptured = true;
            return addUpvalue(state, true, 0);
        }
        return addUpvalue(state, false, captureReceiver(state->enclosing, levels - 1));
    }

    // ---- Global names ----

    bool visibleFrom(const GlobalName& name, const Namespace& owner) const {
        if (!name.declared) return false;
        return name.visibility == GlobalName::Public ||
               (name.visibility == GlobalName::Internal && owner.library == ns_->library);
    }

    // Worker("physics.astra"): the script is compiled into the program here, and the path
    // in the call is replaced by one that says which module it became (see worker.cpp).
    const LiteralExpr* workerLiteral_ = nullptr;
    std::string workerSpec_;

    void noteWorkerScript(const CallExpr& e) {
        if (e.callee->kind != ExprKind::Name || static_cast<const NameExpr&>(*e.callee).name != "Worker") return;
        if (e.args.empty() || e.args[0]->kind != ExprKind::Literal || !e.names[0].empty()) return;
        const auto& literal = static_cast<const LiteralExpr&>(*e.args[0]);
        if (literal.type != LiteralExpr::Type::String) return;
        // Only the standard library's Worker: a class of the program's own by that name is left alone.
        auto builtin = session_.builtins_->names.find("Worker");
        if (builtin == session_.builtins_->names.end() || findGlobal("Worker", e, true) != &builtin->second) return;
        const int module = session_.loadWorker(literal.stringValue, directory_, file_, literal);
        workerLiteral_ = &literal;
        workerSpec_ = std::string(1, '\x01') + std::to_string(module) + '\x01' + literal.stringValue;
    }

    // Whether `name` is a built-in that only the standard library's own source may use, and
    // the libraries that come with Astra (the lib folder beside it), which are written on
    // such built-ins: Jade on __jade.
    bool libraryOnly(const std::string& name) const {
        return name.size() > 2 && name[0] == '_' && name[1] == '_' && ns_ != session_.builtins_ && ns_->library == "<program>" &&
               session_.vm_.hasBuiltin(name);
    }

    // What a bare name means at the top level, if anything declares it: a private name of this
    // file, a name of this folder, a name of an imported folder that this file may see, or a
    // built-in. The folder's own names come before imported ones.
    GlobalName* findGlobal(const std::string& name, const Node& at, bool quiet = false) const {
        auto& mine = session_.privates_[fileKey_];
        if (auto it = mine.find(name); it != mine.end()) return &it->second;
        if (auto it = ns_->names.find(name); it != ns_->names.end() && it->second.declared) return &it->second;

        GlobalName* found = nullptr;
        const Import* source = nullptr;
        for (const Import& import : imports_) {
            if (!import.all) continue;  // without .* its names are only reached through its own
            auto it = import.ns->names.find(name);
            if (it == import.ns->names.end() || !visibleFrom(it->second, *import.ns)) continue;
            if (found != nullptr && found != &it->second && quiet) return found;  // reported where the name is used
            if (found != nullptr && found != &it->second)
                throw error(at, "'" + name + "' is declared by both " + source->path + " and " + import.path +
                                    "; say which, as in " + (import.alias.empty() ? "x" : import.alias) + "." + name);
            found = &it->second;
            source = &import;
        }
        if (found != nullptr) return found;

        Namespace& builtins = *session_.builtins_;
        // A built-in whose name starts with two underscores is the standard library's own:
        // to any other file there is no such name (resolveGlobal reports its use).
        if (libraryOnly(name)) return nullptr;
        if (auto it = builtins.names.find(name); it != builtins.names.end() && it->second.declared) return &it->second;
        if (session_.vm_.hasBuiltin(name)) {
            GlobalName& builtin = builtins.names[name];
            if (builtin.slot < 0) builtin.slot = session_.newSlot(name);
            builtin.declared = true;
            return &builtin;
        }
        return nullptr;
    }

    // The global a bare name refers to, whether or not anything declares it yet: it may be
    // declared by a file of this folder that is loaded later.
    GlobalName* resolveGlobal(const std::string& name, const Node& at) const {
        if (GlobalName* known = findGlobal(name, at)) return known;
        if (libraryOnly(name)) throw error(at, "'" + name + "' is for the standard library's own use");
        GlobalName& fresh = ns_->names[name];
        if (fresh.slot < 0) fresh.slot = session_.newSlot(name);
        return &fresh;
    }

    // The slot of a name this file declares at its top level. Names the program writes were
    // noted before compiling began; a name made up here (an object's hidden class) is new.
    int declareGlobal(const std::string& name, bool isVal, const Node&) {
        auto& mine = session_.privates_[fileKey_];
        auto priv = mine.find(name);
        GlobalName& entry = priv != mine.end() ? priv->second : ns_->names[name];
        if (entry.slot < 0) entry.slot = session_.newSlot(name);
        if (!entry.declared) {
            entry.declared = true;
            entry.isVal = isVal;
            entry.file = file_;
        }
        return entry.slot;
    }

    // Whether `name` (jade, jade.chart, ui) is what an import's contents are reached by, with
    // no variable or member called like its first part in the way.
    const Import* aliasOf(const std::string& name) const {
        for (const Import& import : imports_) {
            if (import.alias.empty() || import.alias != name) continue;
            const std::string first = name.substr(0, name.find('.'));
            for (const FunctionState* state = fs_; state != nullptr; state = state->enclosing)
                for (const Local& local : state->locals)
                    if (local.name == first) return nullptr;
            const ClassSide side = classSide();
            if (side == ClassSide::Instance && class_->members.count(first) != 0) return nullptr;
            if (side != ClassSide::None && class_->statics.count(first) != 0) return nullptr;
            return &import;
        }
        return nullptr;
    }

    // The same for an expression: a bare name, or names joined by dots.
    const Import* aliasOf(const Expr& object) const {
        if (imports_.empty()) return nullptr;
        std::string dotted;
        for (const Expr* e = &object;;) {
            if (e->kind == ExprKind::Name) {
                dotted = static_cast<const NameExpr&>(*e).name + dotted;
                break;
            }
            if (e->kind != ExprKind::Member || static_cast<const MemberExpr&>(*e).safe) return nullptr;
            dotted = "." + static_cast<const MemberExpr&>(*e).name + dotted;
            e = static_cast<const MemberExpr&>(*e).object.get();
        }
        return aliasOf(dotted);
    }

    // For a dotted name such as jade.chart.Axis: the import its longest leading part names, and
    // where the rest begins (the position of the dot after that part).
    const Import* aliasPrefix(const std::string& dotted, size_t& dot) const {
        for (size_t at = dotted.rfind('.'); at != std::string::npos && at > 0; at = dotted.rfind('.', at - 1)) {
            if (const Import* import = aliasOf(dotted.substr(0, at))) {
                dot = at;
                return import;
            }
        }
        return nullptr;
    }

    // The name `member` of an import, if the import has one that this file may see.
    const GlobalName* aliasedLookup(const Import& import, const std::string& member) const {
        auto it = import.ns->names.find(member);
        return it == import.ns->names.end() || !visibleFrom(it->second, *import.ns) ? nullptr : &it->second;
    }

    const GlobalName* aliasedName(const Import& import, const std::string& member, const Node& at) const {
        const GlobalName* found = aliasedLookup(import, member);
        if (found == nullptr)
            throw error(at, "\"" + import.path + "\" has no name '" + member + "' that can be seen from here");
        return found;
    }

    // Whether `object.member` names something of an import. A library may hold a value called
    // like itself (the library Jade and its object Jade): `Jade.Button` is then the library's
    // class, and `Jade.theme`, which the library has no name for, a member of the object.
    const GlobalName* importedMember(const Expr& object, const std::string& member, const Node& at) const {
        const Import* import = aliasOf(object);
        if (import == nullptr) return nullptr;
        if (const GlobalName* found = aliasedLookup(*import, member)) return found;
        const bool isValue = object.kind != ExprKind::Name ||
                             findGlobal(static_cast<const NameExpr&>(object).name, at, true) != nullptr;
        if (isValue) return nullptr;
        return aliasedName(*import, member, at);  // reports that the import has no such name
    }

    // ---- Enum entries by their bare names ----
    //
    // Where a value is headed for something declared as an enum (a variable, a field, a
    // parameter with a default, a function's result, the other side of ==), a bare name that
    // means nothing else is an entry of that enum. Passed as an argument, the destination is
    // the parameter of a function only known when the call is made, so the name travels as it
    // is (PendingEnum) and the parameter settles it.

    // What an enum's entries are kept under: the slot of its global name, followed by the path
    // to it for an enum declared inside a class; or, for one declared in a function, its name.
    std::string enumKey(const std::string& qualified) const {
        const size_t dot = qualified.find('.');
        const std::string first = qualified.substr(0, dot);
        const std::string rest = dot == std::string::npos ? "" : qualified.substr(dot);
        for (const FunctionState* state = fs_; state != nullptr; state = state->enclosing)
            for (const Local& local : state->locals)
                if (local.name == first || local.name == qualified) return "local:" + qualified;
        size_t afterImport = 0;
        if (const Import* import = aliasPrefix(qualified, afterImport)) {
            const size_t next = qualified.find('.', afterImport + 1);
            auto it = import->ns->names.find(
                qualified.substr(afterImport + 1, next == std::string::npos ? next : next - afterImport - 1));
            if (it != import->ns->names.end() && visibleFrom(it->second, *import->ns))
                return "#" + std::to_string(it->second.slot) + (next == std::string::npos ? "" : qualified.substr(next));
        }
        if (const GlobalName* global = findGlobal(first, Node(), true)) return "#" + std::to_string(global->slot) + rest;
        return "local:" + qualified;
    }

    void registerEnum(const ClassStmt& s, const std::string& qualified) {
        std::vector<std::string>& entries = session_.enums_[enumKey(qualified)];
        entries.clear();
        for (const EnumEntry& entry : s.entries) entries.push_back(entry.name);
    }

    const std::vector<std::string>* enumEntries(const TypeSpec& type) const {
        if (type.id() != kTypeInstance) return nullptr;
        // An enum declared inside one of the enclosing classes goes by its short name there.
        for (const ClassState* cls = class_; cls != nullptr; cls = cls->outer) {
            auto nested = session_.enums_.find(enumKey(cls->qualified + "." + type.className));
            if (nested != session_.enums_.end()) return &nested->second;
        }
        auto it = session_.enums_.find(enumKey(type.className));
        return it == session_.enums_.end() ? nullptr : &it->second;
    }

    // Whether a bare name already means something: a variable, a member the class declares,
    // or a global.
    bool isKnownName(const std::string& name) const {
        for (const FunctionState* state = fs_; state != nullptr; state = state->enclosing)
            for (const Local& local : state->locals)
                if (local.name == name) return true;
        const ClassSide side = classSide();
        if (side == ClassSide::Instance && class_->members.count(name) != 0) return true;
        if (side != ClassSide::None && class_->statics.count(name) != 0) return true;
        if (outerStaticOwner(name) != nullptr) return true;
        return findGlobal(name, Node(), true) != nullptr;
    }

    // The declared type of what a bare name refers to, as far as it can be told here.
    TypeSpec declaredTypeOf(const std::string& name) const {
        for (const FunctionState* state = fs_; state != nullptr; state = state->enclosing)
            for (size_t i = state->locals.size(); i-- > 0;)
                if (state->locals[i].name == name) return state->locals[i].info.type;
        if (classSide() != ClassSide::None) {
            auto field = class_->fieldTypes.find(name);
            if (field != class_->fieldTypes.end()) return field->second;
            if (class_->members.count(name) != 0 || class_->statics.count(name) != 0) return {};
        }
        TypeSpec type;
        const GlobalName* known = findGlobal(name, Node(), true);
        auto global = known != nullptr ? session_.globalClassTypes_.find(known->slot) : session_.globalClassTypes_.end();
        if (global != session_.globalClassTypes_.end()) {
            type.flags = kTypeInstance;
            type.className = global->second;
        }
        return type;
    }

    // The declared type of an expression, when it is simply a typed variable or field.
    TypeSpec declaredTypeOf(const Expr& expr) const {
        if (expr.kind == ExprKind::Name) return declaredTypeOf(static_cast<const NameExpr&>(expr).name);
        if (expr.kind == ExprKind::This) {
            // In an extension function `this` is of the type the function is for.
            for (const FunctionState* state = fs_; state != nullptr; state = state->enclosing) {
                if (state->receiverType.empty()) continue;
                TypeSpec type;
                type.flags = kTypeInstance;
                type.className = state->receiverType;
                return type;
            }
            return {};
        }
        if (expr.kind == ExprKind::Member && class_ != nullptr) {
            const auto& member = static_cast<const MemberExpr&>(expr);
            auto field = class_->fieldTypes.find(member.name);
            if (member.object->kind == ExprKind::This && field != class_->fieldTypes.end()) return field->second;
        }
        return {};
    }

    bool mayBeMemberHere() const {
        const ClassSide side = classSide();
        return side == ClassSide::None ? namesFromReceiver() : mayBeMember(side);
    }

    // Whether the function being compiled may be run on an object whose members its bare
    // names can mean: a lambda, or an extension function.
    bool namesFromReceiver() const { return fs_->isLambda || fs_->isExtension; }

    // Compiles an expression whose value is headed for something declared as `type`.
    void compileExprFor(const Expr& expr, const TypeSpec& type) {
        // Either side of `c ? a : b`, and of `a ?: b`, is headed for the same place.
        if (type.id() == kTypeInstance && expr.kind == ExprKind::Ternary) {
            const auto& e = static_cast<const TernaryExpr&>(expr);
            line_ = e.line;
            compileExpr(*e.condition);
            int toElse = emitJump(Op::JumpIfFalse);
            compileExprFor(*e.then, type);
            int toEnd = emitJump(Op::Jump);
            patchJump(toElse);
            compileExprFor(*e.otherwise, type);
            patchJump(toEnd);
            return;
        }
        if (type.id() == kTypeInstance && expr.kind == ExprKind::Elvis) {
            const auto& e = static_cast<const ElvisExpr&>(expr);
            line_ = e.line;
            compileExprFor(*e.left, type);
            int toEnd = emitJump(Op::JumpIfNotNull);
            emit(Op::Pop);
            compileExprFor(*e.right, type);
            patchJump(toEnd);
            return;
        }
        const std::vector<std::string>* entries = expr.kind == ExprKind::Name ? enumEntries(type) : nullptr;
        if (entries == nullptr) return compileExpr(expr);
        const std::string& name = static_cast<const NameExpr&>(expr).name;
        if (isKnownName(name)) return compileExpr(expr);

        const bool isEntry = std::find(entries->begin(), entries->end(), name) != entries->end();
        if (!isEntry) {
            // It may still be a member only found when the program runs; otherwise it is nothing.
            if (mayBeMemberHere()) return compileExpr(expr);
            throw error(expr, "'" + name + "' is not an entry of enum " + type.className + ", nor a variable");
        }
        line_ = expr.line;
        int toEnd = -1;
        if (mayBeMemberHere()) {
            emit(Op::TryImplicit, stringConstant(name));
            toEnd = here();
            emitI32(0);
        }
        emitTypeName(type.className, expr);
        emit(Op::GetProp, stringConstant(name));
        if (toEnd >= 0) patchJump(toEnd);
    }

    // Compiles an argument of a call. A bare name that means nothing here is passed on as a
    // name, for the parameter it reaches to settle.
    void compileArg(const Expr& expr) {
        if (expr.kind != ExprKind::Name || isKnownName(static_cast<const NameExpr&>(expr).name)) return compileExpr(expr);
        const std::string& name = static_cast<const NameExpr&>(expr).name;
        line_ = expr.line;
        int toEnd = -1;
        if (mayBeMemberHere()) {
            emit(Op::TryImplicit, stringConstant(name));
            toEnd = here();
            emitI32(0);
        }
        emit(Op::PendingEnum, stringConstant(name));
        if (toEnd >= 0) patchJump(toEnd);
    }

    // A property with accessors written under it keeps its value under another name, so that
    // reading and assigning the property always go through the accessors.
    static std::string storedName(const VarStmt& field) {
        return field.getter || field.setter ? "field:" + field.name : field.name;
    }

    // `field` in an accessor: the property's stored value. Returns the name it is kept under,
    // or an empty string when the code is not in an accessor.
    std::string backingFieldHere(const Node& at) const {
        for (const FunctionState* state = fs_; state != nullptr; state = state->enclosing) {
            if (state->property.empty()) continue;
            if (state->backingField.empty())
                throw error(at, "'" + state->property + "' has no stored value for 'field' to mean; give the property an " +
                                    std::string("initial value"));
            return state->backingField;
        }
        return "";
    }

    // ---- Names written bare inside a class ----
    //
    // After a function's own variables, a bare name in a class is a member of the object, then
    // a static member of the class, then a global. What the class declares itself is known
    // here; what it inherits is only known when the program runs, so a name that could be
    // inherited is looked up on the object then (GetImplicit), as it is for a lambda's receiver.

    enum class ClassSide { None, Instance, Static };

    static bool kindHasThis(FunctionKind kind) {
        return kind == FunctionKind::Method || kind == FunctionKind::Initializer || kind == FunctionKind::Setter;
    }

    // Whether the code being compiled belongs to an object of the innermost class, to the
    // class itself (a static method, the value of a static field), or to neither.
    ClassSide classSide() const {
        if (class_ == nullptr) return ClassSide::None;
        for (const FunctionState* state = fs_; state != nullptr; state = state->enclosing) {
            if (kindHasThis(state->kind)) return ClassSide::Instance;
            if (state->kind == FunctionKind::StaticMethod) return ClassSide::Static;
            if (state == class_->definedIn) break;
        }
        return class_->inStaticInit ? ClassSide::Static : ClassSide::None;
    }

    // True when a bare name that is nothing the class declares might still be a member: of a
    // superclass, of an object without declared fields, or of the object a lambda runs on.
    bool mayBeMember(ClassSide side) const {
        if (side != ClassSide::Instance) return namesFromReceiver();
        // A class written inside a method also reaches the object that method belongs to.
        return !(kindHasThis(fs_->kind) && class_->closed != nullptr && fs_->proto->outerReceivers.empty());
    }

    // The class around the one being compiled, however far out, that has a static member
    // (or a class declared in it) called `name`.
    const ClassState* outerStaticOwner(const std::string& name) const {
        for (const ClassState* cls = class_ != nullptr ? class_->outer : nullptr; cls != nullptr; cls = cls->outer)
            if (cls->statics.count(name) != 0) return cls;
        return nullptr;
    }

    bool emitOuterStatic(const std::string& name, const Node& at) {
        const ClassState* owner = outerStaticOwner(name);
        if (owner == nullptr) return false;
        int toEnd = -1;
        if (mayBeMemberHere()) {
            emit(Op::TryImplicit, stringConstant(name));
            toEnd = here();
            emitI32(0);
        }
        emitGet(owner->storage, at);
        emit(Op::GetProp, stringConstant(name));
        if (toEnd >= 0) patchJump(toEnd);
        return true;
    }

    // A type as written: a name, or `Outer.Inner` for a class declared inside another.
    void emitTypeName(const std::string& name, const Node& at) {
        size_t dot = name.find('.');
        size_t afterImport = 0;
        if (const Import* import = aliasPrefix(name, afterImport)) {
            // jade.Block: a name of an import, by the import's own name.
            dot = afterImport;
            size_t next = name.find('.', dot + 1);
            emit(Op::GetGlobal, aliasedName(*import, name.substr(dot + 1, next == std::string::npos ? next : next - dot - 1), at)->slot);
            dot = next;
        } else {
            emitGet(name.substr(0, dot), at);
        }
        while (dot != std::string::npos) {
            size_t next = name.find('.', dot + 1);
            emit(Op::GetProp, stringConstant(name.substr(dot + 1, next == std::string::npos ? next : next - dot - 1)));
            dot = next;
        }
    }

    void emitImplicit(Op op, const std::string& name) {
        Node here;
        here.line = line_;
        here.col = 1;
        const int slot = resolveGlobal(name, here)->slot;
        emit(op, stringConstant(name));
        emitU16(slot);
    }

    void emitGet(const std::string& name, const Node& at) {
        VarInfo info;
        int local = resolveLocal(fs_, name, at, info);
        if (local != -1) {
            emit(Op::GetLocal, slotOf(local));
            return;
        }

        int upvalue = resolveUpvalue(fs_, name, at, info);
        if (upvalue != -1) {
            if (info.pending) {
                emit(Op::GetUpvalueChecked, upvalue);
                emitU16(stringConstant(name));
            } else {
                emit(Op::GetUpvalue, upvalue);
            }
            return;
        }

        if (name == "field") {
            const std::string backing = backingFieldHere(at);
            if (!backing.empty()) {
                emitGet("this", at);
                emit(Op::GetProp, stringConstant(backing));
                return;
            }
        }

        const ClassSide side = classSide();
        if (side != ClassSide::None) {
            if (side == ClassSide::Instance && class_->members.count(name) != 0) {
                if (kindHasThis(fs_->kind)) {
                    emit(Op::GetLocal, 0);
                    emit(Op::GetProp, stringConstant(name));
                } else {
                    emitImplicit(Op::GetImplicit, name);  // in a function inside the method
                }
                return;
            }
            if (class_->statics.count(name) != 0) {
                // An inherited member, or one of a lambda's receiver, comes before a static.
                int toEnd = -1;
                if (namesFromReceiver() || (side == ClassSide::Instance && class_->hasSuper)) {
                    emit(Op::TryImplicit, stringConstant(name));
                    toEnd = here();
                    emitI32(0);
                }
                emitGet(class_->storage, at);
                emit(Op::GetProp, stringConstant(name));
                if (toEnd >= 0) patchJump(toEnd);
                return;
            }
            if (emitOuterStatic(name, at)) return;
            if (mayBeMember(side)) {
                emitImplicit(Op::GetImplicit, name);
                return;
            }
        } else if (emitOuterStatic(name, at)) {
            return;
        }

        if (namesFromReceiver()) {
            // Not a variable of the lambda or of anything around it: a member of the object
            // the lambda runs on, if that has one by this name, and otherwise a global.
            const int slot = resolveGlobal(name, at)->slot;
            emit(Op::GetImplicit, stringConstant(name));
            emitU16(slot);
            return;
        }
        emit(Op::GetGlobal, resolveGlobal(name, at)->slot);
    }

    void emitSet(const std::string& name, const Node& at) {
        VarInfo info;
        int local = resolveLocal(fs_, name, at, info);
        if (local != -1) {
            if (info.isVal) throw error(at, "cannot assign to val '" + name + "'");
            emitTypeCheck(info.type, name, at);
            emit(Op::SetLocal, slotOf(local));
            return;
        }

        int upvalue = resolveUpvalue(fs_, name, at, info);
        if (upvalue != -1) {
            if (info.isVal) throw error(at, "cannot assign to val '" + name + "'");
            emitTypeCheck(info.type, name, at);
            emit(Op::SetUpvalue, upvalue);
            return;
        }

        if (name == "field") {
            const std::string backing = backingFieldHere(at);
            if (!backing.empty()) {
                emitGet("this", at);
                emit(Op::Swap);
                emit(Op::SetProp, stringConstant(backing));
                return;
            }
        }

        const ClassSide side = classSide();
        if (side != ClassSide::None) {
            if (side == ClassSide::Instance && class_->members.count(name) != 0) {
                if (kindHasThis(fs_->kind)) {
                    emit(Op::GetLocal, 0);
                    emit(Op::Swap);
                    emit(Op::SetProp, stringConstant(name));
                } else {
                    emitImplicit(Op::SetImplicit, name);
                }
                return;
            }
            if (class_->statics.count(name) != 0) {
                emitGet(class_->storage, at);
                emit(Op::Swap);
                emit(Op::SetProp, stringConstant(name));
                return;
            }
            if (mayBeMember(side)) {
                emitImplicit(Op::SetImplicit, name);
                return;
            }
        }

        if (namesFromReceiver()) {
            const int slot = resolveGlobal(name, at)->slot;
            emit(Op::SetImplicit, stringConstant(name));
            emitU16(slot);
            return;
        }

        // A global may be declared after the code that assigns to it, so whether it is a val
        // is checked once everything is compiled, and its type is checked by the VM.
        const GlobalName* global = resolveGlobal(name, at);
        session_.globalAssignments_.emplace_back(global, name, file_, at.line, at.col);
        emit(Op::SetGlobal, global->slot);
    }

    // ---- Declared types ----

    TypeSpec resolveType(const TypeAnnotation& s) {
        TypeSpec type;
        // `any` takes every value; `unit` is what a function that returns nothing is declared as.
        if (!s.present() || s.name == "any" || s.name == "unit") return type;

        uint8_t id = builtinTypeId(s.name);
        if (id == 0) {
            id = kTypeInstance;
            type.className = s.name;
        }
        type.flags = static_cast<uint8_t>(id | (s.nullable ? kTypeNullable : 0));
        return type;
    }

    // An instruction whose operand is a type: a class is passed on the stack, above the value.
    void emitTypeOperand(Op op, const TypeSpec& type, const TypeAnnotation& at) {
        if (type.id() == kTypeInstance) emitTypeName(type.className, at);
        emit(op);
        emitByte(type.flags);
    }

    // Rejects an initializer that can never fit the declared type. Returns whether the value
    // still has to be checked when the program runs.
    bool checkInitializer(const VarStmt& s, const TypeSpec& type) {
        uint8_t actual = s.init ? staticTypeOf(*s.init) : kStaticNull;
        if (actual == 0) return true;

        if (actual == kStaticNull) {
            if (type.nullable()) return false;
            if (!s.init)
                throw error(s, "variable '" + s.name + "' of type " + type.text() + " must be initialized (declare it as " +
                                   type.text() + "? to allow null)");
            throw error(*s.init, "cannot initialize '" + s.name + "' of type " + type.text() + " with null (declare it as " +
                                     type.text() + "? to allow null)");
        }

        bool fits = type.id() == actual || (type.id() == kTypeNumber && (actual == kTypeInt || actual == kTypeFloat));
        if (!fits)
            throw error(*s.init, "cannot initialize '" + s.name + "' of type " + type.text() + " with a value of type " +
                                     typeText(actual, ""));
        return false;
    }

    // Emits a check that the value a typed function is about to return fits its return type.
    // `actual` is what is statically known about that value (see staticTypeOf).
    void emitReturnCheck(uint8_t actual, const Node& at) {
        const TypeSpec& type = fs_->returnType;
        if (!type.typed()) return;

        if (actual == kStaticNull) {
            if (type.nullable()) return;
            throw error(at, fs_->proto->name + "() is declared to return " + type.text() + " and cannot return null");
        }
        if (actual != 0) {
            bool fits = type.id() == actual || (type.id() == kTypeNumber && (actual == kTypeInt || actual == kTypeFloat));
            if (fits) return;
            throw error(at, fs_->proto->name + "() is declared to return " + type.text() +
                                " and cannot return a value of type " + typeText(actual, ""));
        }

        if (type.id() == kTypeInstance) emitTypeName(type.className, at);
        emit(Op::CheckReturn);
        emitByte(type.flags);
    }

    // Rejects an initial or default value that can never fit the declared type.
    void checkStaticFit(const TypeSpec& type, const Expr& value, const std::string& what) {
        uint8_t actual = staticTypeOf(value);
        if (actual == 0) return;
        bool fits = actual == kStaticNull ? type.nullable()
                    : type.id() == actual || (type.id() == kTypeNumber && (actual == kTypeInt || actual == kTypeFloat));
        if (!fits)
            throw error(value, "cannot initialize " + what + " of type " + type.text() + " with " +
                                   (actual == kStaticNull ? std::string("null") : "a value of type " + typeText(actual, "")));
    }

    // Emits a check that the value on top of the stack fits `type`. Does nothing if untyped.
    void emitTypeCheck(const TypeSpec& type, const std::string& variable, const Node& at) {
        if (!type.typed()) return;
        if (type.id() == kTypeInstance) emitTypeName(type.className, at);
        emit(Op::CheckType, stringConstant(variable));
        emitByte(type.flags);
    }

    // ---- Statements ----

    void compileBlockBody(const std::vector<StmtPtr>& statements) {
        for (const StmtPtr& stmt : statements) compileStmt(*stmt);
    }

    void compileStmt(const Stmt& stmt) {
        line_ = stmt.line;
        const bool inExpression = std::exchange(jumpInExpression_, false);
        switch (stmt.kind) {
            case StmtKind::Expr: {
                const Expr& expr = *static_cast<const ExprStmt&>(stmt).expr;
                if (expr.kind == ExprKind::IncDec) {
                    compileIncDec(static_cast<const IncDecExpr&>(expr));  // a statement; leaves no value
                } else {
                    compileExpr(expr);
                    emit(Op::Pop);
                }
                break;
            }

            case StmtKind::Var: {
                const auto& s = static_cast<const VarStmt&>(stmt);
                if (!s.pattern.empty()) {
                    compileDestructuring(s);
                    break;
                }
                TypeSpec type = resolveType(s.type);
                bool checkAtRuntime = type.typed() && checkInitializer(s, type);

                if (isGlobalScope()) {
                    int global = declareGlobal(s.name, s.isVal, s);
                    if (type.id() == kTypeInstance) session_.globalClassTypes_[global] = type.className;
                    else session_.globalClassTypes_.erase(global);
                    if (s.init) compileExprFor(*s.init, type);
                    else emit(Op::Null);
                    line_ = s.line;
                    if (type.typed()) {
                        // The VM remembers the type and enforces it on every later assignment.
                        if (type.id() == kTypeInstance) emitTypeName(type.className, s.type);
                        emit(Op::DefineTypedGlobal, global);
                        emitByte(type.flags);
                    } else {
                        emit(Op::DefineGlobal, global);
                    }
                } else {
                    int slot = addLocal(s.name, s.isVal, s, false, type);
                    size_t codeMark = code().size();
                    size_t protoMark = fs_->proto->protos.size();
                    size_t breakMark = fs_->loop != nullptr ? fs_->loop->breakJumps.size() : 0;
                    if (s.init) compileExprFor(*s.init, type);
                    else emit(Op::Null);
                    line_ = s.line;

                    if (fs_->locals[static_cast<size_t>(slot)].selfCaptured) {
                        // A function inside the initial value refers to the variable itself: a
                        // callback that cancels its own timer, a lambda that calls itself. The
                        // value is compiled again, this time with the variable's slot set aside
                        // first and marked "no value yet", so that such a function finds either
                        // the value or the mark instead of whatever happens to be on the stack.
                        code().resize(codeMark);
                        fs_->proto->lines.resize(codeMark);
                        fs_->proto->protos.resize(protoMark);
                        if (fs_->loop != nullptr) fs_->loop->breakJumps.resize(breakMark);

                        emit(Op::Unset);
                        fs_->locals[static_cast<size_t>(slot)].depth = kPending;
                        compileExprFor(*s.init, type);
                        line_ = s.line;
                        if (type.typed()) emitTypeCheck(type, s.name, s.type);
                        emit(Op::SetLocal, slotOf(slot));
                        emit(Op::Pop);
                    } else if (checkAtRuntime) {
                        emitTypeCheck(type, s.name, s.type);
                    }
                    fs_->locals[static_cast<size_t>(slot)].depth = fs_->scopeDepth;
                }
                break;
            }

            case StmtKind::Block:
                beginScope();
                compileBlockBody(static_cast<const BlockStmt&>(stmt).statements);
                endScope();
                break;

            case StmtKind::If: {
                const auto& s = static_cast<const IfStmt&>(stmt);
                compileExpr(*s.condition);
                int toElse = emitJump(Op::JumpIfFalse);
                compileStmt(*s.then);
                if (!s.otherwise) {
                    patchJump(toElse);
                } else {
                    int toEnd = emitJump(Op::Jump);
                    patchJump(toElse);
                    compileStmt(*s.otherwise);
                    patchJump(toEnd);
                }
                break;
            }

            case StmtKind::While: {
                const auto& s = static_cast<const WhileStmt&>(stmt);
                LoopState loop{fs_->loop, here(), fs_->scopeDepth, fs_->tryDepth, fs_->locals.size(), fs_->slotTop, {}};
                loop.label = s.label;
                fs_->loop = &loop;
                if (s.doWhile) {
                    // The body first, then the test; `continue` goes to the test.
                    loop.continueForward = true;
                    compileStmt(*s.body);
                    for (int jump : loop.continueJumps) patchJump(jump);
                    line_ = s.condition->line;
                    compileExpr(*s.condition);
                    int toExit = emitJump(Op::JumpIfFalse);
                    emitLoop(loop.start);
                    patchJump(toExit);
                    for (int jump : loop.breakJumps) patchJump(jump);
                    fs_->loop = loop.enclosing;
                    break;
                }
                compileExpr(*s.condition);
                int toExit = emitJump(Op::JumpIfFalse);
                compileStmt(*s.body);
                emitLoop(loop.start);
                patchJump(toExit);
                for (int jump : loop.breakJumps) patchJump(jump);
                fs_->loop = loop.enclosing;
                break;
            }

            case StmtKind::ForIn:
                compileForIn(static_cast<const ForInStmt&>(stmt));
                break;

            case StmtKind::Return: {
                const auto& s = static_cast<const ReturnStmt&>(stmt);
                if (!s.label.empty() && compileLabelledReturn(s, inExpression)) break;
                if (fs_->isLambda && !s.implicit && s.label.empty())
                    throw error(s, "'return' leaves the function it is written in, which a lambda cannot do except inside " +
                                       std::string("let, also or takeIf; to leave the lambda itself write return@") +
                                       (!fs_->calleeLabel.empty() ? fs_->calleeLabel : !fs_->label.empty() ? fs_->label : "name"));
                if (fs_->kind == FunctionKind::Initializer) {
                    if (s.value) throw error(s, "cannot return a value from an initializer");
                    emit(Op::GetLocal, 0);
                } else if (fs_->kind == FunctionKind::Setter) {
                    if (s.value) throw error(s, "cannot return a value from a setter");
                    emit(Op::GetLocal, 1);
                } else if (s.value) {
                    compileExprFor(*s.value, fs_->returnType);
                    line_ = s.line;
                    emitReturnCheck(staticTypeOf(*s.value), *s.value);
                } else {
                    emitReturnCheck(kStaticNull, s);
                    emit(Op::Null);
                }
                if (fs_->finally != nullptr) {
                    // The value waits in a slot of its own while the finally blocks run.
                    if (inExpression) emitSettle(true);
                    beginScope();
                    int result = addLocal("(return value)", false, s, true);
                    emitPendingFinally(nullptr);
                    emit(Op::GetLocal, slotOf(result));
                    emit(fs_->kind == FunctionKind::Initializer ? Op::ReturnInit : Op::Return);
                    endScope();
                    break;
                }
                emit(fs_->kind == FunctionKind::Initializer ? Op::ReturnInit : Op::Return);
                break;
            }

            case StmtKind::Break: {
                LoopState* loop = loopFor(static_cast<const BreakStmt&>(stmt).label, stmt, "break");
                if (inExpression) emitSettle(false);
                emitLoopExit(*loop);
                loop->breakJumps.push_back(emitJump(Op::Jump));
                break;
            }

            case StmtKind::Continue: {
                LoopState* loop = loopFor(static_cast<const ContinueStmt&>(stmt).label, stmt, "continue");
                if (inExpression) emitSettle(false);
                emitLoopExit(*loop);
                if (loop->continueForward) loop->continueJumps.push_back(emitJump(Op::Jump));
                else emitLoop(loop->start);
                break;
            }

            case StmtKind::Throw:
                compileExpr(*static_cast<const ThrowStmt&>(stmt).value);
                line_ = stmt.line;
                emit(Op::Throw);
                break;

            case StmtKind::Try:
                compileTry(static_cast<const TryStmt&>(stmt));
                break;

            case StmtKind::Import: {
                const auto& s = static_cast<const ImportStmt&>(stmt);
                if (fs_->kind != FunctionKind::Script || !isGlobalScope())
                    throw error(s, "'import' is only allowed at the top level of a file");
                std::vector<int> modules;
                Namespace* imported = session_.importModule(s.path, directory_, file_, s, modules);
                line_ = s.line;
                for (int module : modules) {
                    emit(Op::Import, module);
                    emit(Op::Pop);
                }
                // From here on this file reaches what it imported by the import's name, and with
                // .* by bare names as well. A file of its own folder shares its names anyway.
                if (s.alias.empty() && !s.all && imported != ns_)
                    throw error(s, "'" + s.path + "' gives no name to reach its contents by; add one (import \"" + s.path +
                                       "\" as name) or import its names with .*");
                if (!s.alias.empty()) {
                    for (const Import& earlier : imports_)
                        if (earlier.alias == s.alias && earlier.ns != imported)
                            throw error(s, "'" + s.alias + "' already names another import");
                }
                bool known = false;
                for (const Import& earlier : imports_)
                    known = known || (earlier.ns == imported && earlier.alias == s.alias && earlier.all == s.all);
                if (!known) imports_.push_back(Import{imported, s.alias, s.path, s.all});
                break;
            }

            case StmtKind::Function: {
                const FunctionDecl& fn = *static_cast<const FunctionStmt&>(stmt).fn;
                if (!static_cast<const FunctionStmt&>(stmt).receiver.empty()) {
                    compileExtension(static_cast<const FunctionStmt&>(stmt));
                    break;
                }
                if (isGlobalScope()) {
                    int global = declareGlobal(fn.name, false, stmt);
                    compileFunction(fn, FunctionKind::Function, fn.name);
                    emit(Op::DefineGlobal, global);
                } else {
                    // Initialized up front so the function can call itself.
                    addLocal(fn.name, false, stmt, true);
                    compileFunction(fn, FunctionKind::Function, fn.name);
                }
                break;
            }

            case StmtKind::Class:
                compileClass(static_cast<const ClassStmt&>(stmt));
                break;
        }
    }

    // `return@name value`. Inside a block compiled in place it ends the block with that value,
    // which this handles, returning true. Naming the lambda being compiled it is an ordinary
    // return from it, left to the caller (false). Anything else is an error.
    bool compileLabelledReturn(const ReturnStmt& s, bool inExpression) {
        for (InlineBlock* block = inline_; block != nullptr && block->function == fs_; block = block->enclosing) {
            if (block->name != s.label && block->label != s.label) continue;
            if (s.value) compileExpr(*s.value);
            else emit(Op::Null);
            line_ = s.line;
            if (inExpression) emitSettle(true);

            // The value waits in a slot of its own while any finally blocks run; then all that
            // the block has put on the stack goes, from under it.
            beginScope();
            int result = addLocal("(block value)", false, s, true);
            int open = emitPendingFinally(nullptr, block->finallyAtEntry);
            for (int i = open; i > block->tryDepth; i--) emit(Op::TryEnd);
            emit(Op::GetLocal, slotOf(result));
            emit(Op::PopUnder, fs_->slotTop - (block->valueSlot + 1));
            block->exits.push_back(emitJump(Op::Jump));
            endScope();
            return true;
        }
        if (fs_->isLambda && (fs_->label == s.label || fs_->calleeLabel == s.label)) return false;

        for (const FunctionState* outer = fs_->enclosing; outer != nullptr; outer = outer->enclosing)
            if (outer->isLambda && (outer->label == s.label || outer->calleeLabel == s.label))
                throw error(s, "return@" + s.label + " can only leave the lambda it is written in, not one further out");
        throw error(s, "there is no lambda called '" + s.label + "' around this return");
    }

    // value.let { ... }, value.also { ... } and value.takeIf { ... } with a lambda literal are
    // compiled in place: the value becomes a variable, the lambda's body runs as part of this
    // function, and so a `return` in it leaves this function. Returns false for any other call.
    bool compileScopeCall(const CallExpr& e) {
        if (e.callee->kind != ExprKind::Member || e.args.size() != 1 || e.namedCount != 0 || e.hasSpread) return false;
        if (e.args[0]->kind != ExprKind::Function) return false;
        const auto& member = static_cast<const MemberExpr&>(*e.callee);
        const FunctionDecl& fn = *static_cast<const FunctionExpr&>(*e.args[0]).fn;
        const bool let = member.name == "let", also = member.name == "also", takeIf = member.name == "takeIf";
        if (!fn.isLambda || fn.params.size() != 1 || !(let || also || takeIf)) return false;

        const int saved = enterValueBlock();
        beginScope();
        compileExpr(*member.object);
        line_ = member.line;
        const int toNull = member.safe ? emitJump(Op::JumpIfNull) : -1;
        const bool parts = !fn.paramPatterns.empty() && !fn.paramPatterns[0].empty();
        const int value = addLocal(parts ? "(scope value)" : fn.params[0], true, fn, true);

        InlineBlock block{inline_, member.name, fn.label, fs_, slotOf(value), fs_->finally, fs_->tryDepth, {}};
        inline_ = &block;
        beginScope();
        if (parts) declareParts(fn.paramPatterns[0], value, true, fn);
        for (size_t i = 0; i + 1 < fn.body.size(); i++) compileStmt(*fn.body[i]);
        const Stmt* last = fn.body.empty() ? nullptr : fn.body.back().get();
        if (last != nullptr && last->kind == StmtKind::Return && static_cast<const ReturnStmt&>(*last).implicit) {
            line_ = last->line;
            compileExpr(*static_cast<const ReturnStmt&>(*last).value);  // the block's value
        } else {
            if (last != nullptr) compileStmt(*last);
            emit(Op::Null);
        }
        endScopeKeepingValue();
        for (int jump : block.exits) patchJump(jump);
        inline_ = block.enclosing;

        // The stack holds the value and, above it, what the block came to.
        line_ = e.line;
        if (also) {
            emit(Op::Pop);
            emit(Op::GetLocal, slotOf(value));
        } else if (takeIf) {
            int toNo = emitJump(Op::JumpIfFalse);
            emit(Op::GetLocal, slotOf(value));
            int toEnd = emitJump(Op::Jump);
            patchJump(toNo);
            emit(Op::Null);
            patchJump(toEnd);
        }
        endScopeKeepingValue();
        if (toNull >= 0) patchJump(toNull);
        leaveValueBlock(saved);
        return true;
    }

    // val (a, b) = value: the value is worked out once, then each name gets its part.
    void compileDestructuring(const VarStmt& s) {
        compileExpr(*s.init);
        line_ = s.line;
        if (isGlobalScope()) {
            for (size_t i = 0; i < s.pattern.size(); i++) {
                if (s.pattern[i] == "_") continue;
                int global = declareGlobal(s.pattern[i], s.isVal, s);
                emit(Op::Dup);
                emit(Op::Component);
                emitByte(static_cast<int>(i));
                emit(Op::DefineGlobal, global);
            }
            emit(Op::Pop);
            return;
        }
        int whole = addLocal("(taken apart)", true, s, true);
        declareParts(s.pattern, whole, s.isVal, s);
    }

    // Declares a local for each name of a pattern, from the parts of the local `whole`.
    void declareParts(const std::vector<std::string>& pattern, int whole, bool isVal, const Node& at) {
        for (size_t i = 0; i < pattern.size(); i++) {
            if (pattern[i] == "_") continue;
            emit(Op::GetLocal, slotOf(whole));
            emit(Op::Component);
            emitByte(static_cast<int>(i));
            addLocal(pattern[i], isVal, at, true);
        }
    }

    // fun Shape.describe() { ... }: the function is compiled as a method, with the value it runs
    // on as `this`, and added to the type.
    void compileExtension(const FunctionStmt& s) {
        const FunctionDecl& fn = *s.fn;
        line_ = s.line;
        uint8_t type = 0;
        if (s.receiver != "any") {
            type = builtinTypeId(s.receiver);
            if (type == 0) {
                type = kTypeInstance;
                emitTypeName(s.receiver, s);
            }
        }
        nextIsExtension_ = true;
        if (type == kTypeInstance) nextReceiverType_ = s.receiver;
        compileFunction(fn, FunctionKind::Method, s.receiver + "." + fn.name);
        emit(Op::Extension);
        emitU16(stringConstant(fn.name));
        emitByte(type | (s.receiverNullable ? kTypeNullable : 0));
    }

    // The loop a break or continue is for: the innermost, or with `@name` the one of that name.
    LoopState* loopFor(const std::string& label, const Stmt& at, const char* keyword) {
        if (fs_->loop == nullptr) throw error(at, std::string("'") + keyword + "' outside of a loop");
        if (label.empty()) return fs_->loop;
        for (LoopState* loop = fs_->loop; loop != nullptr; loop = loop->enclosing)
            if (loop->label == label) return loop;
        throw error(at, "there is no loop called '" + label + "' around this " + keyword);
    }

    static bool loopWithin(const LoopState* inner, const LoopState* outer) {
        for (; inner != nullptr; inner = inner->enclosing)
            if (inner == outer) return true;
        return false;
    }

    // Leaves the open try blocks and discards the locals of a loop body before jumping out of it.
    void emitLoopExit(const LoopState& loop) {
        int open = emitPendingFinally(&loop);
        for (int i = open; i > loop.tryDepth; i--) emit(Op::TryEnd);
        // Everything above the loop's own slots goes: its body's variables, and under a
        // variable declared inside an expression, the values that expression had built.
        const std::vector<Local>& locals = fs_->locals;
        for (int slot = fs_->slotTop; slot-- > loop.baseSlot;) {
            bool captured = false;
            for (size_t i = locals.size(); i-- > loop.localCount;)
                captured = captured || (locals[i].slot == slot && locals[i].isCaptured);
            emit(captured ? Op::CloseUpvalue : Op::Pop);
        }
    }

    // A break, continue or return in the middle of an expression (`val x = next() ?: break`)
    // finds half-built values on the stack, and perhaps a variable whose value is the very
    // thing being worked out. This leaves one slot for each of the function's variables and
    // nothing else, which is what the code for leaving a loop or running a finally block expects.
    void emitSettle(bool keepTop) {
        if (!keepTop) emit(Op::Null);
        emit(Op::Settle, fs_->slotTop);
        if (!keepTop) emit(Op::Pop);
    }

    void compileForIn(const ForInStmt& s) {
        beginScope();
        compileExpr(*s.iterable);
        line_ = s.line;
        emit(Op::IterInit);
        int iteratorSlot = addLocal("(iterator)", false, s, true);

        LoopState loop{fs_->loop, here(), fs_->scopeDepth, fs_->tryDepth, fs_->locals.size(), fs_->slotTop, {}};
        loop.label = s.label;
        fs_->loop = &loop;
        emit(Op::IterNext, slotOf(iteratorSlot));
        int toExit = here();
        emitI32(0);

        // The loop variable lives in a scope of its own, so each iteration gets a fresh
        // variable and closures capture the value of their own iteration.
        beginScope();
        if (s.pattern.empty()) {
            addLocal(s.variable, false, s, true);
        } else {
            int item = addLocal("(item)", true, s, true);
            declareParts(s.pattern, item, false, s);
        }
        compileBlockBody(s.body->statements);
        endScope();

        emitLoop(loop.start);
        patchJump(toExit);
        for (int jump : loop.breakJumps) patchJump(jump);
        fs_->loop = loop.enclosing;
        endScope();
    }

    // Emits the `finally` blocks that control is about to jump out of, innermost first: all of
    // them for a return, and for a break or continue those entered inside `loop`. Each block's
    // own try is left first, so that an error in the block is not caught by that try. Returns
    // how many try blocks are still open where the emitted code ends.
    int emitPendingFinally(const LoopState* loop, const FinallyState* stop = nullptr) {
        FinallyState* savedFinally = fs_->finally;
        const int savedDepth = fs_->tryDepth;
        for (FinallyState* f = savedFinally; f != nullptr && f != stop && (loop == nullptr || loopWithin(f->loop, loop)); f = f->enclosing) {
            for (int i = fs_->tryDepth; i > f->tryDepth; i--) emit(Op::TryEnd);
            fs_->tryDepth = f->tryDepth;
            fs_->finally = f->enclosing;
            compileStmt(*f->block);
        }
        const int open = fs_->tryDepth;
        fs_->finally = savedFinally;
        fs_->tryDepth = savedDepth;
        return open;
    }

    void compileTry(const TryStmt& s) {
        if (s.finalizer == nullptr) {
            compileTryCatch(s);
            return;
        }

        // The try and its catches sit inside an outer try whose only job is the finally block:
        // an error that gets past them lands in its handler, which runs the block and throws
        // the error on. Leaving normally runs the block once, after the outer try has ended.
        int toHandler = emitJump(Op::TryBegin);
        FinallyState state{fs_->finally, s.finalizer.get(), fs_->tryDepth, fs_->loop};
        fs_->tryDepth++;
        fs_->finally = &state;
        if (s.catches.empty()) compileTryBlock(s, *s.body);
        else compileTryCatch(s);
        fs_->finally = state.enclosing;
        fs_->tryDepth--;
        emit(Op::TryEnd);
        compileStmt(*s.finalizer);
        int toEnd = emitJump(Op::Jump);

        patchJump(toHandler);
        beginScope();
        int thrown = addLocal("(exception)", false, s, true);
        compileStmt(*s.finalizer);
        emit(Op::GetLocal, slotOf(thrown));
        emit(Op::Throw);
        endScope();
        patchJump(toEnd);
    }

    void compileTryCatch(const TryStmt& s) {
        int toHandler = emitJump(Op::TryBegin);
        fs_->tryDepth++;
        compileTryBlock(s, *s.body);
        fs_->tryDepth--;
        emit(Op::TryEnd);
        std::vector<int> toEnd = {emitJump(Op::Jump)};

        // The VM enters the handler with the stack cut back to where it was at TryBegin and
        // the thrown value pushed, which makes that value the first local of the handler.
        patchJump(toHandler);
        beginScope();
        int thrown = addLocal("(exception)", false, s, true);
        bool catchesAll = false;
        for (const CatchClause& clause : s.catches) {
            // A catch for one class of error is skipped by errors of any other class.
            int toNext = -1;
            if (clause.type.present()) {
                TypeSpec type = resolveType(clause.type);
                if (type.id() != kTypeInstance)
                    throw error(clause.type, "a catch names a class of error, as in 'catch (e : NotFound)'");
                emit(Op::GetLocal, slotOf(thrown));
                emitTypeName(type.className, clause.type);
                emit(Op::Is);
                toNext = emitJump(Op::JumpIfFalse);
            } else {
                catchesAll = true;
            }

            beginScope();
            if (clause.hasVariable) {
                emit(Op::GetLocal, slotOf(thrown));
                addLocal(clause.variable, false, s, true);
            }
            compileTryBlock(s, *clause.body);
            endScope();

            if (toNext >= 0) {
                emit(Op::Pop);  // the thrown value: this path leaves the handler here
                toEnd.push_back(emitJump(Op::Jump));
                patchJump(toNext);
            }
        }
        if (!catchesAll) {
            // No catch was for this error: it carries on outwards.
            emit(Op::GetLocal, slotOf(thrown));
            emit(Op::Throw);
        }
        endScope();
        for (int jump : toEnd) patchJump(jump);
    }

    // A class is compiled in two parts. Its structure: the class itself, what it inherits, its
    // fields, methods and the classes declared inside it. And its values: the entries of an
    // enum, the static fields, the one instance of an object. At the top level of a file the
    // structure of every class is set up before anything else runs, so that classes can refer
    // to each other whatever order they are written in, while the values are worked out where
    // the class is written, when the code above it has run.
    // The static values are worked out by a function attached to the class, which runs the
    // first time one of them is asked for, or failing that where the class is written.
    enum class ClassPart { All, Structure, Values };

    // `bound` is the name the class is kept under: its own, or `Outer.Inner` when it is declared
    // inside the class `outer`.
    void compileClass(const ClassStmt& s, std::string bound = "", const ClassState* outer = nullptr,
                      ClassPart part = ClassPart::All) {
        if (bound.empty()) bound = s.name;
        // For an object the name belongs to the instance, and the class is kept out of sight.
        const std::string storage = s.isObject ? "(object " + bound + ")" : bound;

        ClassState cls{class_};
        cls.storage = storage;
        cls.qualified = bound;
        cls.outer = outer;
        cls.definedIn = fs_;
        cls.hasSuper = s.hasSuper;
        cls.isEnum = s.isEnum;
        if (!s.isInterface && !s.hasSuper && !s.fields.empty()) cls.closed = &s;
        for (const auto& field : s.fields) cls.members.insert(field->name);
        for (const auto& field : s.staticFields) cls.statics.insert(field->name);
        for (const auto& inner : s.nested) cls.statics.insert(inner->name);
        for (const auto& method : s.methods) (method->isStatic ? cls.statics : cls.members).insert(method->name);
        if (s.isEnum) {
            for (const EnumEntry& entry : s.entries) cls.statics.insert(entry.name);
            cls.statics.insert("entries");
            cls.members.insert("name");
            cls.members.insert("ordinal");
        }

        const bool all = part == ClassPart::All;
        if (all || part == ClassPart::Structure) declareClass(s, storage);
        class_ = &cls;
        for (const auto& field : s.fields) cls.fieldTypes[field->name] = resolveType(field->type);
        for (const auto& field : s.staticFields) cls.fieldTypes[field->name] = resolveType(field->type);
        if (s.isEnum) registerEnum(s, bound);
        for (const auto& inner : s.nested)
            if (inner->isEnum) registerEnum(*inner, bound + "." + inner->name);  // before the methods that use it

        if (all || part == ClassPart::Structure) compileClassStructure(s, bound, storage, cls);
        if (all || part == ClassPart::Values) compileClassValues(s, bound, storage, cls);
        class_ = cls.enclosing;
    }

    void declareClass(const ClassStmt& s, const std::string& storage) {
        int nameConstant = stringConstant(s.name);
        line_ = s.line;
        if (isGlobalScope()) {
            int global = declareGlobal(storage, false, s);
            emit(Op::Class, nameConstant);
            emit(Op::DefineGlobal, global);
        } else {
            addLocal(storage, false, s, true);
            emit(Op::Class, nameConstant);
        }
    }

    // The classes declared inside a class, with any that extends another of them after it.
    static std::vector<const ClassStmt*> nestedInOrder(const ClassStmt& s) {
        std::vector<const ClassStmt*> order;
        std::vector<char> placed(s.nested.size(), 0);
        for (size_t round = 0; round < s.nested.size() && order.size() < s.nested.size(); round++) {
            for (size_t i = 0; i < s.nested.size(); i++) {
                if (placed[i]) continue;
                bool waits = false;
                for (const std::string& supertype : s.nested[i]->supertypes)
                    for (size_t j = 0; j < s.nested.size(); j++)
                        waits = waits || (!placed[j] && j != i && s.nested[j]->name == supertype);
                if (waits) continue;
                placed[i] = 1;
                order.push_back(s.nested[i].get());
            }
        }
        for (size_t i = 0; i < s.nested.size(); i++)
            if (!placed[i]) order.push_back(s.nested[i].get());  // a cycle: left for the VM to report
        return order;
    }

    void compileClassStructure(const ClassStmt& s, const std::string& bound, const std::string& storage, ClassState& cls) {
        if (s.isInterface || s.isAbstract) {
            emitGet(storage, s);
            emit(s.isInterface ? Op::MarkInterface : Op::MarkAbstract);
            emit(Op::Pop);
        }
        if (s.hasSuper) {
            for (const std::string& supertype : s.supertypes) emitTypeName(supertype, s);
            emitGet(storage, s);
            emit(Op::Inherit);
            emitByte(static_cast<int>(s.supertypes.size()));

            // The superclass (null if all of them were interfaces) stays on the stack as a
            // hidden local that methods capture for `super`.
            beginScope();
            addLocal("super", false, s, true);
        }

        emitGet(storage, s);
        if (s.isData) {
            for (const HeaderParam& param : s.header) emit(Op::DataField, stringConstant(param.name));
        }
        if (s.isInterface) {
            // An interface holds no values: a `val` or `var` in it is a property that the
            // implementing class has to have.
            for (const auto& field : s.fields) {
                line_ = field->line;
                if (!field->init && field->getter) continue;  // comes with the interface
                if (field->init)
                    throw error(*field, "an interface cannot hold a value; declare '" + field->name +
                                            "' without one, for the class to supply, or give it a getter");
                emit(Op::Require, stringConstant(field->name));
            }
        } else {
            // What an abstract class leaves for its subclasses, and what is marked override.
            for (const auto& field : s.fields) {
                line_ = field->line;
                if (field->isAbstract) {
                    if (!s.isAbstract)
                        throw error(*field, "'" + field->name + "' is abstract, which only a member of an abstract class can be");
                    emit(Op::Require, stringConstant(field->name));
                } else {
                    emitOverrideCheck(s, field->name, field->isOverride, true);
                }
            }
            compileFields(s);
        }

        if (hasGeneratedInit(s)) {
            compileGeneratedInit(s);
        } else {
            // The class writes its own init, so the header can only declare fields.
            if (s.hasSuperArgs)
                throw error(s, "class '" + s.name + "' passes arguments to its superclass in the header but also defines " +
                                   "init; call super.init(...) inside init instead");
            for (const HeaderParam& param : s.header)
                if (!param.isField)
                    throw error(param, "'" + param.name + "' in the header of class '" + s.name + "' is not a field (no val or " +
                                           "var); since the class defines its own init, declare the parameter there");
        }

        std::unordered_set<std::string> seen;
        for (const auto& method : s.methods) {
            if (s.isInterface && !method->isStatic && method->name == "init")
                throw error(*method, "an interface cannot have an init: it is never created");
            if (!hasGeneratedInit(s) && !s.initBlocks.empty())
                throw error(s, "class '" + s.name + "' has init { } blocks and also defines fun init; use one or the other");
            const bool isAccessor = method->accessor != FunctionDecl::Accessor::None;
            if (!method->isStatic && !method->isAbstract && !method->generated && (isAccessor || method->name != "init")) {
                line_ = method->line;
                emitOverrideCheck(s, method->name, method->isOverride, isAccessor);
            }
            if (method->isAbstract) {
                // No body: the class that implements the interface, or extends the abstract
                // class, writes it.
                line_ = method->line;
                if (!s.isInterface && !s.isAbstract)
                    throw error(*method, "'" + method->name + "' has no body, which only a method of an interface or of an " +
                                             std::string("abstract class can be without"));
                if (method->isStatic) throw error(*method, "a static method needs a body");
                emit(Op::Require, stringConstant(method->name));
                continue;
            }
            if (method->accessor != FunctionDecl::Accessor::None) {
                // A property: a getter, a setter, or one of each.
                bool isGetter = method->accessor == FunctionDecl::Accessor::Get;
                if (!seen.insert(std::string(method->isStatic ? "static " : "") + (isGetter ? "get " : "set ") + method->name).second)
                    throw error(*method, std::string("duplicate ") + (isGetter ? "getter" : "setter") + " for '" + method->name +
                                             "' in class '" + s.name + "'");

                if (method->isStatic) {
                    compileFunction(*method, FunctionKind::StaticMethod, s.name + "." + method->name);
                    emit(isGetter ? Op::StaticGetter : Op::StaticSetter, stringConstant(method->name));
                } else if (isGetter) {
                    compileFunction(*method, FunctionKind::Method, s.name + "." + method->name);
                    emit(Op::Getter, stringConstant(method->name));
                } else {
                    compileFunction(*method, FunctionKind::Setter, s.name + "." + method->name);
                    emit(Op::Setter, stringConstant(method->name));
                }
                continue;
            }

            if (!seen.insert((method->isStatic ? "static " : "") + method->name).second)
                throw error(*method, "duplicate method '" + method->name + "' in class '" + s.name + "'");

            FunctionKind kind = method->isStatic ? FunctionKind::StaticMethod
                                : method->name == "init" ? FunctionKind::Initializer
                                : FunctionKind::Method;
            compileFunction(*method, kind, s.name + "." + method->name);
            emit(method->isStatic ? Op::StaticMethod : Op::Method, stringConstant(method->name));
        }
        // The accessors written under properties.
        for (const auto& field : s.fields) {
            if (!field->getter && !field->setter) continue;
            const std::string backing = field->init ? storedName(*field) : std::string();
            if (!field->getter)
                throw error(*field, "'" + field->name + "' has a setter but nothing to read: give it an initial value, or a get()");
            nextProperty_ = field->name;
            nextBackingField_ = backing;
            compileFunction(*field->getter, FunctionKind::Method, s.name + "." + field->name);
            emit(Op::Getter, stringConstant(field->name));
            if (field->setter) {
                nextProperty_ = field->name;
                nextBackingField_ = backing;
                compileFunction(*field->setter, FunctionKind::Setter, s.name + "." + field->name);
                emit(Op::Setter, stringConstant(field->name));
            }
        }
        emit(Op::FinishClass);
        if (hasStaticValues(s)) compileStaticInit(s, bound, storage, cls);
        emit(Op::Pop);
        if (s.hasSuper) endScope();

        // The classes declared inside come next, each made as a class of its own and stored as a
        // static member. This class is complete by now, so they may extend it.
        for (const ClassStmt* inner : nestedInOrder(s)) {
            const std::string innerBound = bound + "." + inner->name;
            if (cls.members.count(inner->name) != 0)
                throw error(*inner, "'" + inner->name + "' is both a member and a class inside '" + s.name + "'");
            compileClass(*inner, innerBound, &cls, ClassPart::Structure);
            if (!inner->isObject) storeNested(s, storage, *inner, innerBound);
        }
    }

    void storeNested(const ClassStmt& s, const std::string& storage, const ClassStmt& inner, const std::string& innerBound) {
        line_ = inner.line;
        emitGet(storage, s);
        emitGet(innerBound, inner);
        emit(Op::StaticField);
        emitU16(stringConstant(inner.name));
        emitU16(kFieldVal);
        emit(Op::Pop);
    }

    static bool hasStaticValues(const ClassStmt& s) {
        if (s.isEnum || !s.staticFields.empty()) return true;
        for (const auto& inner : s.nested)
            if (inner->isObject) return true;
        return false;
    }

    // The function that works out a class's static values: the entries of an enum, the one
    // instance of each object declared inside, then the static fields, in that order. The class
    // is on the stack, and the function is attached to it.
    void compileStaticInit(const ClassStmt& s, const std::string& bound, const std::string& storage, ClassState& cls) {
        FunctionState state;
        state.enclosing = fs_;
        state.kind = FunctionKind::StaticMethod;
        state.proto = session_.program.newProto();
        state.proto->name = s.name + ".<static>";
        state.proto->source = file_;
        state.scopeDepth = 1;
        state.add(Local{"", 1, VarInfo{}});
        InlineBlock* const outerInline = inline_;
        inline_ = nullptr;
        fs_ = &state;

        line_ = s.line;
        if (s.isEnum) {
            emitGet(storage, s);
            compileEnumEntries(s);
            emit(Op::Pop);
        }
        for (const ClassStmt* inner : nestedInOrder(s)) {
            if (!inner->isObject) continue;
            const std::string innerBound = bound + "." + inner->name;
            compileClass(*inner, innerBound, &cls, ClassPart::Values);
            storeNested(s, storage, *inner, innerBound);
        }
        if (!s.staticFields.empty()) {
            emitGet(storage, s);
            compileStaticFields(s);
            emit(Op::Pop);
        }
        line_ = s.line;
        emit(Op::Null);
        emit(Op::Return);
        finish(state);
        fs_ = state.enclosing;
        inline_ = outerInline;

        int index = static_cast<int>(fs_->proto->protos.size());
        fs_->proto->protos.push_back(state.proto);
        emit(Op::Closure, index);
        emit(Op::ClassInit);
    }

    // Where the class is written: its static values are worked out now, if nothing has asked
    // for them yet, and an object gets its one instance.
    void compileClassValues(const ClassStmt& s, const std::string& bound, const std::string& storage, ClassState& cls) {
        if (hasStaticValues(s)) {
            line_ = s.line;
            emitGet(storage, s);
            emit(Op::EnsureClassInit);
            emit(Op::Pop);
        }
        for (const ClassStmt* inner : nestedInOrder(s))
            if (!inner->isObject) compileClass(*inner, bound + "." + inner->name, &cls, ClassPart::Values);

        if (s.isObject) {
            line_ = s.line;
            emitGet(storage, s);
            emit(Op::Call);
            emitByte(0);
            if (isGlobalScope()) emit(Op::DefineGlobal, declareGlobal(bound, true, s));
            else addLocal(bound, true, s, true);
        }
    }

    // Every enum has `entries` (a static list), and each entry `name` (a string) and `ordinal`
    // (an int). A member of the same name clashes with these and is an error, unless it says
    // `override` and keeps the shape of what it replaces: a property that takes nothing and
    // gives back the same type.
    void checkEnumMembers(const ClassStmt& s) {
        struct Given {
            const char* name;
            bool isStatic;
            const char* type;
            const char* what;
            const char* form;
        };
        static const Given kGiven[] = {
            {"entries", true, "list", "the list of its entries", "override static get entries() : list"},
            {"name", false, "string", "each entry's name", "override get name() : string"},
            {"ordinal", false, "int", "each entry's place in the list", "override get ordinal() : int"},
        };
        for (const Given& given : kGiven) {
            auto clash = [&](const Node& at, bool isOverride) {
                if (isOverride) return;
                throw error(at, "enum '" + s.name + "' already has '" + given.name + "' (" + given.what +
                                    "); write it as an override to replace that: " + given.form);
            };
            auto shape = [&](const Node& at, bool fits) {
                if (fits) return;
                throw error(at, std::string("an override of '") + given.name + "' has to keep its shape: " + given.form);
            };
            auto typeFits = [&](const TypeAnnotation& type) { return !type.present() || (type.name == given.type && !type.nullable); };

            for (const auto& method : s.methods) {
                if (method->name != given.name) continue;
                clash(*method, method->isOverride);
                shape(*method, method->accessor == FunctionDecl::Accessor::Get && method->isStatic == given.isStatic &&
                                   typeFits(method->returnType));
            }
            for (const auto& field : s.fields) {
                if (field->name != given.name) continue;
                clash(*field, field->isOverride);
                shape(*field, false);  // an entry's name and ordinal are set for it: only a getter can stand in
            }
            for (const auto& field : s.staticFields) {
                if (field->name != given.name) continue;
                clash(*field, field->isOverride);
                shape(*field, given.isStatic && typeFits(field->type));
            }
        }
    }

    // A member that takes the place of one the class gets from above, or that supplies what an
    // interface or abstract class asks for, has to say `override`; and one that says it has to
    // have something to replace. What a class gets from above is only known when the program
    // runs, so that is when it is checked: as the class is made.
    void emitOverrideCheck(const ClassStmt& s, const std::string& name, bool marked, bool property) {
        // What every enum is given is replaced under rules of its own (checkEnumMembers).
        if (s.isEnum && (name == "name" || name == "ordinal" || name == "entries")) return;
        emit(Op::CheckOverride);
        emitU16(stringConstant(name));
        emitByte((marked ? 1 : 0) | (property ? 2 : 0));
    }

    // The entries of an enum are made here, in order, once the class is complete: each is an
    // instance of it, given its name and its place in the list.
    void compileEnumEntries(const ClassStmt& s) {
        std::unordered_set<std::string> taken;
        for (const auto& field : s.fields) taken.insert(field->name);
        for (const auto& field : s.staticFields) taken.insert(field->name);
        for (const auto& method : s.methods) taken.insert(method->name);
        checkEnumMembers(s);

        if (s.entries.size() > 0xFFFF) throw error(s, "an enum cannot have more than 65535 entries");
        for (size_t i = 0; i < s.entries.size(); i++) {
            const EnumEntry& entry = s.entries[i];
            line_ = entry.line;
            if (taken.count(entry.name) != 0)
                throw error(entry, "'" + entry.name + "' is both an entry and a member of enum '" + s.name + "'");
            emit(Op::EnumNext);
            emitU16(stringConstant(entry.name));
            emitU16(static_cast<int>(i));
            emit(Op::Dup);
            for (const ExprPtr& arg : entry.args) compileArg(*arg);
            line_ = entry.line;
            emit(Op::Call);
            emitByte(static_cast<int>(entry.args.size()));
            emit(Op::EnumEntry);
            emitU16(stringConstant(entry.name));
            emitU16(static_cast<int>(i));
        }
        emit(Op::EnumSeal);
    }

    // `static val` and `static var`: each gets its value here, once, in the order written, when
    // the class is complete, so a value may be an instance of the class or use its methods.
    void compileStaticFields(const ClassStmt& s) {
        std::unordered_set<std::string> taken;
        for (const auto& method : s.methods)
            if (method->isStatic) taken.insert(method->name);
        for (const auto& field : s.staticFields) {
            const VarStmt& f = *field;
            line_ = f.line;
            if (!taken.insert(f.name).second)
                throw error(f, "duplicate static member '" + f.name + "' in class '" + s.name + "'");

            TypeSpec type = resolveType(f.type);
            if (f.init && type.typed()) checkStaticFit(type, *f.init, "static field '" + f.name + "'");
            if (!f.init && type.typed() && !type.nullable())
                throw error(f, "static field '" + f.name + "' of type " + type.text() + " must be given a value");

            if (type.id() == kTypeInstance) emitTypeThunk(type.className, f.type);
            if (f.init) compileExprFor(*f.init, type);
            else emit(Op::Null);
            line_ = f.line;
            emit(Op::StaticField);
            emitU16(stringConstant(f.name));
            emitU16(type.flags | (f.isVal ? kFieldVal : 0));
        }
    }

    // A class with a header, or with superclass arguments, that writes no init of its own
    // gets one generated for it.
    static bool hasGeneratedInit(const ClassStmt& s) {
        if (!s.hasHeader && !s.hasSuperArgs && s.initBlocks.empty()) return false;
        for (const auto& method : s.methods)
            if (!method->isStatic && method->accessor == FunctionDecl::Accessor::None && method->name == "init") return false;
        return true;
    }

    // Compiles the initializer of `class Name(val a, var b, c) : Super(args)`, as if written
    //   fun init(a, b, c) { super.init(args); this.a = a; this.b = b }
    // An entry written `x = value` may be left out by the caller.
    // and attaches it to the class on top of the stack. Assigning the fields checks their types.
    void compileGeneratedInit(const ClassStmt& s) {
        FunctionState state;
        state.enclosing = fs_;
        state.kind = FunctionKind::Initializer;
        state.proto = session_.program.newProto();
        state.proto->name = s.name + ".init";
        state.proto->source = file_;
        state.proto->arity = static_cast<int>(s.header.size());
        state.scopeDepth = 1;
        state.add(Local{"this", 1, VarInfo{}});
        for (const HeaderParam& param : s.header) {
            state.add(Local{param.name, 1, VarInfo{}});
            state.proto->paramNames.push_back(session_.vm_.intern(param.name));
        }

        fs_ = &state;
        line_ = s.line;

        // Entries with a default value are optional arguments, so they have to come last.
        // Each default is computed here if the caller left the argument out.
        int requiredArgs = 0;
        bool sawDefault = false;
        for (size_t i = 0; i < s.header.size(); i++) {
            const HeaderParam& param = s.header[i];
            if (!param.defaultValue) {
                if (sawDefault)
                    throw error(param, "'" + param.name + "' has no default value but follows an entry that has one; " +
                                           "entries with defaults must come last");
                requiredArgs++;
                continue;
            }
            sawDefault = true;

            TypeSpec type = resolveType(param.type);
            if (type.typed())
                checkStaticFit(type, *param.defaultValue, (param.isField ? "field '" : "parameter '") + param.name + "'");

            line_ = param.line;
            int slot = static_cast<int>(i + 1);
            emit(Op::DefaultArg, slot);
            int toPassed = here();
            emitI32(0);

            // A default can use the entries before it. This one and the later ones may not have
            // a value yet, so they are out of reach while it is compiled.
            for (size_t j = i; j < s.header.size(); j++) state.locals[j + 1].depth = kUnavailable;
            compileExprFor(*param.defaultValue, type);
            for (size_t j = i; j < s.header.size(); j++) state.locals[j + 1].depth = 1;

            line_ = param.line;
            emit(Op::SetLocal, slot);
            emit(Op::Pop);
            patchJump(toPassed);
        }
        state.proto->minArity = requiredArgs;

        // Parameters that are not fields are checked here; fields are checked when assigned below.
        for (size_t i = 0; i < s.header.size(); i++) {
            const HeaderParam& param = s.header[i];
            TypeSpec type = resolveType(param.type);
            // A class-typed field is checked here as well, since a bare name may arrive for it.
            if (!type.typed() || (param.isField && type.id() != kTypeInstance)) continue;
            line_ = param.line;
            emit(Op::GetLocal, static_cast<int>(i + 1));
            if (type.id() == kTypeInstance) emitTypeName(type.className, param.type);
            emit(Op::CheckParam, stringConstant(param.name));
            emitByte(type.flags);
            if (type.id() == kTypeInstance) {
                if (i < 64) state.proto->classParams |= uint64_t{1} << i;
                emit(Op::SetLocal, static_cast<int>(i + 1));
            }
            emit(Op::Pop);
        }

        line_ = s.line;
        if (s.hasSuper) {
            // The superclass is initialized first, as in a hand-written init.
            emit(Op::GetLocal, 0);
            for (const ExprPtr& arg : s.superArgs) compileArg(*arg);
            line_ = s.line;
            emitGet("super", s);
            emit(Op::SuperInit);
            emitByte(static_cast<int>(s.superArgs.size()));
            emit(Op::Pop);
        }

        for (size_t i = 0; i < s.header.size(); i++) {
            const HeaderParam& param = s.header[i];
            if (!param.isField) continue;
            line_ = param.line;
            emit(Op::GetLocal, 0);
            emit(Op::GetLocal, static_cast<int>(i + 1));
            emit(Op::SetProp, stringConstant(param.name));
            emit(Op::Pop);
        }

        // Then the fields of the body, in the order written. Their values can use the header's
        // entries and the fields above them.
        // The init { } blocks run in between, each after the fields written above it.
        size_t nextBlock = 0;
        auto blocksBefore = [&](size_t fieldIndex) {
            for (; nextBlock < s.initBlocks.size() && s.initBlocks[nextBlock].first <= fieldIndex; nextBlock++)
                compileStmt(*s.initBlocks[nextBlock].second);
        };
        for (size_t k = 0; k < class_->initializedByInit.size(); k++) {
            const auto& [field, value] = class_->initializedByInit[k];
            blocksBefore(class_->initializedIndex[k]);
            line_ = field->line;
            emit(Op::GetLocal, 0);
            if (value != nullptr) compileExprFor(*value, resolveType(field->type));
            else emit(Op::Null);
            line_ = field->line;
            emit(Op::SetProp, stringConstant(storedName(*field)));
            emit(Op::Pop);
        }
        blocksBefore(s.fields.size());

        line_ = s.line;
        emit(Op::GetLocal, 0);
        emit(Op::ReturnInit);
        finish(state);
        fs_ = state.enclosing;

        int index = static_cast<int>(fs_->proto->protos.size());
        fs_->proto->protos.push_back(state.proto);
        emit(Op::Closure, index);
        emit(Op::Method, stringConstant("init"));
    }

    // Declares the fields of the class on top of the stack, and compiles the code that gives
    // each new instance its initial field values.
    void compileFields(const ClassStmt& s) {
        if (s.fields.empty()) return;

        std::unordered_set<std::string> methodNames, propertyNames;
        bool explicitInit = false;
        for (const auto& method : s.methods) {
            if (method->isStatic) continue;
            if (method->accessor != FunctionDecl::Accessor::None) {
                propertyNames.insert(method->name);
            } else {
                methodNames.insert(method->name);
                if (method->name == "init") explicitInit = true;
            }
        }
        bool generatedInit = hasGeneratedInit(s);

        // The initial value of each field. A header field's `= value` counts as one only when the
        // class writes its own init; for a generated init it is an argument default instead.
        std::vector<const Expr*> initialValues;
        for (const HeaderParam& param : s.header)
            if (param.isField) initialValues.push_back(generatedInit ? nullptr : param.defaultValue.get());
        for (size_t i = s.headerFieldCount; i < s.fields.size(); i++) initialValues.push_back(s.fields[i]->init.get());

        std::unordered_set<std::string> seen;
        std::vector<size_t> initialized;  // fields that get a value when an instance is created

        for (size_t i = 0; i < s.fields.size(); i++) {
            const VarStmt& f = *s.fields[i];
            const Expr* initialValue = initialValues[i];
            if (f.isAbstract) continue;  // not a field of this class: a subclass has it
            // A generated initializer assigns every header field from its arguments.
            bool setByGeneratedInit = generatedInit && i < s.headerFieldCount;
            line_ = f.line;
            // A property worked out by its accessors, with no value of its own, is not a field.
            if ((f.getter || f.setter) && initialValue == nullptr) {
                if (!seen.insert(f.name).second)
                    throw error(f, "duplicate field '" + f.name + "' in class '" + s.name + "'");
                continue;
            }
            if (!seen.insert(f.name).second)
                throw error(f, "duplicate field '" + f.name + "' in class '" + s.name + "'");
            if (propertyNames.count(f.name) != 0)
                throw error(f, "'" + f.name + "' is both a field and a property of class '" + s.name + "'");

            TypeSpec type = resolveType(f.type);
            if (initialValue != nullptr && type.typed()) checkStaticFit(type, *initialValue, "field '" + f.name + "'");

            // With no initial value, a val, or a var whose type excludes null, is left for init to set.
            bool required = !setByGeneratedInit && initialValue == nullptr && (f.isVal || (type.typed() && !type.nullable()));
            if (required && generatedInit)
                throw error(f, "field '" + f.name + "' has no initial value and is not in the class header, so nothing " +
                                   "would set it; give it a value, move it into the header, or write an init");
            if (required && !explicitInit)
                throw error(f, "field '" + f.name + "' has no initial value, so class '" + s.name +
                                   "' needs an init that sets it");
            if (!required && !setByGeneratedInit) initialized.push_back(i);

            if (type.id() == kTypeInstance) emitTypeThunk(type.className, f.type);
            emit(Op::DeclareField);
            emitU16(stringConstant(storedName(f)));
            emitU16(type.flags | (f.isVal ? kFieldVal : 0) | (required ? kFieldRequired : 0) | (f.isOverride ? kFieldOverride : 0));
        }

        if (initialized.empty()) return;

        if (generatedInit) {
            for (size_t i : initialized) {
                class_->initializedByInit.emplace_back(s.fields[i].get(), initialValues[i]);
                class_->initializedIndex.push_back(i);
            }
            return;
        }

        // A hidden method, run on each new instance before init: this.field = <initial value>.
        FunctionState state;
        state.enclosing = fs_;
        state.kind = FunctionKind::Method;
        state.proto = session_.program.newProto();
        state.proto->name = s.name + ".<fields>";
        state.proto->source = file_;
        state.scopeDepth = 1;
        state.add(Local{"this", 1, VarInfo{}});

        fs_ = &state;
        for (size_t i : initialized) {
            const VarStmt& f = *s.fields[i];
            line_ = f.line;
            emit(Op::GetLocal, 0);
            if (initialValues[i] != nullptr) compileExprFor(*initialValues[i], resolveType(f.type));
            else emit(Op::Null);
            line_ = f.line;
            emit(Op::SetProp, stringConstant(storedName(f)));
            emit(Op::Pop);
        }
        emit(Op::Null);
        emit(Op::Return);
        finish(state);
        fs_ = state.enclosing;

        line_ = s.line;
        int index = static_cast<int>(fs_->proto->protos.size());
        fs_->proto->protos.push_back(state.proto);
        emit(Op::Closure, index);
        emit(Op::FieldInit);
    }

    // Emits a closure that returns the class called `className`. A field's class type is
    // resolved through one on first use, so it can name a class declared further down.
    void emitTypeThunk(const std::string& className, const Node& at) {
        FunctionState state;
        state.enclosing = fs_;
        state.kind = FunctionKind::Function;
        state.proto = session_.program.newProto();
        state.proto->name = "<type " + className + ">";  // (the lookup itself is below)
        state.proto->source = file_;
        state.scopeDepth = 1;
        state.add(Local{"", 1, VarInfo{}});

        fs_ = &state;
        // Written in the class body, so the names of the class are in reach, as in a static member.
        const bool wasStatic = class_ != nullptr && class_->inStaticInit;
        if (class_ != nullptr) class_->inStaticInit = true;
        emitTypeName(className, at);
        if (class_ != nullptr) class_->inStaticInit = wasStatic;
        emit(Op::Return);
        finish(state);
        fs_ = state.enclosing;

        int index = static_cast<int>(fs_->proto->protos.size());
        fs_->proto->protos.push_back(state.proto);
        emit(Op::Closure, index);
    }

    void compileFunction(const FunctionDecl& decl, FunctionKind kind, const std::string& displayName) {
        FunctionState state;
        state.enclosing = fs_;
        state.kind = kind;
        state.proto = session_.program.newProto();
        state.proto->name = displayName;
        state.proto->source = file_;
        state.proto->arity = static_cast<int>(decl.params.size());
        state.proto->varargIndex = decl.varargIndex;
        state.scopeDepth = 1;
        state.isLambda = decl.isLambda;
        state.label = decl.label;
        state.calleeLabel = decl.calleeLabel;
        state.isExtension = std::exchange(nextIsExtension_, false);
        state.receiverType = std::exchange(nextReceiverType_, std::string());
        state.property = std::exchange(nextProperty_, std::string());
        state.backingField = std::exchange(nextBackingField_, std::string());

        // Slot 0 holds the receiver in methods and the callee itself otherwise.
        bool hasThis = kind == FunctionKind::Method || kind == FunctionKind::Initializer || kind == FunctionKind::Setter;
        state.add(Local{hasThis ? "this" : "", 1, VarInfo{}});
        for (size_t i = 0; i < decl.params.size(); i++) {
            state.add(Local{decl.params[i], 1, VarInfo{false, resolveType(decl.paramTypes[i])}});
            state.proto->paramNames.push_back(session_.vm_.intern(decl.params[i]));
        }

        state.returnType = resolveType(decl.returnType);
        if (kind == FunctionKind::Initializer && decl.returnType.present())
            throw error(decl.returnType, "an initializer cannot declare a return type");

        // A lambda written inside other lambdas can also reach the objects those run on,
        // innermost first: in `grid { row { ... } }` the inner block sees the row, then the grid.
        // A function written inside a method reaches that method's object the same way, after
        // any lambdas in between: the object is the last place a bare name is looked for.
        // So does a method of a class written inside a method: after its own object comes the
        // object of the method around the class.
        {
            int levels = 1;
            for (FunctionState* outer = state.enclosing; outer != nullptr; outer = outer->enclosing, levels++) {
                if (outer->isLambda || kindHasThis(outer->kind))
                    state.proto->outerReceivers.push_back(static_cast<uint16_t>(captureReceiver(&state, levels)));
                if (outer->kind != FunctionKind::Function) break;
            }
        }

        fs_ = &state;
        line_ = decl.line;

        // Parameters with a default value are optional, so they have to come last. Each default
        // is computed here if the caller left the argument out.
        int requiredArgs = 0;
        bool sawDefault = false;
        for (size_t i = 0; i < decl.params.size(); i++) {
            const Expr* defaultValue = decl.paramDefaults[i].get();
            if (static_cast<int>(i) == decl.varargIndex) {
                // Always given a list by the call, and what follows it is passed by name.
                sawDefault = true;
                continue;
            }
            if (defaultValue == nullptr) {
                if (!sawDefault) {
                    requiredArgs++;
                    continue;
                }
                // A parameter without a default after one with: `grid(id = null, build)`. It
                // is there to be given as a lambda after the call or by name, so a call may
                // pass fewer arguments than reach it. If it then has no value, that is an error.
                // (Spike: the message is thrown as text; M1 gives it a proper error.)
                line_ = decl.paramPositions[i].line;
                emit(Op::DefaultArg, static_cast<int>(i + 1));
                int toPassed = here();
                emitI32(0);
                emit(Op::MissingArg, stringConstant(decl.params[i]));
                patchJump(toPassed);
                continue;
            }
            sawDefault = true;

            TypeSpec type = state.locals[i + 1].info.type;
            if (type.typed()) checkStaticFit(type, *defaultValue, "parameter '" + decl.params[i] + "'");

            line_ = decl.paramPositions[i].line;
            int slot = static_cast<int>(i + 1);
            emit(Op::DefaultArg, slot);
            int toPassed = here();
            emitI32(0);

            // A default can use the parameters before it. This one and the later ones may not
            // have a value yet, so they are out of reach while it is compiled.
            for (size_t j = i; j < decl.params.size(); j++) state.locals[j + 1].depth = kUnavailable;
            compileExprFor(*defaultValue, type);
            for (size_t j = i; j < decl.params.size(); j++) state.locals[j + 1].depth = 1;

            line_ = decl.paramPositions[i].line;
            emit(Op::SetLocal, slot);
            emit(Op::Pop);
            patchJump(toPassed);
        }
        state.proto->minArity = requiredArgs;
        line_ = decl.line;

        // Typed parameters are checked on entry: slot 0 is the callee or receiver, then the parameters.
        for (size_t i = 0; i < decl.params.size(); i++) {
            TypeSpec type = state.locals[i + 1].info.type;
            if (!type.typed()) continue;
            emit(Op::GetLocal, static_cast<int>(i + 1));
            if (type.id() == kTypeInstance) emitTypeName(type.className, decl.paramTypes[i]);
            emit(Op::CheckParam, stringConstant(decl.params[i]));
            emitByte(type.flags);
            if (type.id() == kTypeInstance) {
                // A bare name that arrived here has been settled as an entry of the enum.
                if (i < 64) state.proto->classParams |= uint64_t{1} << i;
                emit(Op::SetLocal, static_cast<int>(i + 1));
            }
            emit(Op::Pop);
        }

        // A parameter written `(a, b)` comes apart into variables of those names.
        for (size_t i = 0; i < decl.paramPatterns.size(); i++)
            if (!decl.paramPatterns[i].empty()) declareParts(decl.paramPatterns[i], static_cast<int>(i + 1), true, decl);

        // A block being compiled in place belongs to the function around this one.
        InlineBlock* const outerInline = inline_;
        inline_ = nullptr;
        compileBlockBody(decl.body);
        inline_ = outerInline;

        // Running off the end returns null (or the instance, from an initializer).
        if (kind == FunctionKind::Initializer) {
            emit(Op::GetLocal, 0);
        } else if (kind == FunctionKind::Setter) {
            emit(Op::GetLocal, 1);  // the value it was given
        } else {
            emit(Op::Null);
            if (state.returnType.typed() && !state.returnType.nullable()) {
                if (state.returnType.id() == kTypeInstance) emitTypeName(state.returnType.className, decl.returnType);
                emit(Op::CheckReturn);
                emitByte(state.returnType.flags);
            }
        }
        emit(kind == FunctionKind::Initializer ? Op::ReturnInit : Op::Return);
        finish(state);
        fs_ = state.enclosing;

        line_ = decl.line;
        int index = static_cast<int>(fs_->proto->protos.size());
        fs_->proto->protos.push_back(state.proto);
        emit(Op::Closure, index);
    }

    // ---- Expressions ----

    void requireMethodContext(const Node& at, const char* keyword) {
        for (FunctionState* state = fs_; state != nullptr; state = state->enclosing) {
            if (state->kind == FunctionKind::Method || state->kind == FunctionKind::Initializer ||
                state->kind == FunctionKind::Setter)
                return;
            if (state->kind == FunctionKind::StaticMethod)
                throw error(at, std::string("cannot use '") + keyword + "' in a static method");
        }
        throw error(at, std::string("cannot use '") + keyword + "' outside of a method");
    }

    void requireSuper(const Node& at) {
        requireMethodContext(at, "super");
        if (class_ == nullptr || !class_->hasSuper)
            throw error(at, "cannot use 'super' in a class that has no superclass");
    }

    static Op binaryOp(TokenType type) {
        switch (type) {
            case TokenType::Plus:
            case TokenType::PlusEq: return Op::Add;
            case TokenType::Minus:
            case TokenType::MinusEq: return Op::Sub;
            case TokenType::Star:
            case TokenType::StarEq: return Op::Mul;
            case TokenType::Slash:
            case TokenType::SlashEq: return Op::Div;
            case TokenType::Percent:
            case TokenType::PercentEq: return Op::Mod;
            case TokenType::Shl:
            case TokenType::ShlEq: return Op::Shl;
            case TokenType::Shr:
            case TokenType::ShrEq: return Op::Shr;
            case TokenType::Amp:
            case TokenType::AmpEq: return Op::BitAnd;
            case TokenType::Pipe:
            case TokenType::PipeEq: return Op::BitOr;
            case TokenType::Caret:
            case TokenType::CaretEq: return Op::BitXor;
            case TokenType::EqEq:
            case TokenType::BangEq: return Op::Eq;
            case TokenType::EqEqEq:
            case TokenType::BangEqEq: return Op::Identical;
            case TokenType::Lt: return Op::Lt;
            case TokenType::LtEq: return Op::Le;
            case TokenType::Gt: return Op::Gt;
            case TokenType::GtEq: return Op::Ge;
            default: throw std::logic_error("not a binary operator");
        }
    }

    void compileArgs(const std::vector<ExprPtr>& args) {
        for (const ExprPtr& arg : args) compileArg(*arg);
    }

    void compileExpr(const Expr& expr) {
        line_ = expr.line;
        switch (expr.kind) {
            case ExprKind::Literal: {
                const auto& e = static_cast<const LiteralExpr&>(expr);
                switch (e.type) {
                    case LiteralExpr::Type::Null: emit(Op::Null); break;
                    case LiteralExpr::Type::Bool: emit(e.boolValue ? Op::True : Op::False); break;
                    case LiteralExpr::Type::Int: emit(Op::Const, intConstant(e.intValue)); break;
                    case LiteralExpr::Type::Float: emit(Op::Const, floatConstant(e.floatValue)); break;
                    case LiteralExpr::Type::String:
                        if (&e == workerLiteral_) {
                            // The path in Worker("..."): the call is given where the script is.
                            emit(Op::Const, stringConstant(workerSpec_));
                            workerLiteral_ = nullptr;
                        } else {
                            emit(Op::Const, stringConstant(e.stringValue));
                        }
                        break;
                }
                break;
            }

            case ExprKind::Interp: {
                const auto& e = static_cast<const InterpExpr&>(expr);
                for (const ExprPtr& part : e.parts) compileExpr(*part);
                line_ = e.line;
                emit(Op::BuildString, static_cast<int>(e.parts.size()));
                break;
            }

            case ExprKind::Name:
                emitGet(static_cast<const NameExpr&>(expr).name, expr);
                break;

            case ExprKind::This:
                requireMethodContext(expr, "this");
                emitGet("this", expr);
                break;

            case ExprKind::Super: {
                const auto& e = static_cast<const SuperExpr&>(expr);
                if (compileEnumSuper(e)) break;
                requireSuper(e);
                emitGet("this", e);
                emitGet("super", e);
                emit(Op::GetSuper, stringConstant(e.method));
                break;
            }

            case ExprKind::Unary: {
                const auto& e = static_cast<const UnaryExpr&>(expr);
                compileExpr(*e.operand);
                line_ = e.line;
                emit(e.op == TokenType::Minus ? Op::Neg : e.op == TokenType::Tilde ? Op::BitNot :
                     e.op == TokenType::BangBang ? Op::AssertNotNull : Op::Not);
                break;
            }

            case ExprKind::Elvis: {
                const auto& e = static_cast<const ElvisExpr&>(expr);
                compileExpr(*e.left);
                int toEnd = emitJump(Op::JumpIfNotNull);
                emit(Op::Pop);
                compileExpr(*e.right);
                patchJump(toEnd);
                break;
            }

            case ExprKind::When:
                compileWhen(static_cast<const WhenExpr&>(expr));
                break;

            case ExprKind::Object: {
                // The class is made here, as a variable nothing else can name, and then its one object.
                const ClassStmt& cls = *static_cast<const ObjectExpr&>(expr).cls;
                const int saved = enterValueBlock();
                beginScope();
                const std::string bound = "(object " + std::to_string(anonymousClasses_++) + ")";
                compileClass(cls, bound);
                line_ = cls.line;
                emitGet(bound, cls);
                emit(Op::Call);
                emitByte(0);
                endScopeKeepingValue();
                leaveValueBlock(saved);
                break;
            }

            case ExprKind::Statement: {
                const int saved = enterValueBlock();
                compileStatementValue(*static_cast<const StatementExpr&>(expr).statement);
                leaveValueBlock(saved);
                break;
            }

            case ExprKind::Jump:
                // Control leaves, so nothing here produces the value the code around expects.
                jumpInExpression_ = true;
                compileStmt(*static_cast<const JumpExpr&>(expr).jump);
                break;

            case ExprKind::Binary: {
                const auto& e = static_cast<const BinaryExpr&>(expr);
                if (e.op == TokenType::EqEq || e.op == TokenType::BangEq) {
                    // Against something declared as an enum, the other side may name an entry.
                    compileExprFor(*e.left, declaredTypeOf(*e.right));
                    compileExprFor(*e.right, declaredTypeOf(*e.left));
                } else {
                    compileExpr(*e.left);
                    compileExpr(*e.right);
                }
                line_ = e.line;
                emit(binaryOp(e.op));
                if (e.op == TokenType::BangEq || e.op == TokenType::BangEqEq) emit(Op::Not);
                break;
            }

            case ExprKind::Logical: {
                const auto& e = static_cast<const LogicalExpr&>(expr);
                compileExpr(*e.left);
                int toEnd = emitJump(e.isAnd ? Op::And : Op::Or);
                compileExpr(*e.right);
                patchJump(toEnd);
                break;
            }

            case ExprKind::Ternary: {
                const auto& e = static_cast<const TernaryExpr&>(expr);
                compileExpr(*e.condition);
                int toElse = emitJump(Op::JumpIfFalse);
                compileExpr(*e.then);
                int toEnd = emitJump(Op::Jump);
                patchJump(toElse);
                compileExpr(*e.otherwise);
                patchJump(toEnd);
                break;
            }

            case ExprKind::Assign:
                compileAssign(static_cast<const AssignExpr&>(expr));
                break;

            case ExprKind::IncDec: {
                const char* op = static_cast<const IncDecExpr&>(expr).increment ? "++" : "--";
                throw error(expr, std::string("'") + op + "' changes its target but has no value, so it cannot be used " +
                                      "inside an expression; write it as a statement of its own");
            }

            case ExprKind::Call: {
                const auto& e = static_cast<const CallExpr&>(expr);
                int argc = static_cast<int>(e.args.size());
                noteWorkerScript(e);
                // A lambda after the call belongs to the last parameter, which only the
                // function being called knows the position of.
                if (compileScopeCall(e)) {
                    // let, also or takeIf with a lambda: done in place
                } else if (e.namedCount > 0 || e.hasSpread ||
                           (e.trailingLambda &&
                            (e.callee->kind != ExprKind::Member ||
                             importedMember(*static_cast<const MemberExpr&>(*e.callee).object,
                                            static_cast<const MemberExpr&>(*e.callee).name, *e.callee) != nullptr))) {
                    compileNamedCall(e);
                } else if (e.callee->kind == ExprKind::Member &&
                           importedMember(*static_cast<const MemberExpr&>(*e.callee).object,
                                          static_cast<const MemberExpr&>(*e.callee).name, *e.callee) == nullptr) {
                    const auto& member = static_cast<const MemberExpr&>(*e.callee);
                    compileExpr(*member.object);
                    // `object?.method(...)`: with no object the arguments are not worked out either.
                    int toEnd = member.safe ? emitJump(Op::JumpIfNull) : -1;
                    compileArgs(e.args);
                    line_ = member.line;
                    emit(e.trailingLambda ? Op::InvokeTrailing : Op::Invoke, stringConstant(member.name));
                    emitByte(argc);
                    if (toEnd >= 0) patchJump(toEnd);
                } else if (e.callee->kind == ExprKind::Name && compileBareCall(e)) {
                    // done: the name was a method, or may be one
                } else if (e.callee->kind == ExprKind::Super) {
                    const auto& sup = static_cast<const SuperExpr&>(*e.callee);
                    requireSuper(sup);
                    emitGet("this", sup);
                    compileArgs(e.args);
                    emitGet("super", sup);
                    line_ = sup.line;
                    emit(e.trailingLambda ? Op::SuperInvokeTrailing : Op::SuperInvoke, stringConstant(sup.method));
                    emitByte(argc);
                } else {
                    compileExpr(*e.callee);
                    compileArgs(e.args);
                    line_ = e.line;
                    emit(Op::Call);
                    emitByte(argc);
                }
                break;
            }

            case ExprKind::Member: {
                const auto& e = static_cast<const MemberExpr&>(expr);
                if (const GlobalName* imported = importedMember(*e.object, e.name, e)) {
                    line_ = e.line;
                    emit(Op::GetGlobal, imported->slot);
                    break;
                }
                compileExpr(*e.object);
                line_ = e.line;
                int toEnd = e.safe ? emitJump(Op::JumpIfNull) : -1;
                emit(Op::GetProp, stringConstant(e.name));
                if (toEnd >= 0) patchJump(toEnd);
                break;
            }

            case ExprKind::Index: {
                const auto& e = static_cast<const IndexExpr&>(expr);
                compileExpr(*e.object);
                compileExpr(*e.index);
                line_ = e.line;
                emit(Op::GetIndex);
                break;
            }

            case ExprKind::List: {
                const auto& e = static_cast<const ListExpr&>(expr);
                for (const ExprPtr& item : e.items) compileExpr(*item);
                line_ = e.line;
                emit(Op::BuildList, static_cast<int>(e.items.size()));
                break;
            }

            case ExprKind::Map: {
                const auto& e = static_cast<const MapExpr&>(expr);
                for (const auto& entry : e.entries) {
                    compileExpr(*entry.first);
                    compileExpr(*entry.second);
                }
                line_ = e.line;
                emit(Op::BuildMap, static_cast<int>(e.entries.size()));
                break;
            }

            case ExprKind::Range: {
                const auto& e = static_cast<const RangeExpr&>(expr);
                compileExpr(*e.start);
                compileExpr(*e.end);
                if (e.step) compileExpr(*e.step);
                line_ = e.line;
                emit(Op::Range);
                emitByte((e.inclusive ? 1 : 0) | (e.descending ? 2 : 0) | (e.step ? 4 : 0));
                break;
            }

            case ExprKind::Is: {
                const auto& e = static_cast<const IsExpr&>(expr);
                compileExpr(*e.value);
                line_ = e.line;
                TypeSpec type = resolveType(e.type);
                if (type.typed()) {
                    emitTypeOperand(Op::IsType, type, e.type);
                } else {
                    emit(Op::Pop);  // `any` takes every value
                    emit(Op::True);
                }
                if (e.negated) emit(Op::Not);
                break;
            }

            case ExprKind::Cast: {
                const auto& e = static_cast<const CastExpr&>(expr);
                compileExpr(*e.value);
                line_ = e.line;
                TypeSpec type = resolveType(e.type);
                if (type.typed()) emitTypeOperand(e.safe ? Op::CastOrNull : Op::Cast, type, e.type);
                break;
            }

            case ExprKind::In: {
                // container.contains(value), with the value worked out first as it is written.
                const auto& e = static_cast<const InExpr&>(expr);
                compileExpr(*e.value);
                compileExpr(*e.container);
                line_ = e.line;
                emit(Op::Swap);
                emit(Op::Invoke, stringConstant("contains"));
                emitByte(1);
                if (e.negated) emit(Op::Not);
                break;
            }

            case ExprKind::Function: {
                const FunctionDecl& fn = *static_cast<const FunctionExpr&>(expr).fn;
                compileFunction(fn, FunctionKind::Function, fn.name);
                break;
            }
        }
    }

    // A call with `name = value` arguments. The callee is evaluated to a value first (a bound
    // method for `obj.method(...)`), then the positional arguments, then a name and a value for
    // each named one; the VM arranges them to match the callee's parameters.
    // ---- Blocks that are expressions ----

    // A block inside an expression (the branch of a `when`) may declare variables while the
    // values that expression has built so far are on the stack. Its variables go above those,
    // which takes knowing how many there are. Returns what to hand to leaveValueBlock().
    int enterValueBlock() {
        int saved = fs_->slotTop;
        int depth = stackDepthAtEnd(*fs_->proto, session_.program);
        if (depth >= 0) fs_->slotTop = depth;
        return saved;
    }

    void leaveValueBlock(int saved) { fs_->slotTop = saved; }

    // Ends a scope whose value is on top of the stack, above the scope's variables.
    void endScopeKeepingValue() {
        fs_->scopeDepth--;
        std::vector<Local>& locals = fs_->locals;
        int count = 0;
        while (!locals.empty() && locals.back().depth > fs_->scopeDepth) {
            locals.pop_back();
            fs_->slotTop--;
            count++;
        }
        if (count > 0) emit(Op::PopUnder, count);
    }

    // Compiles a statement and leaves a value: its own if it is an expression, else null.
    void compileStatementValue(const Stmt& stmt) {
        if (stmt.kind == StmtKind::Block) {
            const auto& statements = static_cast<const BlockStmt&>(stmt).statements;
            if (statements.empty()) {
                emit(Op::Null);
                return;
            }
            beginScope();
            for (size_t i = 0; i + 1 < statements.size(); i++) compileStmt(*statements[i]);
            compileStatementValue(*statements.back());
            endScopeKeepingValue();
        } else if (stmt.kind == StmtKind::Expr && static_cast<const ExprStmt&>(stmt).expr->kind != ExprKind::IncDec) {
            line_ = stmt.line;
            compileExpr(*static_cast<const ExprStmt&>(stmt).expr);
        } else if (stmt.kind == StmtKind::If) {
            // The value of the branch taken, or null when there is none to take.
            const auto& s = static_cast<const IfStmt&>(stmt);
            line_ = s.line;
            compileExpr(*s.condition);
            int toElse = emitJump(Op::JumpIfFalse);
            compileStatementValue(*s.then);
            int toEnd = emitJump(Op::Jump);
            patchJump(toElse);
            if (s.otherwise) compileStatementValue(*s.otherwise);
            else emit(Op::Null);
            patchJump(toEnd);
        } else if (stmt.kind == StmtKind::Try) {
            // The value of the try block, or of the catch that ran. It is kept in a variable
            // of its own, since a finally block may run before it is handed on.
            const auto& s = static_cast<const TryStmt&>(stmt);
            beginScope();
            emit(Op::Null);
            const int value = addLocal("(try value)", false, s, true);
            const TryStmt* const outerTry = valueTry_;
            const int outerLocal = valueTryLocal_;
            valueTry_ = &s;
            valueTryLocal_ = value;
            compileTry(s);
            valueTry_ = outerTry;
            valueTryLocal_ = outerLocal;
            emit(Op::GetLocal, slotOf(value));
            endScopeKeepingValue();
        } else {
            compileStmt(stmt);
            emit(Op::Null);
        }
    }

    // The block of a try, or of one of its catches. For a try that is wanted for its value the
    // block's value goes into the variable set aside for it.
    void compileTryBlock(const TryStmt& s, const BlockStmt& block) {
        if (&s != valueTry_) {
            compileStmt(block);
            return;
        }
        const int target = valueTryLocal_;
        valueTry_ = nullptr;  // a try inside the block is a statement like any other
        compileStatementValue(block);
        emit(Op::SetLocal, slotOf(target));
        emit(Op::Pop);
        valueTry_ = &s;
        valueTryLocal_ = target;
    }

    void compileWhen(const WhenExpr& e) {
        const int saved = enterValueBlock();
        int subject = -1;
        const TypeSpec subjectType = e.subject ? declaredTypeOf(*e.subject) : TypeSpec();
        if (e.subject) {
            // The subject is worked out once and kept in a variable while the branches are tried.
            beginScope();
            compileExpr(*e.subject);
            subject = addLocal(e.subjectName.empty() ? "(when subject)" : e.subjectName, true, e, true);
        }

        std::vector<int> toEnd;
        bool hasElse = false;
        for (const WhenBranch& branch : e.branches) {
            int toNext = -1;
            if (branch.isElse) {
                hasElse = true;
            } else {
                // Any one of the branch's conditions selects it.
                std::vector<int> toBody;
                for (size_t i = 0; i < branch.conditions.size(); i++) {
                    compileWhenCondition(branch.conditions[i], subject, branch, subjectType);
                    if (i + 1 < branch.conditions.size()) toBody.push_back(emitJump(Op::Or));
                }
                for (int jump : toBody) patchJump(jump);
                toNext = emitJump(Op::JumpIfFalse);
            }
            compileStatementValue(*branch.body);
            if (!branch.isElse) {
                toEnd.push_back(emitJump(Op::Jump));
                patchJump(toNext);
            }
        }
        if (!hasElse) emit(Op::Null);  // no branch was taken
        for (int jump : toEnd) patchJump(jump);

        if (e.subject) endScopeKeepingValue();
        leaveValueBlock(saved);
    }

    void compileWhenCondition(const WhenCondition& condition, int subject, const WhenBranch& branch,
                              const TypeSpec& subjectType) {
        line_ = branch.line;
        if (subject < 0) {
            compileExpr(*condition.value);
            return;
        }
        emit(Op::GetLocal, slotOf(subject));
        switch (condition.kind) {
            case WhenCondition::Kind::Value:
                compileExprFor(*condition.value, subjectType);
                line_ = branch.line;
                emit(Op::Eq);
                break;
            case WhenCondition::Kind::Is: {
                TypeSpec type = resolveType(condition.type);
                if (type.typed()) {
                    emitTypeOperand(Op::IsType, type, condition.type);
                } else {
                    emit(Op::Pop);
                    emit(Op::True);
                }
                break;
            }
            case WhenCondition::Kind::In:
                compileExpr(*condition.value);
                line_ = branch.line;
                emit(Op::Swap);
                emit(Op::Invoke, stringConstant("contains"));
                emitByte(1);
                break;
        }
        if (condition.negated) emit(Op::Not);
    }

    // In an enum, `super.entries`, `super.name` and `super.ordinal` are what every enum is
    // given, whatever the enum has put in their place with `override`.
    bool compileEnumSuper(const SuperExpr& e) {
        if (class_ == nullptr || !class_->isEnum) return false;
        const bool entries = e.method == "entries";
        if (!entries && e.method != "name" && e.method != "ordinal") return false;
        line_ = e.line;
        if (entries) {
            emitGet(class_->storage, e);
            emit(Op::GetProp, stringConstant("(entries)"));
            return true;
        }
        if (classSide() != ClassSide::Instance)
            throw error(e, "super." + e.method + " is that of one entry, so it belongs in a method or property of the enum's entries");
        emitGet("this", e);
        emit(Op::GetProp, stringConstant("(" + e.method + ")"));
        return true;
    }

    // `name(args)` where the name is no variable of the function. In a class it may be a method
    // of the object or a static of the class, and in a lambda a method of the object the lambda
    // runs on; those are called directly, without making a function value of the method first.
    // Returns false when the name is to be called as an ordinary value.
    bool compileBareCall(const CallExpr& e) {
        const auto& callee = static_cast<const NameExpr&>(*e.callee);
        const std::string& name = callee.name;
        VarInfo info;
        if (resolveLocal(fs_, name, callee, info) != -1 || resolveUpvalue(fs_, name, callee, info) != -1) return false;

        const int argc = static_cast<int>(e.args.size());
        const ClassSide side = classSide();
        const bool own = side != ClassSide::None && (class_->statics.count(name) != 0 ||
                                                     (side == ClassSide::Instance && class_->members.count(name) != 0));
        if (!own && outerStaticOwner(name) != nullptr) return false;  // called as a value
        if (side == ClassSide::Instance && class_->members.count(name) != 0 && kindHasThis(fs_->kind)) {
            emit(Op::GetLocal, 0);
            compileArgs(e.args);
            line_ = e.line;
            emit(Op::Invoke, stringConstant(name));
            emitByte(argc);
            return true;
        }
        const bool member = side == ClassSide::Instance && class_->members.count(name) != 0;
        if (!member && side != ClassSide::None && class_->statics.count(name) != 0) {
            // Where something may come before the static, it is called as a value instead.
            if (namesFromReceiver() || (side == ClassSide::Instance && class_->hasSuper)) return false;
            emitGet(class_->storage, callee);
            compileArgs(e.args);
            line_ = e.line;
            emit(Op::Invoke, stringConstant(name));
            emitByte(argc);
            return true;
        }
        if (!member && !(side == ClassSide::None ? namesFromReceiver() : mayBeMember(side))) return false;

        emit(Op::Null);  // where the object, or the function, is put when the call is made
        compileArgs(e.args);
        line_ = e.line;
        emitImplicit(Op::InvokeImplicit, name);
        emitByte(argc);
        return true;
    }

    void compileNamedCall(const CallExpr& e) {
        int toEnd = -1;
        if (e.callee->kind == ExprKind::Member && static_cast<const MemberExpr&>(*e.callee).safe) {
            const auto& member = static_cast<const MemberExpr&>(*e.callee);
            compileExpr(*member.object);
            line_ = member.line;
            toEnd = emitJump(Op::JumpIfNull);
            emit(Op::GetProp, stringConstant(member.name));
        } else {
            compileExpr(*e.callee);
        }

        // A lambda written after the ')' goes to the last parameter, whatever was named before
        // it. It travels with the named arguments under the empty name.
        size_t trailing = e.trailingLambda ? 1 : 0;
        size_t positional = e.args.size() - e.namedCount - trailing;
        if (e.hasSpread) {
            // With `*list` among them, the positional arguments are gathered into one list:
            // each run of plain arguments, and each list written with a star, is added in turn.
            emit(Op::BuildList, 0);
            for (size_t i = 0; i < positional;) {
                int run = 0;
                for (; i < positional && !e.spread[i]; i++, run++) compileArg(*e.args[i]);
                if (run > 0) {
                    emit(Op::BuildList, run);
                    emit(Op::Spread);
                }
                if (i < positional) {
                    compileExpr(*e.args[i]);
                    line_ = e.args[i]->line;
                    emit(Op::Spread);
                    i++;
                }
            }
        } else {
            for (size_t i = 0; i < positional; i++) compileArg(*e.args[i]);
        }
        for (size_t i = positional; i < e.args.size(); i++) {
            line_ = e.args[i]->line;
            emit(Op::Const, stringConstant(e.names[i]));
            compileArg(*e.args[i]);
        }

        line_ = e.line;
        emit(e.hasSpread ? Op::CallSpread : Op::CallNamed);
        if (!e.hasSpread) emitByte(static_cast<int>(positional));
        emitByte(static_cast<int>(e.namedCount + trailing));
        if (toEnd >= 0) patchJump(toEnd);
    }

    // In a class whose fields are all known (it declares some and has no superclass), assigning
    // through `this` to a name that is neither a field nor a property with a setter is a mistake
    // that can be reported straight away.
    void requireDeclaredField(const MemberExpr& target) {
        if (target.object->kind != ExprKind::This || class_ == nullptr || class_->closed == nullptr) return;
        const ClassStmt& owner = *class_->closed;
        bool declared = false;
        for (const auto& field : owner.fields) declared = declared || field->name == target.name;
        for (const auto& method : owner.methods)
            declared = declared || (method->accessor == FunctionDecl::Accessor::Set && method->name == target.name);
        if (!declared)
            throw error(target, "class '" + owner.name + "' has no field '" + target.name +
                                    "'; fields must be declared in the class with val or var");
    }

    // x++ and x-- as statements: the same as x += 1 and x -= 1 with the result discarded.
    void compileIncDec(const IncDecExpr& e) {
        Op change = e.increment ? Op::Add : Op::Sub;

        switch (e.target->kind) {
            case ExprKind::Name: {
                const auto& target = static_cast<const NameExpr&>(*e.target);
                emitGet(target.name, target);
                emit(Op::Const, intConstant(1));
                line_ = e.line;
                emit(change);
                emitSet(target.name, target);
                break;
            }

            case ExprKind::Member: {
                const auto& target = static_cast<const MemberExpr&>(*e.target);
                requireDeclaredField(target);
                compileExpr(*target.object);
                int toEnd = target.safe ? emitJump(Op::JumpIfNull) : -1;
                emit(Op::Dup);
                line_ = e.line;
                emit(Op::GetProp, stringConstant(target.name));
                emit(Op::Const, intConstant(1));
                emit(change);
                emit(Op::SetProp, stringConstant(target.name));
                if (toEnd >= 0) patchJump(toEnd);
                break;
            }

            case ExprKind::Index: {
                const auto& target = static_cast<const IndexExpr&>(*e.target);
                compileExpr(*target.object);
                compileExpr(*target.index);
                emit(Op::Dup2);
                line_ = e.line;
                emit(Op::GetIndex);
                emit(Op::Const, intConstant(1));
                emit(change);
                emit(Op::SetIndex);
                break;
            }

            default:
                throw error(e, "this cannot be incremented or decremented");
        }
        emit(Op::Pop);
    }

    void compileAssign(const AssignExpr& e) {
        bool compound = e.op != TokenType::Eq;

        switch (e.target->kind) {
            case ExprKind::Name: {
                const auto& target = static_cast<const NameExpr&>(*e.target);
                if (compound) emitGet(target.name, target);
                if (compound) compileExpr(*e.value);
                else compileExprFor(*e.value, declaredTypeOf(target.name));
                line_ = e.line;
                if (compound) emit(binaryOp(e.op));
                emitSet(target.name, target);
                break;
            }

            case ExprKind::Member: {
                const auto& target = static_cast<const MemberExpr&>(*e.target);
                requireDeclaredField(target);
                compileExpr(*target.object);
                // `object?.field = value` does nothing, and works nothing out, with no object.
                int toEnd = target.safe ? emitJump(Op::JumpIfNull) : -1;
                if (compound) {
                    emit(Op::Dup);
                    emit(Op::GetProp, stringConstant(target.name));
                }
                if (compound) compileExpr(*e.value);
                else compileExprFor(*e.value, declaredTypeOf(*e.target));
                line_ = e.line;
                if (compound) emit(binaryOp(e.op));
                emit(Op::SetProp, stringConstant(target.name));
                if (toEnd >= 0) patchJump(toEnd);
                break;
            }

            case ExprKind::Index: {
                const auto& target = static_cast<const IndexExpr&>(*e.target);
                compileExpr(*target.object);
                compileExpr(*target.index);
                if (compound) {
                    emit(Op::Dup2);
                    emit(Op::GetIndex);
                }
                compileExpr(*e.value);
                line_ = e.line;
                if (compound) emit(binaryOp(e.op));
                emit(Op::SetIndex);
                break;
            }

            default:
                throw error(e, "invalid assignment target");
        }
    }
};

// ---- CompilerSession ----

CompilerSession::CompilerSession(VM& vm) : vm_(vm) {
    auto builtins = std::make_unique<Namespace>();
    builtins->directory = "<builtin>";
    builtins->library = "<builtin>";
    builtins_ = builtins.get();
    namespaces_["<builtin>"] = std::move(builtins);
    // The standard library is part of the prelude: every program starts with it loaded.
    std::string prelude = kPrelude;
    prelude.append(reinterpret_cast<const char*>(kStdSource), kStdSourceSize);
    program.modules.push_back(compileUnit(prelude, "<std>", "<builtin>", "<prelude>", false));
}

int CompilerSession::compileFile(const std::string& path) {
    try {
        program.entry = loadModule(path, path, path, nullptr);
        checkConstAssignments();
    } catch (...) {
        globalAssignments_.clear();
        throw;
    }
    globalAssignments_.clear();
    return program.entry;
}

int CompilerSession::compileSource(const std::string& source, const std::string& name,
                                   const std::string& baseDirectory, bool repl) {
    try {
        FunctionProto* proto = compileUnit(source, name, baseDirectory, name, repl);
        checkConstAssignments();
        program.modules.push_back(proto);
    } catch (...) {
        globalAssignments_.clear();
        throw;
    }
    globalAssignments_.clear();
    program.entry = static_cast<int>(program.modules.size()) - 1;
    return program.entry;
}

// The namespace of a folder, made the first time it is asked for. A folder under one of the
// library directories belongs to the library named by the first folder below it (lib/jade and
// lib/jade/chart are both "jade"); every other folder is part of the program.
Namespace* CompilerSession::namespaceOf(const std::string& directory) {
    std::string key = directory;
    std::error_code ec;
    if (directory != "<builtin>") {
        fs::path folder(directory.empty() ? "." : directory);
        fs::path canonical = fs::weakly_canonical(folder, ec);
        if (ec) canonical = fs::absolute(folder, ec);
        key = canonical.string();
    }
    auto found = namespaces_.find(key);
    if (found != namespaces_.end()) return found->second.get();

    auto ns = std::make_unique<Namespace>();
    ns->directory = key;
    ns->library = "<program>";
    for (const std::string& library : libraryPaths) {
        fs::path root = fs::weakly_canonical(fs::path(library), ec);
        fs::path relative = fs::path(key).lexically_relative(root);
        if (relative.empty() || *relative.begin() == "..") continue;
        ns->library = relative == "." ? root.string() : (root / *relative.begin()).string();
        break;
    }
    Namespace* result = ns.get();
    namespaces_[key] = std::move(ns);
    return result;
}

int CompilerSession::newSlot(const std::string& name) {
    int slot = static_cast<int>(program.globalNames.size());
    if (slot > 0xFFFF) throw CompileError("<program>", 1, 1, "too many global variables");
    program.globalNames.push_back(name);
    return slot;
}

// Notes what a file declares at its top level, before any of it is compiled: a use of a name
// can then tell a declaration further down, or in another file of the folder, from a name of
// an import. Declaring a name twice in one folder is an error (the REPL may declare again).
void CompilerSession::predeclare(Namespace& ns, const std::string& fileKey, const std::string& file,
                                 const std::vector<StmtPtr>& statements, bool repl) {
    auto declare = [&](const std::string& name, bool isVal, const Stmt& at) {
        if (name == "_") return;
        const bool isPrivate = at.visibility == GlobalName::Private;
        auto& mine = privates_[fileKey];
        if (!repl) {
            // The same name both private and not, in one file.
            auto other = isPrivate ? ns.names.find(name) : mine.find(name);
            auto end = isPrivate ? ns.names.end() : mine.end();
            if (other != end && other->second.declared && (isPrivate ? other->second.file == file : true))
                throw CompileError(file, at.line, at.col, "'" + name + "' is already declared in this file");
        }
        GlobalName& entry = isPrivate ? mine[name] : ns.names[name];
        if (entry.declared) {
            if (entry.isVal) throw CompileError(file, at.line, at.col, "cannot redeclare val '" + name + "'");
            if (!repl)
                throw CompileError(file, at.line, at.col,
                                   "'" + name + "' is already declared" + (entry.file != file ? " in " + entry.file : "") +
                                       "; a name can be declared only once in a folder");
        }
        if (entry.slot < 0) entry.slot = newSlot(name);
        entry.declared = true;
        entry.isVal = isVal;
        entry.visibility = static_cast<GlobalName::Visibility>(at.visibility);
        entry.file = file;
    };

    for (const StmtPtr& stmt : statements) {
        if (stmt->kind == StmtKind::Var) {
            const auto& var = static_cast<const VarStmt&>(*stmt);
            if (var.pattern.empty()) declare(var.name, var.isVal, var);
            for (const std::string& name : var.pattern) declare(name, var.isVal, var);
        } else if (stmt->kind == StmtKind::Function) {
            // An extension function is added to a type; it declares no name of its own.
            if (!static_cast<const FunctionStmt&>(*stmt).receiver.empty()) continue;
            declare(static_cast<const FunctionStmt&>(*stmt).fn->name, false, *stmt);
        } else if (stmt->kind == StmtKind::Class) {
            const auto& cls = static_cast<const ClassStmt&>(*stmt);
            declare(cls.name, cls.isObject, cls);
        }
    }
}

FunctionProto* CompilerSession::compileUnit(const std::string& source, const std::string& name,
                                            const std::string& directory, const std::string& fileKey, bool repl,
                                            Namespace* into) {
    Lexer lexer(source, name);
    std::vector<Token> tokens = lexer.scan();
    Parser parser(tokens, name);
    std::vector<StmtPtr> statements = parser.parseProgram();
    Namespace* ns = into != nullptr ? into : namespaceOf(directory);
    predeclare(*ns, fileKey, name, statements, repl);
    ModuleCompiler compiler(*this, name, directory == "<builtin>" ? "" : directory, ns, fileKey);
    return compiler.compile(statements, repl);
}

// The script named in `Worker("physics.astra")`, looked for next to the file that names it.
// It becomes a module of the program that only a worker runs, so that a program built into
// an executable carries its workers with it. Its names are its own: the rest of the program
// neither sees them nor clashes with them, as befits code that shares nothing with it.
int CompilerSession::loadWorker(const std::string& path, const std::string& fromDirectory, const std::string& file,
                                const Node& at) {
    std::error_code ec;
    fs::path candidate = fs::path(fromDirectory.empty() ? "." : fromDirectory) / fs::path(path);
    if (!fs::is_regular_file(candidate, ec)) candidate += ".astra";
    if (!fs::is_regular_file(candidate, ec))
        throw CompileError(file, at.line, at.col, "cannot find the worker's script '" + path + "' next to this file");
    const fs::path canonical = fs::weakly_canonical(candidate, ec);
    const std::string key = "<worker>" + canonical.string();

    auto found = modules_.find(key);
    if (found != modules_.end()) return found->second;
    std::string source;
    if (!readFile(canonical.string(), source)) throw CompileError(file, at.line, at.col, "cannot read '" + path + "'");

    int index = static_cast<int>(program.modules.size());
    if (index > 0xFFFF) throw CompileError(file, at.line, at.col, "too many modules");
    program.modules.push_back(nullptr);
    modules_[key] = index;
    std::unique_ptr<Namespace>& ns = namespaces_[key];
    if (ns == nullptr) {
        ns = std::make_unique<Namespace>();
        ns->directory = canonical.parent_path().string();
        ns->library = "<program>";
    }
    try {
        program.modules[static_cast<size_t>(index)] =
            compileUnit(source, canonical.filename().string(), canonical.parent_path().string(), key, false, ns.get());
    } catch (...) {
        modules_.erase(key);
        throw;
    }
    return index;
}

// `file` and `at` say where the import was written, for error messages; `at` is null for
// the entry file.
int CompilerSession::loadModule(const std::string& path, const std::string& displayPath, const std::string& file,
                                const Node* at) {
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(fs::path(path), ec);
    if (ec) canonical = fs::absolute(fs::path(path), ec);
    std::string key = canonical.string();

    auto found = modules_.find(key);
    if (found != modules_.end()) return found->second;

    std::string source;
    if (!readFile(key, source))
        throw CompileError(file, at ? at->line : 1, at ? at->col : 1, "cannot read '" + displayPath + "'");

    // Registered before compiling so that circular imports resolve to this slot.
    int index = static_cast<int>(program.modules.size());
    if (index > 0xFFFF) throw CompileError(file, at ? at->line : 1, at ? at->col : 1, "too many modules");
    program.modules.push_back(nullptr);
    modules_[key] = index;
    try {
        program.modules[static_cast<size_t>(index)] =
            compileUnit(source, canonical.filename().string(), canonical.parent_path().string(), key, false);
    } catch (...) {
        modules_.erase(key);
        throw;
    }
    return index;
}

// Loads every .astra file directly in a folder, as one unit: all of them are read and their
// declarations noted before any is compiled, so they see each other whatever their order.
// Files of the folder that were loaded earlier, one at a time, are not loaded again.
void CompilerSession::loadFolder(const std::string& folder, const std::string& displayPath, const std::string& file,
                                 const Node& at, std::vector<int>& modules) {
    struct Unit {
        std::string key, name, source;
        std::vector<StmtPtr> statements;
        int index = 0;
    };
    std::error_code ec;
    std::vector<fs::path> paths;
    for (const fs::directory_entry& entry : fs::directory_iterator(fs::path(folder), ec))
        if (entry.is_regular_file(ec) && entry.path().extension() == ".astra") paths.push_back(entry.path());
    std::sort(paths.begin(), paths.end());
    if (paths.empty())
        throw CompileError(file, at.line, at.col, "the folder '" + displayPath + "' has no .astra files to import");

    std::vector<Unit> units;
    for (const fs::path& path : paths) {
        fs::path canonical = fs::weakly_canonical(path, ec);
        std::string key = canonical.string();
        auto found = modules_.find(key);
        if (found != modules_.end()) {
            modules.push_back(found->second);
            continue;
        }
        Unit unit;
        unit.key = key;
        unit.name = canonical.filename().string();
        if (!readFile(key, unit.source))
            throw CompileError(file, at.line, at.col, "cannot read '" + displayPath + "/" + unit.name + "'");
        unit.index = static_cast<int>(program.modules.size());
        if (unit.index > 0xFFFF) throw CompileError(file, at.line, at.col, "too many modules");
        units.push_back(std::move(unit));
        program.modules.push_back(nullptr);
        modules_[units.back().key] = units.back().index;
        modules.push_back(units.back().index);
    }

    if (units.empty()) return;

    // One more module holds what is set up ahead for all of them. It runs before any of them.
    const int header = static_cast<int>(program.modules.size());
    if (header > 0xFFFF) throw CompileError(file, at.line, at.col, "too many modules");
    program.modules.push_back(nullptr);
    modules.insert(modules.end() - static_cast<std::ptrdiff_t>(units.size()), header);
    auto shared = std::make_shared<std::vector<int>>(modules);
    for (const Unit& unit : units) units_[unit.key] = shared;

    Namespace* ns = namespaceOf(folder);
    try {
        for (Unit& unit : units) {
            Lexer lexer(unit.source, unit.name);
            std::vector<Token> tokens = lexer.scan();
            Parser parser(tokens, unit.name);
            unit.statements = parser.parseProgram();
            predeclare(*ns, unit.key, unit.name, unit.statements, false);
        }

        std::vector<std::unique_ptr<ModuleCompiler>> compilers;
        std::vector<std::pair<size_t, const std::vector<StmtPtr>*>> files;
        std::vector<std::string> names;
        for (size_t i = 0; i < units.size(); i++) {
            compilers.push_back(std::make_unique<ModuleCompiler>(*this, units[i].name, folder, ns, units[i].key));
            files.emplace_back(i, &units[i].statements);
            names.push_back(units[i].name);
        }

        // Imports first, then every function, then the classes, each after what it extends.
        FunctionState ahead;
        compilers[0]->beginScript(ahead, displayPath);
        for (size_t i = 0; i < units.size(); i++) {
            compilers[i]->continueScript(ahead);
            compilers[i]->noteEnums(units[i].statements);
            compilers[i]->hoistImports(units[i].statements);
        }
        for (size_t i = 0; i < units.size(); i++) {
            compilers[i]->continueScript(ahead);
            compilers[i]->hoistFunctions(units[i].statements);
        }
        const std::vector<ModuleCompiler::UnitClass> classes = ModuleCompiler::orderClasses(files, names);
        for (const ModuleCompiler::UnitClass& cls : classes) {
            compilers[cls.file]->continueScript(ahead);
            compilers[cls.file]->hoistClass(*cls.stmt);
        }
        for (size_t i = 0; i < units.size(); i++) {
            compilers[i]->continueScript(ahead);
            compilers[i]->hoistExtensions(units[i].statements);
        }
        compilers[0]->endScript(ahead);
        program.modules[static_cast<size_t>(header)] = ahead.proto;

        for (size_t i = 0; i < units.size(); i++) {
            FunctionState rest;
            compilers[i]->beginScript(rest, units[i].name);
            compilers[i]->compileRemainder(units[i].statements, false);
            compilers[i]->endScript(rest);
            program.modules[static_cast<size_t>(units[i].index)] = rest.proto;
        }
    } catch (...) {
        for (const Unit& unit : units) {
            modules_.erase(unit.key);
            units_.erase(unit.key);
        }
        throw;
    }
}

Namespace* CompilerSession::importModule(const std::string& path, const std::string& fromDirectory,
                                         const std::string& file, const Node& at, std::vector<int>& modules) {
    // Looked for next to the importing file first, then in the library directories. In each
    // place the name is tried as a file, as written and with ".astra" added, then as a folder.
    std::vector<fs::path> places;
    places.push_back(fs::path(fromDirectory));
    for (const std::string& library : libraryPaths) places.push_back(fs::path(library));

    // A file never imports itself: a script called Shapes.astra that says `import Shapes` means
    // the library, so the importing file is passed over.
    std::error_code ec;
    fs::path self = fs::weakly_canonical(fs::path(fromDirectory) / fs::path(file), ec);
    auto usable = [&](const fs::path& candidate) {
        if (!fs::is_regular_file(candidate, ec)) return false;
        return fs::weakly_canonical(candidate, ec) != self;
    };
    auto loadFile = [&](const fs::path& candidate) {
        fs::path canonical = fs::weakly_canonical(candidate, ec);
        auto unit = units_.find(canonical.string());
        if (unit != units_.end()) {
            // A file that was loaded as part of its folder runs as part of it.
            modules.insert(modules.end(), unit->second->begin(), unit->second->end());
        } else {
            modules.push_back(loadModule(candidate.string(), path, file, &at));
        }
        return namespaceOf(canonical.parent_path().string());
    };

    for (const fs::path& place : places) {
        fs::path candidate = place / fs::path(path);
        if (usable(candidate)) return loadFile(candidate);
        fs::path withExtension = candidate;
        withExtension += ".astra";
        if (usable(withExtension)) return loadFile(withExtension);
        if (fs::is_directory(candidate, ec)) {
            std::string folder = fs::weakly_canonical(candidate, ec).string();
            loadFolder(folder, path, file, at, modules);
            return namespaceOf(folder);
        }
    }
    throw CompileError(file, at.line, at.col, "cannot find module '" + path + "'");
}

// A function may assign to a global that is only declared const further down, so
// assignments are checked once everything has been compiled.
void CompilerSession::checkConstAssignments() {
    for (const auto& [global, name, file, line, col] : globalAssignments_)
        if (global->isVal) throw CompileError(file, line, col, "cannot assign to val '" + name + "'");
}

}  // namespace astra
