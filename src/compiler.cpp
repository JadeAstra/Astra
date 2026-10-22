#include "compiler.h"

#include "checker.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#include "parser.h"
#include "vm.h"

#include "section_compiler.h"  // last: this file's own code goes in its section (D161)

namespace astra {

namespace fs = std::filesystem;

// The standard library past the prelude: src/std/*.astra, packed into the executable by the build.
extern const unsigned char kStdSource[];
extern const size_t kStdSourceSize;
extern const size_t kStdFileEnds[];
extern const size_t kStdFileCount;

namespace {

// Compiled into every program as module 0. Runtime errors are instances of Error.
// What comes before the standard library's files. Its classes of errors are its own (errors.astra).
const char* const kPrelude = "";

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

    // Whose code declares it (D59): 0 the function's own, else the body of an inline function
    // copied in, whose names are its own. Code sees only the locals of its owner.
    int owner = 0;
    // Another name for a slot a local already has, which takes no slot of its own.
    bool alias = false;
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
                case LiteralExpr::Type::Char: return kTypeChar;
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
            if (unary.op == TokenType::Caret) return kTypeIndex;
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
    int owner = 0;                   // whose code the loop is (D59): break and continue reach their own
};

// A `finally` block that the code being compiled is inside of. Leaving it early, by return,
// break or continue, has to run the block on the way out.
struct FinallyState {
    FinallyState* enclosing;
    const BlockStmt* block;
    int tryDepth;      // try blocks open in the function before this one was entered
    LoopState* loop;   // the loop it was entered in, if any
    const void* scope = nullptr;  // the CodeScope it is written in: an inline body's, maybe (D59)
};

struct FunctionState;

// An anonymous object being compiled (M6.5 part 4). Its class is made once, so what its members
// use of the code around it cannot be in their closures: it is captured when each object is
// made, kept in the object, and reached through it (Op::NewObject and the rest).
struct ObjectBuild {
    FunctionState* boundary = nullptr;  // the function the object is made in
    ObjectBuild* outer = nullptr;       // an object being compiled around this one
    const void* classState = nullptr;   // its class as it is compiled (a ClassState)
    size_t localsAtStart = 0;           // the boundary's locals before the object
    int superLocal = -1;                // the boundary's local holding the class's `super`
    // Making the class itself uses a variable of the code around (a class declared there as
    // its supertype, say): it is made every time instead of once.
    bool usesOuter = false;

    struct Capture {
        uint8_t kind;  // as Op::Capture: 0 a local, 1 an upvalue, 2 a capture, 3 the superclass
        int index;
    };
    std::vector<Capture> captures;

    int add(uint8_t kind, int index) {
        for (size_t i = 0; i < captures.size(); i++)
            if (captures[i].kind == kind && (kind == 3 || captures[i].index == index)) return static_cast<int>(i);
        captures.push_back(Capture{kind, index});
        return static_cast<int>(captures.size()) - 1;
    }
};

// What resolveUpvalue and captureReceiver give for capture `k` of the object a member runs on,
// as against an upvalue's index.
constexpr int kCaptureMark = 0x10000;
inline bool isCaptureMark(int resolved) { return resolved >= kCaptureMark; }

struct FunctionState {
    FunctionState* enclosing = nullptr;
    const FunctionDecl* decl = nullptr;  // what is compiled, where it is written; null for a script
    ObjectBuild* object = nullptr;  // a member of an anonymous object's class, which reaches the code around through it
    FunctionKind kind = FunctionKind::Script;
    FunctionProto* proto = nullptr;

    // For joining two instructions in one as they are emitted (M6.5 part 4): where the last one
    // emitted begins, and the last place code may jump to.
    int lastOp = -1;
    int beforeLast = -1;  // where the one before it begins
    int barrier = -1;

    int caches = 0;  // how many places to remember a field the function's code has so far

    // A generic function's type parameters, which its frame is given when it is called (M6.6 part 1).
    std::vector<std::string> typeParams;

    std::vector<Local> locals;
    int slotTop = 0;  // the slot the next local will get
    std::map<std::pair<int, uint64_t>, int> constantIndex;
    int owner = 0;    // whose code is being compiled in it now (Local::owner)

    void add(Local local) {
        local.slot = slotTop++;
        local.owner = owner;
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

// The body of a lambda copied into an inline function's call (D59), compiled as part of the
// function it is written in rather than as a function of its own.
struct InlineBlock {
    InlineBlock* enclosing;
    std::string name, label;       // the inline function it is given to, and a label written before it
    FunctionState* function;
    int valueSlot;                 // the slot of the value the block works on
    FinallyState* finallyAtEntry;
    int tryDepth;
    std::vector<int> exits;        // `return@let value` jumps, to be pointed at the end
    int owner = 0;                 // whose code it is in (D59)
};

struct ClassState {
    ClassState* enclosing;
    bool hasSuper = false;
    int id = 0;  // the number the code written in it carries

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
    bool isValue = false;  // a value type (M6.6 part 2)
    std::vector<std::string> typeParams;  // a generic class's, which each of its objects keeps (M6.6 part 1)

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

bool checkingNativeResults() {
    static const bool checking = [] {
        const char* set = std::getenv("ASTRA_CHECK_NATIVE_RESULTS");
        return set != nullptr && *set != '0' && *set != '\0';
    }();
    return checking;
}

bool copyingReads() {
    static const bool copying = [] {
        const char* set = std::getenv("ASTRA_COPY_READS");
        return set != nullptr && *set != '\0' && *set != '0';
    }();
    return copying;
}

bool readFile(const std::string& path, std::string& out) {
    if (!readWholeFile(fs::path(path), out)) return false;
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
    FunctionProto* compile(const std::vector<StmtPtr>& statements, bool shell) {
        shellInput_ = shell;
        FunctionState state;
        beginScript(state, file_);
        noteEnums(statements);
        hoistImports(statements);
        hoistFunctions(statements);
        defineForms();
        std::vector<std::pair<size_t, const std::vector<StmtPtr>*>> files{{0, &statements}};
        std::vector<std::string> names{file_};
        for (const UnitClass& cls : orderClasses(files, names)) hoistClass(*cls.stmt);
        hoistExtensions(statements);
        compileRemainder(statements, shell);
        endScript(state);
        return state.proto;
    }

    // For a folder, whose files are compiled together: these are called on each file's compiler
    // in turn, the hoisted parts all going into one function that runs first.
    void beginScript(FunctionState& state, const std::string& source) {
        state.kind = FunctionKind::Script;
        state.proto = newProto();
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
        session_.fileImports_[fileKey_] = importsNow();  // what an inline function's body sees (D59)
    }

    void hoistFunctions(const std::vector<StmtPtr>& statements) {
        for (const StmtPtr& stmt : statements) {
            if (stmt->kind != StmtKind::Function || !static_cast<const FunctionStmt&>(*stmt).receiver.empty()) continue;
            const int start = static_cast<int>(code().size());
            compileStmt(*stmt);
            noteDeclaration(start, definedIn(start));
            hoisted_.insert(stmt.get());
        }
    }

    // The code from `start` to here makes the top-level declaration kept in global `slot`
    // (M6.5 part 5): a built program that never reaches the global leaves it out.
    void noteDeclaration(int start, int key, DeclarationSpan::Kind kind = DeclarationSpan::Global) {
        const int end = static_cast<int>(code().size());
        if (key < 0 || key > 0xFFFF || end == start || fs_->enclosing != nullptr || fs_->kind != FunctionKind::Script) return;
        fs_->proto->spans.push_back(
            DeclarationSpan{static_cast<uint32_t>(start), static_cast<uint32_t>(end), static_cast<uint16_t>(key), kind});
    }

    // The name of the extension function the code from `start` to here adds (the constant its
    // Extension instruction names), or -1 if it adds none, or more than one.
    int extensionIn(int start) {
        const std::vector<uint8_t>& bytes = code();
        int name = -1, count = 0;
        for (size_t at = static_cast<size_t>(start); at < bytes.size();) {
            if (static_cast<Op>(bytes[at]) == Op::Extension) {
                name = bytes[at + 1] | (bytes[at + 2] << 8);
                count++;
            }
            at += instructionSize(bytes, at);
        }
        return count == 1 ? name : -1;
    }

    // The global the code from `start` to here defines (its last DefineGlobal), or -1.
    int definedIn(int start) {
        const std::vector<uint8_t>& bytes = code();
        int slot = -1;
        for (size_t at = static_cast<size_t>(start); at < bytes.size();) {
            const Op op = static_cast<Op>(bytes[at]);
            if (op == Op::DefineGlobal || op == Op::DefineTypedGlobal) slot = bytes[at + 1] | (bytes[at + 2] << 8);
            at += instructionSize(bytes, at);
        }
        return slot;
    }

    // The sets of the names with several forms whose forms were just defined: each name's slot
    // gets one, which calls the form a call's arguments fit.
    void defineForms() {
        for (const auto& [entry, name] : session_.pendingForms_) {
            const int start = static_cast<int>(code().size());
            for (const auto& [signature, slot] : entry->forms) emit(Op::GetGlobal, slot);
            FunctionProto* set = newProto();
            set->name = name;
            set->source = file_;
            set->chooser = true;
            const int index = static_cast<int>(fs_->proto->protos.size());
            fs_->proto->protos.push_back(set);
            emit(Op::MakeForms, index);
            emitByte(static_cast<int>(entry->forms.size()));
            emit(Op::DefineGlobal, entry->slot);
            noteDeclaration(start, entry->slot);
        }
        session_.pendingForms_.clear();
    }

    // Extension functions, and extends (D33), are added once the classes they are for exist.
    void hoistExtensions(const std::vector<StmtPtr>& statements) {
        for (const StmtPtr& stmt : statements) {
            if (stmt->kind == StmtKind::Extend) {
                compileExtend(static_cast<const ExtendStmt&>(*stmt));
                hoisted_.insert(stmt.get());
                continue;
            }
            if (stmt->kind != StmtKind::Function || static_cast<const FunctionStmt&>(*stmt).receiver.empty()) continue;
            const int start = static_cast<int>(code().size());
            compileStmt(*stmt);
            noteDeclaration(start, extensionIn(start), DeclarationSpan::Extension);
            hoisted_.insert(stmt.get());
        }
    }

    // `extend Person, Car : Weighed { val name : string get() => model }` (D33): each class is
    // fitted to the interface, and given the getters that bridge its names.
    void compileExtend(const ExtendStmt& s) {
        for (const TypeAnnotation& type : s.types) {
            line_ = s.line;
            emitTypeName(type.name, type);
            emitTypeName(s.interfaceType.name, s.interfaceType);
            emit(Op::Extend);
            for (const auto& bridge : s.bridges) {
                nextIsExtension_ = true;
                nextReceiverType_ = type.name;
                compileFunction(*bridge->getter, FunctionKind::Method, type.name + "." + bridge->name);
                line_ = bridge->line;
                emit(Op::Bridge, stringConstant(bridge->name));
            }
            emit(Op::Pop);
        }
    }

    void hoistClass(const ClassStmt& cls) {
        line_ = cls.line;
        hoisted_.insert(&cls);
        const int start = static_cast<int>(code().size());
        hoistClassStructure(cls);
        noteDeclaration(start, classGlobal(cls));
        int key = classGlobal(cls);
        if (key < 0 && !cls.isExternal)
            if (const GlobalName* object = findGlobal(cls.isObject ? "(object " + cls.name + ")" : cls.name, cls, true)) key = object->slot;
        emitClassMetas(cls, cls.name, key);
    }

    // The global a top-level class is reached by, for noteDeclaration: the class, or for an
    // object the object; -1 for an object whose making may do something, which is made when the
    // program starts whether it is used or not.
    int classGlobal(const ClassStmt& cls) const {
        if (cls.isExternal || (cls.isObject && !quietToMake(cls))) return -1;
        const GlobalName* global = findGlobal(cls.name, cls, true);
        return global != nullptr ? global->slot : -1;
    }

    // Whether what is done where a class is written does nothing a program could see (M6.5 part
    // 5): its static values worked out, each quiet (quietValue), no classes inside; and for an
    // object or an enum, whose instances are made there too, no init, no superclass and quiet
    // initial values and entries. Such a class is left out of a built program that never reaches
    // it, though that would be done where it is written; any other is kept.
    bool quietToMake(const ClassStmt& cls) const {
        if (!cls.nested.empty()) return false;
        if (cls.isObject || cls.isEnum) {
            if (!cls.supertypes.empty() || !cls.initBlocks.empty() || cls.hasHeader) return false;
            for (const auto& method : cls.methods)
                if (method->name == "init" && !method->isStatic) return false;
            for (const auto& field : cls.fields)
                if (field->init && !quietValue(*field->init)) return false;
        }
        for (const auto& field : cls.staticFields)
            if (field->init && !quietValue(*field->init)) return false;
        for (const EnumEntry& entry : cls.entries)
            for (const ExprPtr& arg : entry.args)
                if (!quietValue(*arg)) return false;
        return true;
    }

    // A value whose working out does nothing a program could see: a literal, a constant, a
    // lambda, and lists, maps, text and arithmetic made of those.
    bool quietValue(const Expr& e) const {
        ConstValue constant;
        if (session_.checker->folded(e, constant) || session_.checker->constantAt(e, constant)) return true;
        switch (e.kind) {
            case ExprKind::Literal:
            case ExprKind::Function: return true;
            case ExprKind::List:
                for (const ExprPtr& item : static_cast<const ListExpr&>(e).items)
                    if (!quietValue(*item)) return false;
                return true;
            case ExprKind::Map:
                for (const auto& [key, value] : static_cast<const MapExpr&>(e).entries)
                    if (!quietValue(*key) || !quietValue(*value)) return false;
                return true;
            case ExprKind::Unary: return quietValue(*static_cast<const UnaryExpr&>(e).operand);
            default: return false;
        }
    }

    void hoistClassStructure(const ClassStmt& cls) {
        if (!shared_) {
            compileClass(cls, "", nullptr, ClassPart::Structure);
            return;
        }

        // In the script a folder's files share, a class is set up by a function of its own,
        // so that an error in doing so is reported against the file the class is written in.
        FunctionState state;
        state.enclosing = fs_;
        state.kind = FunctionKind::Function;
        state.proto = newProto();
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
    void compileRemainder(const std::vector<StmtPtr>& statements, bool shell) {
        for (size_t i = 0; i < statements.size(); i++) {
            const Stmt& stmt = *statements[i];
            if (hoisted_.count(&stmt) != 0) {
                if (stmt.kind == StmtKind::Class) {
                    line_ = stmt.line;
                    // Its static values are worked out here, and an object made: part of the class
                    // where that does nothing a program could see, and otherwise what keeps it
                    // (M6.5 part 5).
                    const auto& cls = static_cast<const ClassStmt&>(stmt);
                    const int start = static_cast<int>(code().size());
                    compileClass(cls, "", nullptr, ClassPart::Values);
                    if (quietToMake(cls)) noteDeclaration(start, classGlobal(cls));
                }
                continue;
            }
            if (shell && i + 1 == statements.size() && stmt.kind == StmtKind::Expr &&
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
    bool objectInClass_ = false;                   // an object declared inside a class, made when first used (D66)
    std::string nextReceiverType_;
    int anonymousClasses_ = 0;
    ObjectBuild* objectBuild_ = nullptr;  // the anonymous object being compiled, innermost
    bool shellInput_ = false;             // compiling an input of the shell
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

public:
    // The shell's imports, which last from one input to the next.
    void useImports(const std::vector<CompilerSession::ShellImport>& earlier) {
        for (const auto& i : earlier) imports_.push_back(Import{i.ns, i.alias, i.path, i.all});
    }
    std::vector<CompilerSession::ShellImport> importsNow() const {
        std::vector<CompilerSession::ShellImport> now;
        for (const Import& i : imports_) now.push_back({i.ns, i.alias, i.path, i.all});
        return now;
    }

private:
    bool jumpInExpression_ = false;  // the statement about to be compiled is a jump inside an expression

    // ---- Inline functions (D59) ----
    //
    // A call of an inline function is its body copied in: its receiver and arguments become
    // locals, then the body is compiled where the call is, in a scope of its own (CodeScope):
    // its names are those of the file and class it is written in, its locals its own, and it
    // reaches nothing of the code around. A lambda given for a parameter the body only calls is
    // not made: each call of the parameter is the lambda's body, compiled in the scope it was
    // written in, so its `return` leaves the function around and its break and continue reach
    // the loops around the call.
    struct InlineFrame;
    struct CodeScope {
        std::string fileKey;
        Namespace* ns = nullptr;
        std::vector<Import> imports;
        ClassState* cls = nullptr;
        int thisSlot = -1;               // an inline member's receiver
        std::vector<int> implicitSlots;  // objects bare names may be members of, innermost last
        bool implicitOnly = false;       // and no others: an inline function's body
        InlineFrame* frame = nullptr;    // the inline function whose body this is, if any
        int owner = 0;
        int lineFixed = -1;              // the line its code is said to be at: the call's
    };
    struct BoundLambda {
        const FunctionDecl* lambda = nullptr;
        CodeScope scope;      // where it is written, where its body is compiled
        std::string callee;   // the inline function it was given to, a name for return@
    };
    struct InlineFrame {
        const FunctionDecl* fn = nullptr;
        int owner = 0;
        std::unordered_map<std::string, BoundLambda> lambdas;  // parameters given lambdas copied in
        std::vector<int> exits;                                // its returns, to the end
        int baseSlot = 0;                                      // where its own locals start
        FinallyState* finallyAtEntry = nullptr;
        int tryDepth = 0;
        std::unique_ptr<ClassState> cls;                       // a member's class, as its body sees it
    };
    int thisSlot_ = -1;
    std::vector<int> implicitSlots_;
    bool implicitOnly_ = false;
    InlineFrame* frame_ = nullptr;
    int lineFixed_ = -1;
    int nextOwner_ = 0;
    std::unordered_set<int> inlineBodies_;  // owners that are inline bodies, reaching nothing around
    int inlineDepth_ = 0;
    int copiedDepth_ = 0;  // lambdas being copied in
    // What the checker says of reads where a value is (M6.6 parts 4 and 5) is of the code as
    // written, not as copied into another's: in an inline body or a lambda copied in, every
    // read is a copy (D59).
    bool inPlaceOff() const { return inlineDepth_ > 0 || copiedDepth_ > 0; }
    bool copyReadsHere() const { return copyingReads() || inPlaceOff(); }

    CodeScope scopeNow() const {
        CodeScope s;
        s.fileKey = fileKey_;
        s.ns = ns_;
        s.imports = imports_;
        s.cls = class_;
        s.thisSlot = thisSlot_;
        s.implicitSlots = implicitSlots_;
        s.implicitOnly = implicitOnly_;
        s.frame = frame_;
        s.owner = fs_->owner;
        s.lineFixed = lineFixed_;
        return s;
    }
    void enterScope(const CodeScope& s) {
        fileKey_ = s.fileKey;
        ns_ = s.ns;
        imports_ = s.imports;
        class_ = s.cls;
        thisSlot_ = s.thisSlot;
        implicitSlots_ = s.implicitSlots;
        implicitOnly_ = s.implicitOnly;
        frame_ = s.frame;
        fs_->owner = s.owner;
        lineFixed_ = s.lineFixed;
    }

    // Another name for a local's slot, in the code of the current owner.
    void aliasLocal(const std::string& name, int slot, bool isVal = true) {
        Local named{name, fs_->scopeDepth, VarInfo{isVal, TypeSpec{}, false}};
        named.slot = slot;
        named.owner = fs_->owner;
        named.alias = true;
        fs_->locals.push_back(std::move(named));
    }

    // What a member's body sees of its class: its members and statics, as compileClass has them.
    std::unique_ptr<ClassState> classScopeOf(const ClassStmt& s, const std::string& bound) {
        auto cls = std::make_unique<ClassState>(ClassState{nullptr});
        cls->storage = s.isObject ? "(object " + bound + ")" : bound;
        cls->qualified = bound;
        cls->hasSuper = s.hasSuper;
        cls->isEnum = s.isEnum;
        cls->isValue = s.isValue;
        for (const TypeParam& p : s.typeParams) cls->typeParams.push_back(p.name);
        cls->id = classId(s);
        if (!s.isInterface && !s.hasSuper && !s.fields.empty()) cls->closed = &s;
        for (const auto& field : s.fields) cls->members.insert(field->name);
        for (const auto& field : s.staticFields) cls->statics.insert(field->name);
        for (const auto& inner : s.nested) cls->statics.insert(inner->name);
        for (const auto& method : s.methods) (method->isStatic ? cls->statics : cls->members).insert(method->name);
        if (s.isEnum) {
            for (const EnumEntry& entry : s.entries) cls->statics.insert(entry.name);
            cls->statics.insert("entries");
            cls->members.insert("name");
            cls->members.insert("ordinal");
        }
        return cls;
    }

    // The lambda a parameter of the inline function whose body is being compiled was given, if
    // it is copied in and `name` means that parameter here.
    const BoundLambda* boundLambda(const std::string& name) {
        if (frame_ == nullptr || fs_->owner != frame_->owner) return nullptr;
        auto found = frame_->lambdas.find(name);
        if (found == frame_->lambdas.end()) return nullptr;
        VarInfo info;
        Node at;
        if (resolveLocal(fs_, name, at, info) != -1) return nullptr;  // a local of the body, by the same name
        return &found->second;
    }

    bool compileInlineCall(const CallExpr& e) {
        if (session_.checker == nullptr || inlineDepth_ >= 48) return false;
        Checker::InlineCall info;
        if (!session_.checker->inlineCall(e, info)) return false;
        const FunctionDecl& fn = *info.fn;
        if (fn.varargIndex >= 0 || fn.isStatic || fn.isAbstract || e.hasSpread) return false;
        const MemberExpr* member = e.callee->kind == ExprKind::Member ? static_cast<const MemberExpr*>(e.callee.get()) : nullptr;
        if (member != nullptr && importedMember(*member->object, member->name, *member) != nullptr) member = nullptr;
        const bool hasReceiver = info.extension || info.owner != nullptr;
        if (hasReceiver && member == nullptr && e.callee->kind != ExprKind::Name) return false;

        const int saved = enterValueBlock();
        beginScope();
        line_ = e.line;
        int toNull = -1;
        int receiver = -1;
        if (hasReceiver) {
            if (member != nullptr) {
                compileExpr(*member->object);
                line_ = member->line;
                if (member->safe) toNull = emitJump(Op::JumpIfNull);
            } else {
                emitGet("this", e);  // a member called bare, on the object of the code calling it
            }
            receiver = addLocal("(inline receiver)", true, e, true);
        }
        // The arguments, in the order they are written; a lambda the body only calls is kept, not made.
        const size_t n = fn.params.size();
        std::vector<int> given(n, -1);
        std::vector<std::optional<BoundLambda>> bound(n);
        for (size_t i = 0; i < e.args.size(); i++) {
            const int p = i < info.paramOf.size() ? info.paramOf[i] : -1;
            const Expr& arg = *e.args[i];
            if (p < 0 || static_cast<size_t>(p) >= n) {
                compileExpr(arg);
                emit(Op::Pop);
                continue;
            }
            if (session_.checker->copiedIn(fn, p)) {
                RuntimeType iface;
                std::string method;
                int arity = 0;
                if (arg.kind == ExprKind::Function && static_cast<const FunctionExpr&>(arg).fn->isLambda &&
                    !session_.checker->samConversion(arg, iface, method, arity)) {
                    bound[static_cast<size_t>(p)] = BoundLambda{static_cast<const FunctionExpr&>(arg).fn.get(), scopeNow(), fn.name};
                    continue;
                }
                if (arg.kind == ExprKind::Name) {
                    if (const BoundLambda* passed = boundLambda(static_cast<const NameExpr&>(arg).name)) {
                        bound[static_cast<size_t>(p)] = *passed;  // passed on from the inline function around
                        continue;
                    }
                }
            }
            compileExpr(arg);
            given[static_cast<size_t>(p)] = addLocal("(inline argument)", true, arg, true);
        }

        // The body, in a scope of its own.
        const CodeScope caller = scopeNow();
        InlineFrame frame;
        frame.fn = &fn;
        frame.owner = ++nextOwner_;
        inlineBodies_.insert(frame.owner);
        frame.finallyAtEntry = fs_->finally;
        frame.tryDepth = fs_->tryDepth;
        for (size_t p = 0; p < n; p++)
            if (bound[p]) frame.lambdas[fn.params[p]] = *bound[p];
        if (info.owner != nullptr) frame.cls = classScopeOf(*info.owner, info.ownerName);
        CodeScope body;
        body.fileKey = info.fileKey;
        body.ns = info.prelude ? session_.builtins_ : info.ns != nullptr ? info.ns : ns_;
        auto imported = session_.fileImports_.find(info.fileKey);
        if (imported != session_.fileImports_.end())
            for (const auto& i : imported->second) body.imports.push_back(Import{i.ns, i.alias, i.path, i.all});
        body.cls = frame.cls.get();
        if (receiver >= 0) {
            if (info.owner != nullptr) body.thisSlot = slotOf(receiver);
            body.implicitSlots.push_back(slotOf(receiver));
        }
        body.implicitOnly = true;
        body.frame = &frame;
        body.owner = frame.owner;
        body.lineFixed = lineFixed_ >= 0 ? lineFixed_ : e.line;
        enterScope(body);
        inlineDepth_++;
        // It keeps its own rights where it is copied: the function it is copied into may use what
        // its class and its library keep to themselves (see Op::MemberAccess).
        auto grant = [&](int id) {
            std::vector<uint16_t>& ids = fs_->proto->classIds;
            if (std::find(ids.begin(), ids.end(), static_cast<uint16_t>(id)) == ids.end()) ids.insert(ids.end() - 1, static_cast<uint16_t>(id));
        };
        if (class_ != nullptr) grant(class_->id);
        grant(libraryId());
        beginScope();
        frame.baseSlot = fs_->slotTop;
        if (receiver >= 0) aliasLocal("this", slotOf(receiver));
        for (size_t p = 0; p < n; p++) {
            if (bound[p]) continue;
            if (given[p] >= 0) {
                aliasLocal(fn.params[p], slotOf(given[p]));
                continue;
            }
            // Left out: its default, worked out here from the parameters before it.
            if (fn.paramDefaults[p]) compileExpr(*fn.paramDefaults[p]);
            else emit(Op::Null);
            addLocal(fn.params[p], true, fn, true);
        }
        compileInlineBody(fn.body);
        endScopeKeepingValue();
        for (int jump : frame.exits) patchJump(jump);
        inlineDepth_--;
        enterScope(caller);
        line_ = e.line;
        endScopeKeepingValue();
        if (toNull >= 0) patchJump(toNull);
        leaveValueBlock(saved);
        return true;
    }

    // A body compiled in place leaves its value: what its last expression comes to, or null.
    void compileInlineBody(const std::vector<StmtPtr>& body) {
        for (size_t i = 0; i + 1 < body.size(); i++) compileStmt(*body[i]);
        const Stmt* last = body.empty() ? nullptr : body.back().get();
        if (last != nullptr && last->kind == StmtKind::Return && static_cast<const ReturnStmt&>(*last).implicit &&
            static_cast<const ReturnStmt&>(*last).value) {
            line_ = last->line;
            compileExpr(*static_cast<const ReturnStmt&>(*last).value);
        } else {
            if (last != nullptr) compileStmt(*last);
            emit(Op::Null);
        }
    }

    // `return` in an inline function's body: the value of the call, after the finally blocks
    // entered since it began.
    void compileInlineReturn(const ReturnStmt& s, bool inExpression) {
        InlineFrame& frame = *frame_;
        if (s.value) compileExpr(*s.value);
        else emit(Op::Null);
        line_ = s.line;
        if (inExpression) emitSettle(true);
        beginScope();
        const int result = addLocal("(inline value)", false, s, true);
        const int open = emitPendingFinally(nullptr, frame.finallyAtEntry);
        for (int i = open; i > frame.tryDepth; i--) emit(Op::TryEnd);
        emit(Op::GetLocal, slotOf(result));
        emit(Op::PopUnder, fs_->slotTop - frame.baseSlot);
        frame.exits.push_back(emitJump(Op::Jump));
        endScope();
    }

    // A call of a parameter given a lambda copied in: the lambda's body, in the scope it was
    // written in, given the arguments, and the object it runs on if its type says one.
    void compileLambdaCopy(const BoundLambda& b, const std::vector<const Expr*>& args, const Expr* receiver, const Node& at) {
        const FunctionDecl& lambda = *b.lambda;
        const int saved = enterValueBlock();
        beginScope();
        line_ = at.line;
        int object = -1;
        if (receiver != nullptr) {
            compileExpr(*receiver);
            object = addLocal("(block receiver)", true, at, true);
        }
        std::vector<int> given;
        for (const Expr* arg : args) {
            compileExpr(*arg);
            given.push_back(addLocal("(block argument)", true, *arg, true));
        }
        const CodeScope here = scopeNow();
        CodeScope scope = b.scope;
        copiedDepth_++;
        if (object >= 0) scope.implicitSlots.push_back(slotOf(object));
        enterScope(scope);
        InlineBlock block{inline_, b.callee, lambda.label, fs_, fs_->slotTop - 1, fs_->finally, fs_->tryDepth, {}, fs_->owner};
        inline_ = &block;
        beginScope();
        const bool it = lambda.params.size() == 1 && lambda.params[0] == "it" && !lambda.paramTypes[0].present() &&
                        lambda.paramDefaults.size() == 1 && lambda.paramDefaults[0] != nullptr;
        for (size_t i = 0; i < lambda.params.size(); i++) {
            if (it && given.empty()) break;  // no `it`: nothing was given
            if (i < given.size()) {
                if (lambda.params[i] != "_") aliasLocal(lambda.params[i], slotOf(given[i]));
            } else {
                emit(Op::Null);
                addLocal(lambda.params[i], true, lambda, true);
            }
            if (i < lambda.paramPatterns.size() && !lambda.paramPatterns[i].empty() && i < given.size())
                declareParts(lambda.paramPatterns[i], given[i], true, lambda);
        }
        if (object >= 0) {
            aliasLocal("this", slotOf(object));
            if (!lambda.label.empty()) aliasLocal("this@" + lambda.label, slotOf(object));
            if (!b.callee.empty() && b.callee != lambda.label) aliasLocal("this@" + b.callee, slotOf(object));
        }
        compileInlineBody(lambda.body);
        endScopeKeepingValue();
        for (int jump : block.exits) patchJump(jump);
        copiedDepth_--;
        inline_ = block.enclosing;
        enterScope(here);
        endScopeKeepingValue();
        leaveValueBlock(saved);
    }

    // A call in an inline function's body of a parameter given a lambda copied in: `action(x)`,
    // `block(obj, x)` or `obj.block(x)` for one run on an object.
    bool compileBoundCall(const CallExpr& e) {
        if (frame_ == nullptr || fs_->owner != frame_->owner || e.namedCount != 0 || e.hasSpread) return false;
        const Expr* receiver = nullptr;
        std::vector<const Expr*> args;
        const BoundLambda* b = nullptr;
        if (e.callee->kind == ExprKind::Name) {
            b = boundLambda(static_cast<const NameExpr&>(*e.callee).name);
            if (b == nullptr) return false;
            size_t first = 0;
            if (session_.checker->callsOnObject(e) && !e.args.empty()) {
                receiver = e.args[0].get();
                first = 1;
            }
            for (size_t i = first; i < e.args.size(); i++) args.push_back(e.args[i].get());
        } else if (e.callee->kind == ExprKind::Member && session_.checker->memberLocalCall(e)) {
            const auto& m = static_cast<const MemberExpr&>(*e.callee);
            b = boundLambda(m.name);
            if (b == nullptr) return false;
            receiver = m.object.get();
            for (const ExprPtr& a : e.args) args.push_back(a.get());
        } else {
            return false;
        }
        compileLambdaCopy(*b, args, receiver, e);
        return true;
    }

    // `block(obj, x)` and `obj.block(x)` for a function value run on an object (D58).
    bool compileCallOn(const CallExpr& e) {
        if (session_.checker == nullptr || e.namedCount != 0 || e.hasSpread) return false;
        if (e.callee->kind == ExprKind::Name && session_.checker->callsOnObject(e) && !e.args.empty()) {
            compileExpr(*e.callee);
            for (const ExprPtr& a : e.args) compileExpr(*a);
            line_ = e.line;
            emit(Op::CallOn);
            emitByte(static_cast<int>(e.args.size()) - 1);
            return true;
        }
        if (e.callee->kind == ExprKind::Member && session_.checker->memberLocalCall(e)) {
            const auto& m = static_cast<const MemberExpr&>(*e.callee);
            emitGet(m.name, m);
            compileExpr(*m.object);
            for (const ExprPtr& a : e.args) compileExpr(*a);
            line_ = e.line;
            emit(Op::CallOn);
            emitByte(static_cast<int>(e.args.size()));
            return true;
        }
        return false;
    }

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
    // A place code may jump to: what is emitted there is never joined to what comes before.
    int here() {
        fs_->barrier = static_cast<int>(code().size());
        return fs_->barrier;
    }

    void emitByte(int b) {
        fs_->proto->code.push_back(static_cast<uint8_t>(b));
        fs_->proto->lines.push_back(lineFixed_ >= 0 ? lineFixed_ : line_);
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

    void emit(Op op) {
        if (joined(op)) return;
        fs_->beforeLast = fs_->lastOp;
        fs_->lastOp = static_cast<int>(code().size());
        emitByte(static_cast<int>(op));
    }

    // Two instructions in one (M6.5 part 4): `op` with the instruction just emitted, where that
    // pushed the local or the constant `op` takes as its second operand, or `op` pops what that
    // stored. Never across a place code jumps to.
    bool joined(Op op) {
        const int end = static_cast<int>(code().size());
        if (fs_->lastOp < 0 || fs_->barrier == end || end - fs_->lastOp != 3) return false;
        const Op last = static_cast<Op>(code()[static_cast<size_t>(fs_->lastOp)]);
        Op both = Op::Count_;
        if (op == Op::Pop && last == Op::SetLocal) both = Op::StoreLocal;
        if (last == Op::GetLocal || last == Op::Const) {
            const bool local = last == Op::GetLocal;
            switch (op) {
                case Op::Add: both = local ? Op::AddLocal : Op::AddConst; break;
                case Op::Sub: both = local ? Op::SubLocal : Op::SubConst; break;
                case Op::Mul: both = local ? Op::MulLocal : Op::MulConst; break;
                case Op::Div: both = local ? Op::DivLocal : Op::DivConst; break;
                case Op::Mod: both = local ? Op::ModLocal : Op::ModConst; break;
                case Op::Lt: both = local ? Op::LtLocal : Op::LtConst; break;
                case Op::Le: both = local ? Op::LeLocal : Op::LeConst; break;
                case Op::Gt: both = local ? Op::GtLocal : Op::GtConst; break;
                case Op::Ge: both = local ? Op::GeLocal : Op::GeConst; break;
                case Op::Eq: both = local ? Op::EqLocal : Op::EqConst; break;
                default: break;
            }
        }
        if (both == Op::Count_) return false;
        code()[static_cast<size_t>(fs_->lastOp)] = static_cast<uint8_t>(both);
        joinedAgain(both);
        return true;
    }

    // And three: the instruction just joined with a GetLocal before it, `GetLocal a; MulLocal b`
    // being MulLocals a b and `GetLocal a; StoreLocal b` CopyLocal a b.
    void joinedAgain(Op both) {
        const int at = fs_->beforeLast;
        if (at < 0 || fs_->barrier > at || fs_->lastOp - at != 3 || static_cast<Op>(code()[static_cast<size_t>(at)]) != Op::GetLocal)
            return;
        Op three = Op::Count_;
        switch (both) {
            case Op::AddLocal: three = Op::AddLocals; break;
            case Op::SubLocal: three = Op::SubLocals; break;
            case Op::MulLocal: three = Op::MulLocals; break;
            case Op::DivLocal: three = Op::DivLocals; break;
            case Op::LtLocal: three = Op::LtLocals; break;
            case Op::LeLocal: three = Op::LeLocals; break;
            case Op::GtLocal: three = Op::GtLocals; break;
            case Op::GeLocal: three = Op::GeLocals; break;
            case Op::EqLocal: three = Op::EqLocals; break;
            case Op::StoreLocal: three = Op::CopyLocal; break;
            default: return;
        }
        // The second's slot moves up behind the first's, and its opcode goes.
        std::vector<uint8_t>& bytes = code();
        const size_t first = static_cast<size_t>(at);
        bytes[first] = static_cast<uint8_t>(three);
        bytes[first + 3] = bytes[first + 4];
        bytes[first + 4] = bytes[first + 5];
        bytes.resize(first + 5);
        fs_->proto->lines.resize(first + 5);
        fs_->lastOp = at;
        fs_->beforeLast = -1;
    }

    void emit(Op op, int u16) {
        if (u16 < 0 || u16 > 0xFFFF) throw CompileError(file_, line_, 1, "function is too large (operand overflow)");
        emit(op);
        emitU16(u16);
        if (op == Op::GetProp || op == Op::SetProp || op == Op::GetOwn || op == Op::SetOwn || op == Op::GetPropRef || op == Op::GetOwnRef) {
            // Each has a place of its own to remember where it found the field (M6.5 part 4).
            if (fs_->caches > 0xFFFF) throw CompileError(file_, line_, 1, "function is too large (too many members used)");
            emitU16(fs_->caches++);
        }
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

    int charConstant(uint32_t c) { return addConstant(Value::character(c), 3, c); }
    int byteConstant(uint8_t b) { return addConstant(Value::byte(b), 4, b); }

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
            if (!locals.back().alias) {
                emit(locals.back().isCaptured ? Op::CloseUpvalue : Op::Pop);
                fs_->slotTop--;
            }
            locals.pop_back();
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
                if (existing.owner != fs_->owner) continue;
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
            if (local.name != name || local.owner != state->owner) continue;
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

    // A variable of a function around `state`, as an upvalue of it; for a member of an
    // anonymous object's class, as a capture of the object (see ObjectBuild).
    int resolveUpvalue(FunctionState* state, const std::string& name, const Node& at, VarInfo& info) {
        info = VarInfo{};
        if (state->enclosing == nullptr || inlineBodies_.count(state->owner) != 0) return -1;
        if (state->object != nullptr) return captureFor(state, name, at, info);

        int local = resolveLocal(state->enclosing, name, at, info, true);
        if (local != -1) {
            Local& captured = state->enclosing->locals[static_cast<size_t>(local)];
            noteOuterUse(state, static_cast<size_t>(local));
            markCaptured(*state->enclosing, captured);
            return addUpvalue(state, true, captured.slot);
        }

        int upvalue = resolveUpvalue(state->enclosing, name, at, info);
        if (upvalue == -1) return -1;
        if (isCaptureMark(upvalue)) return addUpvalue(state, false, upvalue - kCaptureMark, true);
        noteOuterUse(state, 0);
        return addUpvalue(state, false, upvalue);
    }

    // A local a closure keeps: under another name (an inline function's parameter, D59), the
    // local whose slot it is, which is closed when its scope ends.
    static void markCaptured(FunctionState& state, Local& local) {
        local.isCaptured = true;
        if (!local.alias) return;
        for (Local& real : state.locals)
            if (!real.alias && real.slot == local.slot) real.isCaptured = true;
    }

    // What a member of an anonymous object's class uses of the function the object is made in:
    // captured when the object is made, and marked as such (kCaptureMark).
    int captureFor(FunctionState* state, const std::string& name, const Node& at, VarInfo& info) {
        ObjectBuild& object = *state->object;
        FunctionState* around = state->enclosing;
        const int local = resolveLocal(around, name, at, info, true);
        if (local != -1) {
            if (local == object.superLocal) return kCaptureMark + object.add(3, 0);  // the same for every object
            Local& captured = around->locals[static_cast<size_t>(local)];
            markCaptured(*around, captured);
            return kCaptureMark + object.add(0, captured.slot);
        }
        const int upvalue = resolveUpvalue(around, name, at, info);
        if (upvalue == -1) return -1;
        return kCaptureMark + (isCaptureMark(upvalue) ? object.add(2, upvalue - kCaptureMark) : object.add(1, upvalue));
    }

    // The code that makes an anonymous object's class, in the function around, uses one of its
    // variables (`outer`, or a local from before the object): the class is made every time.
    void noteMakingUses(bool outer) {
        if (outer && objectBuild_ != nullptr && fs_ == objectBuild_->boundary) objectBuild_->usesOuter = true;
    }
    bool beforeObject(int local) const {
        return objectBuild_ != nullptr && static_cast<size_t>(local) < objectBuild_->localsAtStart;
    }

    // A function made with an anonymous object's class, not a member of it (a field's type,
    // a class declared inside), uses a variable of the code around: the class is made every time.
    void noteOuterUse(const FunctionState* state, size_t local) {
        if (objectBuild_ != nullptr && state->enclosing == objectBuild_->boundary && local < objectBuild_->localsAtStart)
            objectBuild_->usesOuter = true;
    }

    int addUpvalue(FunctionState* state, bool isLocal, int index, bool fromCapture = false) {
        std::vector<UpvalueDesc>& upvalues = state->proto->upvalues;
        for (size_t i = 0; i < upvalues.size(); i++)
            if (upvalues[i].isLocal == isLocal && upvalues[i].index == index && upvalues[i].fromCapture == fromCapture)
                return static_cast<int>(i);
        upvalues.push_back(UpvalueDesc{isLocal, static_cast<uint16_t>(index), fromCapture});
        return static_cast<int>(upvalues.size()) - 1;
    }

    // Gives `state` access to slot 0 (the receiver, if there is one) of the function `levels`
    // steps out from it, as an upvalue; for a member of an anonymous object's class, as a
    // capture of the object (kCaptureMark).
    int captureReceiver(FunctionState* state, int levels) {
        if (state->object != nullptr) {
            ObjectBuild& object = *state->object;
            if (levels == 1) {
                state->enclosing->locals[0].isCaptured = true;
                return kCaptureMark + object.add(0, 0);
            }
            const int outer = captureReceiver(state->enclosing, levels - 1);
            return kCaptureMark + (isCaptureMark(outer) ? object.add(2, outer - kCaptureMark) : object.add(1, outer));
        }
        noteOuterUse(state, 0);
        if (levels == 1) {
            state->enclosing->locals[0].isCaptured = true;
            return addUpvalue(state, true, 0);
        }
        const int outer = captureReceiver(state->enclosing, levels - 1);
        return isCaptureMark(outer) ? addUpvalue(state, false, outer - kCaptureMark, true) : addUpvalue(state, false, outer);
    }

    // `state`, just made inside the class of the anonymous object being compiled: the object it
    // reaches the code around through, if it is a member that runs on the object.
    ObjectBuild* objectMember(const FunctionState& state) const {
        if (objectBuild_ == nullptr || state.enclosing != objectBuild_->boundary || class_ != objectBuild_->classState) return nullptr;
        return kindHasThis(state.kind) ? objectBuild_ : nullptr;
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

    // A global of the standard library's that the compiler calls itself, by its name.
    int stdGlobal(const std::string& name, const Node& at) const {
        auto found = session_.builtins_->names.find(name);
        if (found == session_.builtins_->names.end() || !found->second.declared || found->second.slot < 0)
            throw error(at, "the standard library has no " + name + " here");
        return found->second.slot;
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
        // The built-ins named after their types took the types' names when those became Title case.
        if (auto it = ns_->names.find(name); (it == ns_->names.end() || !it->second.declared) &&
                                             (name == "bytes" || name == "range"))
            throw error(at, "'" + name + "' is now written '" + (name == "bytes" ? "Bytes" : "Range") +
                                "', as its type is (type names are Title case)");
        GlobalName& fresh = ns_->names[name];
        if (fresh.slot < 0) fresh.slot = session_.newSlot(name);
        return &fresh;
    }

    // The entry of a name this file declares at its top level.
    GlobalName& globalEntry(const std::string& name) {
        auto& mine = session_.privates_[fileKey_];
        auto priv = mine.find(name);
        return priv != mine.end() ? priv->second : ns_->names[name];
    }

    // What the program needs to choose a form when it runs: its signature and its parameters' types.
    void markForm(FunctionProto& proto, const FunctionDecl& fn) {
        proto.signature = signatureText(fn);
        for (size_t i = 0; i < fn.params.size(); i++) {
            TypeSpec type = i < fn.paramTypes.size() ? resolveType(fn.paramTypes[i]) : TypeSpec{};
            proto.paramTypes.push_back(type.flags);
            ObjString* cls = nullptr;
            if (type.id() == kTypeInstance) {
                const size_t dot = type.className.rfind('.');
                cls = session_.vm_.intern(dot == std::string::npos ? type.className : type.className.substr(dot + 1));
            }
            proto.paramClasses.push_back(cls);
            const bool function = i < fn.paramTypes.size() && fn.paramTypes[i].isFunction();
            proto.paramCounts.push_back(static_cast<uint8_t>(function ? std::min<size_t>(fn.paramTypes[i].params.size() + 1, 255) : 0));
        }
    }

    // The name a method is known by when the program runs: where its name has several forms,
    // the name with the signature of the form it is (`draw(Canvas)`); otherwise the name.
    std::string formNameOf(const ClassStmt& cls, const FunctionDecl& method) {
        if (method.accessor != FunctionDecl::Accessor::None || method.name == "init") return method.name;
        if (method.isStatic) return staticForms(cls, method.name) > 1 ? method.name + signatureText(method) : method.name;
        const FunctionDecl* root = session_.checker->formRoot(method);
        return root != nullptr ? method.name + signatureText(*root) : method.name;
    }

    // How many static methods of a name a class declares: its forms, statics being nobody else's.
    static int staticForms(const ClassStmt& cls, const std::string& name) {
        int count = 0;
        for (const auto& method : cls.methods)
            count += method->isStatic && method->accessor == FunctionDecl::Accessor::None && method->name == name;
        return count;
    }

    // The name a method call is compiled with: where the checker could tell which of a name's
    // several forms the call means, that form's own name, with nothing to choose when it runs.
    std::string calledName(const CallExpr& e, const std::string& name) {
        const FunctionDecl* chosen = session_.checker->chosenForm(e);
        if (chosen == nullptr) return name;
        if (chosen->isStatic) return name + signatureText(*chosen);  // chosen among the class's own
        if (const TypeAnnotation* receiver = session_.checker->extensionReceiver(*chosen))
            return name + "[" + annotationText(*receiver) + "]" + signatureText(*chosen);
        const FunctionDecl* root = session_.checker->formRoot(*chosen);
        return root != nullptr ? name + signatureText(*root) : name;
    }

    // The function a call names. Where the name has several forms and the checker could tell
    // which one the call means, that form is called directly, with nothing to choose when it runs.
    // The full name of an enum class's kind written bare (D39), as the checker found it.
    const std::string* kindName(const Node& at) const {
        return session_.checker != nullptr ? session_.checker->kindName(at) : nullptr;
    }

    void compileCallee(const CallExpr& e) {
        if (const std::string* kind = kindName(*e.callee)) {
            emitTypeName(*kind, *e.callee);  // Click(1, 2) where an Event goes (D39)
            return;
        }
        if (e.callee->kind == ExprKind::Name) {
            if (const FunctionDecl* form = session_.checker->chosenForm(e)) {
                const auto& callee = static_cast<const NameExpr&>(*e.callee);
                VarInfo info;
                if (resolveLocal(fs_, callee.name, callee, info) == -1 && resolveUpvalue(fs_, callee.name, callee, info) == -1) {
                    const GlobalName* global = findGlobal(callee.name, callee, true);
                    if (global != nullptr && !global->forms.empty()) {
                        const int slot = global->formSlot(signatureText(*form));
                        if (slot >= 0) {
                            line_ = callee.line;
                            emit(Op::GetGlobal, slot);
                            return;
                        }
                    }
                }
            }
        }
        compileExpr(*e.callee);
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

    // A function, marked with the numbers of the classes it is written inside of, innermost
    // first, and of its library: what it may use of the members that are private, protected
    // or internal (see Op::MemberAccess).
    FunctionProto* newProto() {
        FunctionProto* proto = session_.program.newProto();
        for (const ClassState* cls = class_; cls != nullptr; cls = cls->enclosing)
            proto->classIds.push_back(static_cast<uint16_t>(cls->id));
        proto->classIds.push_back(static_cast<uint16_t>(libraryId()));
        proto->ownClass = class_ != nullptr ? static_cast<uint16_t>(class_->id) : 0;
        return proto;
    }

    int nextNumber(const Node& at) {
        if (session_.classCount_ >= 0xFFFF) throw error(at, "a program cannot have more than 65535 classes and libraries");
        return ++session_.classCount_;
    }

    // The number a class is known by, the same each time a part of it is compiled.
    int classId(const ClassStmt& s) {
        if (s.privateId == 0) s.privateId = nextNumber(s);
        return s.privateId;
    }

    // The number of the library this file belongs to, from the same series.
    int libraryId() { return session_.libraryNumber(ns_->library, file_, line_); }

public:
    // A kept unit put back without compiling it (D145): what compiling the file notes for the
    // rest of the program. What it imports is loaded, its enums' entries are noted, and the
    // classes its typed globals are declared with.
    void declareKept(const std::vector<StmtPtr>& statements) {
        noteEnums(statements);
        for (const StmtPtr& stmt : statements) {
            if (stmt->kind != StmtKind::Import) continue;
            const auto& s = static_cast<const ImportStmt&>(*stmt);
            std::vector<int> modules;
            Namespace* imported = session_.importModule(s.path, directory_, file_, s, modules);
            imports_.push_back(Import{imported, s.alias, s.path, s.all});
        }
        for (const StmtPtr& stmt : statements) {
            if (stmt->kind != StmtKind::Var) continue;
            const auto& v = static_cast<const VarStmt&>(*stmt);
            if (v.isExternal || !v.pattern.empty()) continue;
            const TypeSpec type = resolveType(v.type);
            const GlobalName& entry = globalEntry(v.name);
            if (type.id() == kTypeInstance && entry.slot >= 0) session_.globalClassTypes_[entry.slot] = type.className;
        }
    }

private:

    // Says who may use the members a class declares: the access word in front of each, and
    // the one in front of `set` under a property.
    void emitMemberAccess(const ClassStmt& s, const ClassState& cls) {
        std::unordered_set<std::string> seen;
        auto mark = [&](bool isStatic, const std::string& name, int access, bool isOverride, bool write, int line) {
            int kind = 4, id = cls.id;  // nothing said: as it is above
            if (access == kAccessWordPublic) kind = 0;
            if (access == kAccessWordPrivate) kind = 1;
            if (access == kAccessWordProtected) kind = 2;
            if (access == kAccessWordInternal) {
                kind = 3;
                id = libraryId();
            }
            if (kind == 0 || kind == 4) {
                // Only a member that takes the place of one above has anything to change.
                if (isStatic || !isOverride || !s.hasSuper) return;
                id = 0;
            }
            if (!seen.insert(std::string(isStatic ? "static " : "") + (write ? "set " : "") + name).second) return;
            line_ = line;
            emit(isStatic ? Op::StaticAccess : Op::MemberAccess);
            emitU16(stringConstant(name));
            emitU16(id);
            emitByte(kind | (write ? 0x10 : 0));
        };
        auto field = [&](bool isStatic, const VarStmt& f) {
            mark(isStatic, f.name, f.access, f.isOverride, false, f.line);
            if (f.setAccess != 0) mark(isStatic, f.name, f.setAccess, false, true, f.line);
        };
        for (const auto& f : s.fields) field(false, *f);
        for (const auto& f : s.staticFields) field(true, *f);
        for (const auto& method : s.methods) {
            if (!method->isStatic && method->name == "init") continue;
            // A setter written on its own, `set name(value)`, speaks for assigning only.
            mark(method->isStatic, method->name, method->access, method->isOverride,
                 method->accessor == FunctionDecl::Accessor::Set, method->line);
            const std::string runtimeName = formNameOf(s, *method);
            if (runtimeName != method->name)
                mark(method->isStatic, runtimeName, method->access, method->isOverride, false, method->line);
        }
        for (const auto& inner : s.nested) mark(true, inner->name, inner->access, false, false, inner->line);
    }

    // Whether `object.name` is a member the class declares itself, reached through `this`
    // by one of its methods. Its own code may always use it, on an object of a subclass too,
    // so the instruction that asks nothing is written (GetOwn and the like).
    bool ownMember(const Expr& object, const std::string& name) const {
        return object.kind == ExprKind::This && class_ != nullptr && directThis() &&
               classSide() == ClassSide::Instance && class_->members.count(name) != 0;
    }

    bool mayBeMemberHere() const {
        const ClassSide side = classSide();
        return side == ClassSide::None ? namesFromReceiver() : mayBeMember(side);
    }

    // Whether the function being compiled may be run on an object whose members its bare
    // names can mean: a lambda, or an extension function.
    bool namesFromReceiver() const {
        if (frame_ != nullptr) return !implicitSlots_.empty();  // an inline function's body: its own receiver's alone
        return fs_->isLambda || fs_->isExtension || !implicitSlots_.empty();
    }

    // `this` of the code being compiled reached directly: a method's own, or that of an inline
    // member's body (D59), where no block copied in place runs on an object nearer (D58).
    bool directThis() const {
        if (thisSlot_ >= 0) return implicitSlots_.size() == 1 && implicitSlots_[0] == thisSlot_;
        return kindHasThis(fs_->kind) && implicitSlots_.empty();
    }
    void emitThisValue() { emit(Op::GetLocal, thisSlot_ >= 0 ? thisSlot_ : 0); }

    // A lookup of a bare name when the program runs, told first of the objects of the blocks
    // copied in place around it, innermost last; in an inline function's body, of its receiver
    // and no other (D58, D59).
    void emitImplicitOp(Op op, int operand) {
        for (size_t i = 0; i < implicitSlots_.size(); i++) {
            emit(Op::ImplicitFirst, implicitSlots_[i]);
            emitByte(i + 1 == implicitSlots_.size() && implicitOnly_ ? 1 : 0);
        }
        emit(op, operand);
    }

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
            emitImplicitOp(Op::TryImplicit, stringConstant(name));
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
        if (expr.kind != ExprKind::Name || kindName(expr) != nullptr || isKnownName(static_cast<const NameExpr&>(expr).name)) return compileExpr(expr);
        const std::string& name = static_cast<const NameExpr&>(expr).name;
        line_ = expr.line;
        int toEnd = -1;
        if (mayBeMemberHere()) {
            emitImplicitOp(Op::TryImplicit, stringConstant(name));
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
        if (thisSlot_ >= 0) return ClassSide::Instance;  // an inline member's body (D59)
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
        return !(directThis() && class_->closed != nullptr && (thisSlot_ >= 0 || fs_->proto->outerReceivers.empty()));
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
            emitImplicitOp(Op::TryImplicit, stringConstant(name));
            toEnd = here();
            emitI32(0);
        }
        emitGet(owner->storage, at);
        emit(Op::GetOwn, stringConstant(name));
        if (toEnd >= 0) patchJump(toEnd);
        return true;
    }

    // A type as written: a name, or `Outer.Inner` for a class declared inside another.
    void emitTypeName(const std::string& name, const Node& at) {
        // A class by its global's slot, as the compiler names one it makes a class of (D28).
        if (name.rfind("(global ", 0) == 0) {
            emit(Op::GetGlobal, std::stoi(name.substr(8)));
            return;
        }
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
        emitImplicitOp(op, stringConstant(name));
        emitU16(slot);
    }

    void emitGet(const std::string& name, const Node& at) {
        VarInfo info;
        int local = resolveLocal(fs_, name, at, info);
        if (local != -1) {
            noteMakingUses(beforeObject(local));
            emit(Op::GetLocal, slotOf(local));
            return;
        }

        int upvalue = resolveUpvalue(fs_, name, at, info);
        if (upvalue != -1 && isCaptureMark(upvalue)) {
            emit(Op::GetCaptured, upvalue - kCaptureMark);
            emitU16(stringConstant(name));
            return;
        }
        if (upvalue != -1) {
            noteMakingUses(true);
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
                if (directThis()) {
                    emitThisValue();
                    const bool inPlace = session_.checker != nullptr && !copyReadsHere() && session_.checker->readsFieldInPlace(at);
                    emit(inPlace ? Op::GetOwnRef : Op::GetOwn, stringConstant(name));
                } else {
                    emitImplicit(Op::GetImplicit, name);  // in a function inside the method
                }
                return;
            }
            if (class_->statics.count(name) != 0) {
                // An inherited member, or one of a lambda's receiver, comes before a static.
                int toEnd = -1;
                if (namesFromReceiver() || (side == ClassSide::Instance && class_->hasSuper)) {
                    emitImplicitOp(Op::TryImplicit, stringConstant(name));
                    toEnd = here();
                    emitI32(0);
                }
                emitGet(class_->storage, at);
                emit(Op::GetOwn, stringConstant(name));
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
            emitImplicitOp(Op::GetImplicit, stringConstant(name));
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
            noteMakingUses(beforeObject(local));
            emitTypeCheck(info.type, name, at);
            emit(Op::SetLocal, slotOf(local));
            return;
        }

        int upvalue = resolveUpvalue(fs_, name, at, info);
        if (upvalue != -1) {
            if (info.isVal) throw error(at, "cannot assign to val '" + name + "'");
            emitTypeCheck(info.type, name, at);
            if (isCaptureMark(upvalue)) {
                emit(Op::SetCaptured, upvalue - kCaptureMark);
            } else {
                noteMakingUses(true);
                emit(Op::SetUpvalue, upvalue);
            }
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
                if (directThis()) {
                    emitThisValue();
                    emit(Op::Swap);
                    emit(Op::SetOwn, stringConstant(name));
                } else {
                    emitImplicit(Op::SetImplicit, name);
                }
                return;
            }
            if (class_->statics.count(name) != 0) {
                emitGet(class_->storage, at);
                emit(Op::Swap);
                emit(Op::SetOwn, stringConstant(name));
                return;
            }
            if (mayBeMember(side)) {
                emitImplicit(Op::SetImplicit, name);
                return;
            }
        }

        if (namesFromReceiver()) {
            const int slot = resolveGlobal(name, at)->slot;
            emitImplicitOp(Op::SetImplicit, stringConstant(name));
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

    // A type written with a name `type` gave it (D41): the type it names; otherwise as written.
    const TypeAnnotation& written(const TypeAnnotation& t) const {
        if (session_.checker != nullptr)
            if (const TypeAnnotation* named = session_.checker->aliasExpansion(t)) return *named;
        return t;
    }

    TypeSpec resolveType(const TypeAnnotation& written_) {
        const TypeAnnotation& s = written(written_);
        TypeSpec type;
        // A union (D40) is checked by the checker alone: nothing of it is kept when the program runs.
        if (s.name == "|") return type;
        // `any` takes every value; `unit` is what a function that returns nothing is declared as.
        if (!s.present() || s.typeParam || s.name == "any" || s.name == "unit" || s.name == "nothing") return type;

        uint8_t id = builtinTypeId(s.name);
        if (const std::string* kind = kindName(s)) {
            type.className = *kind;  // `is Click` for an Event (D39)
            type.flags = static_cast<uint8_t>(kTypeInstance | (s.nullable ? kTypeNullable : 0));
            return type;
        }
        // Kotlin's MutableList and the others: the checker says what to write (D32).
        if (s.name == "MutableList") id = kTypeList;
        else if (s.name == "MutableMap") id = kTypeMap;
        else if (s.name == "MutableSet") id = kTypeSet;
        if (id == 0) {
            id = kTypeInstance;
            type.className = s.name;
        }
        type.flags = static_cast<uint8_t>(id | (s.nullable ? kTypeNullable : 0));
        return type;
    }

    // ---- Types when the program runs (M6.6 part 1) ----

    // Where the value of a type parameter is when the program runs: one of the frame's type
    // arguments, or one of those of the object a method runs on.
    struct TypeParamPlace {
        bool ofObject = false;
        int index = -1;
        FunctionState* owner = nullptr;  // the generic function, for one of a frame's
    };

    // The type parameter `name`, as the code being compiled reaches it; false where it cannot:
    // a class's in a static member, a function's from inside a generic function of its own
    // (whose frame has its own type arguments), and a class's from a class inside it.
    bool findTypeParam(const std::string& name, TypeParamPlace& place) const {
        bool crossedGeneric = false;
        for (FunctionState* state = fs_; state != nullptr; state = state->enclosing) {
            auto found = std::find(state->typeParams.begin(), state->typeParams.end(), name);
            if (found != state->typeParams.end()) {
                if (crossedGeneric || found - state->typeParams.begin() > 0xFF) return false;
                place = TypeParamPlace{false, static_cast<int>(found - state->typeParams.begin()), state};
                return true;
            }
            if (!state->typeParams.empty()) crossedGeneric = true;
            if (state->kind == FunctionKind::StaticMethod) return false;
            if (kindHasThis(state->kind)) {
                if (class_ == nullptr) return false;
                auto own = std::find(class_->typeParams.begin(), class_->typeParams.end(), name);
                if (own == class_->typeParams.end() || own - class_->typeParams.begin() > 0xFF) return false;
                place = TypeParamPlace{true, static_cast<int>(own - class_->typeParams.begin()), nullptr};
                return true;
            }
        }
        return false;
    }

    // Pushes the value of a type parameter that findTypeParam found.
    void emitTypeParam(const TypeParamPlace& place, const Node& at) {
        if (place.ofObject) {
            emitGet("this", at);
            emitGet(class_->storage, at);
            emit(Op::InstanceTypeArg);
        } else {
            // The lambdas between keep the generic function's, each in its closure.
            for (FunctionState* state = fs_; state != place.owner && state != nullptr; state = state->enclosing)
                state->proto->typeArgs |= kTypeArgsKept;
            emit(Op::TypeArg);
        }
        emitByte(place.index);
    }

    // What a MakeType takes from the stack: the global of a class, or a type parameter.
    struct TypeOperand {
        int global = -1;
        TypeParamPlace param;
    };

    // A type the checker worked out, as the shape of a MakeType and what it takes from the
    // stack, in order; false where a part of it cannot be made when the program runs. A type
    // parameter out of reach is made `any`, which takes every value.
    bool shapeOf(const RuntimeType& t, std::string& shape, std::vector<TypeOperand>& operands) {
        if (t.nullable) shape += '?';
        switch (t.kind) {
            case RuntimeType::Any: shape += 'a'; return true;
            case RuntimeType::Int:
            case RuntimeType::Float:
                if (t.storage != 0) {
                    shape += t.storage == kTypeInt16 ? 'h' : t.storage == kTypeInt32 ? 'd' : t.storage == kTypeUint16 ? 'H' : t.storage == kTypeUint32 ? 'D' : 'g';
                    return true;
                }
                shape += t.kind == RuntimeType::Int ? 'i' : 'f';
                return true;
            case RuntimeType::Char: shape += 'c'; return true;
            case RuntimeType::Byte: shape += 'u'; return true;
            case RuntimeType::Index: shape += 'x'; return true;
            case RuntimeType::Number: shape += 'n'; return true;
            case RuntimeType::String: shape += 's'; return true;
            case RuntimeType::Bool: shape += 'b'; return true;
            case RuntimeType::Bytes: shape += 'y'; return true;
            case RuntimeType::Range: shape += 'r'; return true;
            case RuntimeType::TypeValue: shape += 't'; return true;
            case RuntimeType::Function: shape += 'F'; return true;
            case RuntimeType::List:
                if (t.frozen) shape += 'Z';
                shape += 'L';
                return t.args.size() == 1 && shapeOf(t.args[0], shape, operands);
            case RuntimeType::Set:
                if (t.frozen) shape += 'Z';
                shape += 'S';
                return t.args.size() == 1 && shapeOf(t.args[0], shape, operands);
            case RuntimeType::Map:
                if (t.frozen) shape += 'Z';
                shape += 'M';
                return t.args.size() == 2 && shapeOf(t.args[0], shape, operands) && shapeOf(t.args[1], shape, operands);
            case RuntimeType::Class: {
                const GlobalName* global = nullptr;
                if (!t.privateFile.empty()) {
                    auto& file = session_.privates_[t.privateFile];
                    if (auto found = file.find(t.global); found != file.end()) global = &found->second;
                } else if (t.ns != nullptr) {
                    if (auto found = t.ns->names.find(t.global); found != t.ns->names.end()) global = &found->second;
                }
                if (global == nullptr || global->slot < 0 || t.args.size() > 9) return false;
                operands.push_back(TypeOperand{global->slot, {}});
                shape += 'C';
                shape += static_cast<char>('0' + t.args.size());
                for (const RuntimeType& arg : t.args)
                    if (!shapeOf(arg, shape, operands)) return false;
                return true;
            }
            case RuntimeType::TypeParam: {
                TypeParamPlace place;
                if (!findTypeParam(t.typeParam, place)) {
                    shape += 'a';
                    return true;
                }
                operands.push_back(TypeOperand{-1, place});
                shape += 'P';
                return true;
            }
            default:
                return false;
        }
    }

    // Pushes the type of `shape` and `operands`. One made of nothing that changes is made once,
    // where it is written, and kept there; one with a type parameter is made each time.
    bool emitShape(const std::string& shape, const std::vector<TypeOperand>& operands, const Node& at) {
        bool fixed = true;
        for (const TypeOperand& operand : operands) fixed = fixed && operand.global >= 0;
        if (fixed && fs_->caches > 0xFFFF) return false;
        int cache = -1, skip = -1;
        if (fixed) {
            cache = fs_->caches++;
            emit(Op::CachedType, cache);
            skip = static_cast<int>(code().size());
            emitI32(0);
        }
        for (const TypeOperand& operand : operands) {
            if (operand.global >= 0) emit(Op::GetGlobal, operand.global);
            else emitTypeParam(operand.param, at);
        }
        emit(Op::MakeType, stringConstant(shape));
        if (fixed) {
            emit(Op::CacheType, cache);
            patchJump(skip);
        }
        return true;
    }

    // Gives the list or map on the stack the type the checker worked out for it, where it has
    // none yet. A type made of nothing that changes is made the first time and kept, and after
    // that the one instruction gives it (TagCollection); false where it cannot be made.
    bool emitTag(const RuntimeType& t, const Node& at) {
        std::string shape;
        std::vector<TypeOperand> operands;
        if (!shapeOf(t, shape, operands)) return false;
        bool fixed = true;
        for (const TypeOperand& operand : operands) fixed = fixed && operand.global >= 0;
        if (!fixed) {
            if (!emitShape(shape, operands, at)) return false;
            emit(Op::TypeCollection);
            return true;
        }
        if (fs_->caches > 0xFFFF) return false;
        const int cache = fs_->caches++;
        emit(Op::TagCollection, cache);
        const int skip = static_cast<int>(code().size());
        emitI32(0);
        for (const TypeOperand& operand : operands) emit(Op::GetGlobal, operand.global);
        emit(Op::MakeType, stringConstant(shape));
        emit(Op::CacheType, cache);
        emit(Op::TypeCollection);
        patchJump(skip);
        return true;
    }

    // Pushes a type the checker worked out; false, pushing nothing, where it cannot be made.
    bool emitRuntimeType(const RuntimeType& t, const Node& at) {
        std::string shape;
        std::vector<TypeOperand> operands;
        if (!shapeOf(t, shape, operands)) return false;
        return emitShape(shape, operands, at);
    }

    // A call of a generic function or class written in Astra: its type arguments, as the
    // checker worked them out, given to it by WithTypes right before the call. Each emitter of
    // a call instruction calls this after the arguments.
    // The call a function returns the result of, which it passes where its own result goes; and
    // the call whose result goes to the place just under it (`pts[i] = f(...)`).
    const CallExpr* tailCall_ = nullptr;
    const CallExpr* destinationCall_ = nullptr;

    // Right before a call: where its result goes, if anywhere (M6.6 part 4), and its type
    // arguments. `above` is how many slots the call takes, the callee or object and the arguments;
    // -1 where that is not so simply told.
    void emitCallPrelude(const CallExpr& call, int above) {
        if (&call == tailCall_) {
            emit(Op::WithFrameDestination);
        } else if (&call == destinationCall_ && above >= 0 && above <= 0xFF) {
            emit(Op::WithDestinationAt);
            emitByte(above);
        } else if (session_.checker != nullptr && !copyReadsHere() && !session_.checker->constructsValue(call) && session_.checker->readsFieldInPlace(call)) {
            emit(Op::WithRefResult);  // what it gives back may stay where it is (M6.6 part 5)
        }
        emitTypeArguments(call);
    }

    void emitTypeArguments(const CallExpr& call) {
        std::vector<RuntimeType> args;
        if (session_.checker == nullptr || !session_.checker->typeArguments(call, args) || args.empty() || args.size() > 9) return;
        std::string shape = "A";
        shape += static_cast<char>('0' + args.size());
        std::vector<TypeOperand> operands;
        for (const RuntimeType& arg : args) {
            std::string part;
            std::vector<TypeOperand> partOperands;
            if (arg.kind == RuntimeType::Unknown || !shapeOf(arg, part, partOperands)) {
                shape += 'a';  // not worked out: any value fits it
                continue;
            }
            shape += part;
            operands.insert(operands.end(), partOperands.begin(), partOperands.end());
        }
        if (emitShape(shape, operands, call)) emit(Op::WithTypes);
    }

    // After a call of a function of the C++'s (`external`), on the value it gave: a list or
    // map is given the type the checker worked out where the C++ gave it none (`args()` a
    // List<string>), and with ASTRA_CHECK_NATIVE_RESULTS the value is checked against the
    // declaration, so that the suite proves the declarations say what the C++ does.
    void emitExternalResult(const CallExpr& call) {
        RuntimeType result;
        std::string name;
        if (session_.checker == nullptr || !session_.checker->externalResult(call, result, name)) return;
        if (checkingNativeResults() && emitRuntimeType(result, call)) emit(Op::CheckResult, stringConstant(name));
        if (result.kind != RuntimeType::List && result.kind != RuntimeType::Map && result.kind != RuntimeType::Set) return;
        result.nullable = false;  // a list it gave, or null, which is left as it is
        result.frozen = false;    // its items' type, whether frozen or not
        emitTag(result, call);
    }

    // A type as written (`List<Point?>`, `T`), with its type arguments, pushed for IsFull or
    // CastFull: the classes it names found as any name of a type is.
    void emitWrittenType(const TypeAnnotation& written_, std::string& shape) {
        const TypeAnnotation& t = written(written_);
        if (t.name == "|") {
            shape += 'a';  // a union (D40): any, when the program runs
            return;
        }
        if (t.nullable) shape += '?';
        if (t.typeParam) {
            TypeParamPlace place;
            if (findTypeParam(t.name, place)) {
                emitTypeParam(place, t);
                shape += 'P';
            } else {
                shape += 'a';
            }
            return;
        }
        if (t.star || !t.present() || t.name == "any") {
            shape += 'a';
            return;
        }
        if (!t.result.empty() || t.name == "Function") {
            shape += 'F';
            return;
        }
        uint8_t id = builtinTypeId(t.name);
        if (unfrozenTypeId(id) != id) {
            shape += 'Z';  // FrozenList<T>: as List<T>, frozen (D26)
            id = unfrozenTypeId(id);
        }
        if (id == kTypeList && t.args.size() == 1) {
            shape += 'L';
            emitWrittenType(t.args[0], shape);
            return;
        }
        if (id == kTypeSet && t.args.size() == 1) {
            shape += 'S';
            emitWrittenType(t.args[0], shape);
            return;
        }
        if (id == kTypeMap && t.args.size() == 2) {
            shape += 'M';
            emitWrittenType(t.args[0], shape);
            emitWrittenType(t.args[1], shape);
            return;
        }
        // The others, and a list or map with nothing said of what it holds: of anything.
        static const char* const kShapes[] = {"", "i", "f", "n", "s", "b", "La", "Maa", "r", "F", "y", "t", "",
                                              "c", "u", "h", "d", "H", "D", "g", "x", "Sa", "ZLa", "ZMaa", "ZSa"};
        static_assert(sizeof kShapes / sizeof kShapes[0] == kTypeCount_, "a shape for each type");
        if (isBuiltinTypeId(id)) {
            shape += kShapes[id];
            return;
        }
        emitTypeName(t.name, t);
        shape += 'C';
        shape += static_cast<char>('0' + std::min<size_t>(t.args.size(), 9));
        for (size_t i = 0; i < t.args.size() && i < 9; i++) emitWrittenType(t.args[i], shape);
    }

    // Whether a type as written says more than its class or kind: type arguments to check, or
    // a type parameter, whose value the code is given when it runs.
    bool hasTypeArguments(const TypeAnnotation& written_) const {
        const TypeAnnotation& t = written(written_);
        if (t.name == "|") return false;
        TypeParamPlace place;
        if (t.typeParam) return findTypeParam(t.name, place);
        return !t.args.empty() && t.result.empty();
    }

    // An instruction whose operand is a type: a class is passed on the stack, above the value.
    void emitTypeOperand(Op op, const TypeSpec& type, const TypeAnnotation& at) {
        if (type.id() == kTypeInstance) emitTypeName(type.className, at);
        emit(op);
        emitByte(type.flags);
    }

    // Rejects an initializer that can never fit the declared type. Returns whether the value
    // still has to be checked when the program runs.
    // What an expression's value is when the program runs, an int constant the checker made a
    // byte included (D19).
    uint8_t staticTypeHere(const Expr& e) const {
        uint8_t b;
        if (session_.checker != nullptr && session_.checker->byteAt(e, b)) return kTypeByte;
        if (e.kind == ExprKind::Function && session_.checker != nullptr) {
            RuntimeType iface;
            std::string method;
            int arity = 0;
            if (session_.checker->samConversion(e, iface, method, arity)) return 0;  // an object of the interface (D28)
        }
        return staticTypeOf(e);
    }

    // Whether a value of static type `actual` can go where `declared` is asked for, before what
    // is checked when the program runs: an int goes into a storage width, whose range is
    // checked then, and a float into a float32 (D148).
    static bool staticFits(uint8_t declared, uint8_t actual) {
        return declared == actual || (declared == kTypeNumber && (actual == kTypeInt || actual == kTypeFloat)) ||
               (isIntStorage(declared) && actual == kTypeInt) || (declared == kTypeFloat32 && actual == kTypeFloat) ||
               (declared == kTypeByte && actual == kTypeInt) ||  // the checker says why an int is not a byte there
               unfrozenTypeId(declared) == actual;                  // and why a list is not a FrozenList (D26)
    }

    bool checkInitializer(const VarStmt& s, const TypeSpec& type) {
        uint8_t actual = s.init ? staticTypeHere(*s.init) : kStaticNull;
        if (actual == 0) return !provenFits(provenType(*s.init), type);

        if (actual == kStaticNull) {
            if (type.nullable()) return false;
            if (!s.init)
                throw error(s, "variable '" + s.name + "' of type " + type.text() + " must be initialized (declare it as " +
                                   type.text() + "? to allow null)");
            throw error(*s.init, "cannot initialize '" + s.name + "' of type " + type.text() + " with null (declare it as " +
                                     type.text() + "? to allow null)");
        }

        if (!staticFits(type.id(), actual))
            throw error(*s.init, "cannot initialize '" + s.name + "' of type " + type.text() + " with a value of type " +
                                     typeText(actual, ""));
        return isIntStorage(type.id());  // the int's range, when it runs
    }

    // What the value of `expr` is certain to be when the program runs, as far as can be told from
    // what is checked while it runs (M6.5 part 4): a literal; a local or parameter with a type
    // written, each value it is given being checked; arithmetic, comparisons and `!` of such;
    // a choice between two of one type; and a call of a function of the program declared once with
    // a return type of those, whose returns are all checked or proved in turn (the name of a
    // function is never given another value). 0 where nothing is certain. Used only to leave
    // out a check, never to report an error (staticTypeOf and the checker do that).
    uint8_t provenType(const Expr& expr) const {
        switch (expr.kind) {
            case ExprKind::Call: {
                const auto& call = static_cast<const CallExpr&>(expr);
                if (call.callee->kind != ExprKind::Name || mayBeMemberHere()) return 0;
                const std::string& name = static_cast<const NameExpr&>(*call.callee).name;
                // Not a variable of this function or of one around it, nor a member of a class.
                for (const FunctionState* state = fs_; state != nullptr; state = state->enclosing)
                    for (const Local& local : state->locals)
                        if (local.name == name) return 0;
                const ClassSide side = classSide();
                if (side != ClassSide::None && (class_->statics.count(name) != 0 || class_->members.count(name) != 0)) return 0;
                if (outerStaticOwner(name) != nullptr) return 0;
                const GlobalName* global = findGlobal(name, *call.callee, true);
                return global != nullptr && global->isFunction && global->forms.empty() ? global->returnType : 0;
            }
            case ExprKind::Literal: {
                const uint8_t literal = staticTypeHere(expr);
                return literal == kStaticNull ? 0 : literal;
            }
            case ExprKind::Interp: return kTypeString;
            case ExprKind::Name: {
                const std::string& name = static_cast<const NameExpr&>(expr).name;
                for (size_t i = fs_->locals.size(); i-- > 0;) {
                    const Local& local = fs_->locals[i];
                    if (local.name != name) continue;
                    const TypeSpec& type = local.info.type;
                    if (local.depth < 0 || !type.typed() || type.nullable()) return 0;
                    const uint8_t id = type.id();
                    return id == kTypeInt || id == kTypeFloat || id == kTypeBool || id == kTypeString ? id : 0;
                }
                return 0;  // a variable of a function around this one, or a global: not looked into
            }
            case ExprKind::Unary: {
                const auto& unary = static_cast<const UnaryExpr&>(expr);
                const uint8_t operand = provenType(*unary.operand);
                if (unary.op == TokenType::Bang) return operand == kTypeBool ? kTypeBool : 0;
                if (unary.op == TokenType::Tilde) return operand == kTypeInt ? kTypeInt : 0;
                if (unary.op == TokenType::Minus) return operand == kTypeInt || operand == kTypeFloat ? operand : 0;
                return 0;
            }
            case ExprKind::Binary: {
                const auto& binary = static_cast<const BinaryExpr&>(expr);
                const uint8_t left = provenType(*binary.left);
                const uint8_t right = provenType(*binary.right);
                if (left == 0 || left != right) return 0;
                const bool number = left == kTypeInt || left == kTypeFloat;
                switch (binary.op) {
                    case TokenType::Plus:
                        return number || left == kTypeString ? left : 0;
                    case TokenType::Minus:
                    case TokenType::Star:
                    case TokenType::Slash:
                    case TokenType::Percent:
                        return number ? left : 0;
                    case TokenType::Lt:
                    case TokenType::LtEq:
                    case TokenType::Gt:
                    case TokenType::GtEq:
                        return number ? kTypeBool : 0;
                    case TokenType::EqEq:
                    case TokenType::BangEq:
                        return kTypeBool;  // two ints, floats, bools or strings
                    default:
                        return 0;
                }
            }
            case ExprKind::Logical: {
                const auto& logical = static_cast<const LogicalExpr&>(expr);
                return provenType(*logical.left) == kTypeBool && provenType(*logical.right) == kTypeBool ? kTypeBool : 0;
            }
            case ExprKind::Ternary: {
                const auto& ternary = static_cast<const TernaryExpr&>(expr);
                const uint8_t then = provenType(*ternary.then);
                return then != 0 && then == provenType(*ternary.otherwise) ? then : 0;
            }
            default:
                return 0;
        }
    }

    // Whether a value proved to be `actual` (provenType) fits `type`.
    static bool provenFits(uint8_t actual, const TypeSpec& type) {
        if (actual == 0 || !type.typed()) return false;
        return type.id() == actual || (type.id() == kTypeNumber && (actual == kTypeInt || actual == kTypeFloat));
    }

    // Emits a check that the value a typed function is about to return fits its return type.
    // `actual` is what is statically known about that value (see staticTypeOf), and `value` the
    // expression, where there is one.
    void emitReturnCheck(uint8_t actual, const Node& at, const Expr* value = nullptr) {
        const TypeSpec& type = fs_->returnType;
        if (!type.typed()) return;
        if (actual == 0 && value != nullptr && provenFits(provenType(*value), type)) return;  // certain already

        if (actual == kStaticNull) {
            if (type.nullable()) return;
            throw error(at, fs_->proto->name + "() is declared to return " + type.text() + " and cannot return null");
        }
        if (actual != 0) {
            if (!staticFits(type.id(), actual))
                throw error(at, fs_->proto->name + "() is declared to return " + type.text() +
                                    " and cannot return a value of type " + typeText(actual, ""));
            if (!isIntStorage(type.id())) return;  // an int's range is checked as it is returned
        }

        if (type.id() == kTypeInstance) emitTypeName(type.className, at);
        emit(Op::CheckReturn);
        emitByte(type.flags);
    }

    // Rejects an initial or default value that can never fit the declared type.
    void checkStaticFit(const TypeSpec& type, const Expr& value, const std::string& what) {
        uint8_t actual = staticTypeHere(value);
        if (actual == 0) return;
        bool fits = actual == kStaticNull ? type.nullable() : staticFits(type.id(), actual);
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
                if (s.isExternal) {
                    checkExternalProperty(s);
                    break;
                }
                if (!s.pattern.empty()) {
                    compileDestructuring(s);
                    break;
                }
                TypeSpec type = resolveType(s.type);
                bool checkAtRuntime = type.typed() && checkInitializer(s, type);

                if (isGlobalScope() && s.isLazy) {
                    // Worked out when first read (D37): its type is the checker's to see to.
                    int global = declareGlobal(s.name, s.isVal, s);
                    session_.globalClassTypes_.erase(global);
                    compileLazy(*s.init, s.name);
                    line_ = s.line;
                    emit(Op::DefineGlobal, global);
                    break;
                }
                if (s.isLazy) throw error(s, "lazy is for a field, a static or a top-level value; a local is worked out where it is");
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
                    fs_->barrier = static_cast<int>(codeMark);  // compiled again from here, it may be
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
                loop.owner = fs_->owner;
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
                if (s.label.empty() && frame_ != nullptr && fs_->owner == frame_->owner) {
                    compileInlineReturn(s, inExpression);  // leaves the inline function's body (D59)
                    break;
                }
                if (fs_->isLambda && !s.implicit && s.label.empty())
                    throw error(s, "'return' leaves the function it is written in, which a lambda cannot do unless it is copied into an " +
                                       std::string("inline function, as forEach and let are (D59); to leave the lambda itself write return@") +
                                       (!fs_->calleeLabel.empty() ? fs_->calleeLabel : !fs_->label.empty() ? fs_->label : "name"));
                if (fs_->kind == FunctionKind::Initializer) {
                    if (s.value) throw error(s, "cannot return a value from an initializer");
                    emit(Op::GetLocal, 0);
                } else if (fs_->kind == FunctionKind::Setter) {
                    if (s.value) throw error(s, "cannot return a value from a setter");
                    emit(Op::GetLocal, 1);
                } else if (s.value && !inExpression && tailReturnHere() && hasSelfCall(*s.value)) {
                    compileTailReturn(*s.value, s);
                    break;
                } else if (s.value) {
                    // A function given where its result goes passes that on to a call it returns
                    // the result of (M6.6 part 4).
                    const CallExpr* savedTail = tailCall_;
                    if ((fs_->proto->typeArgs & kTakesDestination) != 0 && s.value->kind == ExprKind::Call)
                        tailCall_ = static_cast<const CallExpr*>(s.value.get());
                    compileExprFor(*s.value, fs_->returnType);
                    tailCall_ = savedTail;
                    line_ = s.line;
                    emitReturnCheck(staticTypeHere(*s.value), *s.value, s.value.get());
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
                if (static_cast<const FunctionStmt&>(stmt).isExternal) {
                    checkExternal(static_cast<const FunctionStmt&>(stmt));
                    break;
                }
                if (!static_cast<const FunctionStmt&>(stmt).receiver.empty()) {
                    compileExtension(static_cast<const FunctionStmt&>(stmt));
                    break;
                }
                if (isGlobalScope()) {
                    int global = declareGlobal(fn.name, false, stmt);
                    GlobalName& entry = globalEntry(fn.name);
                    compileFunction(fn, FunctionKind::Function, fn.name);
                    if (!entry.forms.empty()) {
                        // One of several forms: in its own slot, and the set is made once all are.
                        const int form = entry.formSlot(signatureText(fn));
                        if (form < 0) throw error(stmt, "'" + fn.name + "' was not noted as a form");
                        markForm(*fs_->proto->protos.back(), fn);
                        global = form;
                        bool pending = false;
                        for (const auto& [name, noted] : session_.pendingForms_) pending = pending || name == &entry;
                        if (!pending) session_.pendingForms_.emplace_back(&entry, fn.name);
                    }
                    emit(Op::DefineGlobal, global);
                } else {
                    // Initialized up front so the function can call itself.
                    addLocal(fn.name, false, stmt, true);
                    compileFunction(fn, FunctionKind::Function, fn.name);
                }
                break;
            }

            case StmtKind::Class: {
                const auto& cls = static_cast<const ClassStmt&>(stmt);
                compileClass(cls);
                emitClassMetas(cls, cls.name, -1);
                break;
            }

            case StmtKind::TypeAlias:
                break;  // another way of writing a type (D41), for the checker

            case StmtKind::Extend:
                compileExtend(static_cast<const ExtendStmt&>(stmt));  // where an extend is not at the top
                break;
        }
    }

    // ---- Tail calls (D29) ----
    //
    // A function calling itself as its last step, on the same object, starts again in the same
    // frame: `return walk(next)`, or such a call as a branch of `? :` or after `?:` in what it
    // returns. Not inside a try, whose handlers belong to the frame's round, nor for a generic
    // function, whose type arguments the call may change, nor with arguments by name or spread.
    bool tailReturnHere() const {
        return session_.checker != nullptr && fs_->decl != nullptr && !fs_->isLambda && fs_->tryDepth == 0 && fs_->finally == nullptr &&
               (fs_->kind == FunctionKind::Function || fs_->kind == FunctionKind::Method) && fs_->decl->typeParams.empty() &&
               fs_->decl->varargIndex < 0 && fs_->typeParams.empty();
    }
    bool selfCallFits(const Expr& e) const {
        if (e.kind != ExprKind::Call) return false;
        const auto& call = static_cast<const CallExpr&>(e);
        return session_.checker->callsItself(call) && call.namedCount == 0 && !call.hasSpread &&
               call.args.size() <= fs_->decl->params.size();
    }
    bool hasSelfCall(const Expr& e) const {
        if (selfCallFits(e)) return true;
        if (e.kind == ExprKind::Ternary) {
            const auto& t = static_cast<const TernaryExpr&>(e);
            return hasSelfCall(*t.then) || hasSelfCall(*t.otherwise);
        }
        if (e.kind == ExprKind::Elvis) return hasSelfCall(*static_cast<const ElvisExpr&>(e).right);
        return false;
    }
    // In a `tail fun`, a call of itself that is not made a loop is an error saying why (D29).
    void refuseTailCall(const CallExpr& call) {
        bool overridden = false;
        if (!session_.checker->selfCall(call, overridden)) return;
        const std::string name = fs_->decl->name;
        std::string why = "it is not the last step, what it gives being used afterwards; a parameter that gathers the result can do that instead";
        if (overridden) why = "a subclass overrides " + name + ", so the call may run another function";
        else if (fs_->tryDepth > 0 || fs_->finally != nullptr) why = "it is inside a try, whose handlers belong to this round";
        else if (!fs_->decl->typeParams.empty() || !fs_->typeParams.empty()) why = "a generic function may be given other type arguments by it";
        else if (fs_->decl->varargIndex >= 0) why = "it takes a vararg";
        else if (call.namedCount != 0 || call.hasSpread) why = "its arguments are given by name or spread";
        throw error(call, "this call cannot be a loop, as tail fun " + name + " asks: " + why + " (D29)");
    }

    void compileTailReturn(const Expr& value, const ReturnStmt& s) {
        if (selfCallFits(value)) {
            const auto& call = static_cast<const CallExpr&>(value);
            for (size_t i = 0; i < call.args.size(); i++) compileExprFor(*call.args[i], fs_->locals[i + 1].info.type);
            for (size_t i = call.args.size(); i < fs_->decl->params.size(); i++) emit(Op::Unset);  // its default, worked out again
            line_ = call.line;
            emit(Op::TailLoop);
            emitByte(static_cast<int>(fs_->decl->params.size()));
            return;
        }
        if (value.kind == ExprKind::Ternary) {
            const auto& t = static_cast<const TernaryExpr&>(value);
            compileExpr(*t.condition);
            const int toElse = emitJump(Op::JumpIfFalse);
            compileTailReturn(*t.then, s);
            patchJump(toElse);
            compileTailReturn(*t.otherwise, s);
            return;
        }
        if (value.kind == ExprKind::Elvis && hasSelfCall(value)) {
            const auto& e = static_cast<const ElvisExpr&>(value);
            compileExpr(*e.left);
            const int toRight = emitJump(Op::JumpIfNotNull);
            const int skip = emitJump(Op::Jump);
            patchJump(toRight);
            line_ = s.line;
            emitReturnCheck(staticTypeHere(*e.left), *e.left, e.left.get());
            emit(Op::Return);
            patchJump(skip);
            emit(Op::Pop);
            compileTailReturn(*e.right, s);
            return;
        }
        compileExprFor(value, fs_->returnType);
        line_ = s.line;
        emitReturnCheck(staticTypeHere(value), value, &value);
        emit(Op::Return);
    }

    // `return@name value`. Inside a block compiled in place it ends the block with that value,
    // which this handles, returning true. Naming the lambda being compiled it is an ordinary
    // return from it, left to the caller (false). Anything else is an error.
    bool compileLabelledReturn(const ReturnStmt& s, bool inExpression) {
        for (InlineBlock* block = inline_; block != nullptr && block->function == fs_; block = block->enclosing) {
            if (block->owner != fs_->owner || (block->name != s.label && block->label != s.label)) continue;
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
        // `fun <T> T.describe()`: a type parameter as the receiver is for every value, as `any` is.
        if (s.receiver != "any" && !s.receiverType.typeParam) {
            type = unfrozenTypeId(builtinTypeId(s.receiver));  // a FrozenList's are a List's
            if (type == 0) {
                type = kTypeInstance;
                emitTypeName(s.receiver, s);
            }
        }
        nextIsExtension_ = true;
        if (type == kTypeInstance) nextReceiverType_ = s.receiver;
        compileFunction(fn, FunctionKind::Method, s.receiver + "." + fn.name);
        // Extensions of a name for the same type are its forms (D56): the receiver's type as
        // written is part of what tells them apart, with the parameters'.
        FunctionProto& proto = *fs_->proto->protos.back();
        markForm(proto, fn);
        proto.signature = "[" + annotationText(s.receiverType) + "]" + proto.signature;
        emit(Op::Extension);
        emitU16(stringConstant(fn.name));
        emitByte(type | (s.receiverNullable ? kTypeNullable : 0));
    }

    // ---- External declarations ----

    // The built-in type an external method or property is declared on.
    bool builtinTypeOf(const std::string& name, BuiltinType& type) const {
        if (name == "string") type = BuiltinType::String;
        else if (name == "List") type = BuiltinType::List;
        else if (name == "Map") type = BuiltinType::Map;
        else if (name == "Range") type = BuiltinType::Range;
        else if (name == "Bytes") type = BuiltinType::Bytes;
        else if (name == "Number" || name == "int" || name == "float") type = BuiltinType::Number;
        else if (name == "char") type = BuiltinType::Char;
        else if (name == "byte") type = BuiltinType::Byte;
        else if (name == "Index") type = BuiltinType::Index;
        else if (name == "Set") type = BuiltinType::Set;
        else return false;
        return true;
    }

    // `external fun`: compiles to nothing, but must name what the C++ provides, taking as many
    // arguments as it says, so a declaration cannot drift from the code it describes.
    void checkExternal(const FunctionStmt& s) {
        const FunctionDecl& fn = *s.fn;
        if (ns_ != session_.builtins_) throw error(s, "'external' declarations are the standard library's: they describe what the C++ provides");
        int min = 0, max = 0;
        bool found;
        std::string what;
        if (s.receiver.empty()) {
            found = session_.vm_.builtinArity(fn.name, min, max);
            what = "built-in function '" + fn.name + "'";
        } else {
            BuiltinType type;
            if (!builtinTypeOf(s.receiver, type)) throw error(s, "'" + s.receiver + "' is not a built-in type");
            found = session_.vm_.methodArity(type, fn.name, min, max);
            what = "method '" + fn.name + "' on " + s.receiver;
        }
        if (!found) throw error(s, "there is no " + what + " in the C++ for this declaration");
        int required = 0;
        for (size_t i = 0; i < fn.params.size(); i++)
            if (fn.paramDefaults[i] == nullptr && static_cast<int>(i) != fn.varargIndex) required++;
        const int most = fn.varargIndex >= 0 ? -1 : static_cast<int>(fn.params.size());
        auto counts = [](int low, int high) {
            return std::to_string(low) + " to " + (high < 0 ? std::string("any number of") : std::to_string(high));
        };
        // A name with several forms (`add(item)`, `add(index, item)`) has each within what the
        // C++ takes, and all of them together cover it, which the session checks at the end.
        if (required < min || (max >= 0 && (most < 0 || most > max)))
            throw error(s, "the " + what + " takes " + counts(min, max) + " arguments in the C++; this declaration says " +
                               counts(required, most));
        // Its parameters' names, for calls that name them (D116): handed to the VM as the
        // standard library loads, so a built program has them too.
        if (!fn.params.empty() && fn.varargIndex < 0) {
            line_ = s.line;
            emit(Op::GetGlobal, resolveGlobal("__nameParams", s)->slot);
            emit(Op::Const, stringConstant(s.receiver));
            emit(Op::Const, stringConstant(fn.name));
            for (const std::string& p : fn.params) emit(Op::Const, stringConstant(p));
            emit(Op::BuildList, static_cast<int>(fn.params.size()));
            emit(Op::Call);
            emitByte(3);
            emit(Op::Pop);
        }
        auto& cover = session_.externalForms_[what];
        if (cover.file.empty()) {
            cover = {min, max, required, most, file_, s.line, s.col};
        } else {
            cover.lowest = std::min(cover.lowest, required);
            cover.highest = cover.highest < 0 || most < 0 ? -1 : std::max(cover.highest, most);
        }
    }

    // `external object Math { ... }`: each function is one the C++ has, taking what its
    // declaration says, and is given its parameters' names (D116).
    void checkExternalObject(const ClassStmt& s) {
        if (ns_ != session_.builtins_) throw error(s, "'external' declarations are the standard library's: they describe what the C++ provides");
        for (const auto& method : s.methods) {
            const FunctionDecl& fn = *method;
            const std::string what = "function '" + fn.name + "' of " + s.name;
            ObjNative* native = session_.vm_.builtinStatic(s.name, fn.name);
            if (native == nullptr) throw error(fn, "there is no " + what + " in the C++ for this declaration");
            int required = 0;
            for (size_t i = 0; i < fn.params.size(); i++)
                if (fn.paramDefaults[i] == nullptr && static_cast<int>(i) != fn.varargIndex) required++;
            const int most = fn.varargIndex >= 0 ? -1 : static_cast<int>(fn.params.size());
            if (required < native->minArgs || (native->maxArgs >= 0 && (most < 0 || most > native->maxArgs)))
                throw error(fn, "the " + what + " takes " + std::to_string(native->minArgs) + " to " +
                                    (native->maxArgs < 0 ? std::string("any number of") : std::to_string(native->maxArgs)) +
                                    " arguments in the C++; this declaration says " + std::to_string(required) + " to " +
                                    (most < 0 ? std::string("any number of") : std::to_string(most)));
            if (fn.params.empty() || fn.varargIndex >= 0) continue;
            line_ = fn.line;
            emit(Op::GetGlobal, resolveGlobal("__nameParams", fn)->slot);
            emit(Op::Const, stringConstant(s.name));
            emit(Op::Const, stringConstant(fn.name));
            for (const std::string& p : fn.params) emit(Op::Const, stringConstant(p));
            emit(Op::BuildList, static_cast<int>(fn.params.size()));
            emit(Op::Call);
            emitByte(3);
            emit(Op::Pop);
        }
    }

    void checkExternalProperty(const VarStmt& s) {
        if (ns_ != session_.builtins_) throw error(s, "'external' declarations are the standard library's: they describe what the C++ provides");
        BuiltinType type;
        if (!builtinTypeOf(s.receiverType.name, type)) throw error(s, "'" + s.receiverType.name + "' is not a built-in type");
    }

    // The loop a break or continue is for: the innermost, or with `@name` the one of that name.
    LoopState* loopFor(const std::string& label, const Stmt& at, const char* keyword) {
        LoopState* own = fs_->loop;
        while (own != nullptr && own->owner != fs_->owner) own = own->enclosing;
        if (own == nullptr) throw error(at, std::string("'") + keyword + "' outside of a loop");
        if (label.empty()) return own;
        for (LoopState* loop = own; loop != nullptr; loop = loop->enclosing)
            if (loop->label == label && loop->owner == fs_->owner) return loop;
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

    // `a..b`, `a..<b`, `a downTo b`, and either `step n` (D72): what a range written in place is
    // made of, which a Range instruction makes, or a loop counts through, with nothing called.
    struct RangeParts {
        const Expr* start = nullptr;
        const Expr* end = nullptr;
        const Expr* step = nullptr;
        bool inclusive = false, descending = false;
    };
    bool rangeParts(const Expr& e, RangeParts& out) {
        if (e.kind == ExprKind::Range) {
            const auto& r = static_cast<const RangeExpr&>(e);
            if (!r.start || !r.end) return false;
            out = RangeParts{r.start.get(), r.end.get(), r.step.get(), r.inclusive, r.descending};
            return true;
        }
        if (e.kind != ExprKind::Call || session_.checker == nullptr) return false;
        const auto& c = static_cast<const CallExpr&>(e);
        std::string which;
        if (!c.isInfix || c.args.size() != 1 || !session_.checker->rangeWord(c, which)) return false;
        const auto& m = static_cast<const MemberExpr&>(*c.callee);
        if (which == "downTo") {
            out = RangeParts{m.object.get(), c.args[0].get(), nullptr, true, true};
            return true;
        }
        if (!rangeParts(*m.object, out) || out.step != nullptr) return false;
        out.step = c.args[0].get();
        return true;
    }
    bool compileRangeCall(const CallExpr& e) {
        RangeParts parts;
        if (!e.isInfix || !rangeParts(e, parts)) return false;
        compileExpr(*parts.start);
        compileExpr(*parts.end);
        if (parts.step) compileExpr(*parts.step);
        line_ = e.line;
        emit(Op::Range);
        emitByte((parts.inclusive ? 1 : 0) | (parts.descending ? 2 : 0) | (parts.step ? 4 : 0));
        return true;
    }

    // `for (x in channel)`: through an Iterator, its hasNext() and next() called as methods are,
    // so that a task may be set aside in them.
    void compileProtocolLoop(const ForInStmt& s, bool callIterator) {
        beginScope();
        compileExpr(*s.iterable);
        line_ = s.line;
        if (callIterator) {
            emit(Op::Invoke, stringConstant("iterator"));
            emitByte(0);
        }
        const int iteratorSlot = addLocal("(iterator)", false, s, true);
        LoopState loop{fs_->loop, here(), fs_->scopeDepth, fs_->tryDepth, fs_->locals.size(), fs_->slotTop, {}};
        loop.owner = fs_->owner;
        loop.label = s.label;
        fs_->loop = &loop;
        line_ = s.line;
        emit(Op::GetLocal, slotOf(iteratorSlot));
        emit(Op::Invoke, stringConstant("hasNext"));
        emitByte(0);
        const int toExit = emitJump(Op::JumpIfFalse);
        emit(Op::GetLocal, slotOf(iteratorSlot));
        emit(Op::Invoke, stringConstant("next"));
        emitByte(0);
        beginScope();
        if (s.pattern.empty()) {
            addLocal(s.variable, false, s, true);
        } else {
            const int item = addLocal("(item)", true, s, true);
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

    void compileForIn(const ForInStmt& s) {
        if (const int protocol = session_.checker != nullptr ? session_.checker->protocolLoop(s) : 0; protocol != 0) {
            compileProtocolLoop(s, protocol == 2);
            return;
        }
        beginScope();
        int iteratorSlot;
        RangeParts range;
        const bool counted = rangeParts(*s.iterable, range);
        if (counted) {
            // A range written in place is counted through, with nothing made for it.
            compileExpr(*range.start);
            compileExpr(*range.end);
            if (range.step) compileExpr(*range.step);
            line_ = s.iterable->line;
            emit(Op::RangeLoop);
            emitByte((range.inclusive ? 1 : 0) | (range.descending ? 2 : 0) | (range.step ? 4 : 0));
            iteratorSlot = addLocal("(next)", false, s, true);
            addLocal("(left)", false, s, true);
            addLocal("(step)", false, s, true);
        } else {
            compileExpr(*s.iterable);
            line_ = s.line;
            emit(Op::IterInit);
            iteratorSlot = addLocal("(iterator)", false, s, true);
        }

        LoopState loop{fs_->loop, here(), fs_->scopeDepth, fs_->tryDepth, fs_->locals.size(), fs_->slotTop, {}};
        loop.owner = fs_->owner;
        loop.label = s.label;
        fs_->loop = &loop;
        line_ = s.line;
        // A list of values may hand the loop its items where they are (M6.6 part 4).
        const bool inPlace = !counted && session_.checker != nullptr && !inPlaceOff() && session_.checker->loopsInPlace(s) &&
                             (!copyingReads() || session_.checker->loopPassesOn(s));
        // Two names taking the item apart, `for ((key, value) in map)`: both parts at once, with
        // nothing made for a map's entry (D64, D73).
        const bool pair = !counted && !inPlace && s.pattern.size() == 2;
        emit(counted ? Op::RangeNext : pair ? Op::IterNextPair : inPlace ? Op::IterNextRef : Op::IterNext, slotOf(iteratorSlot));
        int toExit = here();
        emitI32(0);

        // The loop variable lives in a scope of its own, so each iteration gets a fresh
        // variable and closures capture the value of their own iteration.
        beginScope();
        if (s.pattern.empty()) {
            addLocal(s.variable, false, s, true);
        } else if (pair) {
            addLocal(s.pattern[0] == "_" ? "(first)" : s.pattern[0], false, s, true);
            addLocal(s.pattern[1] == "_" ? "(second)" : s.pattern[1], false, s, true);
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
            // In the scope it is written in, which a return in a lambda copied in from elsewhere is not.
            const CodeScope saved = scopeNow();
            if (f->scope != nullptr) enterScope(*static_cast<const CodeScope*>(f->scope));
            compileStmt(*f->block);
            enterScope(saved);
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
        const CodeScope written = scopeNow();
        FinallyState state{fs_->finally, s.finalizer.get(), fs_->tryDepth, fs_->loop};
        state.scope = &written;
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
                    throw error(clause.type, "a catch names a class of Throwable, as in 'catch (e : IOException)' (D35)");
                emit(Op::GetLocal, slotOf(thrown));
                emitTypeName(type.className, clause.type);
                emit(Op::Is);
                toNext = emitJump(Op::JumpIfFalse);
            } else {
                // A bare catch is for every Exception, and not an Error, which is caught only
                // where it is named (D151).
                emit(Op::GetLocal, slotOf(thrown));
                emitTypeName("Exception", s);
                emit(Op::Is);
                toNext = emitJump(Op::JumpIfFalse);
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
        if (s.isExternal) {
            if (part != ClassPart::Values) checkExternalObject(s);
            return;
        }
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
        cls.isValue = s.isValue;
        for (const TypeParam& p : s.typeParams) cls.typeParams.push_back(p.name);
        cls.id = classId(s);
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
        if (objectBuild_ != nullptr && objectBuild_->classState == nullptr && fs_ == objectBuild_->boundary)
            objectBuild_->classState = &cls;  // the anonymous object's own (not one declared inside it)
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
        line_ = s.line;
        emitGet(storage, s);
        emit(Op::ClassNumber);
        emitU16(cls.id);
        emitU16(libraryId());
        emit(Op::Pop);
        if (s.isInterface || s.isAbstract) {
            emitGet(storage, s);
            emit(s.isInterface ? Op::MarkInterface : Op::MarkAbstract);
            emit(Op::Pop);
        }
        if (s.isValue) {
            // Made from its header alone: no fields of the body, no init of its own and no init
            // blocks, so that a value can be made by writing its arguments where it goes.
            bool ownInit = false;
            for (const auto& method : s.methods) ownInit = ownInit || (!method->isStatic && method->name == "init");
            const bool headerAlone = !ownInit && s.initBlocks.empty() && s.fields.size() == s.headerFieldCount;
            emitGet(storage, s);
            emit(Op::MarkValue);
            emitByte(headerAlone ? 1 : 0);
            emit(Op::Pop);
        }
        if (s.hasSuper) {
            for (const std::string& supertype : s.supertypes) emitTypeName(supertype, s);
            emitGet(storage, s);
            emit(Op::Inherit);
            emitByte(static_cast<int>(s.supertypes.size()));

            // A supertype written with type arguments: a function that makes it from the class's own.
            for (size_t i = 0; i < s.supertypeTypes.size(); i++)
                if (!s.supertypeTypes[i].args.empty()) compileSupertypeMaker(s, s.supertypeTypes[i], storage);

            // The superclass (null if all of them were interfaces) stays on the stack as a
            // hidden local that methods capture for `super`.
            beginScope();
            addLocal("super", false, s, true);
            if (objectBuild_ != nullptr && class_ == objectBuild_->classState && fs_ == objectBuild_->boundary)
                objectBuild_->superLocal = static_cast<int>(fs_->locals.size()) - 1;
        }

        emitGet(storage, s);
        if (s.isData) {
            for (const HeaderParam& param : s.header) emit(Op::DataField, stringConstant(param.name));
            if (s.header.empty()) emit(Op::DataField, stringConstant(""));  // an annotation with no values: Marked()
        }
        // Its objects can change in what == sees, so they are no keys (D26).
        if (session_.checker != nullptr && session_.checker->unstableKey(s)) emit(Op::UnstableKey);
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
            // What an abstract class leaves for its subclasses, and what is marked override. A data
            // class's first fields are its parent's, passed up (D65).
            const size_t inherited = session_.checker != nullptr ? session_.checker->inheritedDataFields(s) : 0;
            for (size_t i = 0; i < s.fields.size(); i++) {
                const auto& field = s.fields[i];
                line_ = field->line;
                if (i < inherited) continue;
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
            // A form of a name with several (D56) is known by its signature as well.
            const std::string runtimeName = formNameOf(s, *method);
            if (s.isInterface && !method->isStatic && method->name == "init")
                throw error(*method, "an interface cannot have an init: it is never created");
            if (!hasGeneratedInit(s) && !s.initBlocks.empty())
                throw error(s, "class '" + s.name + "' has init { } blocks and also defines fun init; use one or the other");
            const bool isAccessor = method->accessor != FunctionDecl::Accessor::None;
            if (!method->isStatic && !method->isAbstract && !method->generated && (isAccessor || method->name != "init")) {
                line_ = method->line;
                emitOverrideCheck(s, runtimeName, method->isOverride, isAccessor);
            }
            if (method->isAbstract) {
                // No body: the class that implements the interface, or extends the abstract
                // class, writes it.
                line_ = method->line;
                if (!s.isInterface && !s.isAbstract)
                    throw error(*method, "'" + method->name + "' has no body, which only a method of an interface or of an " +
                                             std::string("abstract class can be without"));
                if (method->isStatic) throw error(*method, "a static method needs a body");
                emit(Op::Require, stringConstant(runtimeName));
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

            if (!seen.insert((method->isStatic ? "static " : "") + runtimeName).second)
                throw error(*method, "'" + method->name + signatureText(*method) + "' is declared twice in class '" + s.name +
                                         "'; the forms of a name differ in their parameters' types");

            FunctionKind kind = method->isStatic ? FunctionKind::StaticMethod
                                : method->name == "init" ? FunctionKind::Initializer
                                : FunctionKind::Method;
            compileFunction(*method, kind, s.name + "." + method->name);
            if (runtimeName != method->name) markForm(*fs_->proto->protos.back(), *method);
            emit(method->isStatic ? Op::StaticMethod : Op::Method, stringConstant(runtimeName));
        }
        // A name with several forms: the class's set of them goes under the name, the forms it
        // declares and those it inherits.
        if (!s.isInterface) {
            std::unordered_set<std::string> made;
            for (const auto& method : s.methods) {
                if (method->isStatic || method->accessor != FunctionDecl::Accessor::None || method->name == "init" ||
                    !made.insert(method->name).second)
                    continue;
                const std::vector<const FunctionDecl*> roots = session_.checker->formRoots(s, method->name);
                if (roots.empty()) continue;
                line_ = method->line;
                for (const FunctionDecl* root : roots) emit(Op::Const, stringConstant(method->name + signatureText(*root)));
                FunctionProto* set = newProto();
                set->name = method->name;
                set->source = file_;
                set->chooser = true;
                const int index = static_cast<int>(fs_->proto->protos.size());
                fs_->proto->protos.push_back(set);
                emit(Op::MethodForms, index);
                emitByte(static_cast<int>(roots.size()));
            }
        }
        std::unordered_set<std::string> madeStatic;
        for (const auto& method : s.methods) {
            if (!method->isStatic || method->accessor != FunctionDecl::Accessor::None || staticForms(s, method->name) < 2 ||
                !madeStatic.insert(method->name).second)
                continue;
            line_ = method->line;
            int count = 0;
            for (const auto& form : s.methods) {
                if (!form->isStatic || form->accessor != FunctionDecl::Accessor::None || form->name != method->name) continue;
                emit(Op::Const, stringConstant(form->name + signatureText(*form)));
                count++;
            }
            FunctionProto* set = newProto();
            set->name = method->name;
            set->source = file_;
            set->chooser = true;
            const int index = static_cast<int>(fs_->proto->protos.size());
            fs_->proto->protos.push_back(set);
            emit(Op::StaticForms, index);
            emitByte(count);
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
        emitMemberAccess(s, cls);
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
        state.proto = newProto();
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
            const bool wasInClass = std::exchange(objectInClass_, true);  // made when first used (D66)
            compileClass(*inner, innerBound, &cls, ClassPart::Values);
            objectInClass_ = wasInClass;
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
            // A named object is made when it is first used, as a lazy value is (D66): at the top
            // level and inside a class. One inside a function is made where it is written.
            const bool lazy = isGlobalScope() || objectInClass_;
            auto make = [&] {
                emitGet(storage, s);
                emit(Op::Call);
                emitByte(0);
            };
            if (lazy) compileLazyWith(make, bound, s.line);
            else make();
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
            {"entries", true, "List", "the list of its entries", "override val entries : List get() => ..., in its static { } block"},
            {"name", false, "string", "each entry's name", "override val name : string get() => ..."},
            {"ordinal", false, "int", "each entry's place in the list", "override val ordinal : int get() => ..."},
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
                // An entry's name and ordinal are set for it: only a getter can stand in.
                shape(*field, !given.isStatic && field->getter && !field->setter && !field->init && typeFits(field->type));
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

            if (f.isLazy) {
                compileLazy(*f.init, s.name + "." + f.name);
                line_ = f.line;
                emit(Op::StaticField);
                emitU16(stringConstant(f.name));
                emitU16(f.isVal ? kFieldVal : 0);
                continue;
            }
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
        state.object = objectMember(state);
        state.proto = newProto();
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

        // A data class's first fields are set by its parent's init, which they are passed to (D65).
        size_t inherited = session_.checker != nullptr ? session_.checker->inheritedDataFields(s) : 0;
        for (size_t i = 0; i < s.header.size(); i++) {
            const HeaderParam& param = s.header[i];
            if (!param.isField) continue;
            if (inherited > 0) {
                inherited--;
                continue;
            }
            line_ = param.line;
            emit(Op::GetLocal, 0);
            emit(Op::GetLocal, static_cast<int>(i + 1));
            emit(Op::SetOwn, stringConstant(param.name));
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
            if (field->isLazy) compileLazy(*value, s.name + "." + field->name);
            else if (value != nullptr) compileExprFor(*value, resolveType(field->type));
            else emit(Op::Null);
            line_ = field->line;
            emit(Op::SetOwn, stringConstant(storedName(*field)));
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

        // A data class's first fields are its parent's (D65), which declares them.
        const size_t inherited = session_.checker != nullptr ? session_.checker->inheritedDataFields(s) : 0;
        for (size_t i = 0; i < s.fields.size(); i++) {
            const VarStmt& f = *s.fields[i];
            const Expr* initialValue = initialValues[i];
            if (f.isAbstract) continue;  // not a field of this class: a subclass has it
            if (i < inherited) continue;
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
            if (f.isLateinit && (type.id() == kTypeInt || type.id() == kTypeFloat || type.id() == kTypeBool || type.id() == kTypeNumber ||
                                 narrowOf(type.id()) != Narrow::None))
                throw error(f, "lateinit var '" + f.name + "' is " + type.text() + ", which always has a value (0, false): give it " +
                                   "a starting one; lateinit is for a field that holds an object (D36)");
            // A lateinit field is given its value after the object is made (D36).
            bool required = !f.isLateinit && !setByGeneratedInit && initialValue == nullptr && (f.isVal || (type.typed() && !type.nullable()));
            if (required && generatedInit)
                throw error(f, "field '" + f.name + "' has no initial value and is not in the class header, so nothing " +
                                   "would set it; give it a value, move it into the header, or write an init");
            if (required && !explicitInit)
                throw error(f, "field '" + f.name + "' has no initial value, so class '" + s.name +
                                   "' needs an init that sets it");
            if (!required && !setByGeneratedInit && !f.isLateinit) initialized.push_back(i);  // a lateinit one keeps none

            uint16_t flags = type.flags;
            if (f.isLazy) {
                flags = 0;  // the slot holds what works its value out until it is first read (D37)
            } else if (type.id() == kTypeInstance) {
                emitTypeThunk(type.className, f.type);
            } else if (!type.typed() && initialValue != nullptr && initialValue->kind == ExprKind::Call && session_.checker != nullptr &&
                       session_.checker->constructsValue(static_cast<const CallExpr&>(*initialValue)) &&
                       static_cast<const CallExpr&>(*initialValue).callee->kind == ExprKind::Name) {
                // `var cached = Size(0, 0)`: a value type's field, by what it is made as, so that
                // the object can keep it inside itself (M6.6 part 5).
                emitTypeThunk(static_cast<const NameExpr&>(*static_cast<const CallExpr&>(*initialValue).callee).name, f);
                flags = kTypeInstance;
            } else if (!type.typed() && initialValue != nullptr && session_.checker != nullptr) {
                // `var total = 0`: an int, a float or a bool by what it is given, so that the
                // objects keep it as a payload (M6.6 part 6).
                RuntimeType inferred;
                if (session_.checker->inferredFieldType(f, inferred))
                    flags = inferred.kind == RuntimeType::Int ? kTypeInt : inferred.kind == RuntimeType::Float ? kTypeFloat : kTypeBool;
            }
            emit(Op::DeclareField);
            emitU16(stringConstant(storedName(f)));
            emitU16(flags | (f.isVal ? kFieldVal : 0) | (required ? kFieldRequired : 0) | (f.isOverride ? kFieldOverride : 0));
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
        state.object = objectMember(state);
        state.proto = newProto();
        state.proto->name = s.name + ".<fields>";
        state.proto->source = file_;
        state.scopeDepth = 1;
        state.add(Local{"this", 1, VarInfo{}});

        fs_ = &state;
        for (size_t i : initialized) {
            const VarStmt& f = *s.fields[i];
            line_ = f.line;
            emit(Op::GetLocal, 0);
            if (f.isLazy) compileLazy(*initialValues[i], s.name + "." + f.name);
            else if (initialValues[i] != nullptr) compileExprFor(*initialValues[i], resolveType(f.type));
            else emit(Op::Null);
            line_ = f.line;
            emit(Op::SetOwn, stringConstant(storedName(f)));
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

    // What works a lazy value out (D37): a function of no parameters giving `value`, written
    // where the value is (so it reaches `this` and the names around it as a lambda does),
    // wrapped by Op::Lazy.
    void compileLazy(const Expr& value, const std::string& displayName) {
        compileLazyWith([&] { compileExpr(value); }, displayName, value.line);
    }

    void compileLazyWith(const std::function<void()>& body, const std::string& displayName, int line) {
        FunctionState state;
        state.enclosing = fs_;
        state.kind = FunctionKind::Function;
        state.object = objectMember(state);
        state.proto = newProto();
        state.proto->name = displayName + " (lazy)";
        state.proto->source = file_;
        state.scopeDepth = 1;
        state.isLambda = true;
        state.add(Local{"", 1, VarInfo{}});
        int levels = 1;
        for (FunctionState* outer = state.enclosing; outer != nullptr; outer = outer->enclosing, levels++) {
            if (outer->isLambda || kindHasThis(outer->kind)) {
                const int receiver = captureReceiver(&state, levels);
                state.proto->outerReceivers.push_back(
                    static_cast<uint16_t>(isCaptureMark(receiver) ? kCapturedReceiver | (receiver - kCaptureMark) : receiver));
            }
            if (outer->kind != FunctionKind::Function) break;
        }
        fs_ = &state;
        line_ = line;
        body();
        emit(Op::Return);
        finish(state);
        fs_ = state.enclosing;
        line_ = line;
        int index = static_cast<int>(fs_->proto->protos.size());
        fs_->proto->protos.push_back(state.proto);
        emit(Op::Closure, index);
        emit(Op::Lazy);
    }

    // Emits a closure that returns the class called `className`. A field's class type is
    // resolved through one on first use, so it can name a class declared further down.
    void emitTypeThunk(const std::string& className, const Node& at) {
        FunctionState state;
        state.enclosing = fs_;
        state.kind = FunctionKind::Function;
        state.proto = newProto();
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
        state.object = objectMember(state);
        state.proto = newProto();
        state.proto->name = displayName;
        state.proto->source = file_;
        state.proto->arity = static_cast<int>(decl.params.size());
        state.proto->varargIndex = decl.varargIndex;
        state.scopeDepth = 1;
        state.isLambda = decl.isLambda;
        state.decl = &decl;
        for (const TypeParam& p : decl.typeParams) state.typeParams.push_back(p.name);
        if (!decl.typeParams.empty()) state.proto->typeArgs |= kTypeArgsOwn;
        if (session_.checker != nullptr && session_.checker->returnsValue(decl)) state.proto->typeArgs |= kTakesDestination;
        state.label = decl.label;
        state.calleeLabel = decl.calleeLabel;
        state.isExtension = std::exchange(nextIsExtension_, false);
        state.receiverType = std::exchange(nextReceiverType_, std::string());
        state.property = std::exchange(nextProperty_, std::string());
        state.backingField = std::exchange(nextBackingField_, std::string());

        // Slot 0 holds the receiver in methods and the callee itself otherwise. A lambda run on
        // an object its type says has that object there, which is what `this` means in it (D58).
        const bool onObject = decl.isLambda && session_.checker != nullptr && session_.checker->runsOnObject(decl);
        bool hasThis = kind == FunctionKind::Method || kind == FunctionKind::Initializer || kind == FunctionKind::Setter;
        state.add(Local{hasThis || onObject ? "this" : "", 1, VarInfo{}});
        // Given a function type that takes nothing, a lambda without an arrow has no `it`: the
        // name is that of a lambda around it (D55).
        const bool noIt = decl.isLambda && session_.checker != nullptr && session_.checker->hasNoIt(decl);
        for (size_t i = 0; i < decl.params.size(); i++) {
            state.add(Local{noIt ? std::string("(no it)") : decl.params[i], 1, VarInfo{false, resolveType(decl.paramTypes[i])}});
            state.proto->paramNames.push_back(session_.vm_.intern(decl.params[i]));
        }
        // `this@Name` (D57): slot 0 under the names that reach it, the class's for a method, the
        // function's for an extension, and the lambda's labels for a block run on an object.
        auto alias = [&](const std::string& label) {
            if (label.empty()) return;
            Local named{"this@" + label, 1, VarInfo{true, TypeSpec{}, false}};
            named.slot = 0;
            state.locals.push_back(std::move(named));
        };
        if (hasThis && class_ != nullptr && kind != FunctionKind::Setter) {
            const std::string& q = class_->qualified;
            alias(q.substr(q.rfind('.') == std::string::npos ? 0 : q.rfind('.') + 1));
        }
        if (hasThis && state.isExtension) alias(decl.name);
        if (onObject) {
            alias(decl.label);
            if (decl.calleeLabel != decl.label) alias(decl.calleeLabel);
        }

        state.returnType = resolveType(decl.returnType);
        if (kind == FunctionKind::Initializer && decl.returnType.present())
            throw error(decl.returnType, "an initializer cannot declare a return type");

        // Inside an inline function's body or a block copied in place, the objects they run on
        // come first, kept as upvalues; and nothing of their scope is the new function's (D59).
        if (state.object == nullptr)
            for (size_t i = implicitSlots_.size(); i-- > 0;) {
                for (Local& local : fs_->locals)
                    if (local.slot == implicitSlots_[i]) markCaptured(*fs_, local);
                state.proto->outerReceivers.push_back(static_cast<uint16_t>(addUpvalue(&state, true, implicitSlots_[i])));
            }
        struct InlineScopeGuard {
            ModuleCompiler& c;
            int thisSlot;
            std::vector<int> implicitSlots;
            bool implicitOnly;
            InlineFrame* frame;
            explicit InlineScopeGuard(ModuleCompiler& m)
                : c(m), thisSlot(m.thisSlot_), implicitSlots(std::move(m.implicitSlots_)), implicitOnly(m.implicitOnly_), frame(m.frame_) {
                c.thisSlot_ = -1;
                c.implicitSlots_.clear();
                c.implicitOnly_ = false;
                c.frame_ = nullptr;
            }
            ~InlineScopeGuard() {
                c.thisSlot_ = thisSlot;
                c.implicitSlots_ = std::move(implicitSlots);
                c.implicitOnly_ = implicitOnly;
                c.frame_ = frame;
            }
        } inlineScope(*this);

        // A lambda written inside other lambdas can also reach the objects those run on,
        // innermost first: in `grid { row { ... } }` the inner block sees the row, then the grid.
        // A function written inside a method reaches that method's object the same way, after
        // any lambdas in between: the object is the last place a bare name is looked for.
        // So does a method of a class written inside a method: after its own object comes the
        // object of the method around the class.
        {
            int levels = 1;
            for (FunctionState* outer = state.enclosing; outer != nullptr; outer = outer->enclosing, levels++) {
                if (outer->isLambda || kindHasThis(outer->kind)) {
                    const int receiver = captureReceiver(&state, levels);
                    state.proto->outerReceivers.push_back(static_cast<uint16_t>(
                        isCaptureMark(receiver) ? kCapturedReceiver | (receiver - kCaptureMark) : receiver));
                }
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

        // Given an item of a list where it is, a function that may change such lists as it runs
        // takes a copy first (M6.6 part 4).
        if (session_.checker != nullptr) {
            for (int param : session_.checker->paramsOwned(decl)) emit(Op::OwnLocal, param + 1);
            if (session_.checker->ownsThis(decl)) emit(Op::OwnLocal, 0);
        }

        // Typed parameters are checked on entry: slot 0 is the callee or receiver, then the parameters.
        for (size_t i = 0; i < decl.params.size(); i++) {
            TypeSpec type = state.locals[i + 1].info.type;
            if (!type.typed()) {
                // A union holding one enum takes that enum's entries by their bare names.
                if (i < decl.paramTypes.size()) {
                    std::string onlyEnum;
                    int enums = 0;
                    for (const TypeAnnotation& part : decl.paramTypes[i].unionOf) {
                        const TypeSpec spec = resolveType(part);
                        if (spec.id() == kTypeInstance && enumEntries(spec) != nullptr) {
                            enums++;
                            onlyEnum = spec.className;
                        }
                    }
                    if (enums == 1) {
                        emit(Op::GetLocal, static_cast<int>(i + 1));
                        emitTypeName(onlyEnum, decl.paramTypes[i]);
                        emit(Op::SettleEnum);
                        if (i < 64) state.proto->classParams |= uint64_t{1} << i;
                        emit(Op::SetLocal, static_cast<int>(i + 1));
                        emit(Op::Pop);
                    }
                }
                continue;
            }
            if (type.id() != kTypeInstance) {
                // Checked where it is, without being pushed and popped (M6.5 part 4).
                emit(Op::CheckLocal, static_cast<int>(i + 1));
                emitU16(stringConstant(decl.params[i]));
                emitByte(type.flags);
                continue;
            }
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

    // For `class Pair<A> : Base<List<A>>()`: a function that makes `Base<List<A>>` from the
    // class's own type arguments, which it is given as a generic function is (M6.6 part 1).
    void compileSupertypeMaker(const ClassStmt& s, const TypeAnnotation& supertype, const std::string& storage) {
        line_ = supertype.line;
        emitGet(storage, s);
        FunctionState state;
        state.enclosing = fs_;
        state.kind = FunctionKind::Function;
        state.proto = newProto();
        state.proto->name = s.name + " (supertype " + supertype.name + ")";
        state.proto->source = file_;
        state.proto->typeArgs = kTypeArgsOwn;
        state.scopeDepth = 1;
        for (const TypeParam& p : s.typeParams) state.typeParams.push_back(p.name);
        state.add(Local{"", 1, VarInfo{}});
        fs_ = &state;
        std::string shape;
        emitWrittenType(supertype, shape);
        emit(Op::MakeType, stringConstant(shape));
        emit(Op::Return);
        finish(state);
        fs_ = state.enclosing;
        line_ = supertype.line;
        const int index = static_cast<int>(fs_->proto->protos.size());
        fs_->proto->protos.push_back(state.proto);
        emit(Op::Closure, index);
        emit(Op::SupertypeMaker);
        emit(Op::Pop);
    }

    // ---- Reflection (M6.6 part 8, group 6; D38, D50) ----

    // The two functions that describe a class's members for reflection, each given to the class
    // by Op::ClassMeta: `which` 0 its fields, with its annotations and an enum class's kinds;
    // 1 its methods and constructors. Each is called with the class's own type arguments, as a
    // generic function is, and gives lists that the standard library's Class reads
    // (src/reflect.cpp, "What a class says of itself"). Gives the nested function's number.
    int compileMeta(const ClassStmt& s, const ReflectedClass& r, int which) {
        FunctionState state;
        state.enclosing = fs_;
        state.kind = FunctionKind::StaticMethod;  // reaching no type parameters but the class's
        state.proto = newProto();
        state.proto->name = s.name + (which == 0 ? " (fields)" : " (methods)");
        state.proto->source = file_;
        state.proto->typeArgs = kTypeArgsOwn;
        state.scopeDepth = 1;
        for (const TypeParam& p : s.typeParams) state.typeParams.push_back(p.name);
        state.add(Local{"", 1, VarInfo{}});
        InlineBlock* const outerInline = inline_;
        inline_ = nullptr;
        fs_ = &state;
        line_ = s.line;
        if (which == 0) {
            for (const ReflectedMember& m : r.fields) {
                emit(Op::Const, stringConstant(m.name));
                emitMetaType(m.type, s);
                emit(Op::Const, intConstant(m.flags));
                emitAnnotations(m.annotations);
                emitBuildList(4, s);
            }
            emitBuildList(r.fields.size(), s);
            emitAnnotations(&s.annotations);
            for (const RuntimeType& kind : r.kinds) emitMetaType(kind, s);
            emitBuildList(r.kinds.size(), s);
            emitBuildList(3, s);
        } else {
            for (const ReflectedMember& m : r.methods) {
                emit(Op::Const, stringConstant(m.name));
                emit(Op::Const, stringConstant(formNameOf(s, *m.fn)));
                emitMetaParams(m, s);
                emitMetaType(m.type, s);
                emitAnnotations(m.annotations);
                emitBuildList(5, s);
            }
            emitBuildList(r.methods.size(), s);
            for (const ReflectedMember& m : r.constructors) {
                emitMetaParams(m, s);
                emitAnnotations(m.annotations);
                emitBuildList(2, s);
            }
            emitBuildList(r.constructors.size(), s);
            emitBuildList(2, s);
        }
        emit(Op::Return);
        finish(state);
        fs_ = state.enclosing;
        inline_ = outerInline;
        const int index = static_cast<int>(fs_->proto->protos.size());
        fs_->proto->protos.push_back(state.proto);
        return index;
    }

    void emitBuildList(size_t count, const Node& at) {
        if (count > 0xFFFF) throw error(at, "a class has too many members to describe");
        emit(Op::BuildList, static_cast<int>(count));
    }

    // A member's type in its description: `any?` where it cannot be named as the program runs,
    // and null where a method gives nothing.
    void emitMetaType(const RuntimeType& t, const Node& at) {
        if (t.kind == RuntimeType::Unknown) {
            emit(Op::Null);
            return;
        }
        std::string shape;
        std::vector<TypeOperand> operands;
        if (!shapeOf(t, shape, operands) || !emitShape(shape, operands, at)) {
            emitShape("?a", {}, at);
        }
    }

    // A method's or constructor's parameters: each its name, its type, whether it may be left
    // out, and its annotations.
    void emitMetaParams(const ReflectedMember& m, const Node& at) {
        for (size_t i = 0; i < m.params.size(); i++) {
            emit(Op::Const, stringConstant(m.paramNames[i]));
            emitMetaType(m.params[i], at);
            emit(m.optional[i] ? Op::True : Op::False);
            emitAnnotations(i < m.paramAnnotations.size() ? m.paramAnnotations[i] : nullptr);
            emitBuildList(4, at);
        }
        emitBuildList(m.params.size(), at);
    }

    // The annotations written on a declaration, made as the program runs (D153).
    void emitAnnotations(const Annotations* written) {
        const size_t count = written != nullptr ? written->size() : 0;
        for (size_t i = 0; i < count; i++) compileExpr(*(*written)[i].call);
        emitBuildList(count, Node{});
    }

    // Gives a class the functions describing it (compileMeta), at the top of the script for a
    // class set up ahead, each kept in a built program only where reflection reaches it (D50):
    // `key` the global of the top-level declaration it is part of. Then the same for the classes
    // inside it.
    void emitClassMetas(const ClassStmt& s, const std::string& bound, int key) {
        if (s.isExternal || session_.checker == nullptr) return;
        ReflectedClass r;
        if (session_.checker->reflected(s, r)) {
            const std::string storage = s.isObject ? "(object " + bound + ")" : bound;
            for (int which = 0; which < 2; which++) {
                const int start = static_cast<int>(code().size());
                line_ = s.line;
                const int meta = compileMeta(s, r, which);
                line_ = s.line;
                emitGet(storage, s);
                emit(Op::ClassMeta, meta);
                emitByte(which);
                emit(Op::Pop);
                if (key >= 0) noteDeclaration(start, key, which == 0 ? DeclarationSpan::Reflect : DeclarationSpan::ReflectMethods);
            }
        }
        for (const auto& inner : s.nested) emitClassMetas(*inner, bound + "." + inner->name, key);
    }

    // ---- Changing a value through its place (M6.6 part 4, value-lists.html section 8) ----

    // The object of `obj.name`: as a value is read, or, where the checker found it to be the
    // place of a change (`pts[i]` in `pts[i].x = 4`, `p` in `p.move()`), as that place.
    void compileObjectOf(const MemberExpr& member) {
        if (session_.checker != nullptr && session_.checker->isValuePlace(*member.object)) compileValuePlace(*member.object);
        else compileExpr(*member.object);
    }

    // Pushes a Ref to where a value of a value type is kept, through which it is changed: an
    // item of a list where it is, and a variable or field given a box of its own first.
    void compileValuePlace(const Expr& e) {
        line_ = e.line;
        switch (e.kind) {
            case ExprKind::Name: {
                const auto& n = static_cast<const NameExpr&>(e);
                VarInfo info;
                const int local = resolveLocal(fs_, n.name, n, info);
                if (local != -1) {
                    emit(Op::PlaceLocal, slotOf(local));
                    return;
                }
                const int upvalue = resolveUpvalue(fs_, n.name, n, info);
                if (upvalue != -1 && !isCaptureMark(upvalue)) {
                    emit(Op::PlaceUpvalue, upvalue);
                    return;
                }
                if (upvalue == -1 && classSide() == ClassSide::Instance && class_->members.count(n.name) != 0 && directThis()) {
                    // A field of what the method runs on.
                    emitThisValue();
                    emit(class_->isValue ? Op::GetOwn : Op::PlaceField, stringConstant(n.name));
                    return;
                }
                if (upvalue == -1 && classSide() == ClassSide::None) {
                    if (const GlobalName* global = findGlobal(n.name, n, true); global != nullptr && !global->isFunction) {
                        emit(Op::PlaceGlobal, global->slot);
                        return;
                    }
                }
                break;
            }
            case ExprKind::This:
                emitGet("this", e);
                return;
            case ExprKind::Index: {
                const auto& index = static_cast<const IndexExpr&>(e);
                if (index.defaultValue) break;
                compileExpr(*index.object);
                compileExpr(*index.index);
                line_ = e.line;
                emit(Op::PlaceIndex);
                return;
            }
            case ExprKind::Member: {
                const auto& member = static_cast<const MemberExpr&>(e);
                if (session_.checker->isValuePlace(*member.object)) {
                    compileValuePlace(*member.object);  // a value in a value: further into the same place
                    line_ = e.line;
                    emit(Op::GetProp, stringConstant(member.name));
                } else {
                    compileExpr(*member.object);
                    line_ = e.line;
                    emit(Op::PlaceField, stringConstant(member.name));
                }
                return;
            }
            default:
                break;
        }
        compileExpr(e);  // as a value; what is changed through it then reports where it cannot be
    }

    // `pts.add(Point(1, 2))` on a list of a value type (M6.6 part 3): the value is written into
    // a new item of the list where the list keeps its items packed and the type is made from
    // its header alone (AddNew); otherwise the call and the add as usual, which follow it.
    bool compileAddNew(const CallExpr& e, const MemberExpr& member) {
        if (member.safe || member.name != "add" || e.args.size() != 1 || e.trailingLambda || e.namedCount > 0 || e.hasSpread)
            return false;
        if (session_.checker == nullptr || !session_.checker->addsNewValue(e)) return false;
        if (e.args[0]->kind != ExprKind::Call) return false;
        const auto& made = static_cast<const CallExpr&>(*e.args[0]);
        std::vector<RuntimeType> typeArgs;
        if (made.namedCount > 0 || made.hasSpread || made.trailingLambda || made.args.size() > 0xFF ||
            session_.checker->typeArguments(made, typeArgs))
            return false;
        compileExpr(*member.object);
        compileCallee(made);
        compileArgs(made.args);
        line_ = made.line;
        emit(Op::AddNew);
        emitByte(static_cast<int>(made.args.size()));
        const int skip = here();
        emitI32(0);
        emit(Op::Call);
        emitByte(static_cast<int>(made.args.size()));
        line_ = member.line;
        emit(Op::Invoke, stringConstant(calledName(e, member.name)));
        emitByte(1);
        patchJump(skip);
        return true;
    }

    // ---- Expressions ----

    void requireMethodContext(const Node& at, const char* keyword) {
        if (std::string(keyword) == "this") {
            VarInfo info;
            if (resolveLocal(fs_, "this", at, info) != -1) return;  // an inline body's receiver, or a block's object (D58, D59)
        }
        for (FunctionState* state = fs_; state != nullptr; state = state->enclosing) {
            if (state->isLambda && !state->locals.empty() && state->locals[0].name == "this" && std::string(keyword) == "this") return;
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
            case TokenType::Ushr:
            case TokenType::UshrEq: return Op::Ushr;
            case TokenType::AmpPlus: return Op::AddWrap;
            case TokenType::AmpMinus: return Op::SubWrap;
            case TokenType::AmpStar: return Op::MulWrap;
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
        if (emitConstant(expr)) return;
        switch (expr.kind) {
            case ExprKind::Literal: {
                const auto& e = static_cast<const LiteralExpr&>(expr);
                switch (e.type) {
                    case LiteralExpr::Type::Null: emit(Op::Null); break;
                    case LiteralExpr::Type::Bool: emit(e.boolValue ? Op::True : Op::False); break;
                    case LiteralExpr::Type::Int: emit(Op::Const, intConstant(e.intValue)); break;
                    case LiteralExpr::Type::Float: emit(Op::Const, floatConstant(e.floatValue)); break;
                    case LiteralExpr::Type::Char: emit(Op::Const, charConstant(static_cast<uint32_t>(e.intValue))); break;
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
                // A kind of an enum class by its bare name (D39): by its full name.
                if (const std::string* kind = kindName(expr)) emitTypeName(*kind, expr);
                else emitGet(static_cast<const NameExpr&>(expr).name, expr);
                break;

            case ExprKind::This: {
                const auto& e = static_cast<const ThisExpr&>(expr);
                if (!e.label.empty()) {
                    // `this@Name` (D57): what slot 0 is called where it is that object.
                    VarInfo info;
                    const std::string named = "this@" + e.label;
                    if (resolveLocal(fs_, named, e, info) == -1 && resolveUpvalue(fs_, named, e, info) == -1)
                        throw error(e, "there is no class or block called '" + e.label + "' around this: this@Name names the object of a class " +
                                          "the code is written in, or of the block given to the function name (D57)");
                    emitGet(named, e);
                    break;
                }
                requireMethodContext(expr, "this");
                emitGet("this", expr);
                break;
            }

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
                emit(e.op == TokenType::Minus ? Op::Neg : e.op == TokenType::Tilde ? Op::BitNot : e.op == TokenType::Caret ? Op::FromEnd :
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

            case ExprKind::Object:
                compileAnonymousObject(*static_cast<const ObjectExpr&>(expr).cls);
                break;

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
                if (Checker::AtomicFieldOp atomic; session_.checker != nullptr && session_.checker->atomicField(e, atomic)) {
                    compileAtomicField(atomic, e);  // `x.hits.compareAndSet(a, b)` (D92)
                    break;
                }
                if (fs_->decl != nullptr && fs_->decl->isTail && session_.checker != nullptr) refuseTailCall(e);
                if (session_.checker != nullptr && session_.checker->samCall(e)) {
                    compileExpr(*e.args[0]);  // `Action { ... }`: the lambda, as the interface (D28)
                    break;
                }
                if (compileRangeCall(e) || compileBoundCall(e) || compileInlineCall(e) || compileCallOn(e)) break;
                noteWorkerScript(e);
                // A lambda after the call belongs to the last parameter, which only the
                // function being called knows the position of.
                const std::vector<int>* order = session_.checker != nullptr && !e.hasSpread && e.callee->kind == ExprKind::Member &&
                                                        importedMember(*static_cast<const MemberExpr&>(*e.callee).object,
                                                                       static_cast<const MemberExpr&>(*e.callee).name, *e.callee) == nullptr
                                                    ? session_.checker->argumentOrder(e)
                                                    : nullptr;
                if (order != nullptr) {
                    // A C++ method given named arguments or a lambda after the call: each in its
                    // parameter's place, as the form the checker chose has them.
                    const auto& member = static_cast<const MemberExpr&>(*e.callee);
                    compileObjectOf(member);
                    int toEnd = member.safe ? emitJump(Op::JumpIfNull) : -1;
                    for (int at : *order) {
                        if (at < 0) emit(Op::Null);
                        else compileArg(*e.args[static_cast<size_t>(at)]);
                    }
                    argc = static_cast<int>(order->size());
                    emitCallPrelude(e, argc + 1);
                    line_ = member.line;
                    emit(ownMember(*member.object, member.name) ? Op::InvokeOwn : Op::Invoke, stringConstant(calledName(e, member.name)));
                    emitByte(argc);
                    emitExternalResult(e);
                    if (toEnd >= 0) patchJump(toEnd);
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
                    if (compileNumeric(e, member)) break;
                    if (compileAddNew(e, member)) break;
                    compileObjectOf(member);
                    // `object?.method(...)`: with no object the arguments are not worked out either.
                    int toEnd = member.safe ? emitJump(Op::JumpIfNull) : -1;
                    compileArgs(e.args);
                    const bool trailing = e.trailingLambda;
                    emitCallPrelude(e, argc + 1);
                    line_ = member.line;
                    if (ownMember(*member.object, member.name))
                        emit(trailing ? Op::InvokeOwnTrailing : Op::InvokeOwn, stringConstant(calledName(e, member.name)));
                    else
                        emit(trailing ? Op::InvokeTrailing : Op::Invoke, stringConstant(calledName(e, member.name)));
                    emitByte(argc);
                    emitExternalResult(e);
                    if (toEnd >= 0) patchJump(toEnd);
                } else if (e.callee->kind == ExprKind::Name && compileBareCall(e)) {
                    // done: the name was a method, or may be one
                } else if (e.callee->kind == ExprKind::Super) {
                    const auto& sup = static_cast<const SuperExpr&>(*e.callee);
                    requireSuper(sup);
                    emitGet("this", sup);
                    compileArgs(e.args);
                    emitGet("super", sup);
                    emitCallPrelude(e, argc + 2);
                    line_ = sup.line;
                    emit(e.trailingLambda ? Op::SuperInvokeTrailing : Op::SuperInvoke, stringConstant(calledName(e, sup.method)));
                    emitByte(argc);
                    emitExternalResult(e);
                } else {
                    compileCallee(e);
                    compileArgs(e.args);
                    emitCallPrelude(e, argc + 1);
                    line_ = e.line;
                    emit(Op::Call);
                    emitByte(argc);
                    emitExternalResult(e);
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
                if (session_.checker != nullptr && session_.checker->extensionGetter(e)) {
                    // An extension property: its getter called on the value.
                    emit(Op::Invoke, stringConstant(e.name));
                    emitByte(0);
                } else {
                    const bool own = ownMember(*e.object, e.name);
                    const bool inPlace = session_.checker != nullptr && !copyReadsHere() && session_.checker->readsFieldInPlace(e);
                    emit(inPlace ? (own ? Op::GetOwnRef : Op::GetPropRef) : (own ? Op::GetOwn : Op::GetProp), stringConstant(e.name));
                }
                if (toEnd >= 0) patchJump(toEnd);
                break;
            }

            case ExprKind::Index: {
                const auto& e = static_cast<const IndexExpr&>(expr);
                compileExpr(*e.object);
                compileExpr(*e.index);
                if (e.defaultValue) compileExpr(*e.defaultValue);
                line_ = e.line;
                if (e.defaultValue) emit(Op::GetIndexOr);
                else if (session_.checker != nullptr && !copyReadsHere() && session_.checker->readsInPlace(e)) emit(Op::GetIndexRef);  // M6.6 part 4
                else emit(Op::GetIndex);
                break;
            }

            case ExprKind::List: {
                const auto& e = static_cast<const ListExpr&>(expr);
                for (const ExprPtr& item : e.items) compileExpr(*item);
                line_ = e.line;
                emit(Op::BuildList, static_cast<int>(e.items.size()));
                RuntimeType made;
                if (session_.checker->literalType(e, made)) emitTag(made, e);
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
                RuntimeType made;
                if (session_.checker->literalType(e, made)) emitTag(made, e);
                break;
            }

            case ExprKind::Range: {
                const auto& e = static_cast<const RangeExpr&>(expr);
                // A slice's range may leave out its start, 0, or its end, ^1 (D147).
                if (e.start) compileExpr(*e.start);
                else emit(Op::Const, intConstant(0));
                if (e.end) {
                    compileExpr(*e.end);
                } else {
                    emit(Op::Const, intConstant(1));
                    emit(Op::FromEnd);
                }
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
                if (hasTypeArguments(e.type)) {
                    // `x is List<int>`: what the list was made to hold is checked too.
                    std::string shape;
                    emitWrittenType(e.type, shape);
                    emit(Op::MakeType, stringConstant(shape));
                    emit(Op::IsFull);
                } else if (type.typed()) {
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
                if (hasTypeArguments(e.type)) {
                    std::string shape;
                    emitWrittenType(e.type, shape);
                    emit(Op::MakeType, stringConstant(shape));
                    emit(Op::CastFull);
                    emitByte(e.safe ? 1 : 0);
                } else if (type.typed()) {
                    emitTypeOperand(e.safe ? Op::CastOrNull : Op::Cast, type, e.type);
                }
                break;
            }

            case ExprKind::ClassOf: {
                // `Image#`, `x#` (D48): the standard library's Class, made from the type and from
                // the function written here, whose rights to members it has.
                const auto& e = static_cast<const ClassOfExpr&>(expr);
                line_ = e.line;
                emit(Op::GetGlobal, stdGlobal("__classOf", e));
                RuntimeType named;
                const int how = session_.checker != nullptr ? session_.checker->namedClassOf(e, named) : 0;
                if (how == 1) {
                    std::string shape;
                    std::vector<TypeOperand> operands;
                    if (!shapeOf(named, shape, operands) || !emitShape(shape, operands, e))
                        throw error(e, "this class cannot be named where the program runs");
                } else {
                    compileExpr(*e.value);
                }
                line_ = e.line;
                emit(Op::ClassOf);
                emitByte(how == 1 ? 0 : how == 2 ? 2 : 1);
                emit(Op::Call);
                emitByte(2);
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
                RuntimeType iface;
                std::string method;
                int arity = 0;
                if (session_.checker != nullptr && session_.checker->samConversion(expr, iface, method, arity)) {
                    compileSamObject(expr, fn, iface, method, arity);
                    break;
                }
                compileFunction(fn, FunctionKind::Function, fn.name);
                break;
            }
        }
    }

    // An anonymous object: the class is made the first time and kept in a global of the file's
    // own that nothing else can name (M6.5 part 4), in a variable here as well; then its one
    // object, which keeps what its members use of the code around (ObjectBuild).
    void compileAnonymousObject(const ClassStmt& cls) {
        const int saved = enterValueBlock();
        beginScope();
        const int number = anonymousClasses_++;
        const std::string bound = "(object " + std::to_string(number) + ")";
        ObjectBuild build;
        build.boundary = fs_;
        build.outer = objectBuild_;
        build.localsAtStart = fs_->locals.size();
        objectBuild_ = &build;

        // The shell's inputs may write the same object again: there it is made every time.
        int kept = 0xFFFF;
        int skip = -1;
        if (!shellInput_) {
            const std::string hiddenName = "(object class " + std::to_string(number) + ")";
            GlobalName& hidden = session_.privates_[fileKey_][hiddenName];
            if (hidden.slot < 0) hidden.slot = session_.newSlot(hiddenName);
            hidden.declared = true;
            kept = hidden.slot;
            line_ = cls.line;
            emit(Op::ObjectClass, kept);
            skip = static_cast<int>(code().size());
            emitI32(0);
        }
        compileClass(cls, bound);
        objectBuild_ = build.outer;
        line_ = cls.line;
        if (skip >= 0) {
            emitGet(bound, cls);
            const int keptAt = static_cast<int>(code().size()) + 1;  // StoreClass's operand
            emit(Op::StoreClass, kept);
            emit(Op::Pop);
            patchJump(skip);
            if (build.usesOuter) {
                // Made from what the code around holds: made every time, and kept nowhere.
                code()[static_cast<size_t>(skip) - 2] = 0xFF;
                code()[static_cast<size_t>(skip) - 1] = 0xFF;
                code()[static_cast<size_t>(keptAt)] = 0xFF;
                code()[static_cast<size_t>(keptAt) + 1] = 0xFF;
            }
        }

        emitGet(bound, cls);
        if (build.captures.size() > 255)
            throw error(cls, "an object can use at most 255 variables of the code around it");
        for (size_t i = 0; i < build.captures.size(); i++) {
            const ObjectBuild::Capture& capture = build.captures[i];
            emit(Op::Capture, capture.kind == 3 ? static_cast<int>(i) : capture.index);
            emitByte(capture.kind);
        }
        emit(Op::NewObject);
        emitByte(static_cast<int>(build.captures.size()));
        endScopeKeepingValue();
        leaveValueBlock(saved);
    }

    // A lambda given as an interface with one abstract method (D28): one small object of a class
    // made once, as `object : I { override fun m(a, b) => lambda(a, b) }` would be, which keeps
    // the lambda. The interface is named by its global, which this file need not import.
    std::vector<std::unique_ptr<ClassStmt>> samClasses_;
    void compileSamObject(const Expr& at, const FunctionDecl& fn, const RuntimeType& iface, const std::string& method, int arity) {
        std::string shape;
        std::vector<TypeOperand> operands;
        if (!shapeOf(iface, shape, operands) || operands.size() != 1 || operands[0].global < 0)
            throw error(at, "the interface this lambda is given as cannot be named here (D28)");
        auto cls = std::make_unique<ClassStmt>();
        cls->line = at.line;
        cls->col = at.col;
        cls->name = "(lambda)";
        const std::string super = "(global " + std::to_string(operands[0].global) + ")";
        cls->supertypes.push_back(super);
        TypeAnnotation written;
        written.name = super;
        cls->supertypeTypes.push_back(written);
        cls->hasSuper = true;
        cls->superName = super;
        auto m = std::make_unique<FunctionDecl>();
        m->line = at.line;
        m->col = at.col;
        m->name = method;
        m->isOverride = true;
        m->generated = true;
        auto call = std::make_unique<CallExpr>();
        call->line = at.line;
        call->col = at.col;
        auto callee = std::make_unique<NameExpr>();
        callee->line = at.line;
        callee->col = at.col;
        callee->name = "(lambda kept)";
        call->callee = std::move(callee);
        for (int i = 0; i < arity; i++) {
            const std::string param = "(" + std::to_string(i) + ")";
            m->params.push_back(param);
            Node position;
            position.line = at.line;
            position.col = at.col;
            m->paramPositions.push_back(position);
            m->paramTypes.emplace_back();
            m->paramDefaults.push_back(nullptr);
            auto arg = std::make_unique<NameExpr>();
            arg->line = at.line;
            arg->col = at.col;
            arg->name = param;
            call->args.push_back(std::move(arg));
            call->names.emplace_back();
            call->spread.push_back(0);
        }
        auto ret = std::make_unique<ReturnStmt>();
        ret->line = at.line;
        ret->col = at.col;
        ret->value = std::move(call);
        m->body.push_back(std::move(ret));
        cls->methods.push_back(std::move(m));
        const ClassStmt& made = *cls;
        samClasses_.push_back(std::move(cls));

        const int saved = enterValueBlock();
        beginScope();
        compileFunction(fn, FunctionKind::Function, fn.name);
        addLocal("(lambda kept)", true, at, true);
        compileAnonymousObject(made);
        endScopeKeepingValue();
        leaveValueBlock(saved);
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
            if (!locals.back().alias) {
                fs_->slotTop--;
                count++;
            }
            locals.pop_back();
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
        if (kindName(*e.callee) != nullptr) return false;  // an enum class's kind: compileCallee
        const auto& callee = static_cast<const NameExpr&>(*e.callee);
        const std::string& name = callee.name;
        VarInfo info;
        if (resolveLocal(fs_, name, callee, info) != -1 || resolveUpvalue(fs_, name, callee, info) != -1) return false;

        const int argc = static_cast<int>(e.args.size());
        const ClassSide side = classSide();
        const bool own = side != ClassSide::None && (class_->statics.count(name) != 0 ||
                                                     (side == ClassSide::Instance && class_->members.count(name) != 0));
        if (!own && outerStaticOwner(name) != nullptr) return false;  // called as a value
        if (side == ClassSide::Instance && class_->members.count(name) != 0 && directThis()) {
            emitThisValue();
            compileArgs(e.args);
            emitCallPrelude(e, argc + 1);
            line_ = e.line;
            emit(Op::InvokeOwn, stringConstant(calledName(e, name)));
            emitByte(argc);
            emitExternalResult(e);
            return true;
        }
        const bool member = side == ClassSide::Instance && class_->members.count(name) != 0;
        if (!member && side != ClassSide::None && class_->statics.count(name) != 0) {
            // Where something may come before the static, it is called as a value instead.
            if (namesFromReceiver() || (side == ClassSide::Instance && class_->hasSuper)) return false;
            emitGet(class_->storage, callee);
            compileArgs(e.args);
            emitCallPrelude(e, argc + 1);
            line_ = e.line;
            emit(Op::InvokeOwn, stringConstant(name));
            emitByte(argc);
            emitExternalResult(e);
            return true;
        }
        if (!member && !(side == ClassSide::None ? namesFromReceiver() : mayBeMember(side))) return false;

        emit(Op::Null);  // where the object, or the function, is put when the call is made
        compileArgs(e.args);
        emitCallPrelude(e, argc + 1);
        line_ = e.line;
        emitImplicit(Op::InvokeImplicit, name);
        emitByte(argc);
        emitExternalResult(e);
        return true;
    }

    void compileNamedCall(const CallExpr& e) {
        int toEnd = -1;
        if (e.callee->kind == ExprKind::Member && static_cast<const MemberExpr&>(*e.callee).safe) {
            const auto& member = static_cast<const MemberExpr&>(*e.callee);
            compileExpr(*member.object);
            line_ = member.line;
            toEnd = emitJump(Op::JumpIfNull);
            emit(Op::GetProp, stringConstant(calledName(e, member.name)));
        } else if (e.callee->kind == ExprKind::Member && calledName(e, static_cast<const MemberExpr&>(*e.callee).name) !=
                                                             static_cast<const MemberExpr&>(*e.callee).name) {
            const auto& member = static_cast<const MemberExpr&>(*e.callee);
            compileExpr(*member.object);
            line_ = member.line;
            emit(Op::GetProp, stringConstant(calledName(e, member.name)));
        } else {
            compileCallee(e);
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

        emitCallPrelude(e, -1);
        line_ = e.line;
        emit(e.hasSpread ? Op::CallSpread : Op::CallNamed);
        if (!e.hasSpread) emitByte(static_cast<int>(positional));
        emitByte(static_cast<int>(e.namedCount + trailing));
        emitExternalResult(e);
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
    // A read of a constant (R2): its value, worked out by the checker, in its place.
    // An expression made only of literals and constants is worked out the same way (R2).
    bool emitConstant(const Expr& expr) {
        ConstValue v;
        if (session_.checker == nullptr) return false;
        if (uint8_t b; session_.checker->byteAt(expr, b)) {
            emit(Op::Const, byteConstant(b));  // an int constant where a byte goes (D19)
            return true;
        }
        if (expr.kind == ExprKind::Name || expr.kind == ExprKind::Member) {
            if (!session_.checker->constantAt(expr, v)) return false;
            // A bare name two imports both declare is still reported, as an ordinary read is.
            if (expr.kind == ExprKind::Name) {
                const auto& name = static_cast<const NameExpr&>(expr);
                VarInfo info;
                if (resolveLocal(fs_, name.name, name, info) == -1) findGlobal(name.name, name);
            }
        } else if (expr.kind == ExprKind::Binary || expr.kind == ExprKind::Unary || expr.kind == ExprKind::Logical ||
                   expr.kind == ExprKind::Interp) {
            if (!session_.checker->folded(expr, v)) return false;
        } else {
            return false;
        }
        switch (v.kind) {
            case ConstValue::Int: emit(Op::Const, intConstant(v.i)); break;
            case ConstValue::Float: emit(Op::Const, floatConstant(v.d)); break;
            case ConstValue::Bool: emit(v.b ? Op::True : Op::False); break;
            case ConstValue::String: emit(Op::Const, stringConstant(v.s)); break;
            case ConstValue::Char: emit(Op::Const, charConstant(static_cast<uint32_t>(v.i))); break;
            case ConstValue::Byte: emit(Op::Const, byteConstant(static_cast<uint8_t>(v.i))); break;
            default: return false;
        }
        return true;
    }

    // `++?`, `+=?` and the like (L118): with the target's value on the stack, over the parts
    // the store needs, a null skips the change; the parts go and the null is the result.
    int beginNullSafe(bool nullSafe) { return nullSafe ? emitJump(Op::JumpIfNull) : -1; }
    void endNullSafe(int skip, int parts) {
        if (skip < 0) return;
        const int done = emitJump(Op::Jump);
        patchJump(skip);
        if (parts > 0) emit(Op::PopUnder, parts);
        patchJump(done);
    }

    // The value at an index that is being changed: the entry, or its start value.
    void emitIndexForUpdate(const IndexExpr& target) {
        emit(Op::Dup2);
        if (target.defaultValue) {
            compileExpr(*target.defaultValue);
            emit(Op::GetIndexOr);
        } else {
            emit(Op::GetIndex);
        }
    }

    void compileIncDec(const IncDecExpr& e) {
        Op change = e.increment ? Op::Add : Op::Sub;

        switch (e.target->kind) {
            case ExprKind::Name: {
                const auto& target = static_cast<const NameExpr&>(*e.target);
                emitGet(target.name, target);
                const int skip = beginNullSafe(e.nullSafe);
                emit(Op::Const, intConstant(1));
                line_ = e.line;
                emit(change);
                emitSet(target.name, target);
                endNullSafe(skip, 0);
                break;
            }

            case ExprKind::Member: {
                const auto& target = static_cast<const MemberExpr&>(*e.target);
                requireDeclaredField(target);
                compileObjectOf(target);
                int toEnd = target.safe ? emitJump(Op::JumpIfNull) : -1;
                emit(Op::Dup);
                line_ = e.line;
                emit(ownMember(*target.object, target.name) ? Op::GetOwn : Op::GetProp, stringConstant(target.name));
                const int skip = beginNullSafe(e.nullSafe);
                emit(Op::Const, intConstant(1));
                emit(change);
                emit(ownMember(*target.object, target.name) ? Op::SetOwn : Op::SetProp, stringConstant(target.name));
                endNullSafe(skip, 1);
                if (toEnd >= 0) patchJump(toEnd);
                break;
            }

            case ExprKind::Index: {
                const auto& target = static_cast<const IndexExpr&>(*e.target);
                compileExpr(*target.object);
                compileExpr(*target.index);
                line_ = e.line;
                emitIndexForUpdate(target);
                const int skip = beginNullSafe(e.nullSafe);
                emit(Op::Const, intConstant(1));
                emit(change);
                emit(Op::SetIndex);
                endNullSafe(skip, 2);
                break;
            }

            default:
                throw error(e, "this cannot be incremented or decremented");
        }
        emit(Op::Pop);
    }

    // A number's conversion or one of Math's functions, where the checker knows the call is the
    // standard library's (M6.6 part 6): one instruction, Op::Numeric, in place of a call.
    bool compileNumeric(const CallExpr& e, const MemberExpr& member) {
        if (member.safe || e.trailingLambda || session_.checker == nullptr) return false;
        RuntimeType result;
        std::string name;
        if (!session_.checker->externalResult(e, result, name)) return false;
        const size_t argc = e.args.size();
        const bool ints = result.kind == RuntimeType::Int, floats = result.kind == RuntimeType::Float;
        bool onReceiver = true;  // a Number method, on what it is called on; else Math's, on its arguments
        NumericOp op;
        if (name == "Number.toInt" && argc == 0) op = NumericOp::ToInt;
        else if (name == "Number.toFloat" && argc == 0) op = NumericOp::ToFloat;
        else if (name == "Number.roundToInt" && argc == 0) op = NumericOp::RoundToInt;
        else if (name == "Number.isNaN" && argc == 0) op = NumericOp::IsNaN;
        else if (name == "Number.isInfinite" && argc == 0) op = NumericOp::IsInfinite;
        else if (name == "Number.isFinite" && argc == 0) op = NumericOp::IsFinite;
        else {
            onReceiver = false;
            if (name == "Math.sqrt" && argc == 1) op = NumericOp::Sqrt;
            else if (name == "Math.floor" && argc == 1) op = NumericOp::Floor;
            else if (name == "Math.ceil" && argc == 1) op = NumericOp::Ceil;
            else if (name == "Math.round" && argc == 1) op = NumericOp::Round;
            else if (name == "Math.abs" && argc == 1 && (ints || floats)) op = ints ? NumericOp::AbsInt : NumericOp::AbsFloat;
            else if (name == "Math.min" && argc == 2 && (ints || floats)) op = ints ? NumericOp::MinInt : NumericOp::MinFloat;
            else if (name == "Math.max" && argc == 2 && (ints || floats)) op = ints ? NumericOp::MaxInt : NumericOp::MaxFloat;
            else return false;
        }
        if (onReceiver) compileExpr(*member.object);
        for (const auto& arg : e.args) compileExpr(*arg);
        line_ = e.line;
        emit(Op::Numeric);
        emitByte(static_cast<int>(op));
        emitExternalResult(e);
        return true;
    }

    // The value of `place = f(...)` with the place on the stack: the call is given it as where
    // its result goes (M6.6 part 4), and it is gone once the result is on the stack.
    void compileWhereItGoes(const AssignExpr& e) {
        const CallExpr* savedTail = tailCall_;
        const CallExpr* savedDestination = destinationCall_;
        tailCall_ = nullptr;
        destinationCall_ = static_cast<const CallExpr*>(e.value.get());
        compileExpr(*e.value);
        tailCall_ = savedTail;
        destinationCall_ = savedDestination;
        line_ = e.line;
        emit(Op::Swap);
        emit(Op::Pop);  // the place: the result is there already, or goes there now
    }

    // An atomic field's change (D92), in one step.
    void compileAtomicField(const Checker::AtomicFieldOp& op, const Node& at) {
        line_ = at.line;
        if (op.object != nullptr) compileExpr(*op.object);
        else emitGet("this", at);
        if (op.expected != nullptr) compileExpr(*op.expected);
        compileExpr(*op.operand);
        line_ = at.line;
        emit(Op::AtomicField, stringConstant(op.field));
        emitByte(op.kind);
    }

    void compileAssign(const AssignExpr& e) {
        if (Checker::AtomicFieldOp atomic; session_.checker != nullptr && session_.checker->atomicField(e, atomic)) {
            compileAtomicField(atomic, e);
            return;
        }
        bool compound = e.op != TokenType::Eq;

        switch (e.target->kind) {
            case ExprKind::Name: {
                const auto& target = static_cast<const NameExpr&>(*e.target);
                if (!compound && session_.checker != nullptr && session_.checker->buildsInPlace(e)) {
                    // `bounds = Rect(...)` in a method: the field, inside the object, is where the
                    // result goes (M6.6 part 5).
                    emit(Op::GetLocal, 0);
                    emit(Op::FieldDestination, stringConstant(target.name));
                    compileWhereItGoes(e);
                    emitSet(target.name, target);
                    break;
                }
                if (compound) emitGet(target.name, target);
                const int skip = beginNullSafe(compound && e.nullSafe);
                if (compound) compileExpr(*e.value);
                else compileExprFor(*e.value, declaredTypeOf(target.name));
                line_ = e.line;
                if (compound) emit(binaryOp(e.op));
                emitSet(target.name, target);
                endNullSafe(skip, 0);
                break;
            }

            case ExprKind::Member: {
                const auto& target = static_cast<const MemberExpr&>(*e.target);
                requireDeclaredField(target);
                compileObjectOf(target);
                // `object?.field = value` does nothing, and works nothing out, with no object.
                int toEnd = target.safe ? emitJump(Op::JumpIfNull) : -1;
                const bool own = ownMember(*target.object, target.name);
                if (!compound && session_.checker != nullptr && session_.checker->buildsInPlace(e)) {
                    // `comp.bounds = Rect(...)`: the field, inside the object, is where the result
                    // goes (M6.6 part 5).
                    emit(Op::Dup);
                    emit(Op::FieldDestination, stringConstant(target.name));
                    compileWhereItGoes(e);
                    emit(own ? Op::SetOwn : Op::SetProp, stringConstant(target.name));
                    if (toEnd >= 0) patchJump(toEnd);
                    break;
                }
                if (compound) {
                    emit(Op::Dup);
                    emit(own ? Op::GetOwn : Op::GetProp, stringConstant(target.name));
                }
                const int skip = beginNullSafe(compound && e.nullSafe);
                if (compound) compileExpr(*e.value);
                else compileExprFor(*e.value, declaredTypeOf(*e.target));
                line_ = e.line;
                if (compound) emit(binaryOp(e.op));
                emit(own ? Op::SetOwn : Op::SetProp, stringConstant(target.name));
                endNullSafe(skip, 1);
                if (toEnd >= 0) patchJump(toEnd);
                break;
            }

            case ExprKind::Index: {
                const auto& target = static_cast<const IndexExpr&>(*e.target);
                compileExpr(*target.object);
                compileExpr(*target.index);
                if (!compound && session_.checker != nullptr && session_.checker->buildsInPlace(e)) {
                    // `pts[i] = pts[i].moved(1)`: the item is where the result goes (M6.6 part 4),
                    // under the call as it is made.
                    emit(Op::Dup2);
                    emit(Op::PlaceIndex);
                    compileWhereItGoes(e);
                    emit(Op::SetIndex);
                    break;
                }
                if (compound) emitIndexForUpdate(target);
                const int skip = beginNullSafe(compound && e.nullSafe);
                compileExpr(*e.value);
                line_ = e.line;
                if (compound) emit(binaryOp(e.op));
                emit(Op::SetIndex);
                endNullSafe(skip, 2);
                break;
            }

            case ExprKind::Super: {
                // super.name = value: the superclass's setter, run on this object (D68).
                const auto& target = static_cast<const SuperExpr&>(*e.target);
                if (compound) throw error(e, "super." + target.method + " is given a value with = only");
                requireSuper(target);
                emitGet("this", target);
                emitGet("super", target);
                compileExpr(*e.value);
                line_ = e.line;
                emit(Op::SetSuper, stringConstant(target.method));
                break;
            }

            default:
                throw error(e, "invalid assignment target");
        }
    }
};

// ---- Forms ----

std::string annotationText(const TypeAnnotation& t) {
    if (!t.present() || t.star) return "*";
    std::string text;
    if (t.isFunction()) {
        text = "(";
        for (size_t i = 0; i < t.params.size(); i++) text += (i > 0 ? ", " : "") + annotationText(t.params[i]);
        text += ") -> " + annotationText(t.result[0]);
        return t.nullable ? "(" + text + ")?" : text;
    }
    // A class is known by the last part of its name, however it was reached (ui.Button).
    const size_t dot = t.name.rfind('.');
    text = dot == std::string::npos ? t.name : t.name.substr(dot + 1);
    if (!t.args.empty()) {
        text += "<";
        for (size_t i = 0; i < t.args.size(); i++) text += (i > 0 ? ", " : "") + annotationText(t.args[i]);
        text += ">";
    }
    return t.nullable ? text + "?" : text;
}

std::string signatureText(const FunctionDecl& fn) {
    std::string text = "(";
    for (size_t i = 0; i < fn.params.size(); i++) {
        if (i > 0) text += ", ";
        if (static_cast<int>(i) == fn.varargIndex) text += "vararg ";
        text += i < fn.paramTypes.size() ? annotationText(fn.paramTypes[i]) : std::string("*");
    }
    return text + ")";
}

// ---- CompilerSession ----

// The key a file is known by: its canonical path.
// What the system says of a path, asked once per compile: the answers do not change while a
// program compiles, and on Windows each asking opens the file. The shell forgets them before
// each input, since files may change between them.
namespace {
struct PathFacts {
    std::unordered_map<fs::path::string_type, std::pair<fs::path, std::error_code>> canonical;
    std::unordered_map<fs::path::string_type, int> kind;  // 0 nothing there, 1 a file, 2 a folder
    std::unordered_map<std::string, std::vector<fs::path>> folders;
};
// Made the first time a thread needs it, not as each thread starts (M6.5 part 5).
PathFacts& pathFacts() {
    thread_local PathFacts facts;
    return facts;
}
}  // namespace

static void forgetPaths() { pathFacts() = PathFacts{}; }

static fs::path canonicalPath(const fs::path& path, std::error_code& ec) {
    auto found = pathFacts().canonical.find(path.native());
    if (found == pathFacts().canonical.end()) {
        std::error_code asked;
        fs::path answer = fs::weakly_canonical(path, asked);
        found = pathFacts().canonical.emplace(path.native(), std::make_pair(std::move(answer), asked)).first;
    }
    ec = found->second.second;
    return found->second.first;
}

static int pathKind(const fs::path& path) {
    auto found = pathFacts().kind.find(path.native());
    if (found != pathFacts().kind.end()) return found->second;
    std::error_code ec;
    const fs::file_status status = fs::status(path, ec);
    const int kind = ec ? 0 : fs::is_regular_file(status) ? 1 : fs::is_directory(status) ? 2 : 0;
    pathFacts().kind.emplace(path.native(), kind);
    return kind;
}

static std::string fileKeyOf(const fs::path& path) {
    std::error_code ec;
    fs::path canonical = canonicalPath(path, ec);
    if (ec) canonical = fs::absolute(path, ec);
    return canonical.string();
}

// The .astra files directly in a folder, in name order.
static std::vector<fs::path> folderFiles(const std::string& folder) {
    auto known = pathFacts().folders.find(folder);
    if (known != pathFacts().folders.end()) return known->second;
    std::error_code ec;
    std::vector<fs::path> paths;
    for (const fs::directory_entry& entry : fs::directory_iterator(fs::path(folder), ec))
        if (entry.is_regular_file(ec) && entry.path().extension() == ".astra") paths.push_back(entry.path());
    std::sort(paths.begin(), paths.end());
    pathFacts().folders.emplace(folder, paths);
    return paths;
}

CompileTimes compileTimes;
bool compilingPrelude = false;

namespace {
// Adds the time from here to the end of the scope to one part of CompileTimes.
class PartTimer {
public:
    explicit PartTimer(double& total) : total_(total), start_(std::chrono::steady_clock::now()) { depth()[&total]++; }
    ~PartTimer() {
        if (--depth()[&total_] == 0)
            total_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count();
    }

private:
    static std::unordered_map<double*, int>& depth() {
        static thread_local std::unordered_map<double*, int> counts;
        return counts;
    }
    double& total_;
    std::chrono::steady_clock::time_point start_;
};
}  // namespace

// What other files can see of a file, by hash: its tokens, but for the bodies of its named
// functions and methods, of which they learn only what its summary adds (D145).
static uint64_t skeletonOf(const std::vector<Token>& tokens, const std::vector<StmtPtr>& statements) {
    std::vector<std::pair<size_t, size_t>> bodies;
    auto add = [&](const FunctionDecl& d) {
        // An inline function's body is copied into the files calling it: theirs to see too (D59).
        if (d.isInline) return;
        if (d.bodyTo > d.bodyFrom) bodies.emplace_back(d.bodyFrom, d.bodyTo);
    };
    std::function<void(const ClassStmt&)> inClass = [&](const ClassStmt& c) {
        for (const auto& m : c.methods) add(*m);
        for (const auto& nested : c.nested) inClass(*nested);
    };
    for (const StmtPtr& s : statements) {
        if (s->kind == StmtKind::Function) add(*static_cast<const FunctionStmt&>(*s).fn);
        else if (s->kind == StmtKind::Class) inClass(static_cast<const ClassStmt&>(*s));
    }
    std::sort(bodies.begin(), bodies.end());
    std::string text;
    size_t body = 0;
    for (size_t i = 0; i < tokens.size(); i++) {
        while (body < bodies.size() && bodies[body].second <= i) body++;
        if (body < bodies.size() && bodies[body].first <= i) {
            i = bodies[body].second - 1;
            text += "\x02";
            continue;
        }
        const Token& t = tokens[i];
        text += static_cast<char>(static_cast<int>(t.type) & 0x7F);
        if (t.newlineBefore) text += "\n";
        text += t.text;
        text += "\x01";
    }
    return textHash(text);
}

// `timed` is false on the threads that parse ahead, whose time is counted where they are waited
// for. With `skeleton` given, the file's skeleton hash goes there.
static std::vector<StmtPtr> parseSource(const std::string& source, const std::string& name, bool timed = true,
                                        uint64_t* skeleton = nullptr) {
    std::optional<PartTimer> timer;
    if (timed) timer.emplace(compileTimes.parse);
    Lexer lexer(source, name);
    std::vector<Token> tokens = lexer.scan();
    Parser parser(tokens, name);
    std::vector<StmtPtr> statements = parser.parseProgram();
    if (skeleton != nullptr) *skeleton = skeletonOf(tokens, statements);
    return statements;
}

CompilerSession::CompilerSession(VM& vm, Checker* given) : checker(given), vm_(vm) {
    if (checker == nullptr) {
        ownChecker_ = std::make_unique<Checker>();
        checker = ownChecker_.get();
    }
    auto builtins = std::make_unique<Namespace>();
    builtins->directory = "<builtin>";
    builtins->library = "<builtin>";
    builtins_ = builtins.get();
    namespaces_["<builtin>"] = std::move(builtins);
    // The standard library is part of the prelude: every program starts with it loaded. It
    // comes compiled with astra.exe, unless its own mistakes are to be listed (`astra check
    // --std`) or astra.exe is making that compiled copy.
    const std::string prelude = preludeSource();
    std::string attached;
    if (!checker->reportStandardLibrary && !compilingPrelude && !checkingNativeResults() && !copyingReads() && attachedPrelude(attached) &&
        usePrelude(prelude, attached))
        return;
    program.modules.push_back(compileUnit(prelude, "<std>", "<builtin>", "<prelude>", false));
    noteModule(0, "<prelude>");
    for (const auto& [what, cover] : externalForms_) {
        if (cover.lowest == cover.min && cover.highest == cover.max) continue;
        auto counts = [](int low, int high) {
            return std::to_string(low) + " to " + (high < 0 ? std::string("any number of") : std::to_string(high));
        };
        throw CompileError(cover.file, cover.line, cover.col,
                           "the " + what + " takes " + counts(cover.min, cover.max) + " arguments in the C++; its declarations say " +
                               counts(cover.lowest, cover.highest));
    }
}

CompilerSession::~CompilerSession() = default;

// A folder whose files the program took, noted for the cache as it was when listed.
void CompilerSession::noteFolder(const std::string& folder, const std::vector<fs::path>& files) {
    if (foldersRead_.count(folder)) return;
    FolderRead read;
    read.path = folder;
    uintmax_t size = 0;
    if (!fileStamp(folder, size, read.time)) return;
    std::vector<std::string> names;
    for (const fs::path& file : files) names.push_back(file.filename().string());
    read.hash = namesHash(std::move(names));
    foldersRead_[folder] = read;
}

std::vector<FolderRead> CompilerSession::foldersRead() const {
    std::vector<FolderRead> all;
    for (const auto& [path, read] : foldersRead_) all.push_back(read);
    return all;
}

int CompilerSession::compileFile(const std::string& path) {
    forgetPaths();
    keepingUnits_ = stopAtTypeErrors;  // `astra check` lists mistakes, and keeps nothing
    try {
        // The whole program is parsed and checked first, then compiled.
        const std::string key = fileKeyOf(fs::path(path));
        const fs::path canonical(key);
        const std::string name = canonical.filename().string();
        const std::string directory = canonical.parent_path().string();
        if (std::vector<StmtPtr>* statements = parseFile(key, key, name)) {
            gather(key, name, directory, *statements, namespaceOf(directory), false);
            planReuse();
            check();
            settleReuse();
        }
        program.entry = loadModule(path, path, path, nullptr);
        checkConstAssignments();
    } catch (...) {
        globalAssignments_.clear();
        toKeep_.clear();
        keepingUnits_ = false;
        throw;
    }
    globalAssignments_.clear();
    keepUnits();
    keepingUnits_ = false;
    return program.entry;
}

const std::vector<StmtPtr>* CompilerSession::statementsOf(const std::string& path) const {
    auto found = parsed_.find(fileKeyOf(fs::path(path)));
    return found == parsed_.end() ? nullptr : found->second.get();
}

int CompilerSession::globalOf(const std::string& path, const std::string& name) {
    const std::string key = fileKeyOf(fs::path(path));
    auto& mine = privates_[key];
    if (auto it = mine.find(name); it != mine.end()) return it->second.slot;
    Namespace* ns = namespaceOf(fs::path(key).parent_path().string());
    if (auto it = ns->names.find(name); it != ns->names.end() && it->second.declared) return it->second.slot;
    if (auto it = builtins_->names.find(name); it != builtins_->names.end() && it->second.declared) return it->second.slot;
    return -1;
}

// ---- Units kept compiled (D145, points 1 and 2) ----
//
// A file's modules, or a folder's, are kept once compiled, and put back into a later compile
// of a program that has every file they were compiled from as it was. What their code says of
// the rest of the program goes by numbers this compile gave out, so each is kept with what it
// stands for: a global by the table its name is in and the name, a module by what it is, a
// library by its name; the numbers of the unit's own classes need only be its own.

void CompilerSession::noteModule(int index, const std::string& key) {
    if (moduleKeys_.size() <= static_cast<size_t>(index)) moduleKeys_.resize(static_cast<size_t>(index) + 1);
    moduleKeys_[static_cast<size_t>(index)] = key;
    moduleByKey_[key] = index;
}

int CompilerSession::libraryNumber(const std::string& library, const std::string& file, int line) {
    auto found = libraryIds_.find(library);
    if (found != libraryIds_.end()) return found->second;
    if (classCount_ >= 0xFFFF) throw CompileError(file, line, 1, "a program cannot have more than 65535 classes and libraries");
    const int id = ++classCount_;
    libraryIds_[library] = id;
    return id;
}

// The files compiling `files` depended on, besides themselves, in path order.
std::vector<std::string> CompilerSession::dependencies(const std::vector<std::string>& files) const {
    const std::set<std::string> own(files.begin(), files.end());
    std::vector<std::string> uses;
    for (const auto& [file, hash] : compiledFrom(files))
        if (own.count(file) == 0) uses.push_back(file);
    return uses;
}

// Every file compiling `files` depended on, with its text's hash, in path order.
std::vector<std::pair<std::string, uint64_t>> CompilerSession::compiledFrom(const std::vector<std::string>& files) const {
    std::set<std::string> seen;
    std::vector<std::string> todo(files.begin(), files.end());
    while (!todo.empty()) {
        const std::string file = todo.back();
        todo.pop_back();
        if (!seen.insert(file).second) continue;
        if (auto uses = fileUses_.find(file); uses != fileUses_.end()) todo.insert(todo.end(), uses->second.begin(), uses->second.end());
        if (auto dir = directoryOf_.find(file); dir != directoryOf_.end()) {
            const std::set<std::string>& siblings = filesByDirectory_.at(dir->second);
            todo.insert(todo.end(), siblings.begin(), siblings.end());
        }
    }
    std::vector<std::pair<std::string, uint64_t>> all;
    for (const std::string& file : seen) {
        auto hash = textHashes_.find(file);
        all.emplace_back(file, hash == textHashes_.end() ? 0 : hash->second);
    }
    return all;
}

namespace {

const char kKeyPart = '\x1f';

void putU32(std::string& out, uint32_t v) {
    for (int i = 0; i < 4; i++) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void putText(std::string& out, const std::string& s) {
    putU32(out, static_cast<uint32_t>(s.size()));
    out += s;
}

// What a kept unit's image holds besides its modules' code.
struct UnitImage {
    std::vector<std::string> own;                       // its modules, by what they are
    std::vector<std::pair<int, std::string>> globals;   // a slot in its code, and the global it is
    std::vector<std::pair<int, std::string>> modules;   // a module index in its code, and the module
    std::vector<std::pair<int, std::string>> numbers;   // a class or library number, "lib:" and the library, or "own"
    std::map<std::string, std::string> facts;           // a file, and what the checker learned of its functions (R1)
    std::string code;
};

bool readImage(const std::string& data, UnitImage& image) {
    size_t at = 0;
    auto u32 = [&](uint32_t& v) {
        if (data.size() - at < 4) return false;
        v = 0;
        for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(static_cast<uint8_t>(data[at++])) << (8 * i);
        return true;
    };
    auto text = [&](std::string& s) {
        uint32_t n = 0;
        if (!u32(n) || data.size() - at < n) return false;
        s = data.substr(at, n);
        at += n;
        return true;
    };
    uint32_t n = 0;
    if (!u32(n)) return false;
    for (uint32_t i = 0; i < n; i++) {
        std::string key;
        if (!text(key)) return false;
        image.own.push_back(key);
    }
    for (auto* table : {&image.globals, &image.modules, &image.numbers}) {
        if (!u32(n)) return false;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t number = 0;
            std::string key;
            if (!u32(number) || !text(key)) return false;
            table->emplace_back(static_cast<int>(number), key);
        }
    }
    if (!u32(n)) return false;
    for (uint32_t i = 0; i < n; i++) {
        std::string file, facts;
        if (!text(file) || !text(facts)) return false;
        image.facts[file] = facts;
    }
    image.code = data.substr(at);
    return true;
}

}  // namespace

// The slot of the global a key names, in this compile; one the unit's own compile made up (an
// object's hidden class) is made again. -1 if there is no such global any more.
int CompilerSession::slotFor(const std::string& key) {
    std::vector<std::string> parts;
    for (size_t start = 0;;) {
        const size_t end = key.find(kKeyPart, start);
        parts.push_back(key.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (parts.size() < 3) return -1;
    std::unordered_map<std::string, GlobalName>* table = nullptr;
    if (parts[0] == "n") {
        auto ns = namespaces_.find(parts[1]);
        if (ns == namespaces_.end()) return -1;
        table = &ns->second->names;
    } else if (parts[0] == "p") {
        table = &privates_[parts[1]];
    } else {
        return -1;
    }
    GlobalName& entry = (*table)[parts[2]];
    if (parts.size() == 4) return entry.formSlot(parts[3]);
    if (entry.slot < 0) entry.slot = newSlot(parts[2]);
    return entry.slot;
}

int CompilerSession::reuseUnit(const std::string& unit, const std::vector<std::string>& files, const std::vector<int>& modules,
                               const std::vector<const std::vector<StmtPtr>*>& statements, const std::vector<std::string>& names,
                               const std::string& directory, Namespace* ns) {
    if (!keepingUnits_) return 0;
    // Only what was settled to be put back, its own files and what it depends on as it was.
    auto plannedUnit = planned_.find(unit);
    if (plannedUnit == planned_.end()) return 0;
    KeptUnit kept = std::move(plannedUnit->second);
    planned_.erase(plannedUnit);
    const int reused = putBack(kept.image, files, modules, statements, names, directory, ns);
    if (reused == 1) compileTimes.unitsReused++;
    return reused;
}

// Puts a unit's kept code back as modules `modules` of the program: what compiling its files
// would have noted for the rest is noted, and its code given this compile's numbers.
int CompilerSession::putBack(const std::string& data, const std::vector<std::string>& files, const std::vector<int>& modules,
                             const std::vector<const std::vector<StmtPtr>*>& statements, const std::vector<std::string>& names,
                             const std::string& directory, Namespace* ns) {
    UnitImage image;
    if (!readImage(data, image) || image.own.size() != modules.size()) return 0;
    for (size_t i = 0; i < modules.size(); i++)
        if (image.own[i] != moduleKeys_[static_cast<size_t>(modules[i])]) return 0;
    std::vector<FunctionProto*> protos;
    try {
        protos = readModules(program, vm_, image.code);
    } catch (const std::exception&) {
        return 0;
    }
    if (protos.size() != modules.size()) return 0;

    // From here it is as if it had been compiled: what compiling it notes for the rest.
    for (size_t i = 0; i < files.size(); i++) predeclare(*ns, files[i], names[i], *statements[i], false);
    for (size_t i = 0; i < files.size(); i++) {
        ModuleCompiler declaring(*this, names[i], directory, ns, files[i]);
        declaring.declareKept(*statements[i]);
    }
    for (const auto& [old, key] : image.modules)
        if (key.rfind("<worker>", 0) == 0 && moduleByKey_.count(key) == 0) loadWorker(key.substr(8), "", names[0], Node());

    std::unordered_map<int, int> globals, moduleIndex, numbers;
    for (const auto& [old, key] : image.globals) {
        const int slot = slotFor(key);
        if (slot < 0) return 2;
        globals[old] = slot;
    }
    for (const auto& [old, key] : image.modules) {
        auto found = moduleByKey_.find(key);
        if (found == moduleByKey_.end()) return 2;
        moduleIndex[old] = found->second;
    }
    for (const auto& [old, key] : image.numbers) {
        if (key.rfind("lib:", 0) == 0) {
            numbers[old] = libraryNumber(key.substr(4), names[0], 1);
        } else {
            if (classCount_ >= 0xFFFF) return 2;
            numbers[old] = ++classCount_;
        }
    }
    bool missing = false;
    auto mapped = [&missing](const std::unordered_map<int, int>& map) {
        return [&missing, &map](int v) {
            auto found = map.find(v);
            if (found == map.end()) {
                missing = true;
                return v;
            }
            return found->second;
        };
    };
    const Renumbering into{mapped(globals), mapped(moduleIndex), mapped(numbers)};
    for (FunctionProto* proto : protos) renumber(*proto, into, vm_);
    if (missing) return 2;
    try {
        for (FunctionProto* proto : protos) verifyModule(*proto, program);
    } catch (const std::exception&) {
        return 2;
    }
    for (size_t i = 0; i < modules.size(); i++) program.modules[static_cast<size_t>(modules[i])] = protos[i];
    return 1;
}

// Before checking: each unit whose kept code holds, everything it was compiled from being as it
// was, will be put back, so its bodies are not checked again. A folder imported whole is one
// unit; any other file is one by itself.
void CompilerSession::planReuse() {
    if (!keepingUnits_) return;
    std::set<std::string> inFolders;
    for (const auto& [folder, files] : foldersGathered_) {
        inFolders.insert(files.begin(), files.end());
        planUnit("folder:" + folder, files);
    }
    for (const auto& [file, directory] : directoryOf_)
        if (inFolders.count(file) == 0 && file.rfind("<worker>", 0) != 0) planUnit("file:" + file, {file});
}

void CompilerSession::planUnit(const std::string& unit, const std::vector<std::string>& files) {
    KeptUnit kept;
    UnitImage image;
    if (!keptUnit(unit, kept) || kept.own.size() != files.size() || !readImage(kept.image, image)) return;
    std::vector<std::string> sorted(files.begin(), files.end());
    std::sort(sorted.begin(), sorted.end());
    for (size_t i = 0; i < sorted.size(); i++) {
        auto text = textHashes_.find(sorted[i]);
        if (kept.own[i].path != sorted[i] || text == textHashes_.end() || text->second != kept.own[i].text) return;
    }
    const std::vector<std::string> uses = dependencies(files);
    if (uses.size() != kept.uses.size()) return;
    for (size_t i = 0; i < uses.size(); i++)
        if (kept.uses[i].path != uses[i]) return;
    for (const std::string& file : files) {
        auto facts = image.facts.find(file);
        checker->skipBodiesOf(file, facts == image.facts.end() ? std::string() : facts->second);
    }
    planned_[unit] = std::move(kept);
    plannedFiles_[unit] = files;
}

// After checking: a unit planned to be put back holds only if every file it depends on shows
// the summary it was compiled against. Those of files checked this time are worked out; one
// that differs makes the unit be checked and compiled after all, which may change its own
// summary in turn, and so on until nothing more changes.
void CompilerSession::settleReuse() {
    if (!keepingUnits_) return;
    for (;;) {
        summaries_.clear();
        for (const auto& [unit, kept] : planned_)
            for (const KeptFile& f : kept.own) summaries_[f.path] = f.summary;
        for (const auto& [file, directory] : directoryOf_) {
            if (summaries_.count(file) != 0 || file.rfind("<worker>", 0) == 0) continue;
            std::string seen = std::to_string(skeletons_[file]) + "\n" + checker->summaryFacts(file);
            summaries_[file] = textHash(seen);
        }
        std::vector<std::string> stale;
        for (const auto& [unit, kept] : planned_)
            for (const KeptFile& f : kept.uses) {
                auto now = summaries_.find(f.path);
                if (now == summaries_.end() || now->second != f.summary) {
                    stale.push_back(unit);
                    break;
                }
            }
        if (stale.empty()) return;
        for (const std::string& unit : stale) {
            planned_.erase(unit);
            checkSkipped(plannedFiles_[unit]);
        }
    }
}

// A unit planned to be put back that is compiled after all: its bodies are checked first.
void CompilerSession::checkSkipped(const std::vector<std::string>& files) {
    const size_t before = checker->diagnostics.size();
    for (const std::string& file : files) checker->checkBodiesOf(file);
    reportNew(before);
}

// Keeps the units compiled this time, once the whole program has been: their code's numbers
// listed now, their code written by the cache's own thread as the program runs.
// What each global's slot and each library's number stand for, for the images of units.
void CompilerSession::numberNames(std::unordered_map<int, std::string>& slotKeys, std::unordered_map<int, std::string>& libraries) const {
    for (const auto& [nsKey, ns] : namespaces_)
        for (const auto& [name, entry] : ns->names) {
            const std::string key = std::string("n") + kKeyPart + nsKey + kKeyPart + name;
            if (entry.slot >= 0) slotKeys[entry.slot] = key;
            for (const auto& [signature, slot] : entry.forms) slotKeys[slot] = key + kKeyPart + signature;
        }
    for (const auto& [fileKey, names] : privates_)
        for (const auto& [name, entry] : names) {
            const std::string key = std::string("p") + kKeyPart + fileKey + kKeyPart + name;
            if (entry.slot >= 0) slotKeys[entry.slot] = key;
            for (const auto& [signature, slot] : entry.forms) slotKeys[slot] = key + kKeyPart + signature;
        }
    for (const auto& [library, number] : libraryIds_) libraries[number] = library;
}

// A unit's image: what its code's numbers stand for, and what the checker learned of its
// functions (`head`), and its modules (`protos`, written after the head). False if something
// in it cannot be named again, and it is to be compiled each time.
bool CompilerSession::unitImage(const ToKeep& unit, const std::unordered_map<int, std::string>& slotKeys,
                                const std::unordered_map<int, std::string>& libraries, std::string& head,
                                std::vector<const FunctionProto*>& protos) {
    std::set<int> globals, modules, numbers;
    const Renumbering listed{[&](int v) { globals.insert(v); return v; }, [&](int v) { modules.insert(v); return v; },
                             [&](int v) { numbers.insert(v); return v; }};
    bool whole = true;
    for (int index : unit.modules) {
        FunctionProto* proto = program.modules[static_cast<size_t>(index)];
        if (proto == nullptr) {
            whole = false;
        } else {
            renumber(*proto, listed, vm_);
            protos.push_back(proto);
        }
    }
    putU32(head, static_cast<uint32_t>(unit.modules.size()));
    for (int index : unit.modules) putText(head, moduleKeys_[static_cast<size_t>(index)]);
    putU32(head, static_cast<uint32_t>(globals.size()));
    for (int slot : globals) {
        auto key = slotKeys.find(slot);
        if (key == slotKeys.end()) whole = false;
        putU32(head, static_cast<uint32_t>(slot));
        putText(head, key == slotKeys.end() ? std::string() : key->second);
    }
    putU32(head, static_cast<uint32_t>(modules.size()));
    for (int index : modules) {
        const bool known = static_cast<size_t>(index) < moduleKeys_.size() && !moduleKeys_[static_cast<size_t>(index)].empty();
        if (!known) whole = false;
        putU32(head, static_cast<uint32_t>(index));
        putText(head, known ? moduleKeys_[static_cast<size_t>(index)] : std::string());
    }
    putU32(head, static_cast<uint32_t>(numbers.size()));
    for (int number : numbers) {
        auto library = libraries.find(number);
        putU32(head, static_cast<uint32_t>(number));
        putText(head, library == libraries.end() ? std::string("own") : "lib:" + library->second);
    }
    putU32(head, static_cast<uint32_t>(unit.files.size()));
    for (const std::string& file : unit.files) {
        putText(head, file);
        putText(head, checker->inPlaceFacts(file));
    }
    return whole;
}

// Keeps the units compiled this time, once the whole program has been: their code's numbers
// listed now, their code written by the cache's own thread as the program runs.
void CompilerSession::keepUnits() {
    if (toKeep_.empty()) return;
    std::unordered_map<int, std::string> slotKeys, libraries;
    numberNames(slotKeys, libraries);
    for (const ToKeep& unit : toKeep_) {
        std::string head;
        std::vector<const FunctionProto*> protos;
        bool whole = unitImage(unit, slotKeys, libraries, head, protos);
        KeptUnit kept;
        std::vector<std::string> own(unit.files.begin(), unit.files.end());
        std::sort(own.begin(), own.end());
        for (const std::string& file : own) {
            auto summary = summaries_.find(file);
            if (summary == summaries_.end()) whole = false;
            else kept.own.push_back(KeptFile{file, textHashes_[file], summary->second});
        }
        for (const std::string& file : dependencies(unit.files)) {
            auto summary = summaries_.find(file);
            if (summary == summaries_.end()) whole = false;
            else kept.uses.push_back(KeptFile{file, 0, summary->second});
        }
        if (!whole) continue;  // something it says cannot be named again: compiled each time
        keepUnit(unit.unit, std::move(kept), [head, protos] {
            std::ostringstream code;
            writeModules(protos, code);
            return head + code.str();
        });
    }
    toKeep_.clear();
}

// ---- The standard library, compiled when astra.exe is built (D145, point 3) ----
//
// The build compiles the prelude once, as astra.exe's last step, and attaches its image to the
// file. A session then puts it back as it would a kept unit: the prelude is read, for what it
// declares, but its bodies are not checked and nothing of it is compiled. The image is of this
// very file's prelude, so only a hash of the prelude's text is checked against it.

std::string CompilerSession::preludeImage() {
    std::unordered_map<int, std::string> slotKeys, libraries;
    numberNames(slotKeys, libraries);
    std::string head;
    std::vector<const FunctionProto*> protos;
    if (!unitImage(ToKeep{"<prelude>", {"<prelude>"}, {0}}, slotKeys, libraries, head, protos)) return {};
    std::string image;
    putU32(image, 1);  // the form of what follows
    image += std::to_string(textHash(preludeSource())) + "\n";
    std::ostringstream code;
    writeModules(protos, code);
    return image + head + code.str();
}

// The prelude, the same as parseSource gives it, but parsed a file at a time on threads.
static std::vector<StmtPtr> parsePrelude(const std::string& prelude) {
    PartTimer timed(compileTimes.parse);
    const size_t base = prelude.size() - kStdSourceSize;  // what comes before the standard library's files
    std::vector<std::pair<size_t, size_t>> parts{{0, base}};
    size_t from = base;
    for (size_t i = 0; i < kStdFileCount; i++) {
        parts.emplace_back(from, base + kStdFileEnds[i]);
        from = base + kStdFileEnds[i];
    }
    std::vector<int> lines;
    int line = 1;
    size_t counted = 0;
    for (const auto& [start, end] : parts) {
        line += static_cast<int>(std::count(prelude.begin() + static_cast<std::ptrdiff_t>(counted),
                                            prelude.begin() + static_cast<std::ptrdiff_t>(start), '\n'));
        counted = start;
        lines.push_back(line);
    }
    std::vector<std::vector<StmtPtr>> parsed(parts.size());
    std::vector<std::unique_ptr<CompileError>> errors(parts.size());
    std::atomic<size_t> next{0};
    auto work = [&] {
        for (size_t i = next++; i < parts.size(); i = next++) {
            try {
                Lexer lexer(prelude, "<std>", parts[i].first, parts[i].second, lines[i]);
                std::vector<Token> tokens = lexer.scan();
                Parser parser(tokens, "<std>");
                parsed[i] = parser.parseProgram();
            } catch (const CompileError& e) {
                errors[i] = std::make_unique<CompileError>(e);
            }
        }
    };
    const size_t cores = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> threads;
    for (size_t i = 0; i + 1 < std::min<size_t>({parts.size(), cores, 8}); i++) threads.emplace_back(work);
    work();
    for (std::thread& t : threads) t.join();
    std::vector<StmtPtr> statements;
    for (size_t i = 0; i < parts.size(); i++) {
        if (errors[i]) throw CompileError(*errors[i]);
        for (StmtPtr& s : parsed[i]) statements.push_back(std::move(s));
    }
    return statements;
}

std::string CompilerSession::preludeSource() {
    std::string prelude = kPrelude;
    prelude.append(reinterpret_cast<const char*>(kStdSource), kStdSourceSize);
    return prelude;
}

bool CompilerSession::usePrelude(const std::string& prelude, const std::string& image) {
    if (image.size() < 4 || image[0] != 1 || image[1] != 0 || image[2] != 0 || image[3] != 0) return false;
    const size_t line = image.find('\n', 4);
    if (line == std::string::npos || image.substr(4, line - 4) != std::to_string(textHash(prelude))) return false;
    const std::string kept = image.substr(line + 1);
    UnitImage parts;
    if (!readImage(kept, parts) || parts.own.size() != 1) return false;
    // Read for what it declares; its bodies are as they were when astra.exe was built. Its files
    // are parsed apart, a few at a time, each at its own lines, and joined in order.
    sources_.push_back(std::make_unique<std::vector<StmtPtr>>(parsePrelude(prelude)));
    const std::vector<StmtPtr>& statements = *sources_.back();
    gather("<prelude>", "<std>", "<builtin>", statements, builtins_, true);
    checker->skipBodiesOf("<prelude>", parts.facts["<prelude>"]);
    check();
    program.modules.push_back(nullptr);
    noteModule(0, "<prelude>");
    if (putBack(kept, {"<prelude>"}, {0}, {&statements}, {"<std>"}, "", builtins_) != 1)
        throw CompileError("<std>", 1, 1, "the standard library attached to astra.exe could not be put back; build astra.exe again");
    compileTimes.preludeAttached = true;
    return true;
}

int CompilerSession::compileSource(const std::string& source, const std::string& name,
                                   const std::string& baseDirectory, bool shell) {
    forgetPaths();
    try {
        FunctionProto* proto = compileUnit(source, name, baseDirectory, name, shell);
        checkConstAssignments();
        program.modules.push_back(proto);
        noteModule(static_cast<int>(program.modules.size()) - 1, "<input>");
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
        fs::path canonical = canonicalPath(folder, ec);
        if (ec) canonical = fs::absolute(folder, ec);
        key = canonical.string();
    }
    auto found = namespaces_.find(key);
    if (found != namespaces_.end()) return found->second.get();

    auto ns = std::make_unique<Namespace>();
    ns->directory = key;
    ns->library = "<program>";
    for (const std::string& library : libraryPaths) {
        fs::path root = canonicalPath(fs::path(library), ec);
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
// an import. Declaring a name twice in one folder is an error (the shell may declare again).
void CompilerSession::predeclare(Namespace& ns, const std::string& fileKey, const std::string& file,
                                 const std::vector<StmtPtr>& statements, bool shell) {
    auto declare = [&](const std::string& name, bool isVal, const Stmt& at, const FunctionDecl* fn = nullptr) {
        if (name == "_") return;
        const bool isPrivate = at.visibility == GlobalName::Private;
        auto& mine = privates_[fileKey];
        if (!shell) {
            // The same name both private and not, in one file.
            auto other = isPrivate ? ns.names.find(name) : mine.find(name);
            auto end = isPrivate ? ns.names.end() : mine.end();
            if (other != end && other->second.declared && (isPrivate ? other->second.file == file : true))
                throw CompileError(file, at.line, at.col, "'" + name + "' is already declared in this file");
        }
        GlobalName& entry = isPrivate ? mine[name] : ns.names[name];
        if (entry.declared && fn != nullptr && entry.isFunction && !shell) {
            // Another form of a function: it differs from the others in its parameters' types.
            const std::string signature = signatureText(*fn);
            const bool repeated = entry.forms.empty() ? signature == entry.firstSignature : entry.formSlot(signature) >= 0;
            if (repeated)
                throw CompileError(file, at.line, at.col,
                                   "'" + name + signature + "' is declared twice" + (entry.file != file ? " (also in " + entry.file + ")" : "") +
                                       "; the forms of a name differ in their parameters' types");
            if (entry.forms.empty()) entry.forms.emplace_back(entry.firstSignature, newSlot(name + entry.firstSignature));
            entry.forms.emplace_back(signature, newSlot(name + signature));
            entry.returnType = 0;  // which form a call gets is not known here
            return;
        }
        if (entry.declared) {
            if (entry.isVal) throw CompileError(file, at.line, at.col, "cannot redeclare val '" + name + "'");
            if (!shell)
                throw CompileError(file, at.line, at.col,
                                   "'" + name + "' is already declared" + (entry.file != file ? " in " + entry.file : "") +
                                       "; a name can be declared only once in a folder");
        }
        if (entry.slot < 0) entry.slot = newSlot(name);
        entry.declared = true;
        entry.isVal = isVal;
        entry.visibility = static_cast<GlobalName::Visibility>(at.visibility);
        entry.file = file;
        entry.isFunction = fn != nullptr;
        if (fn != nullptr) entry.firstSignature = signatureText(*fn);
        entry.returnType = 0;
        if (fn != nullptr && !shell) {
            const TypeAnnotation& result = fn->returnType;
            if (!result.nullable && !result.typeParam && result.args.empty() && result.result.empty()) {
                if (result.name == "int") entry.returnType = kTypeInt;
                else if (result.name == "float") entry.returnType = kTypeFloat;
                else if (result.name == "bool") entry.returnType = kTypeBool;
                else if (result.name == "string") entry.returnType = kTypeString;
            }
        }
    };

    for (const StmtPtr& stmt : statements) {
        if (stmt->kind == StmtKind::Var) {
            const auto& var = static_cast<const VarStmt&>(*stmt);
            if (var.isExternal) continue;  // a property of a built-in type: no name of its own
            if (var.pattern.empty()) declare(var.name, var.isVal, var);
            for (const std::string& name : var.pattern) declare(name, var.isVal, var);
        } else if (stmt->kind == StmtKind::Function) {
            // An extension function is added to a type; it declares no name of its own. An
            // external one names what the C++ provides, which needs no slot.
            if (!static_cast<const FunctionStmt&>(*stmt).receiver.empty() || static_cast<const FunctionStmt&>(*stmt).isExternal) continue;
            declare(static_cast<const FunctionStmt&>(*stmt).fn->name, false, *stmt, static_cast<const FunctionStmt&>(*stmt).fn.get());
        } else if (stmt->kind == StmtKind::Class) {
            const auto& cls = static_cast<const ClassStmt&>(*stmt);
            if (cls.isExternal) continue;  // the C++'s own global, which needs no slot
            declare(cls.name, cls.isObject, cls);
        }
    }
}

// A file's statements, parsed the first time they are asked for and kept: the checker reads
// them, then the compiler. Null if the file cannot be read.
std::vector<StmtPtr>* CompilerSession::parseFile(const std::string& key, const std::string& path, const std::string& name) {
    auto found = parsed_.find(key);
    if (found != parsed_.end()) return found->second.get();
    if (auto ahead = prefetched_.find(key); ahead != prefetched_.end()) {
        // Read and parsed ahead: taken as it was found then, and what went wrong is said now.
        Prefetched done = std::move(ahead->second);
        prefetched_.erase(ahead);
        if (!done.read) return nullptr;
        SourceRead read{path, textHash(done.source), done.size, done.time};
        textHashes_[key] = read.hash;
        skeletons_[key] = done.skeleton;
        if (done.time != 0) sourcesRead_.push_back(std::move(read));
        if (done.error) throw CompileError(*done.error);
        std::vector<StmtPtr>* raw = done.statements.get();
        parsed_[key] = std::move(done.statements);
        return raw;
    }
    std::string source;
    if (!readFile(path, source)) return nullptr;
    SourceRead read{path, textHash(source)};
    textHashes_[key] = read.hash;
    if (fileStamp(path, read.size, read.time)) sourcesRead_.push_back(std::move(read));
    auto statements = std::make_unique<std::vector<StmtPtr>>(parseSource(source, name, true, &skeletons_[key]));
    std::vector<StmtPtr>* raw = statements.get();
    parsed_[key] = std::move(statements);
    return raw;
}

// Where an import leads: a file (as written, or with ".astra" added) or a folder, looked for
// next to the importing file first, then in the library directories. A file never imports
// itself: a script called Shapes.astra that says `import Shapes` means the library, so the
// importing file is passed over.
bool CompilerSession::findImport(const std::string& path, const std::string& fromDirectory, const std::string& file,
                                 std::string& found, bool& folder) const {
    std::vector<fs::path> places;
    places.push_back(fs::path(fromDirectory));
    for (const std::string& library : libraryPaths) places.push_back(fs::path(library));
    std::error_code ec;
    fs::path self = canonicalPath(fs::path(fromDirectory) / fs::path(file), ec);
    auto usable = [&](const fs::path& candidate) {
        if (pathKind(candidate) != 1) return false;
        return canonicalPath(candidate, ec) != self;
    };
    for (const fs::path& place : places) {
        fs::path candidate = place / fs::path(path);
        fs::path withExtension = candidate;
        withExtension += ".astra";
        folder = false;
        if (usable(candidate)) {
            found = candidate.string();
            return true;
        }
        if (usable(withExtension)) {
            found = withExtension.string();
            return true;
        }
        if (pathKind(candidate) == 2) {
            found = canonicalPath(candidate, ec).string();
            folder = true;
            return true;
        }
    }
    return false;
}

// Before any of a program is compiled: a file and everything it imports, each parsed once and
// handed to the checker, which then checks them together (M6.5 part 3), so that compiling
// can use what it worked out. What cannot be found or read is passed over here; compiling the
// import reports it.
void CompilerSession::gather(const std::string& key, const std::string& name, const std::string& directory,
                             const std::vector<StmtPtr>& statements, Namespace* ns, bool prelude) {
    // Each input of the shell is a unit of its own, under the one key, which takes over the
    // imports of the inputs before it.
    const bool shell = gatheringShell_;
    gatheringShell_ = false;
    if (!gathered_.insert(key).second && !shell) return;
    checker->addUnit(statements, name, key, ns, prelude, shell);
    if (!prelude && !shell) {
        directoryOf_[key] = directory;
        filesByDirectory_[directory].insert(key);
    }
    // What it imports is read and parsed ahead, all at once, then gathered in order.
    std::vector<std::pair<std::string, std::string>> ahead;
    for (const StmtPtr& stmt : statements) {
        if (stmt->kind != StmtKind::Import) continue;
        const auto& s = static_cast<const ImportStmt&>(*stmt);
        std::string found;
        bool folder = false;
        if (!findImport(s.path, directory, name, found, folder)) continue;
        if (folder) {
            for (const fs::path& path : folderFiles(found)) ahead.emplace_back(fileKeyOf(path), path.filename().string());
        } else {
            const std::string fileKey = fileKeyOf(fs::path(found));
            ahead.emplace_back(fileKey, fs::path(fileKey).filename().string());
        }
    }
    prefetch(ahead);
    for (const StmtPtr& stmt : statements) {
        if (stmt->kind != StmtKind::Import) continue;
        const auto& s = static_cast<const ImportStmt&>(*stmt);
        std::string found;
        bool folder = false;
        if (!findImport(s.path, directory, name, found, folder)) continue;
        Namespace* imported = nullptr;
        if (folder) {
            imported = namespaceOf(found);
            noteFolder(found, folderFiles(found));
            std::vector<std::string>& whole = foldersGathered_[found];
            whole.clear();
            for (const fs::path& path : folderFiles(found)) {
                const std::string fileKey = fileKeyOf(path);
                const std::string fileName = path.filename().string();
                fileUses_[key].insert(fileKey);
                whole.push_back(fileKey);
                if (std::vector<StmtPtr>* parsed = parseFile(fileKey, fileKey, fileName))
                    gather(fileKey, fileName, found, *parsed, imported, false);
            }
        } else {
            const std::string fileKey = fileKeyOf(fs::path(found));
            const fs::path canonical(fileKey);
            const std::string parent = canonical.parent_path().string();
            imported = namespaceOf(parent);
            fileUses_[key].insert(fileKey);
            if (std::vector<StmtPtr>* parsed = parseFile(fileKey, fileKey, canonical.filename().string()))
                gather(fileKey, canonical.filename().string(), parent, *parsed, imported, false);
        }
        checker->addImport(key, s.alias, s.all, imported);
    }
}

void CompilerSession::prefetch(const std::vector<std::pair<std::string, std::string>>& files) {
    std::vector<std::pair<std::string, std::string>> todo;
    for (const auto& [key, name] : files)
        if (parsed_.count(key) == 0 && prefetched_.count(key) == 0) {
            prefetched_[key];
            todo.emplace_back(key, name);
        }
    if (todo.size() < 2) {
        for (const auto& [key, name] : todo) prefetched_.erase(key);  // as well parsed when gathered
        return;
    }
    PartTimer timed(compileTimes.parse);
    std::vector<Prefetched*> slots;
    for (const auto& [key, name] : todo) slots.push_back(&prefetched_[key]);
    std::atomic<size_t> next{0};
    auto work = [&] {
        for (size_t i = next++; i < todo.size(); i = next++) {
            Prefetched& p = *slots[i];
            const std::string& key = todo[i].first;
            if (!readFile(key, p.source)) continue;
            p.read = true;
            if (!fileStamp(key, p.size, p.time)) p.time = 0;
            try {
                p.statements = std::make_unique<std::vector<StmtPtr>>(parseSource(p.source, todo[i].second, false, &p.skeleton));
            } catch (const CompileError& e) {
                p.error = std::make_unique<CompileError>(e);
            }
        }
    };
    const size_t cores = std::max(1u, std::thread::hardware_concurrency());
    const size_t helpers = std::min<size_t>({todo.size() - 1, cores - 1, 7});
    std::vector<std::thread> threads;
    for (size_t i = 0; i < helpers; i++) threads.emplace_back(work);
    work();
    for (std::thread& t : threads) t.join();
}

// Runs the type checker on what has been added, and makes what it finds in the program a
// compile error: the first, with the rest listed after it (M6.5 part 3, step 5).
void CompilerSession::check() {
    const size_t before = checker->diagnostics.size();
    {
        PartTimer timed(compileTimes.check);
        checker->run();
    }
    reportNew(before);
}

// What the checker found since `before` in the program, as a compile error.
void CompilerSession::reportNew(size_t before) {
    if (!stopAtTypeErrors) return;
    // What `astra check --std` finds in the standard library is listed, never a program's error.
    std::vector<const CheckDiagnostic*> found;
    for (size_t i = before; i < checker->diagnostics.size(); i++)
        if (checker->diagnostics[i].file != "<std>") found.push_back(&checker->diagnostics[i]);
    if (found.empty()) return;
    CompileError error(found[0]->file, found[0]->line, found[0]->col, found[0]->message);
    for (size_t i = 1; i < found.size(); i++)
        error.more.push_back(found[i]->file + ":" + std::to_string(found[i]->line) + ":" + std::to_string(found[i]->col) +
                             ": error: " + found[i]->message);
    throw error;
}

// Source text that is not a file (the standard library, the shell, `astra -e`): parsed, and
// checked with what it imports; then compiled. An input of the shell that does not compile is
// taken back from the checker, as it is from the program.
FunctionProto* CompilerSession::compileUnit(const std::string& source, const std::string& name,
                                            const std::string& directory, const std::string& fileKey, bool shell,
                                            Namespace* into) {
    sources_.push_back(std::make_unique<std::vector<StmtPtr>>(parseSource(source, name)));
    std::vector<StmtPtr>& statements = *sources_.back();
    Namespace* ns = into != nullptr ? into : namespaceOf(directory);
    gatheringShell_ = shell;
    try {
        gather(fileKey, name, directory, statements, ns, directory == "<builtin>");
        check();
        return compileStatements(statements, name, directory, fileKey, shell, ns);
    } catch (...) {
        gatheringShell_ = false;
        if (shell) checker->forgetShellUnit();
        throw;
    }
}

FunctionProto* CompilerSession::compileStatements(const std::vector<StmtPtr>& statements, const std::string& name,
                                                  const std::string& directory, const std::string& fileKey, bool shell,
                                                  Namespace* ns, bool predeclared) {
    PartTimer timed(compileTimes.compile);
    if (!predeclared) predeclare(*ns, fileKey, name, statements, shell);
    ModuleCompiler compiler(*this, name, directory == "<builtin>" ? "" : directory, ns, fileKey);
    if (!shell) return compiler.compile(statements, shell);
    compiler.useImports(shellImports_);
    FunctionProto* proto = compiler.compile(statements, shell);
    shellImports_ = compiler.importsNow();
    return proto;
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
    const fs::path canonical = canonicalPath(candidate, ec);
    const std::string key = "<worker>" + canonical.string();

    auto found = modules_.find(key);
    if (found != modules_.end()) return found->second;
    const std::string name = canonical.filename().string();
    const std::string directory = canonical.parent_path().string();
    std::vector<StmtPtr>* statements = parseFile(key, canonical.string(), name);
    if (statements == nullptr) throw CompileError(file, at.line, at.col, "cannot read '" + path + "'");

    int index = static_cast<int>(program.modules.size());
    if (index > 0xFFFF) throw CompileError(file, at.line, at.col, "too many modules");
    program.modules.push_back(nullptr);
    modules_[key] = index;
    noteModule(index, key);
    std::unique_ptr<Namespace>& ns = namespaces_[key];
    if (ns == nullptr) {
        ns = std::make_unique<Namespace>();
        ns->directory = directory;
        ns->library = "<program>";
    }
    try {
        gather(key, name, directory, *statements, ns.get(), false);
        check();
        program.modules[static_cast<size_t>(index)] = compileStatements(*statements, name, directory, key, false, ns.get());
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
    const std::string key = fileKeyOf(fs::path(path));
    const fs::path canonical(key);

    auto found = modules_.find(key);
    if (found != modules_.end()) return found->second;

    std::vector<StmtPtr>* statements = parseFile(key, key, canonical.filename().string());
    if (statements == nullptr)
        throw CompileError(file, at ? at->line : 1, at ? at->col : 1, "cannot read '" + displayPath + "'");

    // Registered before compiling so that circular imports resolve to this slot.
    int index = static_cast<int>(program.modules.size());
    if (index > 0xFFFF) throw CompileError(file, at ? at->line : 1, at ? at->col : 1, "too many modules");
    program.modules.push_back(nullptr);
    modules_[key] = index;
    noteModule(index, key);
    try {
        const std::string directory = canonical.parent_path().string();
        const std::string name = canonical.filename().string();
        Namespace* ns = namespaceOf(directory);
        const int reused = reuseUnit("file:" + key, {key}, {index}, {statements}, {name}, directory, ns);
        if (reused == 1) return index;
        checkSkipped({key});
        program.modules[static_cast<size_t>(index)] = compileStatements(*statements, name, directory, key, false, ns, reused == 2);
        if (keepingUnits_) toKeep_.push_back(ToKeep{"file:" + key, {key}, {index}});
        compileTimes.unitsCompiled++;
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
        std::string key, name;
        const std::vector<StmtPtr>* statements = nullptr;
        int index = 0;
    };
    std::vector<fs::path> paths = folderFiles(folder);
    noteFolder(folder, paths);
    if (paths.empty())
        throw CompileError(file, at.line, at.col, "the folder '" + displayPath + "' has no .astra files to import");

    std::vector<Unit> units;
    for (const fs::path& path : paths) {
        Unit unit;
        unit.key = fileKeyOf(path);
        auto found = modules_.find(unit.key);
        if (found != modules_.end()) {
            modules.push_back(found->second);
            continue;
        }
        unit.name = fs::path(unit.key).filename().string();
        unit.statements = parseFile(unit.key, unit.key, unit.name);
        if (unit.statements == nullptr)
            throw CompileError(file, at.line, at.col, "cannot read '" + displayPath + "/" + unit.name + "'");
        unit.index = static_cast<int>(program.modules.size());
        if (unit.index > 0xFFFF) throw CompileError(file, at.line, at.col, "too many modules");
        units.push_back(std::move(unit));
        program.modules.push_back(nullptr);
        modules_[units.back().key] = units.back().index;
        noteModule(units.back().index, units.back().key);
        modules.push_back(units.back().index);
    }

    if (units.empty()) return;

    // One more module holds what is set up ahead for all of them. It runs before any of them.
    const int header = static_cast<int>(program.modules.size());
    if (header > 0xFFFF) throw CompileError(file, at.line, at.col, "too many modules");
    program.modules.push_back(nullptr);
    noteModule(header, "folder:" + folder);
    modules.insert(modules.end() - static_cast<std::ptrdiff_t>(units.size()), header);
    auto shared = std::make_shared<std::vector<int>>(modules);
    for (const Unit& unit : units) units_[unit.key] = shared;

    Namespace* ns = namespaceOf(folder);
    try {
        // The whole folder is one unit kept compiled, when the whole of it is loaded here.
        std::vector<std::string> keys, fileNames;
        std::vector<int> indices{header};
        std::vector<const std::vector<StmtPtr>*> statements;
        for (const Unit& unit : units) {
            keys.push_back(unit.key);
            fileNames.push_back(unit.name);
            indices.push_back(unit.index);
            statements.push_back(unit.statements);
        }
        const int reused = units.size() == paths.size() ? reuseUnit("folder:" + folder, keys, indices, statements, fileNames, folder, ns) : 0;
        if (reused == 1) return;
        checkSkipped(keys);
        if (reused != 2)
            for (Unit& unit : units) predeclare(*ns, unit.key, unit.name, *unit.statements, false);
        if (keepingUnits_ && units.size() == paths.size()) toKeep_.push_back(ToKeep{"folder:" + folder, keys, indices});
        compileTimes.unitsCompiled++;

        std::vector<std::unique_ptr<ModuleCompiler>> compilers;
        std::vector<std::pair<size_t, const std::vector<StmtPtr>*>> files;
        std::vector<std::string> names;
        for (size_t i = 0; i < units.size(); i++) {
            compilers.push_back(std::make_unique<ModuleCompiler>(*this, units[i].name, folder, ns, units[i].key));
            files.emplace_back(i, units[i].statements);
            names.push_back(units[i].name);
        }

        // Imports first, then every function, then the classes, each after what it extends.
        FunctionState ahead;
        compilers[0]->beginScript(ahead, displayPath);
        for (size_t i = 0; i < units.size(); i++) {
            compilers[i]->continueScript(ahead);
            compilers[i]->noteEnums(*units[i].statements);
            compilers[i]->hoistImports(*units[i].statements);
        }
        for (size_t i = 0; i < units.size(); i++) {
            compilers[i]->continueScript(ahead);
            compilers[i]->hoistFunctions(*units[i].statements);
        }
        compilers.back()->defineForms();
        const std::vector<ModuleCompiler::UnitClass> classes = ModuleCompiler::orderClasses(files, names);
        for (const ModuleCompiler::UnitClass& cls : classes) {
            compilers[cls.file]->continueScript(ahead);
            compilers[cls.file]->hoistClass(*cls.stmt);
        }
        for (size_t i = 0; i < units.size(); i++) {
            compilers[i]->continueScript(ahead);
            compilers[i]->hoistExtensions(*units[i].statements);
        }
        compilers[0]->endScript(ahead);
        program.modules[static_cast<size_t>(header)] = ahead.proto;

        for (size_t i = 0; i < units.size(); i++) {
            FunctionState rest;
            compilers[i]->beginScript(rest, units[i].name);
            compilers[i]->compileRemainder(*units[i].statements, false);
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
    std::string found;
    bool folder = false;
    if (!findImport(path, fromDirectory, file, found, folder))
        throw CompileError(file, at.line, at.col, "cannot find module '" + path + "'");
    if (folder) {
        loadFolder(found, path, file, at, modules);
        return namespaceOf(found);
    }
    const std::string key = fileKeyOf(fs::path(found));
    auto unit = units_.find(key);
    if (unit != units_.end()) {
        // A file that was loaded as part of its folder runs as part of it.
        modules.insert(modules.end(), unit->second->begin(), unit->second->end());
    } else {
        modules.push_back(loadModule(found, path, file, &at));
    }
    return namespaceOf(fs::path(key).parent_path().string());
}

// A function may assign to a global that is only declared const further down, so
// assignments are checked once everything has been compiled.
void CompilerSession::checkConstAssignments() {
    for (const auto& [global, name, file, line, col] : globalAssignments_)
        if (global->isVal) throw CompileError(file, line, col, "cannot assign to val '" + name + "'");
        else if (global->isFunction && stopAtTypeErrors)  // astra check: the checker lists it with the rest
            throw CompileError(file, line, col, "'" + name + "' is a function, and the name of a function is never given another value; " +
                                                    "to change what is called, keep it in a var of a function type");
}

}  // namespace astra
