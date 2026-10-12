#include "compiler.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

#include "parser.h"
#include "vm.h"

namespace astra {

namespace fs = std::filesystem;

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
            return operand == kTypeInt || operand == kTypeFloat ? operand : 0;
        }
        default: return 0;
    }
}

struct LoopState {
    LoopState* enclosing;
    int start;       // address that `continue` jumps to
    int scopeDepth;  // locals deeper than this are discarded by break/continue
    int tryDepth;
    std::vector<int> breakJumps;
};

struct FunctionState {
    FunctionState* enclosing = nullptr;
    FunctionKind kind = FunctionKind::Script;
    FunctionProto* proto = nullptr;

    std::vector<Local> locals;
    std::map<std::pair<int, uint64_t>, int> constantIndex;

    int scopeDepth = 0;
    LoopState* loop = nullptr;
    int tryDepth = 0;  // try blocks currently open in this function
    TypeSpec returnType;  // untyped unless the function declares one
};

struct ClassState {
    ClassState* enclosing;
    bool hasSuper = false;

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
    ModuleCompiler(CompilerSession& session, std::string file, std::string directory)
        : session_(session), file_(std::move(file)), directory_(std::move(directory)) {}

    FunctionProto* compile(const std::vector<StmtPtr>& statements, bool repl) {
        FunctionState state;
        state.kind = FunctionKind::Script;
        state.proto = session_.program.newProto();
        state.proto->name = "<script>";
        state.proto->source = file_;
        state.locals.push_back(Local{"", 0, VarInfo{}});
        fs_ = &state;

        for (size_t i = 0; i < statements.size(); i++) {
            const Stmt& stmt = *statements[i];
            if (repl && i + 1 == statements.size() && stmt.kind == StmtKind::Expr) {
                compileExpr(*static_cast<const ExprStmt&>(stmt).expr);
                emit(Op::Return);
            } else {
                compileStmt(stmt);
            }
        }

        emit(Op::Null);
        emit(Op::Return);
        finish(state);
        return state.proto;
    }

private:
    CompilerSession& session_;
    std::string file_;
    std::string directory_;

    FunctionState* fs_ = nullptr;
    ClassState* class_ = nullptr;
    int line_ = 1;

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
        }
    }

    // Adds a local whose value is (or is about to be) on top of the stack. Returns its slot.
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

        locals.push_back(Local{name, initialized ? fs_->scopeDepth : kDeclaring, VarInfo{isVal, std::move(type), false}});
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
            state->enclosing->locals[static_cast<size_t>(local)].isCaptured = true;
            return addUpvalue(state, true, local);
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

    void emitGet(const std::string& name, const Node& at) {
        VarInfo info;
        int slot = resolveLocal(fs_, name, at, info);
        if (slot != -1) {
            emit(Op::GetLocal, slot);
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

        emit(Op::GetGlobal, session_.globalSlot(name));
    }

    void emitSet(const std::string& name, const Node& at) {
        VarInfo info;
        int slot = resolveLocal(fs_, name, at, info);
        if (slot != -1) {
            if (info.isVal) throw error(at, "cannot assign to val '" + name + "'");
            emitTypeCheck(info.type, name, at);
            emit(Op::SetLocal, slot);
            return;
        }

        int upvalue = resolveUpvalue(fs_, name, at, info);
        if (upvalue != -1) {
            if (info.isVal) throw error(at, "cannot assign to val '" + name + "'");
            emitTypeCheck(info.type, name, at);
            emit(Op::SetUpvalue, upvalue);
            return;
        }

        // A global may be declared after the code that assigns to it, so whether it is a val
        // is checked once everything is compiled, and its type is checked by the VM.
        session_.globalAssignments_.emplace_back(name, file_, at.line, at.col);
        emit(Op::SetGlobal, session_.globalSlot(name));
    }

    // ---- Declared types ----

    TypeSpec resolveType(const TypeAnnotation& s) {
        TypeSpec type;
        if (!s.present() || s.name == "any") return type;

        uint8_t id = builtinTypeId(s.name);
        if (id == 0) {
            id = kTypeInstance;
            type.className = s.name;
        }
        type.flags = static_cast<uint8_t>(id | (s.nullable ? kTypeNullable : 0));
        return type;
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

        if (type.id() == kTypeInstance) emitGet(type.className, at);
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
        if (type.id() == kTypeInstance) emitGet(type.className, at);
        emit(Op::CheckType, stringConstant(variable));
        emitByte(type.flags);
    }

    // ---- Statements ----

    void compileBlockBody(const std::vector<StmtPtr>& statements) {
        for (const StmtPtr& stmt : statements) compileStmt(*stmt);
    }

    void compileStmt(const Stmt& stmt) {
        line_ = stmt.line;
        switch (stmt.kind) {
            case StmtKind::Expr:
                compileExpr(*static_cast<const ExprStmt&>(stmt).expr);
                emit(Op::Pop);
                break;

            case StmtKind::Var: {
                const auto& s = static_cast<const VarStmt&>(stmt);
                TypeSpec type = resolveType(s.type);
                bool checkAtRuntime = type.typed() && checkInitializer(s, type);

                if (isGlobalScope()) {
                    int global = session_.declareGlobal(s.name, s.isVal, file_, s);
                    if (s.init) compileExpr(*s.init);
                    else emit(Op::Null);
                    line_ = s.line;
                    if (type.typed()) {
                        // The VM remembers the type and enforces it on every later assignment.
                        if (type.id() == kTypeInstance) emitGet(type.className, s.type);
                        emit(Op::DefineTypedGlobal, global);
                        emitByte(type.flags);
                    } else {
                        emit(Op::DefineGlobal, global);
                    }
                } else {
                    int slot = addLocal(s.name, s.isVal, s, false, type);
                    size_t codeMark = code().size();
                    size_t protoMark = fs_->proto->protos.size();
                    if (s.init) compileExpr(*s.init);
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

                        emit(Op::Unset);
                        fs_->locals[static_cast<size_t>(slot)].depth = kPending;
                        compileExpr(*s.init);
                        line_ = s.line;
                        if (type.typed()) emitTypeCheck(type, s.name, s.type);
                        emit(Op::SetLocal, slot);
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
                LoopState loop{fs_->loop, here(), fs_->scopeDepth, fs_->tryDepth, {}};
                fs_->loop = &loop;
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
                if (fs_->kind == FunctionKind::Initializer) {
                    if (s.value) throw error(s, "cannot return a value from an initializer");
                    emit(Op::GetLocal, 0);
                } else if (fs_->kind == FunctionKind::Setter) {
                    if (s.value) throw error(s, "cannot return a value from a setter");
                    emit(Op::GetLocal, 1);
                } else if (s.value) {
                    compileExpr(*s.value);
                    line_ = s.line;
                    emitReturnCheck(staticTypeOf(*s.value), *s.value);
                } else {
                    emitReturnCheck(kStaticNull, s);
                    emit(Op::Null);
                }
                emit(fs_->kind == FunctionKind::Initializer ? Op::ReturnInit : Op::Return);
                break;
            }

            case StmtKind::Break: {
                LoopState* loop = fs_->loop;
                if (loop == nullptr) throw error(stmt, "'break' outside of a loop");
                emitLoopExit(*loop);
                loop->breakJumps.push_back(emitJump(Op::Jump));
                break;
            }

            case StmtKind::Continue: {
                LoopState* loop = fs_->loop;
                if (loop == nullptr) throw error(stmt, "'continue' outside of a loop");
                emitLoopExit(*loop);
                emitLoop(loop->start);
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
                int module = session_.importModule(s.path, directory_, file_, s);
                line_ = s.line;
                emit(Op::Import, module);
                emit(Op::Pop);
                break;
            }

            case StmtKind::Function: {
                const FunctionDecl& fn = *static_cast<const FunctionStmt&>(stmt).fn;
                if (isGlobalScope()) {
                    int global = session_.declareGlobal(fn.name, false, file_, stmt);
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

    // Leaves the open try blocks and discards the locals of a loop body before jumping out of it.
    void emitLoopExit(const LoopState& loop) {
        for (int i = fs_->tryDepth; i > loop.tryDepth; i--) emit(Op::TryEnd);
        const std::vector<Local>& locals = fs_->locals;
        for (size_t i = locals.size(); i-- > 0 && locals[i].depth > loop.scopeDepth;)
            emit(locals[i].isCaptured ? Op::CloseUpvalue : Op::Pop);
    }

    void compileForIn(const ForInStmt& s) {
        beginScope();
        compileExpr(*s.iterable);
        line_ = s.line;
        emit(Op::IterInit);
        int iteratorSlot = addLocal("(iterator)", false, s, true);

        LoopState loop{fs_->loop, here(), fs_->scopeDepth, fs_->tryDepth, {}};
        fs_->loop = &loop;
        emit(Op::IterNext, iteratorSlot);
        int toExit = here();
        emitI32(0);

        // The loop variable lives in a scope of its own, so each iteration gets a fresh
        // variable and closures capture the value of their own iteration.
        beginScope();
        addLocal(s.variable, false, s, true);
        compileBlockBody(s.body->statements);
        endScope();

        emitLoop(loop.start);
        patchJump(toExit);
        for (int jump : loop.breakJumps) patchJump(jump);
        fs_->loop = loop.enclosing;
        endScope();
    }

    void compileTry(const TryStmt& s) {
        int toHandler = emitJump(Op::TryBegin);
        fs_->tryDepth++;
        compileStmt(*s.body);
        fs_->tryDepth--;
        emit(Op::TryEnd);
        int toEnd = emitJump(Op::Jump);

        // The VM enters the handler with the stack cut back to where it was at TryBegin and
        // the thrown value pushed, which makes that value the first local of the catch scope.
        patchJump(toHandler);
        beginScope();
        addLocal(s.hasCatchVariable ? s.catchVariable : "(exception)", false, s, true);
        compileBlockBody(s.handler->statements);
        endScope();
        patchJump(toEnd);
    }

    void compileClass(const ClassStmt& s) {
        int nameConstant = stringConstant(s.name);
        if (isGlobalScope()) {
            int global = session_.declareGlobal(s.name, false, file_, s);
            emit(Op::Class, nameConstant);
            emit(Op::DefineGlobal, global);
        } else {
            addLocal(s.name, false, s, true);
            emit(Op::Class, nameConstant);
        }

        ClassState cls{class_};
        class_ = &cls;

        if (s.hasSuper) {
            if (s.superName == s.name) throw error(s, "class '" + s.name + "' cannot inherit from itself");
            emitGet(s.superName, s);

            // The superclass stays on the stack as a hidden local that methods capture for `super`.
            beginScope();
            addLocal("super", false, s, true);
            emitGet(s.name, s);
            emit(Op::Inherit);
            cls.hasSuper = true;
        }

        emitGet(s.name, s);
        compileFields(s);
        if (!s.hasSuper && !s.fields.empty()) cls.closed = &s;

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
        std::unordered_set<std::string> methodNames, propertyNames;
        for (const auto& method : s.methods) {
            if (method->isStatic) continue;
            (method->accessor == FunctionDecl::Accessor::None ? methodNames : propertyNames).insert(method->name);
        }

        for (const auto& method : s.methods) {
            if (method->accessor != FunctionDecl::Accessor::None) {
                // A property: a getter, a setter, or one of each.
                bool isGetter = method->accessor == FunctionDecl::Accessor::Get;
                if (!seen.insert((isGetter ? "get " : "set ") + method->name).second)
                    throw error(*method, std::string("duplicate ") + (isGetter ? "getter" : "setter") + " for '" + method->name +
                                             "' in class '" + s.name + "'");
                if (methodNames.count(method->name) != 0)
                    throw error(*method, "'" + method->name + "' is both a property and a method of class '" + s.name + "'");

                if (isGetter) {
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
        emit(Op::Pop);

        if (cls.hasSuper) endScope();
        class_ = cls.enclosing;
    }

    // A class with a header, or with superclass arguments, that writes no init of its own
    // gets one generated for it.
    static bool hasGeneratedInit(const ClassStmt& s) {
        if (!s.hasHeader && !s.hasSuperArgs) return false;
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
        state.locals.push_back(Local{"this", 1, VarInfo{}});
        for (const HeaderParam& param : s.header) {
            state.locals.push_back(Local{param.name, 1, VarInfo{}});
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
            compileExpr(*param.defaultValue);
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
            if (param.isField || !type.typed()) continue;
            line_ = param.line;
            emit(Op::GetLocal, static_cast<int>(i + 1));
            if (type.id() == kTypeInstance) emitGet(type.className, param.type);
            emit(Op::CheckParam, stringConstant(param.name));
            emitByte(type.flags);
            emit(Op::Pop);
        }

        line_ = s.line;
        if (s.hasSuper) {
            // The superclass is initialized first, as in a hand-written init.
            emit(Op::GetLocal, 0);
            for (const ExprPtr& arg : s.superArgs) compileExpr(*arg);
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
            // A generated initializer assigns every header field from its arguments.
            bool setByGeneratedInit = generatedInit && i < s.headerFieldCount;
            line_ = f.line;
            if (!seen.insert(f.name).second)
                throw error(f, "duplicate field '" + f.name + "' in class '" + s.name + "'");
            if (methodNames.count(f.name) != 0)
                throw error(f, "'" + f.name + "' is both a field and a method of class '" + s.name + "'");
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
            emitU16(stringConstant(f.name));
            emitU16(type.flags | (f.isVal ? kFieldVal : 0) | (required ? kFieldRequired : 0));
        }

        if (initialized.empty()) return;

        // A hidden method, run on each new instance before init: this.field = <initial value>.
        FunctionState state;
        state.enclosing = fs_;
        state.kind = FunctionKind::Method;
        state.proto = session_.program.newProto();
        state.proto->name = s.name + ".<fields>";
        state.proto->source = file_;
        state.scopeDepth = 1;
        state.locals.push_back(Local{"this", 1, VarInfo{}});

        fs_ = &state;
        for (size_t i : initialized) {
            const VarStmt& f = *s.fields[i];
            line_ = f.line;
            emit(Op::GetLocal, 0);
            if (initialValues[i] != nullptr) compileExpr(*initialValues[i]);
            else emit(Op::Null);
            line_ = f.line;
            emit(Op::SetProp, stringConstant(f.name));
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
        state.proto->name = "<type " + className + ">";
        state.proto->source = file_;
        state.scopeDepth = 1;
        state.locals.push_back(Local{"", 1, VarInfo{}});

        fs_ = &state;
        emitGet(className, at);
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
        state.scopeDepth = 1;

        // Slot 0 holds the receiver in methods and the callee itself otherwise.
        bool hasThis = kind == FunctionKind::Method || kind == FunctionKind::Initializer || kind == FunctionKind::Setter;
        state.locals.push_back(Local{hasThis ? "this" : "", 1, VarInfo{}});
        for (size_t i = 0; i < decl.params.size(); i++) {
            state.locals.push_back(Local{decl.params[i], 1, VarInfo{false, resolveType(decl.paramTypes[i])}});
            state.proto->paramNames.push_back(session_.vm_.intern(decl.params[i]));
        }

        state.returnType = resolveType(decl.returnType);
        if (kind == FunctionKind::Initializer && decl.returnType.present())
            throw error(decl.returnType, "an initializer cannot declare a return type");

        fs_ = &state;
        line_ = decl.line;

        // Parameters with a default value are optional, so they have to come last. Each default
        // is computed here if the caller left the argument out.
        int requiredArgs = 0;
        bool sawDefault = false;
        for (size_t i = 0; i < decl.params.size(); i++) {
            const Expr* defaultValue = decl.paramDefaults[i].get();
            if (defaultValue == nullptr) {
                if (sawDefault)
                    throw error(decl.paramPositions[i], "parameter '" + decl.params[i] + "' has no default value but follows " +
                                                            "one that has; parameters with defaults must come last");
                requiredArgs++;
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
            compileExpr(*defaultValue);
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
            if (type.id() == kTypeInstance) emitGet(type.className, decl.paramTypes[i]);
            emit(Op::CheckParam, stringConstant(decl.params[i]));
            emitByte(type.flags);
            emit(Op::Pop);
        }

        compileBlockBody(decl.body);

        // Running off the end returns null (or the instance, from an initializer).
        if (kind == FunctionKind::Initializer) {
            emit(Op::GetLocal, 0);
        } else if (kind == FunctionKind::Setter) {
            emit(Op::GetLocal, 1);  // the value it was given
        } else {
            emit(Op::Null);
            if (state.returnType.typed() && !state.returnType.nullable()) {
                if (state.returnType.id() == kTypeInstance) emitGet(state.returnType.className, decl.returnType);
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
            case TokenType::EqEq:
            case TokenType::BangEq: return Op::Eq;
            case TokenType::Lt: return Op::Lt;
            case TokenType::LtEq: return Op::Le;
            case TokenType::Gt: return Op::Gt;
            case TokenType::GtEq: return Op::Ge;
            default: throw std::logic_error("not a binary operator");
        }
    }

    void compileArgs(const std::vector<ExprPtr>& args) {
        for (const ExprPtr& arg : args) compileExpr(*arg);
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
                    case LiteralExpr::Type::String: emit(Op::Const, stringConstant(e.stringValue)); break;
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
                emit(e.op == TokenType::Minus ? Op::Neg : Op::Not);
                break;
            }

            case ExprKind::Binary: {
                const auto& e = static_cast<const BinaryExpr&>(expr);
                compileExpr(*e.left);
                compileExpr(*e.right);
                line_ = e.line;
                emit(binaryOp(e.op));
                if (e.op == TokenType::BangEq) emit(Op::Not);
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

            case ExprKind::Call: {
                const auto& e = static_cast<const CallExpr&>(expr);
                int argc = static_cast<int>(e.args.size());
                if (e.namedCount > 0) {
                    compileNamedCall(e);
                } else if (e.callee->kind == ExprKind::Member) {
                    const auto& member = static_cast<const MemberExpr&>(*e.callee);
                    compileExpr(*member.object);
                    compileArgs(e.args);
                    line_ = member.line;
                    emit(Op::Invoke, stringConstant(member.name));
                    emitByte(argc);
                } else if (e.callee->kind == ExprKind::Super) {
                    const auto& sup = static_cast<const SuperExpr&>(*e.callee);
                    requireSuper(sup);
                    emitGet("this", sup);
                    compileArgs(e.args);
                    emitGet("super", sup);
                    line_ = sup.line;
                    emit(Op::SuperInvoke, stringConstant(sup.method));
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
                compileExpr(*e.object);
                line_ = e.line;
                emit(Op::GetProp, stringConstant(e.name));
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
                line_ = e.line;
                emit(Op::Range);
                emitByte(e.inclusive ? 1 : 0);
                break;
            }

            case ExprKind::Is: {
                const auto& e = static_cast<const IsExpr&>(expr);
                compileExpr(*e.value);
                compileExpr(*e.type);
                line_ = e.line;
                emit(Op::Is);
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
    void compileNamedCall(const CallExpr& e) {
        compileExpr(*e.callee);

        size_t positional = e.args.size() - e.namedCount;
        for (size_t i = 0; i < e.args.size(); i++) {
            if (i >= positional) {
                line_ = e.args[i]->line;
                emit(Op::Const, stringConstant(e.names[i]));
            }
            compileExpr(*e.args[i]);
        }

        line_ = e.line;
        emit(Op::CallNamed);
        emitByte(static_cast<int>(positional));
        emitByte(static_cast<int>(e.namedCount));
    }

    void compileAssign(const AssignExpr& e) {
        bool compound = e.op != TokenType::Eq;

        switch (e.target->kind) {
            case ExprKind::Name: {
                const auto& target = static_cast<const NameExpr&>(*e.target);
                if (compound) emitGet(target.name, target);
                compileExpr(*e.value);
                line_ = e.line;
                if (compound) emit(binaryOp(e.op));
                emitSet(target.name, target);
                break;
            }

            case ExprKind::Member: {
                const auto& target = static_cast<const MemberExpr&>(*e.target);
                if (target.object->kind == ExprKind::This && class_ != nullptr && class_->closed != nullptr) {
                    const ClassStmt& owner = *class_->closed;
                    bool declared = false;
                    for (const auto& field : owner.fields) declared = declared || field->name == target.name;
                    for (const auto& method : owner.methods)
                        declared = declared || (method->accessor == FunctionDecl::Accessor::Set && method->name == target.name);
                    if (!declared)
                        throw error(target, "class '" + owner.name + "' has no field '" + target.name +
                                                "'; fields must be declared in the class with val or var");
                }
                compileExpr(*target.object);
                if (compound) {
                    emit(Op::Dup);
                    emit(Op::GetProp, stringConstant(target.name));
                }
                compileExpr(*e.value);
                line_ = e.line;
                if (compound) emit(binaryOp(e.op));
                emit(Op::SetProp, stringConstant(target.name));
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
    program.modules.push_back(compileUnit(kPrelude, "<prelude>", "", false));
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
        FunctionProto* proto = compileUnit(source, name, baseDirectory, repl);
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

FunctionProto* CompilerSession::compileUnit(const std::string& source, const std::string& name,
                                            const std::string& directory, bool repl) {
    Lexer lexer(source, name);
    std::vector<Token> tokens = lexer.scan();
    Parser parser(tokens, name);
    std::vector<StmtPtr> statements = parser.parseProgram();
    ModuleCompiler compiler(*this, name, directory);
    return compiler.compile(statements, repl);
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
            compileUnit(source, canonical.filename().string(), canonical.parent_path().string(), false);
    } catch (...) {
        modules_.erase(key);
        throw;
    }
    return index;
}

int CompilerSession::importModule(const std::string& path, const std::string& fromDirectory,
                                  const std::string& file, const Node& at) {
    // Looked for next to the importing file first, then in the library directories; in each
    // place the name is tried as written and with ".astra" added.
    std::vector<fs::path> places;
    places.push_back(fs::path(fromDirectory));
    for (const std::string& library : libraryPaths) places.push_back(fs::path(library));

    // A file never imports itself: a script called gui.astra that says `import "gui"` means
    // the library, so the importing file is passed over.
    std::error_code ec;
    fs::path self = fs::weakly_canonical(fs::path(fromDirectory) / fs::path(file), ec);
    auto usable = [&](const fs::path& candidate) {
        if (!fs::is_regular_file(candidate, ec)) return false;
        return fs::weakly_canonical(candidate, ec) != self;
    };

    for (const fs::path& place : places) {
        fs::path candidate = place / fs::path(path);
        if (usable(candidate)) return loadModule(candidate.string(), path, file, &at);
        candidate += ".astra";
        if (usable(candidate)) return loadModule(candidate.string(), path, file, &at);
    }
    throw CompileError(file, at.line, at.col, "cannot find module '" + path + "'");
}

int CompilerSession::globalSlot(const std::string& name) {
    auto it = globals_.find(name);
    if (it != globals_.end()) return it->second;

    int slot = static_cast<int>(program.globalNames.size());
    if (slot > 0xFFFF) throw CompileError("<program>", 1, 1, "too many global variables");
    program.globalNames.push_back(name);
    globals_[name] = slot;
    return slot;
}

int CompilerSession::declareGlobal(const std::string& name, bool isVal, const std::string& file, const Node& at) {
    if (constGlobals_.count(name) != 0)
        throw CompileError(file, at.line, at.col, "cannot redeclare val '" + name + "'");
    if (isVal) constGlobals_.insert(name);
    return globalSlot(name);
}

// A function may assign to a global that is only declared const further down, so
// assignments are checked once everything has been compiled.
void CompilerSession::checkConstAssignments() {
    for (const auto& [name, file, line, col] : globalAssignments_)
        if (constGlobals_.count(name) != 0)
            throw CompileError(file, line, col, "cannot assign to val '" + name + "'");
}

}  // namespace astra
