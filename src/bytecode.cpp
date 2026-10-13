#include "bytecode.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <ostream>
#include <stdexcept>

#include "vm.h"

namespace astra {

namespace {

enum class Operand : uint8_t { None, U8, U16, I32, U16U8, U16I32, U16U16, U8U8, U16U16U8 };

struct OpInfo {
    const char* name;
    Operand operand;
};

// Same order as the Op enum.
const OpInfo kOps[] = {
    {"CONST", Operand::U16},
    {"NULL", Operand::None},
    {"TRUE", Operand::None},
    {"FALSE", Operand::None},
    {"POP", Operand::None},
    {"DUP", Operand::None},
    {"DUP2", Operand::None},
    {"GET_LOCAL", Operand::U16},
    {"SET_LOCAL", Operand::U16},
    {"GET_GLOBAL", Operand::U16},
    {"SET_GLOBAL", Operand::U16},
    {"DEFINE_GLOBAL", Operand::U16},
    {"GET_UPVALUE", Operand::U16},
    {"SET_UPVALUE", Operand::U16},
    {"GET_PROP", Operand::U16},
    {"SET_PROP", Operand::U16},
    {"GET_INDEX", Operand::None},
    {"SET_INDEX", Operand::None},
    {"GET_SUPER", Operand::U16},
    {"ADD", Operand::None},
    {"SUB", Operand::None},
    {"MUL", Operand::None},
    {"DIV", Operand::None},
    {"MOD", Operand::None},
    {"NEG", Operand::None},
    {"NOT", Operand::None},
    {"EQ", Operand::None},
    {"LT", Operand::None},
    {"LE", Operand::None},
    {"GT", Operand::None},
    {"GE", Operand::None},
    {"IS", Operand::None},
    {"JUMP", Operand::I32},
    {"LOOP", Operand::I32},
    {"JUMP_IF_FALSE", Operand::I32},
    {"AND", Operand::I32},
    {"OR", Operand::I32},
    {"CALL", Operand::U8},
    {"INVOKE", Operand::U16U8},
    {"SUPER_INVOKE", Operand::U16U8},
    {"CLOSURE", Operand::U16},
    {"CLOSE_UPVALUE", Operand::None},
    {"RETURN", Operand::None},
    {"CLASS", Operand::U16},
    {"INHERIT", Operand::U8},
    {"METHOD", Operand::U16},
    {"STATIC_METHOD", Operand::U16},
    {"BUILD_LIST", Operand::U16},
    {"BUILD_MAP", Operand::U16},
    {"BUILD_STRING", Operand::U16},
    {"RANGE", Operand::U8},
    {"ITER_INIT", Operand::None},
    {"ITER_NEXT", Operand::U16I32},
    {"TRY_BEGIN", Operand::I32},
    {"TRY_END", Operand::None},
    {"THROW", Operand::None},
    {"IMPORT", Operand::U16},
    {"CHECK_TYPE", Operand::U16U8},
    {"DEFINE_TYPED_GLOBAL", Operand::U16U8},
    {"CHECK_PARAM", Operand::U16U8},
    {"CHECK_RETURN", Operand::U8},
    {"DECLARE_FIELD", Operand::U16U16},
    {"FIELD_INIT", Operand::None},
    {"RETURN_INIT", Operand::None},
    {"SUPER_INIT", Operand::U8},
    {"DEFAULT_ARG", Operand::U16I32},
    {"CALL_NAMED", Operand::U8U8},
    {"GETTER", Operand::U16},
    {"SETTER", Operand::U16},
    {"UNSET", Operand::None},
    {"GET_UPVALUE_CHECKED", Operand::U16U16},
    {"SHL", Operand::None},
    {"SHR", Operand::None},
    {"BIT_AND", Operand::None},
    {"BIT_OR", Operand::None},
    {"BIT_XOR", Operand::None},
    {"BIT_NOT", Operand::None},
    {"GET_IMPLICIT", Operand::U16U16},
    {"SET_IMPLICIT", Operand::U16U16},
    {"IDENTICAL", Operand::None},
    {"JUMP_IF_NULL", Operand::I32},
    {"JUMP_IF_NOT_NULL", Operand::I32},
    {"ASSERT_NOT_NULL", Operand::None},
    {"SETTLE", Operand::U16},
    {"IS_TYPE", Operand::U8},
    {"CAST", Operand::U8},
    {"CAST_OR_NULL", Operand::U8},
    {"SWAP", Operand::None},
    {"POP_UNDER", Operand::U16},
    {"SPREAD", Operand::None},
    {"CALL_SPREAD", Operand::U8},
    {"STATIC_FIELD", Operand::U16U16},
    {"STATIC_GETTER", Operand::U16},
    {"STATIC_SETTER", Operand::U16},
    {"TRY_IMPLICIT", Operand::U16I32},
    {"INVOKE_IMPLICIT", Operand::U16U16U8},
    {"ENUM_ENTRY", Operand::U16U16},
    {"ENUM_SEAL", Operand::None},
    {"ENUM_NEXT", Operand::U16U16},
    {"PENDING_ENUM", Operand::U16},
    {"MARK_INTERFACE", Operand::None},
    {"REQUIRE", Operand::U16},
    {"FINISH_CLASS", Operand::None},
    {"DATA_FIELD", Operand::U16},
    {"COMPONENT", Operand::U8},
    {"MISSING_ARG", Operand::U16},
    {"CLASS_INIT", Operand::None},
    {"ENSURE_CLASS_INIT", Operand::None},
    {"EXTENSION", Operand::U16U8},
    {"MARK_ABSTRACT", Operand::None},
    {"CHECK_OVERRIDE", Operand::U16U8},
};

const char* const kTypeNames[] = {"", "int", "float", "number", "string", "bool", "list", "map", "range", "function"};

static_assert(sizeof(kOps) / sizeof(kOps[0]) == static_cast<size_t>(Op::Count_), "kOps must match the Op enum");

size_t operandSize(Operand o) {
    switch (o) {
        case Operand::None: return 0;
        case Operand::U8: return 1;
        case Operand::U16: return 2;
        case Operand::I32: return 4;
        case Operand::U16U8: return 3;
        case Operand::U16I32: return 6;
        case Operand::U16U16: return 4;
        case Operand::U8U8: return 2;
        case Operand::U16U16U8: return 5;
    }
    return 0;
}

int readU16(const std::vector<uint8_t>& code, size_t at) { return code[at] | (code[at + 1] << 8); }

int32_t readI32(const std::vector<uint8_t>& code, size_t at) {
    uint32_t v = static_cast<uint32_t>(code[at]) | (static_cast<uint32_t>(code[at + 1]) << 8) |
                 (static_cast<uint32_t>(code[at + 2]) << 16) | (static_cast<uint32_t>(code[at + 3]) << 24);
    return static_cast<int32_t>(v);
}

// Decodes the operands of the instruction at `ip`: `a` is the first, `b` the second.
void decodeOperands(const std::vector<uint8_t>& code, size_t ip, Operand operand, int& a, int& b) {
    a = b = 0;
    switch (operand) {
        case Operand::None: break;
        case Operand::U8: a = code[ip + 1]; break;
        case Operand::U16: a = readU16(code, ip + 1); break;
        case Operand::I32: a = readI32(code, ip + 1); break;
        case Operand::U16U8:
            a = readU16(code, ip + 1);
            b = code[ip + 3];
            break;
        case Operand::U16I32:
            a = readU16(code, ip + 1);
            b = readI32(code, ip + 3);
            break;
        case Operand::U16U16:
        case Operand::U16U16U8:  // the third operand is read where it is needed
            a = readU16(code, ip + 1);
            b = readU16(code, ip + 3);
            break;
        case Operand::U8U8:
            a = code[ip + 1];
            b = code[ip + 2];
            break;
    }
}

}  // namespace

uint8_t builtinTypeId(const std::string& name) {
    for (uint8_t id = kTypeInt; id < kTypeInstance; id++)
        if (name == kTypeNames[id]) return id;
    return 0;
}

std::string typeText(uint8_t type, const std::string& className) {
    uint8_t id = type & kTypeMask;
    std::string text = id == kTypeInstance ? className : id >= kTypeInt && id < kTypeInstance ? kTypeNames[id] : "?";
    if (type & kTypeNullable) text += '?';
    return text;
}

FunctionProto* Program::newProto() {
    owned_.push_back(std::make_unique<FunctionProto>());
    return owned_.back().get();
}

// ---- Verification ----

// Follows every path through a function, checking each instruction and working out the
// stack depth it runs at. With `partial` the function is still being compiled: jumps that
// have no target yet are passed over, and the answer is the depth at which the code written
// so far ends, or -1 if nothing reaches that point.
static int analyzeStack(FunctionProto& f, const Program& program, bool partial) {
    auto fail = [&](const std::string& why) {
        throw std::runtime_error("invalid bytecode in " + f.name + ": " + why);
    };

    const std::vector<uint8_t>& code = f.code;
    const size_t n = code.size();
    if (n == 0 && partial) return 1 + f.arity;
    if (n == 0) fail("empty function");
    if (f.lines.size() != n) fail("line table does not match the code");
    if (f.arity < 0 || f.arity > 255) fail("bad arity");
    if (f.minArity < -1 || f.minArity > f.arity) fail("bad arity");
    if (f.varargIndex < -1 || f.varargIndex >= f.arity) fail("bad vararg parameter");
    if (!f.paramNames.empty() && f.paramNames.size() != static_cast<size_t>(f.arity)) fail("bad parameter names");
    for (uint16_t upvalue : f.outerReceivers)
        if (upvalue >= f.upvalues.size()) fail("receiver upvalue out of range");

    // Stack depth on entry to each instruction, counted from the frame base (slot 0 is the
    // callee or receiver, then the parameters). -1 means not reached yet.
    std::vector<int> depthAt(n, -1);
    std::vector<size_t> work;
    int maxDepth = 1 + f.arity;

    int endDepth = -1;
    auto reachEnd = [&](int depth) {
        if (endDepth != -1 && endDepth != depth) fail("inconsistent stack depth");
        endDepth = depth;
    };

    auto flow = [&](int64_t target, int depth) {
        if (partial && target == 0) return;  // a forward jump that has not been given its target
        if (partial && static_cast<size_t>(target) == n) return reachEnd(depth);
        if (target < 0 || static_cast<size_t>(target) >= n) fail("jump out of range");
        int& d = depthAt[static_cast<size_t>(target)];
        if (d == -1) {
            d = depth;
            work.push_back(static_cast<size_t>(target));
        } else if (d != depth) {
            fail("inconsistent stack depth");
        }
    };

    auto requireName = [&](int index) {
        if (static_cast<size_t>(index) >= f.constants.size() || !f.constants[index].isString())
            fail("name operand is not a string constant");
    };

    depthAt[0] = 1 + f.arity;
    work.push_back(0);

    while (!work.empty()) {
        size_t ip = work.back();
        work.pop_back();
        int depth = depthAt[ip];

        for (;;) {
            uint8_t raw = code[ip];
            if (raw >= static_cast<uint8_t>(Op::Count_)) fail("unknown opcode");
            Op op = static_cast<Op>(raw);
            size_t size = 1 + operandSize(kOps[raw].operand);
            if (ip + size > n) fail("truncated instruction");

            int a, b;
            decodeOperands(code, ip, kOps[raw].operand, a, b);

            int pops = 0, pushes = 0;
            bool terminal = false;

            switch (op) {
                case Op::Const:
                    if (static_cast<size_t>(a) >= f.constants.size()) fail("constant out of range");
                    pushes = 1;
                    break;
                case Op::Null:
                case Op::True:
                case Op::False:
                case Op::Unset:
                    pushes = 1;
                    break;
                case Op::GetUpvalueChecked:
                    if (static_cast<size_t>(a) >= f.upvalues.size()) fail("upvalue out of range");
                    requireName(b);
                    pushes = 1;
                    break;
                case Op::Pop:
                case Op::CloseUpvalue:
                    pops = 1;
                    break;
                case Op::Dup:
                    pops = 1;
                    pushes = 2;
                    break;
                case Op::Dup2:
                    pops = 2;
                    pushes = 4;
                    break;
                case Op::GetLocal:
                    if (a >= depth) fail("local slot out of range");
                    pushes = 1;
                    break;
                case Op::SetLocal:
                    if (a >= depth) fail("local slot out of range");
                    pops = pushes = 1;
                    break;
                case Op::GetGlobal:
                case Op::SetGlobal:
                case Op::DefineGlobal:
                    if (static_cast<size_t>(a) >= program.globalNames.size()) fail("global out of range");
                    if (op == Op::GetGlobal) pushes = 1;
                    else if (op == Op::DefineGlobal) pops = 1;
                    else pops = pushes = 1;
                    break;
                case Op::GetUpvalue:
                case Op::SetUpvalue:
                    if (static_cast<size_t>(a) >= f.upvalues.size()) fail("upvalue out of range");
                    if (op == Op::GetUpvalue) pushes = 1;
                    else pops = pushes = 1;
                    break;
                case Op::GetImplicit:
                case Op::SetImplicit:
                    requireName(a);
                    if (static_cast<size_t>(b) >= program.globalNames.size()) fail("global out of range");
                    if (op == Op::GetImplicit) {
                        pushes = 1;
                    } else {
                        pops = pushes = 1;
                        // On the way, the object is slid in under the value.
                        maxDepth = std::max(maxDepth, depth + 1);
                    }
                    break;
                case Op::GetProp:
                    requireName(a);
                    pops = pushes = 1;
                    break;
                case Op::SetProp:
                case Op::GetSuper:
                case Op::Method:
                case Op::StaticMethod:
                case Op::Getter:
                case Op::Setter:
                case Op::StaticGetter:
                case Op::StaticSetter:
                    requireName(a);
                    pops = 2;
                    pushes = 1;
                    break;
                case Op::GetIndex:
                case Op::Add:
                case Op::Sub:
                case Op::Mul:
                case Op::Div:
                case Op::Mod:
                case Op::Identical:
                case Op::Eq:
                case Op::Lt:
                case Op::Le:
                case Op::Gt:
                case Op::Ge:
                case Op::Is:
                case Op::Shl:
                case Op::Shr:
                case Op::BitAnd:
                case Op::BitOr:
                case Op::BitXor:
                    pops = 2;
                    pushes = 1;
                    break;
                case Op::SetIndex:
                    pops = 3;
                    pushes = 1;
                    break;
                case Op::Neg:
                case Op::Not:
                case Op::BitNot:
                case Op::IterInit:
                    pops = pushes = 1;
                    break;
                case Op::Jump:
                case Op::Loop:
                    flow(a, depth);
                    terminal = true;
                    break;
                case Op::JumpIfFalse:
                    if (depth < 2) fail("stack underflow");
                    flow(a, depth - 1);
                    pops = 1;
                    break;
                case Op::And:
                case Op::Or:
                    if (depth < 2) fail("stack underflow");
                    flow(a, depth);
                    pops = 1;
                    break;
                case Op::JumpIfNull:
                case Op::JumpIfNotNull:
                    if (depth < 2) fail("stack underflow");
                    flow(a, depth);
                    break;
                case Op::AssertNotNull:
                    pops = pushes = 1;
                    break;
                case Op::Swap:
                    pops = pushes = 2;
                    break;
                case Op::PopUnder:
                    pops = a + 1;
                    pushes = 1;
                    break;
                case Op::Spread:
                    pops = 2;
                    pushes = 1;
                    break;
                case Op::EnumEntry:
                    requireName(a);
                    pops = 2;
                    pushes = 1;
                    break;
                case Op::EnumNext:
                    requireName(a);
                    pops = pushes = 1;
                    break;
                case Op::EnumSeal:
                case Op::MarkInterface:
                case Op::FinishClass:
                    pops = pushes = 1;
                    break;
                case Op::Require:
                case Op::DataField:
                    requireName(a);
                    pops = pushes = 1;
                    break;
                case Op::Component:
                    pops = pushes = 1;
                    break;
                case Op::Range:
                    if ((a & ~7) != 0) fail("bad range");
                    pops = (a & 4) != 0 ? 3 : 2;
                    pushes = 1;
                    break;
                case Op::MissingArg:
                    requireName(a);
                    terminal = true;
                    break;
                case Op::ClassInit:
                    pops = 2;
                    pushes = 1;
                    break;
                case Op::EnsureClassInit:
                case Op::MarkAbstract:
                    pops = pushes = 1;
                    break;
                case Op::CheckOverride:
                    requireName(a);
                    pops = pushes = 1;
                    break;
                case Op::Extension:
                    requireName(a);
                    if ((b & kTypeMask) > kTypeInstance) fail("unknown type");
                    pops = (b & kTypeMask) == kTypeInstance ? 2 : 1;
                    break;
                case Op::Inherit:
                    if (a < 1) fail("bad supertype count");
                    pops = a + 1;
                    pushes = 1;
                    break;
                case Op::PendingEnum:
                    requireName(a);
                    pushes = 1;
                    break;
                case Op::TryImplicit:
                    requireName(a);
                    flow(b, depth + 1);
                    maxDepth = std::max(maxDepth, depth + 1);
                    break;
                case Op::InvokeImplicit:
                    requireName(a);
                    if (static_cast<size_t>(b) >= program.globalNames.size()) fail("global out of range");
                    pops = code[ip + 5] + 1;
                    pushes = 1;
                    break;
                case Op::StaticField: {
                    requireName(a);
                    int id = b & kTypeMask;
                    if (id > kTypeInstance || (b & ~(0xFF | kFieldVal)) != 0) fail("bad field declaration");
                    pops = id == kTypeInstance ? 3 : 2;
                    pushes = 1;
                    break;
                }
                case Op::CallSpread:
                    pops = 2 + 2 * a;
                    pushes = 1;
                    break;
                case Op::IsType:
                case Op::Cast:
                case Op::CastOrNull: {
                    int id = a & kTypeMask;
                    if (id < kTypeInt || id > kTypeInstance) fail("unknown type");
                    pops = id == kTypeInstance ? 2 : 1;
                    pushes = 1;
                    break;
                }
                case Op::Settle:
                    // At most one slot is added: that of a variable whose value was being worked out.
                    if (depth < 2 || a < 1 || a > depth) fail("bad stack size");
                    depth = a + 1;
                    break;
                case Op::Call:
                    pops = a + 1;
                    pushes = 1;
                    break;
                case Op::Invoke:
                    requireName(a);
                    pops = b + 1;
                    pushes = 1;
                    break;
                case Op::SuperInvoke:
                    requireName(a);
                    pops = b + 2;
                    pushes = 1;
                    break;
                case Op::SuperInit:
                    pops = a + 2;
                    pushes = 1;
                    break;
                case Op::CallNamed:
                    pops = 1 + a + 2 * b;
                    pushes = 1;
                    break;
                case Op::Closure: {
                    if (static_cast<size_t>(a) >= f.protos.size()) fail("nested function out of range");
                    // A local function may capture its own slot, which is the one being pushed.
                    for (const UpvalueDesc& uv : f.protos[a]->upvalues) {
                        if (uv.isLocal ? uv.index > depth : uv.index >= f.upvalues.size())
                            fail("captured variable out of range");
                    }
                    pushes = 1;
                    break;
                }
                case Op::Return:
                case Op::ReturnInit:
                    terminal = true;
                    break;
                case Op::DeclareField: {
                    requireName(a);
                    int id = b & kTypeMask;
                    if (id > kTypeInstance || (b & ~(0xFF | kFieldVal | kFieldRequired | kFieldOverride)) != 0) fail("bad field declaration");
                    pops = id == kTypeInstance ? 2 : 1;
                    pushes = 1;
                    break;
                }
                case Op::FieldInit:
                    pops = 2;
                    pushes = 1;
                    break;
                case Op::Class:
                    requireName(a);
                    pushes = 1;
                    break;
                case Op::BuildList:
                case Op::BuildString:
                    pops = a;
                    pushes = 1;
                    break;
                case Op::BuildMap:
                    pops = 2 * a;
                    pushes = 1;
                    break;
                case Op::IterNext:
                    if (a >= depth) fail("local slot out of range");
                    flow(b, depth);
                    pushes = 1;
                    break;
                case Op::DefaultArg:
                    if (a >= depth) fail("local slot out of range");
                    flow(b, depth);
                    break;
                case Op::TryBegin:
                    // The handler starts with the thrown value on top of the current stack.
                    flow(a, depth + 1);
                    maxDepth = std::max(maxDepth, depth + 1);
                    break;
                case Op::TryEnd:
                    break;
                case Op::Throw:
                    pops = 1;
                    terminal = true;
                    break;
                case Op::Import:
                    if (static_cast<size_t>(a) >= program.modules.size()) fail("module out of range");
                    pushes = 1;
                    break;
                case Op::CheckType:
                case Op::CheckParam:
                case Op::CheckReturn:
                case Op::DefineTypedGlobal: {
                    int id = (op == Op::CheckReturn ? a : b) & kTypeMask;
                    if (id < kTypeInt || id > kTypeInstance) fail("unknown type");
                    if (op == Op::DefineTypedGlobal) {
                        if (static_cast<size_t>(a) >= program.globalNames.size()) fail("global out of range");
                    } else {
                        if (op != Op::CheckReturn) requireName(a);
                        pushes = 1;
                    }
                    pops = id == kTypeInstance ? 2 : 1;
                    break;
                }
                case Op::Count_:
                    fail("unknown opcode");
            }

            // Slot 0 belongs to the frame and is never popped.
            if (depth - pops < 1) fail("stack underflow");
            depth = depth - pops + pushes;
            maxDepth = std::max(maxDepth, depth);
            if (terminal) break;

            size_t nextIp = ip + size;
            if (nextIp >= n && partial) {
                reachEnd(depth);
                break;
            }
            if (nextIp >= n) fail("control runs off the end of the function");
            int& d = depthAt[nextIp];
            if (d != -1) {
                if (d != depth) fail("inconsistent stack depth");
                break;
            }
            d = depth;
            ip = nextIp;
        }
    }

    if (!partial) f.maxStack = maxDepth;
    return endDepth;
}

void verifyFunction(FunctionProto& f, const Program& program) { analyzeStack(f, program, false); }

int stackDepthAtEnd(FunctionProto& f, const Program& program) { return analyzeStack(f, program, true); }

// ---- Disassembly ----

namespace {

std::string constantText(const Value& v) {
    switch (v.kind) {
        case ValueKind::Int: return std::to_string(v.as.i);
        case ValueKind::Float: return formatFloat(v.as.d);
        case ValueKind::Obj:
            if (v.isString()) return quoteString(asString(v)->chars);
            return "<object>";
        default: return "?";
    }
}

void disassembleFunction(const FunctionProto& f, const Program& program, std::ostream& out) {
    std::string arity = std::to_string(f.arity);
    if (f.requiredArgs() != f.arity) arity = std::to_string(f.requiredArgs()) + ".." + arity;
    out << "== " << f.name << " (" << f.source << ")  arity=" << arity << " upvalues=" << f.upvalues.size()
        << " stack=" << f.maxStack << " ==\n";

    const std::vector<uint8_t>& code = f.code;
    size_t ip = 0;
    int lastLine = -1;
    while (ip < code.size()) {
        uint8_t raw = code[ip];
        if (raw >= static_cast<uint8_t>(Op::Count_)) {
            out << std::setw(5) << ip << "  ??? " << static_cast<int>(raw) << "\n";
            break;
        }
        const OpInfo& info = kOps[raw];
        size_t size = 1 + operandSize(info.operand);
        if (ip + size > code.size()) break;

        int a, b;
        decodeOperands(code, ip, info.operand, a, b);

        out << std::setw(5) << ip << " ";
        int line = f.lines[ip];
        if (line == lastLine) out << "    | ";
        else out << std::setw(5) << line << " ";
        lastLine = line;
        out << std::left << std::setw(20) << info.name << std::right;

        switch (static_cast<Op>(raw)) {
            case Op::Const:
                out << " " << a << "  ; " << constantText(f.constants[a]);
                break;
            case Op::GetGlobal:
            case Op::SetGlobal:
            case Op::DefineGlobal:
                out << " " << a << "  ; " << program.globalNames[a];
                break;
            case Op::GetProp:
            case Op::SetProp:
            case Op::GetSuper:
            case Op::Class:
            case Op::Method:
            case Op::StaticMethod:
            case Op::Getter:
            case Op::Setter:
            case Op::StaticGetter:
            case Op::StaticSetter:
                out << " " << a << "  ; " << asString(f.constants[a])->chars;
                break;
            case Op::Invoke:
            case Op::SuperInvoke:
                out << " " << a << " argc=" << b << "  ; " << asString(f.constants[a])->chars;
                break;
            case Op::GetImplicit:
            case Op::SetImplicit:
                out << " " << a << " " << b << "  ; " << asString(f.constants[a])->chars;
                break;
            case Op::CallNamed:
                out << " positional=" << a << " named=" << b;
                break;
            case Op::TryImplicit:
                out << " " << a << " -> " << b << "  ; " << asString(f.constants[a])->chars;
                break;
            case Op::InvokeImplicit:
                out << " " << a << " " << b << " argc=" << static_cast<int>(f.code[ip + 5]) << "  ; " << asString(f.constants[a])->chars;
                break;
            case Op::Closure:
                out << " " << a << "  ; " << f.protos[a]->name;
                break;
            case Op::DefaultArg:
            case Op::IterNext:
                out << " slot=" << a << " -> " << b;
                break;
            case Op::Jump:
            case Op::Loop:
            case Op::JumpIfFalse:
            case Op::And:
            case Op::Or:
            case Op::JumpIfNull:
            case Op::JumpIfNotNull:
            case Op::TryBegin:
                out << " -> " << a;
                break;
            case Op::Import:
                out << " " << a << "  ; " << program.modules[a]->source;
                break;
            case Op::CheckType:
            case Op::CheckParam:
                out << " " << a << "  ; " << asString(f.constants[a])->chars << " : " << typeText(static_cast<uint8_t>(b), "<class>");
                break;
            case Op::CheckReturn:
            case Op::IsType:
            case Op::Cast:
            case Op::CastOrNull:
                out << "  ; " << typeText(static_cast<uint8_t>(a), "<class>");
                break;
            case Op::StaticField:
            case Op::DeclareField:
                out << " " << a << "  ; " << ((b & kFieldVal) ? "val " : "var ") << asString(f.constants[a])->chars;
                if ((b & kTypeMask) != 0) out << " : " << typeText(static_cast<uint8_t>(b & 0xFF), "<class>");
                if (b & kFieldRequired) out << " (set by init)";
                break;
            case Op::DefineTypedGlobal:
                out << " " << a << "  ; " << program.globalNames[a] << " : " << typeText(static_cast<uint8_t>(b), "<class>");
                break;
            default:
                if (info.operand != Operand::None) out << " " << a;
                break;
        }
        out << "\n";
        ip += size;
    }
    out << "\n";

    for (const FunctionProto* nested : f.protos) disassembleFunction(*nested, program, out);
}

}  // namespace

void disassemble(const Program& program, std::ostream& out) {
    for (const FunctionProto* module : program.modules)
        if (module != nullptr) disassembleFunction(*module, program, out);
}

// ---- Bytecode files ----

namespace {

const char kMagic[4] = {'A', 'S', 'T', 'R'};
const uint8_t kFormatVersion = 4;
const int kMaxProtoNesting = 200;

enum ConstantTag : uint8_t { TagInt = 0, TagFloat = 1, TagString = 2 };

struct Writer {
    std::ostream& out;

    void u8(uint8_t v) { out.put(static_cast<char>(v)); }
    void u16(uint16_t v) {
        u8(static_cast<uint8_t>(v & 0xFF));
        u8(static_cast<uint8_t>(v >> 8));
    }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; i++) u8(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
    void u64(uint64_t v) {
        for (int i = 0; i < 8; i++) u8(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
    void str(const std::string& s) {
        u32(static_cast<uint32_t>(s.size()));
        out.write(s.data(), static_cast<std::streamsize>(s.size()));
    }

    void proto(const FunctionProto& f) {
        str(f.name);
        str(f.source);
        u32(static_cast<uint32_t>(f.arity));
        u32(static_cast<uint32_t>(f.requiredArgs()));
        u32(static_cast<uint32_t>(f.varargIndex + 1));
        u64(f.classParams);
        u32(static_cast<uint32_t>(f.paramNames.size()));
        for (const ObjString* param : f.paramNames) str(param->chars);

        u32(static_cast<uint32_t>(f.upvalues.size()));
        for (const UpvalueDesc& uv : f.upvalues) {
            u8(uv.isLocal ? 1 : 0);
            u16(uv.index);
        }
        u32(static_cast<uint32_t>(f.outerReceivers.size()));
        for (uint16_t upvalue : f.outerReceivers) u16(upvalue);

        u32(static_cast<uint32_t>(f.code.size()));
        out.write(reinterpret_cast<const char*>(f.code.data()), static_cast<std::streamsize>(f.code.size()));

        // Line table, run-length encoded.
        std::vector<std::pair<uint32_t, uint32_t>> runs;
        for (int line : f.lines) {
            if (!runs.empty() && runs.back().first == static_cast<uint32_t>(line)) runs.back().second++;
            else runs.emplace_back(static_cast<uint32_t>(line), 1u);
        }
        u32(static_cast<uint32_t>(runs.size()));
        for (const auto& run : runs) {
            u32(run.first);
            u32(run.second);
        }

        u32(static_cast<uint32_t>(f.constants.size()));
        for (const Value& c : f.constants) {
            if (c.isInt()) {
                u8(TagInt);
                u64(static_cast<uint64_t>(c.as.i));
            } else if (c.isFloat()) {
                uint64_t bits;
                std::memcpy(&bits, &c.as.d, sizeof bits);
                u8(TagFloat);
                u64(bits);
            } else {
                u8(TagString);
                str(asString(c)->chars);
            }
        }

        u32(static_cast<uint32_t>(f.protos.size()));
        for (const FunctionProto* nested : f.protos) proto(*nested);
    }
};

struct Reader {
    const std::string& data;
    Program& program;
    VM& vm;
    size_t pos = 0;

    [[noreturn]] void corrupt() { throw std::runtime_error("corrupt or truncated bytecode file"); }

    void need(size_t n) {
        if (n > data.size() - pos) corrupt();
    }
    uint8_t u8() {
        need(1);
        return static_cast<uint8_t>(data[pos++]);
    }
    uint16_t u16() {
        uint16_t lo = u8();
        return static_cast<uint16_t>(lo | (u8() << 8));
    }
    uint32_t u32() {
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(u8()) << (8 * i);
        return v;
    }
    uint64_t u64() {
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) v |= static_cast<uint64_t>(u8()) << (8 * i);
        return v;
    }
    // A count of items that each take at least `itemSize` bytes in the file.
    uint32_t count(size_t itemSize) {
        uint32_t n = u32();
        if (n > (data.size() - pos) / itemSize) corrupt();
        return n;
    }
    std::string str() {
        uint32_t n = count(1);
        std::string s = data.substr(pos, n);
        pos += n;
        return s;
    }

    FunctionProto* proto(int nesting) {
        if (nesting > kMaxProtoNesting) corrupt();
        FunctionProto* f = program.newProto();
        f->name = str();
        f->source = str();
        f->arity = static_cast<int>(u32());
        f->minArity = static_cast<int>(u32());
        if (f->minArity < 0 || f->minArity > f->arity) corrupt();
        uint32_t vararg = u32();
        if (vararg > static_cast<uint32_t>(f->arity)) corrupt();
        f->varargIndex = static_cast<int>(vararg) - 1;
        f->classParams = u64();
        uint32_t paramCount = count(4);
        for (uint32_t i = 0; i < paramCount; i++) f->paramNames.push_back(vm.intern(str()));

        uint32_t upvalueCount = count(3);
        for (uint32_t i = 0; i < upvalueCount; i++) {
            bool isLocal = u8() != 0;
            f->upvalues.push_back(UpvalueDesc{isLocal, u16()});
        }
        uint32_t receiverCount = count(2);
        for (uint32_t i = 0; i < receiverCount; i++) {
            uint16_t upvalue = u16();
            if (upvalue >= f->upvalues.size()) corrupt();
            f->outerReceivers.push_back(upvalue);
        }

        uint32_t codeSize = count(1);
        f->code.assign(data.begin() + static_cast<std::ptrdiff_t>(pos),
                       data.begin() + static_cast<std::ptrdiff_t>(pos + codeSize));
        pos += codeSize;

        uint32_t runs = count(8);
        for (uint32_t i = 0; i < runs; i++) {
            uint32_t line = u32();
            uint32_t length = u32();
            if (length > codeSize - f->lines.size()) corrupt();
            f->lines.insert(f->lines.end(), length, static_cast<int>(line));
        }

        uint32_t constantCount = count(1);
        for (uint32_t i = 0; i < constantCount; i++) {
            switch (u8()) {
                case TagInt:
                    f->constants.push_back(Value::integer(static_cast<int64_t>(u64())));
                    break;
                case TagFloat: {
                    uint64_t bits = u64();
                    double d;
                    std::memcpy(&d, &bits, sizeof d);
                    f->constants.push_back(Value::number(d));
                    break;
                }
                case TagString:
                    f->constants.push_back(Value::object(vm.intern(str())));
                    break;
                default:
                    corrupt();
            }
        }

        uint32_t protoCount = count(1);
        for (uint32_t i = 0; i < protoCount; i++) f->protos.push_back(proto(nesting + 1));
        return f;
    }
};

void verifyTree(FunctionProto& f, const Program& program) {
    verifyFunction(f, program);
    for (FunctionProto* nested : f.protos) verifyTree(*nested, program);
}

}  // namespace

void writeProgram(const Program& program, std::ostream& out) {
    Writer w{out};
    out.write(kMagic, sizeof kMagic);
    w.u8(kFormatVersion);

    w.u32(static_cast<uint32_t>(program.globalNames.size()));
    for (const std::string& name : program.globalNames) w.str(name);

    w.u32(static_cast<uint32_t>(program.modules.size()));
    for (const FunctionProto* module : program.modules) w.proto(*module);
    w.u32(static_cast<uint32_t>(program.entry));
}

bool looksLikeBytecode(const std::string& data) {
    return data.size() >= sizeof kMagic && std::memcmp(data.data(), kMagic, sizeof kMagic) == 0;
}

void readProgram(Program& program, VM& vm, const std::string& data) {
    if (!looksLikeBytecode(data)) throw std::runtime_error("not an Astra bytecode file");
    Reader r{data, program, vm, sizeof kMagic};

    uint8_t version = r.u8();
    if (version != kFormatVersion)
        throw std::runtime_error("unsupported bytecode version " + std::to_string(version) +
                                 " (this build reads version " + std::to_string(kFormatVersion) + ")");

    uint32_t globalCount = r.count(4);
    if (globalCount > 65536) r.corrupt();
    for (uint32_t i = 0; i < globalCount; i++) program.globalNames.push_back(r.str());

    uint32_t moduleCount = r.count(1);
    if (moduleCount == 0 || moduleCount > 65536) r.corrupt();
    for (uint32_t i = 0; i < moduleCount; i++) program.modules.push_back(r.proto(0));

    uint32_t entry = r.u32();
    if (entry >= moduleCount) r.corrupt();
    program.entry = static_cast<int>(entry);

    for (FunctionProto* module : program.modules) {
        if (module->arity != 0 || !module->upvalues.empty())
            throw std::runtime_error("invalid bytecode: a module cannot take arguments or capture variables");
        verifyTree(*module, program);
    }
}

}  // namespace astra
