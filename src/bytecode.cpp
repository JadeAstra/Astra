#include "bytecode.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <ostream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "vm.h"

namespace astra {

namespace {

enum class Operand : uint8_t { None, U8, U16, I32, U16U8, U16I32, U16U16, U8U8, U16U16U8, U8I32 };

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
    {"GET_PROP", Operand::U16U16},
    {"SET_PROP", Operand::U16U16},
    {"GET_INDEX", Operand::None},
    {"SET_INDEX", Operand::None},
    {"GET_SUPER", Operand::U16},
    {"SET_SUPER", Operand::U16},
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
    {"UNSTABLE_KEY", Operand::None},
    {"LAZY", Operand::None},
    {"EXTEND", Operand::None},
    {"BRIDGE", Operand::U16},
    {"COMPONENT", Operand::U8},
    {"MISSING_ARG", Operand::U16},
    {"TAIL_LOOP", Operand::U8},
    {"IMPLICIT_FIRST", Operand::U16U8},
    {"CALL_ON", Operand::U8},
    {"CLASS_INIT", Operand::None},
    {"ENSURE_CLASS_INIT", Operand::None},
    {"EXTENSION", Operand::U16U8},
    {"MARK_ABSTRACT", Operand::None},
    {"MARK_VALUE", Operand::U8},
    {"CHECK_OVERRIDE", Operand::U16U8},
    {"INVOKE_TRAILING", Operand::U16U8},
    {"SUPER_INVOKE_TRAILING", Operand::U16U8},
    {"CLASS_NUMBER", Operand::U16U16},
    {"MEMBER_ACCESS", Operand::U16U16U8},
    {"STATIC_ACCESS", Operand::U16U16U8},
    {"GET_OWN", Operand::U16U16},
    {"SET_OWN", Operand::U16U16},
    {"INVOKE_OWN", Operand::U16U8},
    {"INVOKE_OWN_TRAILING", Operand::U16U8},
    {"MAKE_FORMS", Operand::U16U8},
    {"METHOD_FORMS", Operand::U16U8},
    {"STATIC_FORMS", Operand::U16U8},
    {"GET_INDEX_OR", Operand::None},
    {"ADD_LOCAL", Operand::U16}, {"SUB_LOCAL", Operand::U16}, {"MUL_LOCAL", Operand::U16}, {"DIV_LOCAL", Operand::U16},
    {"MOD_LOCAL", Operand::U16}, {"LT_LOCAL", Operand::U16}, {"LE_LOCAL", Operand::U16}, {"GT_LOCAL", Operand::U16},
    {"GE_LOCAL", Operand::U16}, {"EQ_LOCAL", Operand::U16},
    {"ADD_CONST", Operand::U16}, {"SUB_CONST", Operand::U16}, {"MUL_CONST", Operand::U16}, {"DIV_CONST", Operand::U16},
    {"MOD_CONST", Operand::U16}, {"LT_CONST", Operand::U16}, {"LE_CONST", Operand::U16}, {"GT_CONST", Operand::U16},
    {"GE_CONST", Operand::U16}, {"EQ_CONST", Operand::U16},
    {"STORE_LOCAL", Operand::U16},
    {"CHECK_LOCAL", Operand::U16U16U8},
    {"RANGE_LOOP", Operand::U8},
    {"RANGE_NEXT", Operand::U16I32},
    {"ADD_LOCALS", Operand::U16U16},
    {"SUB_LOCALS", Operand::U16U16},
    {"MUL_LOCALS", Operand::U16U16},
    {"DIV_LOCALS", Operand::U16U16},
    {"LT_LOCALS", Operand::U16U16},
    {"LE_LOCALS", Operand::U16U16},
    {"GT_LOCALS", Operand::U16U16},
    {"GE_LOCALS", Operand::U16U16},
    {"EQ_LOCALS", Operand::U16U16},
    {"COPY_LOCAL", Operand::U16U16},
    {"OBJECT_CLASS", Operand::U16I32},
    {"STORE_CLASS", Operand::U16},
    {"CAPTURE", Operand::U16U8},
    {"NEW_OBJECT", Operand::U8},
    {"GET_CAPTURED", Operand::U16U16},
    {"SET_CAPTURED", Operand::U16},
    {"MAKE_TYPE", Operand::U16},
    {"CACHED_TYPE", Operand::U16I32},
    {"CACHE_TYPE", Operand::U16},
    {"TAG_COLLECTION", Operand::U16I32},
    {"TYPE_COLLECTION", Operand::None},
    {"IS_FULL", Operand::None},
    {"CAST_FULL", Operand::U8},
    {"WITH_TYPES", Operand::None},
    {"TYPE_ARG", Operand::U8},
    {"INSTANCE_TYPE_ARG", Operand::U8},
    {"SUPERTYPE_MAKER", Operand::None},
    {"CHECK_RESULT", Operand::U16},
    {"ADD_NEW", Operand::U8I32},
    {"PLACE_LOCAL", Operand::U16},
    {"PLACE_UPVALUE", Operand::U16},
    {"PLACE_GLOBAL", Operand::U16},
    {"PLACE_FIELD", Operand::U16},
    {"PLACE_INDEX", Operand::None},
    {"GET_INDEX_REF", Operand::None},
    {"ITER_NEXT_REF", Operand::U16I32},
    {"GET_PROP_REF", Operand::U16U16},
    {"GET_OWN_REF", Operand::U16U16},
    {"FIELD_DESTINATION", Operand::U16},
    {"WITH_REF_RESULT", Operand::None},
    {"NUMERIC", Operand::U8},
    {"USHR", Operand::None},
    {"ADD_WRAP", Operand::None},
    {"SUB_WRAP", Operand::None},
    {"MUL_WRAP", Operand::None},
    {"OWN_LOCAL", Operand::U16},
    {"WITH_DESTINATION_AT", Operand::U8},
    {"WITH_FRAME_DESTINATION", Operand::None},
    {"FROM_END", Operand::None},
    {"ITER_NEXT_PAIR", Operand::U16I32},
};

const char* const kTypeNames[] = {"", "int", "float", "Number", "string", "bool", "List", "Map", "Range", "Function", "Bytes", "Type", "",
                                   "char", "byte", "int16", "int32", "uint16", "uint32", "float32", "Index", "Set",
                                   "FrozenList", "FrozenMap", "FrozenSet"};
static_assert(sizeof(kTypeNames) / sizeof(kTypeNames[0]) == kTypeCount_, "kTypeNames must match TypeId");

static_assert(sizeof(kOps) / sizeof(kOps[0]) == static_cast<size_t>(Op::Count_), "kOps must match the Op enum");

size_t operandSize(Operand o) {
    switch (o) {
        case Operand::None: return 0;
        case Operand::U8: return 1;
        case Operand::U16: return 2;
        case Operand::I32: return 4;
        case Operand::U16U8: return 3;
        case Operand::U16I32: return 6;
        case Operand::U8I32: return 5;
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
        case Operand::U8I32:
            a = code[ip + 1];
            b = readI32(code, ip + 2);
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
    for (uint8_t id = kTypeInt; id < kTypeCount_; id++)
        if (id != kTypeInstance && name == kTypeNames[id]) return id;
    return 0;
}

std::string typeText(uint8_t type, const std::string& className) {
    uint8_t id = type & kTypeMask;
    std::string text = id == kTypeInstance ? className : isBuiltinTypeId(id) ? kTypeNames[id] : "?";
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

    if (f.chooser) {
        // A set of forms runs none of its own code.
        if (!f.code.empty() || f.arity != 0) fail("a set of forms has code of its own");
        return 1;
    }
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
        if ((upvalue & kCapturedReceiver) == 0 && upvalue >= f.upvalues.size()) fail("receiver upvalue out of range");

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

    int caches = 0;  // one more than the highest cache a GetProp or the like names

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
                case Op::AddLocal:
                case Op::SubLocal:
                case Op::MulLocal:
                case Op::DivLocal:
                case Op::ModLocal:
                case Op::LtLocal:
                case Op::LeLocal:
                case Op::GtLocal:
                case Op::GeLocal:
                case Op::EqLocal:
                    if (a >= depth) fail("local slot out of range");
                    pops = pushes = 1;
                    maxDepth = std::max(maxDepth, depth + 1);  // the local is pushed where they are not worked out at once
                    break;
                case Op::AddLocals:
                case Op::SubLocals:
                case Op::MulLocals:
                case Op::DivLocals:
                case Op::LtLocals:
                case Op::LeLocals:
                case Op::GtLocals:
                case Op::GeLocals:
                case Op::EqLocals:
                    if (a >= depth || b >= depth) fail("local slot out of range");
                    pushes = 1;
                    maxDepth = std::max(maxDepth, depth + 2);  // both are pushed where they are not worked out at once
                    break;
                case Op::CopyLocal:
                    if (a >= depth || b >= depth) fail("local slot out of range");
                    break;
                case Op::AddConst:
                case Op::SubConst:
                case Op::MulConst:
                case Op::DivConst:
                case Op::ModConst:
                case Op::LtConst:
                case Op::LeConst:
                case Op::GtConst:
                case Op::GeConst:
                case Op::EqConst:
                    if (static_cast<size_t>(a) >= f.constants.size()) fail("constant out of range");
                    pops = pushes = 1;
                    maxDepth = std::max(maxDepth, depth + 1);
                    break;
                case Op::StoreLocal:
                    if (a >= depth - 1) fail("local slot out of range");
                    pops = 1;
                    break;
                case Op::CheckLocal:
                    if (a >= depth) fail("local slot out of range");
                    requireName(b);
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
                case Op::GetOwn:
                case Op::GetPropRef:
                case Op::GetOwnRef:
                    requireName(a);
                    caches = std::max(caches, b + 1);
                    pops = pushes = 1;
                    break;
                case Op::FieldDestination:
                    requireName(a);
                    pops = pushes = 1;
                    break;
                case Op::WithRefResult:
                    break;
                case Op::Ushr:
                case Op::AddWrap:
                case Op::SubWrap:
                case Op::MulWrap:
                    pops = 2;
                    pushes = 1;
                    break;
                case Op::Numeric:
                    if (a >= static_cast<int>(NumericOp::Count_)) fail("unknown numeric operation");
                    pops = numericArity(static_cast<uint8_t>(a));
                    pushes = 1;
                    break;
                case Op::SetProp:
                case Op::SetOwn:
                    requireName(a);
                    caches = std::max(caches, b + 1);
                    pops = 2;
                    pushes = 1;
                    break;
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
                case Op::SetSuper:
                    requireName(a);
                    pops = 3;
                    pushes = 1;
                    break;
                case Op::Extend:
                    pops = 2;
                    pushes = 1;
                    break;
                case Op::Bridge:
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
                case Op::MemberAccess:
                case Op::StaticAccess:
                    requireName(a);
                    pops = pushes = 1;
                    break;
                case Op::ClassNumber:
                    pops = pushes = 1;
                    break;
                case Op::EnumSeal:
                case Op::UnstableKey:
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
                case Op::Lazy:
                    pops = pushes = 1;
                    break;
                case Op::ObjectClass:
                    if (a >= static_cast<int>(program.globalNames.size()) && a != 0xFFFF) fail("global out of range");
                    flow(b, depth + 1);
                    maxDepth = std::max(maxDepth, depth + 1);
                    break;
                case Op::StoreClass:
                    if (a >= static_cast<int>(program.globalNames.size()) && a != 0xFFFF) fail("global out of range");
                    pops = pushes = 1;
                    break;
                case Op::Capture:
                    if (b > 3 || (b == 0 && a >= depth) || (b == 3 && a >= depth)) fail("bad capture");
                    pushes = 1;
                    break;
                case Op::NewObject:
                    pops = a + 1;
                    pushes = 1;
                    break;
                case Op::GetCaptured:
                    requireName(b);
                    pushes = 1;
                    break;
                case Op::MakeType: {
                    requireName(a);
                    const int operands = shapeOperands(asString(f.constants[static_cast<size_t>(a)])->utf8());
                    if (operands < 0) fail("bad type shape");
                    pops = operands;
                    pushes = 1;
                    break;
                }
                case Op::CachedType:
                    caches = std::max(caches, a + 1);
                    flow(b, depth + 1);
                    maxDepth = std::max(maxDepth, depth + 1);
                    break;
                case Op::CacheType:
                    caches = std::max(caches, a + 1);
                    pops = pushes = 1;
                    break;
                case Op::TagCollection:
                    caches = std::max(caches, a + 1);
                    flow(b, depth);
                    break;
                case Op::TypeCollection:
                case Op::IsFull:
                case Op::CastFull:
                    pops = 2;
                    pushes = 1;
                    break;
                case Op::CheckResult:
                    requireName(a);
                    pops = 2;
                    pushes = 1;
                    break;
                case Op::PlaceLocal:
                    if (a >= depth) fail("local slot out of range");
                    pushes = 1;
                    break;
                case Op::PlaceUpvalue:
                    if (static_cast<size_t>(a) >= f.upvalues.size()) fail("captured variable out of range");
                    pushes = 1;
                    break;
                case Op::PlaceGlobal:
                    if (static_cast<size_t>(a) >= program.globalNames.size()) fail("global out of range");
                    pushes = 1;
                    break;
                case Op::PlaceField:
                    requireName(a);
                    pops = pushes = 1;
                    break;
                case Op::PlaceIndex:
                case Op::GetIndexRef:
                    pops = 2;
                    pushes = 1;
                    break;
                case Op::OwnLocal:
                    if (a >= depth) fail("local slot out of range");
                    break;
                case Op::WithDestinationAt:
                    if (depth < a + 1) fail("stack underflow");
                    break;
                case Op::WithFrameDestination:
                    break;
                case Op::AddNew:
                    // On at the target with the add's result, or on to the call, all as they were.
                    if (depth < a + 2) fail("stack underflow");
                    flow(b, depth - a - 1);
                    break;
                case Op::WithTypes:
                    pops = 1;
                    break;
                case Op::TypeArg:
                    if ((f.typeArgs & (kTypeArgsOwn | kTypeArgsKept)) == 0) fail("type arguments out of reach");
                    pushes = 1;
                    break;
                case Op::InstanceTypeArg:
                case Op::SupertypeMaker:
                    pops = 2;
                    pushes = 1;
                    break;
                case Op::SetCaptured:
                    pops = pushes = 1;
                    break;
                case Op::Range:
                    if ((a & ~7) != 0) fail("bad range");
                    pops = (a & 4) != 0 ? 3 : 2;
                    pushes = 1;
                    break;
                case Op::RangeLoop:
                    if ((a & ~7) != 0) fail("bad range");
                    pops = (a & 4) != 0 ? 3 : 2;
                    pushes = 3;
                    break;
                case Op::RangeNext:
                    if (a + 2 >= depth) fail("local slot out of range");
                    flow(b, depth);
                    pushes = 1;
                    break;
                case Op::MissingArg:
                    requireName(a);
                    terminal = true;
                    break;
                case Op::TailLoop:
                    pops = a;
                    terminal = true;
                    break;
                case Op::CallOn:
                    pops = a + 2;
                    pushes = 1;
                    break;
                case Op::ImplicitFirst:
                    if (a >= depth) fail("local slot out of range");
                    if ((b & 0xFE) != 0) fail("bad flags");
                    break;
                case Op::ClassInit:
                    pops = 2;
                    pushes = 1;
                    break;
                case Op::EnsureClassInit:
                case Op::MarkAbstract:
                case Op::MarkValue:
                    pops = pushes = 1;
                    break;
                case Op::CheckOverride:
                    requireName(a);
                    pops = pushes = 1;
                    break;
                case Op::Extension:
                    requireName(a);
                    if ((b & kTypeMask) >= kTypeCount_) fail("unknown type");
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
                    if (id >= kTypeCount_ || (b & ~(0xFF | kFieldVal)) != 0) fail("bad field declaration");
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
                    if (id < kTypeInt || id >= kTypeCount_) fail("unknown type");
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
                case Op::InvokeOwn:
                    requireName(a);
                    pops = b + 1;
                    pushes = 1;
                    break;
                case Op::InvokeTrailing:
                case Op::InvokeOwnTrailing:
                    requireName(a);
                    if (b < 1) fail("a call with a lambda after it has no arguments");
                    pops = b + 1;
                    pushes = 1;
                    break;
                case Op::SuperInvoke:
                    requireName(a);
                    pops = b + 2;
                    pushes = 1;
                    break;
                case Op::SuperInvokeTrailing:
                    requireName(a);
                    if (b < 1) fail("a call with a lambda after it has no arguments");
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
                case Op::MakeForms:
                    if (static_cast<size_t>(a) >= f.protos.size() || !f.protos[a]->chooser) fail("bad set of forms");
                    if (b < 2) fail("a set of forms has fewer than two");
                    pops = b;
                    pushes = 1;
                    break;
                case Op::GetIndexOr:
                    pops = 3;
                    pushes = 1;
                    break;
                case Op::MethodForms:
                case Op::StaticForms:
                    if (static_cast<size_t>(a) >= f.protos.size() || !f.protos[a]->chooser) fail("bad set of forms");
                    if (b < 1) fail("a set of forms has none");
                    pops = b + 1;
                    pushes = 1;
                    break;
                case Op::Closure: {
                    if (static_cast<size_t>(a) >= f.protos.size()) fail("nested function out of range");
                    // A closure keeps the type arguments of the frame it is made in, which has them.
                    if ((f.protos[a]->typeArgs & kTypeArgsKept) != 0 && (f.typeArgs & (kTypeArgsOwn | kTypeArgsKept)) == 0) fail("type arguments out of reach");
                    // A local function may capture its own slot, which is the one being pushed.
                    for (const UpvalueDesc& uv : f.protos[a]->upvalues) {
                        if (uv.fromCapture) continue;  // checked when the closure is made
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
                    if (id >= kTypeCount_ || (b & ~(0xFF | kFieldVal | kFieldRequired | kFieldOverride)) != 0) fail("bad field declaration");
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
                case Op::IterNextRef:
                    if (a >= depth) fail("local slot out of range");
                    flow(b, depth);
                    pushes = 1;
                    break;
                case Op::IterNextPair:
                    if (a >= depth) fail("local slot out of range");
                    flow(b, depth);
                    pushes = 2;
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
                    if (id < kTypeInt || id >= kTypeCount_) fail("unknown type");
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

    if (!partial) {
        f.maxStack = maxDepth;
        if (static_cast<uint32_t>(caches) > f.cacheCount) {
            f.caches.reset(new std::atomic<uint64_t>[static_cast<size_t>(caches)]());
            f.cacheCount = static_cast<uint32_t>(caches);
        }
    }
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
            if (v.isString()) return quoteString(asString(v)->utf8());
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
            case Op::AddConst:
            case Op::SubConst:
            case Op::MulConst:
            case Op::DivConst:
            case Op::ModConst:
            case Op::LtConst:
            case Op::LeConst:
            case Op::GtConst:
            case Op::GeConst:
            case Op::EqConst:
                out << " " << a << "  ; " << constantText(f.constants[a]);
                break;
            case Op::GetGlobal:
            case Op::SetGlobal:
            case Op::DefineGlobal:
                out << " " << a << "  ; " << program.globalNames[a];
                break;
            case Op::GetProp:
            case Op::SetProp:
            case Op::GetOwn:
            case Op::SetOwn:
            case Op::GetPropRef:
            case Op::GetOwnRef:
            case Op::FieldDestination:
            case Op::GetSuper:
            case Op::SetSuper:
            case Op::Class:
            case Op::Method:
            case Op::StaticMethod:
            case Op::Getter:
            case Op::Setter:
            case Op::StaticGetter:
            case Op::StaticSetter:
                out << " " << a << "  ; " << asString(f.constants[a])->utf8();
                break;
            case Op::Invoke:
            case Op::InvokeTrailing:
            case Op::InvokeOwn:
            case Op::InvokeOwnTrailing:
            case Op::SuperInvoke:
            case Op::SuperInvokeTrailing:
                out << " " << a << " argc=" << b << "  ; " << asString(f.constants[a])->utf8();
                break;
            case Op::GetImplicit:
            case Op::SetImplicit:
                out << " " << a << " " << b << "  ; " << asString(f.constants[a])->utf8();
                break;
            case Op::CallNamed:
                out << " positional=" << a << " named=" << b;
                break;
            case Op::TryImplicit:
                out << " " << a << " -> " << b << "  ; " << asString(f.constants[a])->utf8();
                break;
            case Op::InvokeImplicit:
                out << " " << a << " " << b << " argc=" << static_cast<int>(f.code[ip + 5]) << "  ; " << asString(f.constants[a])->utf8();
                break;
            case Op::Closure:
                out << " " << a << "  ; " << f.protos[a]->name;
                break;
            case Op::MakeForms:
            case Op::MethodForms:
            case Op::StaticForms:
                out << " " << a << " count=" << b << "  ; " << f.protos[a]->name;
                break;
            case Op::DefaultArg:
            case Op::IterNext:
            case Op::IterNextRef:
            case Op::IterNextPair:
            case Op::RangeNext:
                out << " slot=" << a << " -> " << b;
                break;
            case Op::CachedType:
            case Op::TagCollection:
                out << " cache=" << a << " -> " << b;
                break;
            case Op::AddNew:
                out << " argc=" << a << " -> " << b;
                break;
            case Op::ObjectClass:
                out << " " << a << " -> " << b;
                break;
            case Op::Capture:
                out << " " << a << " kind=" << b;
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
                out << " " << a << "  ; " << asString(f.constants[a])->utf8() << " : " << typeText(static_cast<uint8_t>(b), "<class>");
                break;
            case Op::CheckReturn:
            case Op::IsType:
            case Op::Cast:
            case Op::CastOrNull:
                out << "  ; " << typeText(static_cast<uint8_t>(a), "<class>");
                break;
            case Op::StaticField:
            case Op::DeclareField:
                out << " " << a << "  ; " << ((b & kFieldVal) ? "val " : "var ") << asString(f.constants[a])->utf8();
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

size_t instructionSize(const std::vector<uint8_t>& code, size_t at) {
    const uint8_t raw = code[at];
    return raw < static_cast<uint8_t>(Op::Count_) ? 1 + operandSize(kOps[raw].operand) : 1;
}

int shapeOperands(const std::string& shape) {
    int operands = 0;
    for (size_t at = 0; at < shape.size(); at++) {
        const char c = shape[at];
        if (c == 'C') {
            if (at + 1 >= shape.size() || shape[at + 1] < '0' || shape[at + 1] > '9') return -1;
            operands++;
            at++;
        } else if (c == 'A') {
            if (at + 1 >= shape.size() || shape[at + 1] < '0' || shape[at + 1] > '9') return -1;
            at++;
        } else if (c == 'P') {
            operands++;
        } else if (std::string_view("?aifnsbyrtFLMcuhdHDgxSZ").find(c) == std::string_view::npos) {
            return -1;
        }
    }
    return operands;
}

const char* opName(Op op) {
    const size_t i = static_cast<size_t>(op);
    return i < static_cast<size_t>(Op::Count_) ? kOps[i].name : "?";
}

void disassemble(const Program& program, std::ostream& out) {
    // Module 0 is the standard library, the same in every program, and is left out.
    for (size_t i = 1; i < program.modules.size(); i++)
        if (program.modules[i] != nullptr) disassembleFunction(*program.modules[i], program, out);
}

// ---- Bytecode files ----

namespace {

const char kMagic[4] = {'A', 'S', 'T', 'R'};
const uint8_t kFormatVersion = 20;
const int kMaxProtoNesting = 200;

enum ConstantTag : uint8_t { TagInt = 0, TagFloat = 1, TagString = 2, TagChar = 3, TagByte = 4 };

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
        u8(f.typeArgs);
        u32(static_cast<uint32_t>(f.paramNames.size()));
        for (const ObjString* param : f.paramNames) str(param->utf8());

        u32(static_cast<uint32_t>(f.upvalues.size()));
        for (const UpvalueDesc& uv : f.upvalues) {
            u8(uv.fromCapture ? 2 : uv.isLocal ? 1 : 0);
            u16(uv.index);
        }
        u32(static_cast<uint32_t>(f.outerReceivers.size()));
        for (uint16_t upvalue : f.outerReceivers) u16(upvalue);
        u32(static_cast<uint32_t>(f.classIds.size()));
        for (uint16_t id : f.classIds) u16(id);
        u16(f.ownClass);
        u8(f.chooser ? 1 : 0);
        str(f.signature);
        u32(static_cast<uint32_t>(f.paramTypes.size()));
        for (size_t i = 0; i < f.paramTypes.size(); i++) {
            u8(f.paramTypes[i]);
            str(i < f.paramClasses.size() && f.paramClasses[i] != nullptr ? f.paramClasses[i]->utf8() : std::string());
            u8(i < f.paramCounts.size() ? f.paramCounts[i] : 0);
        }

        u32(static_cast<uint32_t>(f.code.size()));
        out.write(reinterpret_cast<const char*>(f.code.data()), static_cast<std::streamsize>(f.code.size()));
        u32(static_cast<uint32_t>(f.spans.size()));
        for (const DeclarationSpan& span : f.spans) {
            u32(span.start);
            u32(span.end);
            u16(span.key);
            u8(span.kind);
        }

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
            } else if (c.isChar() || c.isByte()) {
                u8(c.isChar() ? TagChar : TagByte);
                u64(static_cast<uint64_t>(c.as.i));
            } else {
                u8(TagString);
                str(asString(c)->utf8());
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
        f->typeArgs = u8();
        if ((f->typeArgs & ~(kTypeArgsOwn | kTypeArgsKept | kTakesDestination)) != 0) corrupt();
        uint32_t paramCount = count(4);
        for (uint32_t i = 0; i < paramCount; i++) f->paramNames.push_back(vm.intern(str()));

        uint32_t upvalueCount = count(3);
        for (uint32_t i = 0; i < upvalueCount; i++) {
            const uint8_t kind = u8();
            if (kind > 2) corrupt();
            f->upvalues.push_back(UpvalueDesc{kind == 1, u16(), kind == 2});
        }
        uint32_t receiverCount = count(2);
        for (uint32_t i = 0; i < receiverCount; i++) {
            uint16_t upvalue = u16();
            if ((upvalue & kCapturedReceiver) == 0 && upvalue >= f->upvalues.size()) corrupt();
            f->outerReceivers.push_back(upvalue);
        }
        uint32_t classCount = count(2);
        for (uint32_t i = 0; i < classCount; i++) f->classIds.push_back(u16());
        f->ownClass = u16();
        f->chooser = u8() != 0;
        f->signature = str();
        uint32_t typed = count(2);
        if (typed != 0 && typed != static_cast<uint32_t>(f->arity)) corrupt();
        for (uint32_t i = 0; i < typed; i++) {
            f->paramTypes.push_back(u8());
            const std::string cls = str();
            f->paramClasses.push_back(cls.empty() ? nullptr : vm.intern(cls));
            f->paramCounts.push_back(u8());
        }

        uint32_t codeSize = count(1);
        f->code.assign(data.begin() + static_cast<std::ptrdiff_t>(pos),
                       data.begin() + static_cast<std::ptrdiff_t>(pos + codeSize));
        pos += codeSize;
        uint32_t spanCount = count(11);
        for (uint32_t i = 0; i < spanCount; i++) {
            DeclarationSpan span;
            span.start = u32();
            span.end = u32();
            span.key = u16();
            const uint8_t kind = u8();
            if (kind > DeclarationSpan::Extension || span.start > span.end || span.end > codeSize) corrupt();
            span.kind = static_cast<DeclarationSpan::Kind>(kind);
            f->spans.push_back(span);
        }

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
                case TagChar: {
                    const uint64_t c = u64();
                    if (!isCharacter(static_cast<int64_t>(c))) corrupt();
                    f->constants.push_back(Value::character(static_cast<uint32_t>(c)));
                    break;
                }
                case TagByte: {
                    const uint64_t b = u64();
                    if (b > 255) corrupt();
                    f->constants.push_back(Value::byte(static_cast<uint8_t>(b)));
                    break;
                }
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

void writeU16(std::vector<uint8_t>& code, size_t at, int v) {
    code[at] = static_cast<uint8_t>(v & 0xFF);
    code[at + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

}  // namespace

void renumber(FunctionProto& f, const Renumbering& r, VM& vm) {
    std::vector<uint8_t>& code = f.code;
    for (DeclarationSpan& span : f.spans)
        if (span.kind == DeclarationSpan::Global) span.key = static_cast<uint16_t>(r.global(span.key));
    for (size_t ip = 0; ip < code.size();) {
        const uint8_t raw = code[ip];
        if (raw >= static_cast<uint8_t>(Op::Count_)) break;  // malformed: verifying will say so
        const size_t size = 1 + operandSize(kOps[raw].operand);
        if (ip + size > code.size()) break;
        auto first = [&](const std::function<int(int)>& map) { writeU16(code, ip + 1, map(readU16(code, ip + 1))); };
        auto second = [&](const std::function<int(int)>& map) { writeU16(code, ip + 3, map(readU16(code, ip + 3))); };
        switch (static_cast<Op>(raw)) {
            case Op::GetGlobal:
            case Op::SetGlobal:
            case Op::DefineGlobal:
            case Op::DefineTypedGlobal: first(r.global); break;
            case Op::ObjectClass:
            case Op::StoreClass:
                if (readU16(code, ip + 1) != 0xFFFF) first(r.global);  // 0xFFFF: kept nowhere
                break;
            case Op::GetImplicit:
            case Op::SetImplicit:
            case Op::InvokeImplicit: second(r.global); break;
            case Op::Import: first(r.module); break;
            case Op::ClassNumber:
                first(r.number);
                second(r.number);
                break;
            case Op::MemberAccess:
            case Op::StaticAccess: second(r.number); break;
            default: break;
        }
        ip += size;
    }
    for (uint16_t& id : f.classIds) id = static_cast<uint16_t>(r.number(id));
    if (f.ownClass != 0) f.ownClass = static_cast<uint16_t>(r.number(f.ownClass));
    // The script a worker runs is named by a string the compiler made: "\x01<module>\x01<path>".
    for (Value& c : f.constants) {
        if (!c.isObjType(ObjType::String)) continue;
        const std::string& text = asString(c)->utf8();
        if (text.size() < 3 || text[0] != '\x01') continue;
        const size_t end = text.find('\x01', 1);
        if (end == std::string::npos) continue;
        const int module = r.module(std::stoi(text.substr(1, end - 1)));
        c = Value::object(vm.intern('\x01' + std::to_string(module) + text.substr(end)));
    }
    for (FunctionProto* nested : f.protos) renumber(*nested, r, vm);
}

void writeModules(const std::vector<const FunctionProto*>& modules, std::ostream& out) {
    Writer w{out};
    w.u8(kFormatVersion);
    w.u32(static_cast<uint32_t>(modules.size()));
    for (const FunctionProto* module : modules) w.proto(*module);
}

std::vector<FunctionProto*> readModules(Program& program, VM& vm, const std::string& data) {
    Reader r{data, program, vm, 0};
    if (r.u8() != kFormatVersion) throw std::runtime_error("kept from another version of the bytecode");
    std::vector<FunctionProto*> modules;
    const uint32_t count = r.count(1);
    for (uint32_t i = 0; i < count; i++) modules.push_back(r.proto(0));
    if (r.pos != data.size()) r.corrupt();
    return modules;
}

void verifyModule(FunctionProto& module, const Program& program) {
    if (module.arity != 0 || !module.upvalues.empty())
        throw std::runtime_error("invalid bytecode: a module cannot take arguments or capture variables");
    verifyTree(module, program);
}

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


// ---- What a built program can never reach, left out (M6.5 part 5) ----
//
// The code of every module runs, but what lies inside a top-level declaration's span only
// makes that declaration (a function, or a class with its methods). Starting from the modules,
// everything their code can reach is followed: the globals it names, and for each global the
// spans that make it; the functions the reached code makes. The spans of the globals never
// reached are taken out of their modules' code, with the functions only they made.

namespace {

// Which operand of an instruction is a global's slot: 1 the first, 2 the second, 0 none.
int globalOperand(Op op, int first) {
    switch (op) {
        case Op::GetGlobal:
        case Op::SetGlobal:
        case Op::DefineGlobal:
        case Op::DefineTypedGlobal: return 1;
        case Op::ObjectClass:
        case Op::StoreClass: return first == 0xFFFF ? 0 : 1;
        case Op::GetImplicit:
        case Op::SetImplicit:
        case Op::InvokeImplicit: return 2;
        default: return 0;
    }
}

// Where in an instruction a place in the code to go to is, counted from the instruction's
// start in bytes; 0 for one that has none.
int targetOperand(Op op) {
    switch (op) {
        case Op::Jump:
        case Op::Loop:
        case Op::JumpIfFalse:
        case Op::And:
        case Op::Or:
        case Op::JumpIfNull:
        case Op::JumpIfNotNull:
        case Op::TryBegin: return 1;
        case Op::AddNew: return 2;
        case Op::ObjectClass:
        case Op::CachedType:
        case Op::TagCollection:
        case Op::RangeNext:
        case Op::TryImplicit:
        case Op::IterNext:
        case Op::IterNextRef:
        case Op::IterNextPair:
        case Op::DefaultArg: return 3;
        default: return 0;
    }
}

// An instruction whose first operand is the name of a member it uses.
bool usesMemberName(Op op) {
    switch (op) {
        case Op::GetProp:
        case Op::SetProp:
        case Op::GetOwn:
        case Op::SetOwn:
        case Op::GetPropRef:
        case Op::GetOwnRef:
        case Op::FieldDestination:
        case Op::Invoke:
        case Op::InvokeOwn:
        case Op::InvokeTrailing:
        case Op::InvokeOwnTrailing:
        case Op::SuperInvoke:
        case Op::SuperInvokeTrailing:
        case Op::GetSuper:
        case Op::SetSuper:
        case Op::GetImplicit:
        case Op::SetImplicit:
        case Op::TryImplicit:
        case Op::InvokeImplicit: return true;
        default: return false;
    }
}

// An instruction that makes a function of the code's own nested ones: its first operand.
bool makesFunction(Op op) {
    return op == Op::Closure || op == Op::MakeForms || op == Op::MethodForms || op == Op::StaticForms;
}

struct Reach {
    Program& program;
    std::vector<char> reachedGlobal;
    std::unordered_set<std::string> reachedName;
    std::unordered_set<const FunctionProto*> reachedProto;
    // An extension's name, and where the spans that make extensions of that name are.
    std::unordered_map<std::string, std::vector<std::pair<FunctionProto*, size_t>>> extensionsNamed;
    // A global, and where the spans that make it are: the module, and the span's place in it.
    std::unordered_map<int, std::vector<std::pair<FunctionProto*, size_t>>> spansOf;
    std::vector<std::pair<FunctionProto*, size_t>> spanWork;  // spans to look through
    std::vector<FunctionProto*> protoWork;                   // whole functions to look through

    explicit Reach(Program& p) : program(p), reachedGlobal(p.globalNames.size(), 0) {}

    void global(int slot) {
        if (slot < 0 || static_cast<size_t>(slot) >= reachedGlobal.size() || reachedGlobal[static_cast<size_t>(slot)]) return;
        reachedGlobal[static_cast<size_t>(slot)] = 1;
        auto found = spansOf.find(slot);
        if (found != spansOf.end())
            for (const auto& span : found->second) spanWork.push_back(span);
    }

    // A member name, without the signature a form of a name with several has after it
    // (`lastOrNull[List<T>]()`): extensions are matched by the name alone.
    static std::string bare(const std::string& member) { return member.substr(0, member.find('[')); }

    void name(const std::string& used) {
        const std::string member = bare(used);
        if (!reachedName.insert(member).second) return;
        auto found = extensionsNamed.find(member);
        if (found != extensionsNamed.end())
            for (const auto& span : found->second) spanWork.push_back(span);
    }

    // Whether the span was reached: its global, or for an extension its name.
    bool reached(const FunctionProto& module, const DeclarationSpan& span) const {
        if (span.kind == DeclarationSpan::Global) return reachedGlobal[span.key] != 0;
        return span.key < module.constants.size() && module.constants[span.key].isString() &&
               reachedName.count(bare(asString(module.constants[span.key])->utf8())) != 0;
    }

    void function(FunctionProto* f) {
        if (reachedProto.insert(f).second) protoWork.push_back(f);
    }

    // What the instructions of `f` from `start` to `end` reach; those inside one of its spans
    // only where `skipSpans` says so.
    void look(FunctionProto& f, size_t start, size_t end, bool skipSpans) {
        const std::vector<uint8_t>& code = f.code;
        for (size_t at = start; at < end && at < code.size();) {
            if (skipSpans) {
                bool skipped = false;
                for (const DeclarationSpan& span : f.spans)
                    if (span.start == at && span.end > at) {
                        at = span.end;
                        skipped = true;
                        break;
                    }
                if (skipped) continue;
            }
            const uint8_t raw = code[at];
            if (raw >= static_cast<uint8_t>(Op::Count_)) return;
            const Op op = static_cast<Op>(raw);
            int a = 0, b = 0;
            decodeOperands(code, at, kOps[raw].operand, a, b);
            const int which = globalOperand(op, a);
            if (which == 1) global(a);
            if (which == 2) global(b);
            if (makesFunction(op) && static_cast<size_t>(a) < f.protos.size()) function(f.protos[static_cast<size_t>(a)]);
            if (usesMemberName(op) && static_cast<size_t>(a) < f.constants.size() && f.constants[static_cast<size_t>(a)].isString())
                name(asString(f.constants[static_cast<size_t>(a)])->utf8());
            at += 1 + operandSize(kOps[raw].operand);
        }
    }

    void run() {
        while (!spanWork.empty() || !protoWork.empty()) {
            if (!spanWork.empty()) {
                auto [module, index] = spanWork.back();
                spanWork.pop_back();
                const DeclarationSpan span = module->spans[index];
                look(*module, span.start, span.end, false);
            } else {
                FunctionProto* f = protoWork.back();
                protoWork.pop_back();
                look(*f, 0, f->code.size(), !f->spans.empty());
            }
        }
    }
};

// Takes the spans not reached out of a module's code, and the functions only they made.
// Leaves the module as it was if its spans do not fall on whole instructions.
void leaveOut(FunctionProto& module, const Reach& reach, size_t& before, size_t& after) {
    std::vector<uint8_t>& code = module.code;
    before += code.size();
    std::vector<char> starts(code.size() + 1, 0);
    for (size_t at = 0; at < code.size(); at += instructionSize(code, at)) starts[at] = 1;
    starts[code.size()] = 1;
    std::vector<char> gone(code.size(), 0);
    bool any = false;
    for (const DeclarationSpan& span : module.spans) {
        if (!starts[span.start] || !starts[span.end]) {
            after += code.size();
            return;
        }
        if (reach.reached(module, span)) continue;
        for (uint32_t at = span.start; at < span.end; at++) gone[at] = 1;
        any = true;
    }
    if (!any) {
        after += code.size();
        return;
    }

    // Where each instruction kept goes, and which of the module's own functions are still made.
    std::vector<int64_t> moved(code.size() + 1, -1);
    std::vector<char> made(module.protos.size(), 0);
    size_t length = 0;
    for (size_t at = 0; at < code.size(); at += instructionSize(code, at)) {
        moved[at] = static_cast<int64_t>(length);  // for one left out: where what follows goes
        if (gone[at]) continue;
        length += instructionSize(code, at);
        const Op op = static_cast<Op>(code[at]);
        if (makesFunction(op)) {
            const size_t index = static_cast<size_t>(code[at + 1] | (code[at + 2] << 8));
            if (index < made.size()) made[index] = 1;
        }
    }
    moved[code.size()] = static_cast<int64_t>(length);
    std::vector<int> renumbered(module.protos.size(), -1);
    std::vector<FunctionProto*> protos;
    for (size_t i = 0; i < module.protos.size(); i++) {
        if (!made[i]) continue;
        renumbered[i] = static_cast<int>(protos.size());
        protos.push_back(module.protos[i]);
    }

    std::vector<uint8_t> kept;
    std::vector<int> lines;
    kept.reserve(length);
    lines.reserve(length);
    for (size_t at = 0; at < code.size();) {
        const size_t size = instructionSize(code, at);
        if (gone[at]) {
            at += size;
            continue;
        }
        const size_t to = kept.size();
        kept.insert(kept.end(), code.begin() + static_cast<std::ptrdiff_t>(at), code.begin() + static_cast<std::ptrdiff_t>(at + size));
        lines.insert(lines.end(), module.lines.begin() + static_cast<std::ptrdiff_t>(at),
                     module.lines.begin() + static_cast<std::ptrdiff_t>(at + size));
        const Op op = static_cast<Op>(code[at]);
        const int target = targetOperand(op);
        if (target != 0) {
            const size_t place = to + static_cast<size_t>(target);
            const int32_t old = static_cast<int32_t>(static_cast<uint32_t>(kept[place]) | (static_cast<uint32_t>(kept[place + 1]) << 8) |
                                                     (static_cast<uint32_t>(kept[place + 2]) << 16) | (static_cast<uint32_t>(kept[place + 3]) << 24));
            if (old < 0 || static_cast<size_t>(old) > code.size() || moved[static_cast<size_t>(old)] < 0 ||
                (static_cast<size_t>(old) < code.size() && gone[static_cast<size_t>(old)])) {
                after += code.size();  // goes into what would be left out: keep it all
                return;
            }
            const uint32_t now = static_cast<uint32_t>(moved[static_cast<size_t>(old)]);
            for (int i = 0; i < 4; i++) kept[place + static_cast<size_t>(i)] = static_cast<uint8_t>((now >> (8 * i)) & 0xFF);
        }
        if (makesFunction(op)) {
            const size_t index = static_cast<size_t>(kept[to + 1] | (kept[to + 2] << 8));
            const int now = renumbered[index];
            kept[to + 1] = static_cast<uint8_t>(now & 0xFF);
            kept[to + 2] = static_cast<uint8_t>((now >> 8) & 0xFF);
        }
        at += size;
    }

    std::vector<DeclarationSpan> spans;
    for (const DeclarationSpan& span : module.spans)
        if (reach.reached(module, span))
            spans.push_back(DeclarationSpan{static_cast<uint32_t>(moved[span.start]), static_cast<uint32_t>(moved[span.end]), span.key, span.kind});
    code = std::move(kept);
    module.lines = std::move(lines);
    module.protos = std::move(protos);
    module.spans = std::move(spans);
    module.caches.reset();
    module.cacheCount = 0;
    after += code.size();
}

}  // namespace

bool namesGlobal(const Program& program, const std::vector<std::string>& names) {
    std::vector<char> wanted(program.globalNames.size(), 0);
    bool any = false;
    for (size_t slot = 0; slot < program.globalNames.size(); slot++)
        for (const std::string& name : names)
            if (program.globalNames[slot] == name) wanted[slot] = any = 1;
    if (!any) return false;
    std::vector<const FunctionProto*> work(program.modules.begin(), program.modules.end());
    while (!work.empty()) {
        const FunctionProto* f = work.back();
        work.pop_back();
        if (f == nullptr) continue;
        for (const FunctionProto* nested : f->protos) work.push_back(nested);
        for (size_t at = 0; at < f->code.size();) {
            const uint8_t raw = f->code[at];
            if (raw >= static_cast<uint8_t>(Op::Count_)) break;
            int a = 0, b = 0;
            decodeOperands(f->code, at, kOps[raw].operand, a, b);
            const int which = globalOperand(static_cast<Op>(raw), a);
            const int slot = which == 1 ? a : which == 2 ? b : -1;
            if (slot >= 0 && static_cast<size_t>(slot) < wanted.size() && wanted[static_cast<size_t>(slot)]) return true;
            at += 1 + operandSize(kOps[raw].operand);
        }
    }
    return false;
}

LeftOut leaveOutUnreachable(Program& program, const std::vector<std::string>& globalsByName,
                            const std::vector<std::string>& membersByName) {
    Reach reach(program);
    for (FunctionProto* module : program.modules) {
        if (module == nullptr) continue;
        for (size_t i = 0; i < module->spans.size(); i++) {
            const DeclarationSpan& span = module->spans[i];
            if (span.kind == DeclarationSpan::Global) {
                reach.spansOf[span.key].emplace_back(module, i);
            } else if (span.key < module->constants.size() && module->constants[span.key].isString()) {
                reach.extensionsNamed[Reach::bare(asString(module->constants[span.key])->utf8())].emplace_back(module, i);
            }
        }
    }
    for (size_t slot = 0; slot < program.globalNames.size(); slot++)
        for (const std::string& name : globalsByName)
            if (program.globalNames[slot] == name) reach.global(static_cast<int>(slot));
    for (const std::string& name : membersByName) reach.name(name);
    for (FunctionProto* module : program.modules)
        if (module != nullptr) reach.function(module);
    reach.run();

    LeftOut result;
    for (FunctionProto* module : program.modules) {
        if (module == nullptr) continue;
        for (const DeclarationSpan& span : module->spans)
            if (!reach.reached(*module, span)) result.declarations++;
        leaveOut(*module, reach, result.codeBefore, result.codeAfter);
    }
    for (FunctionProto* module : program.modules)
        if (module != nullptr) verifyModule(*module, program);
    return result;
}

}  // namespace astra
