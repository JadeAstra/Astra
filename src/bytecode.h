#pragma once

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "value.h"

namespace astra {

// Instruction set of the Astra VM. Operands are little-endian: u8, u16, or i32 (absolute
// jump targets within the function).
enum class Op : uint8_t {
    Const,         // u16 constant                  -> value
    Null,
    True,
    False,
    Pop,
    Dup,
    Dup2,

    GetLocal,      // u16 slot
    SetLocal,      // u16 slot                      (value stays on the stack)
    GetGlobal,     // u16 global
    SetGlobal,     // u16 global
    DefineGlobal,  // u16 global                    value ->
    GetUpvalue,    // u16 index
    SetUpvalue,    // u16 index

    GetProp,       // u16 name                      obj -> value
    SetProp,       // u16 name                      obj value -> value
    GetIndex,      //                               obj index -> value
    SetIndex,      //                               obj index value -> value
    GetSuper,      // u16 name                      this superclass -> bound method

    Add, Sub, Mul, Div, Mod, Neg, Not,
    Eq, Lt, Le, Gt, Ge,
    Is,            //                               value class -> bool

    Jump,          // i32 target
    Loop,          // i32 target                    backward jump; a garbage-collection safe point
    JumpIfFalse,   // i32 target                    pops the condition
    And,           // i32 target                    jumps keeping a falsy value, else pops
    Or,            // i32 target                    jumps keeping a truthy value, else pops

    Call,          // u8 argc                       callee args... -> result
    Invoke,        // u16 name, u8 argc             receiver args... -> result
    SuperInvoke,   // u16 name, u8 argc             this args... superclass -> result
    Closure,       // u16 nested function
    CloseUpvalue,  //                               pops a local that a closure captured
    Return,

    Class,         // u16 name                      -> class
    Inherit,       //                               superclass subclass -> superclass
    Method,        // u16 name                      class closure -> class
    StaticMethod,  // u16 name                      class closure -> class

    BuildList,     // u16 count
    BuildMap,      // u16 entry count               key value ... -> map
    BuildString,   // u16 part count
    Range,         // u8 inclusive                  start end -> range

    IterInit,      //                               iterable -> iterator
    IterNext,      // u16 slot, i32 target          pushes the next item, or jumps when exhausted

    TryBegin,      // i32 handler address
    TryEnd,
    Throw,
    Import,        // u16 module                    -> null (runs the module on first use)

    // Declared variable types. The u8 is a TypeId, optionally with kTypeNullable; for
    // kTypeInstance the class is on the stack above the value.
    CheckType,          // u16 variable name, u8 type    value [class] -> value  (throws on a mismatch)
    DefineTypedGlobal,  // u16 global, u8 type           value [class] ->
    CheckParam,         // u16 parameter name, u8 type   value [class] -> value  (throws on a mismatch)
    CheckReturn,        // u8 type                       value [class] -> value  (throws on a mismatch)

    // Declared class fields. DeclareField's second operand is the type in its low byte plus
    // kFieldVal / kFieldRequired; a class-typed field passes a closure that returns the class.
    DeclareField,       // u16 name, u16 flags           class [type closure] -> class
    FieldInit,          //                               class closure -> class  (runs on each new instance)
    ReturnInit,         //                               Return from an initializer: checks required fields
    SuperInit,          // u8 argc                       this args... superclass -> value  (runs its init, if any)
    DefaultArg,         // u16 slot, i32 target             jumps if the argument in `slot` was passed
    CallNamed,          // u8 positional, u8 named         callee args... (name value)... -> result
    Getter,             // u16 name                        class closure -> class  (get name())
    Setter,             // u16 name                        class closure -> class  (set name(value))

    // A local variable that a function inside its own initial value refers to: its slot is
    // set aside first, marked "no value yet", and such a function reads it with a check.
    Unset,              //                                 -> (the "no value yet" mark)
    GetUpvalueChecked,  // u16 index, u16 variable name    -> value  (throws if it has no value yet)

    Shl,                //                                 a b -> a << b
    Shr,                //                                 a b -> a >> b
    BitAnd,             //                                 a b -> a & b
    BitOr,              //                                 a b -> a | b
    BitXor,             //                                 a b -> a ^ b
    BitNot,             //                                 a -> ~a

    // A name in a lambda literal that is none of its variables: a member of the object the
    // lambda is run on, or of one that an enclosing lambda is run on, and failing that a global.
    GetImplicit,        // u16 name, u16 global            -> value
    SetImplicit,        // u16 name, u16 global            value -> value

    Count_,
};

// The types a variable can be declared with (`var n : int`). Any other name is a class.
enum TypeId : uint8_t {
    kTypeInt = 1,
    kTypeFloat,
    kTypeNumber,  // int or float
    kTypeString,
    kTypeBool,
    kTypeList,
    kTypeMap,
    kTypeRange,
    kTypeFunction,
    kTypeInstance,  // an instance of a particular class or of one of its subclasses
};

const uint8_t kTypeNullable = 0x80;  // `type?`: null is allowed as well
const uint8_t kTypeMask = 0x7F;

const int kFieldVal = 0x100;       // the field can be assigned only once
const int kFieldRequired = 0x200;  // the initializer has to assign it

// Maps a built-in type name to its TypeId; returns 0 for any other name.
uint8_t builtinTypeId(const std::string& name);

// "int", "string?", ... For kTypeInstance pass the class name.
std::string typeText(uint8_t type, const std::string& className);

struct UpvalueDesc {
    bool isLocal;    // captured from the enclosing function's locals (else from its upvalues)
    uint16_t index;
};

// A compiled function: the unit the compiler emits and the bytecode file stores.
struct FunctionProto {
    std::string name;
    std::string source;
    int arity = 0;

    // Fewest arguments a call may pass. Parameters beyond that have default values, which the
    // function fills in itself (see DefaultArg). -1 means "the same as arity".
    int minArity = -1;
    int requiredArgs() const { return minArity < 0 ? arity : minArity; }

    // Parameter names (interned), for calls that pass arguments by name. Empty for functions
    // that cannot be called that way.
    std::vector<ObjString*> paramNames;

    // Stack slots the function needs, counted from its frame base. Filled in by verifyFunction.
    int maxStack = 0;

    std::vector<uint8_t> code;
    std::vector<int> lines;                // source line of each byte in `code`
    std::vector<Value> constants;          // ints, floats and interned strings
    std::vector<FunctionProto*> protos;    // nested functions
    std::vector<UpvalueDesc> upvalues;

    // For a lambda literal inside other lambda literals: the upvalues holding slot 0 of each
    // enclosing one, innermost first. That slot is the object the lambda was run on, if any.
    std::vector<uint16_t> outerReceivers;
};

// A whole compiled program, ready to run or to write to a bytecode file.
class Program {
public:
    std::vector<std::string> globalNames;

    // Top-level function of each source file. Module 0 is always the prelude.
    std::vector<FunctionProto*> modules;
    int entry = 0;

    FunctionProto* newProto();

private:
    std::vector<std::unique_ptr<FunctionProto>> owned_;
};

// Checks that every operand of `f` is in range and that the stack depth is consistent on
// all paths, and records the function's stack requirement. The VM relies on both.
// Throws std::runtime_error if the bytecode is malformed.
void verifyFunction(FunctionProto& f, const Program& program);

void disassemble(const Program& program, std::ostream& out);

// Bytecode files. readProgram verifies everything it loads and throws std::runtime_error
// on a malformed file.
void writeProgram(const Program& program, std::ostream& out);
void readProgram(Program& program, VM& vm, const std::string& data);
bool looksLikeBytecode(const std::string& data);

}  // namespace astra
