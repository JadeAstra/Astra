#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
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

    GetProp,       // u16 name, u16 cache           obj -> value
    SetProp,       // u16 name, u16 cache           obj value -> value
    GetIndex,      //                               obj index -> value
    SetIndex,      //                               obj index value -> value
    GetSuper,      // u16 name                      this superclass -> bound method
    SetSuper,      // u16 name                      this superclass value -> what the setter gave: super.name = value

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
    Inherit,       // u8 count                      supertypes... subclass -> superclass or null
    Method,        // u16 name                      class closure -> class
    StaticMethod,  // u16 name                      class closure -> class

    BuildList,     // u16 count
    BuildMap,      // u16 entry count               key value ... -> map
    BuildString,   // u16 part count
    Range,         // u8 flags                      start end [step] -> range  (1 inclusive, 2 counts down, 4 has a step)

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

    Identical,          //                                 a b -> bool  (a === b: the very same object)

    JumpIfNull,         // i32 target                      jumps if the top is null; it stays either way
    JumpIfNotNull,      // i32 target                      jumps if the top is not null; it stays either way
    AssertNotNull,      //                                 value -> value  (throws if it is null: `value!!`)

    // `break`, `continue` or a `return` that has finally blocks to run, in the middle of an
    // expression: drops the half-built values under the top one, leaving exactly `count`
    // slots below it (the function's variables, one of which may not have its value yet).
    Settle,             // u16 count                       ... value -> locals value

    // Tests against a type written in the source; the u8 is as for CheckType.
    IsType,             // u8 type                         value [class] -> bool
    Cast,               // u8 type                         value [class] -> value  (throws on a mismatch)
    CastOrNull,         // u8 type                         value [class] -> value or null
    Swap,               //                                 a b -> b a

    // The end of a block that is an expression: drops the block's `count` variables from
    // under its value, closing any that a function captured.
    PopUnder,           // u16 count                       locals... value -> value

    // A call with `*list` among its arguments. The positional arguments are gathered into
    // one list first, with Spread adding the elements of each list in turn.
    Spread,             //                                 list value -> list  (value must be a list)
    CallSpread,         // u8 named                        callee list (name value)... -> result

    // Static members. StaticField's operands are as for DeclareField.
    StaticField,        // u16 name, u16 flags             class [type closure] value -> class
    StaticGetter,       // u16 name                        class closure -> class  (static get name())
    StaticSetter,       // u16 name                        class closure -> class  (static set name(value))

    // More for names that may be members of an object the code runs on (see GetImplicit).
    // TryImplicit pushes the member and jumps if one of those objects has the name, and
    // otherwise does nothing. InvokeImplicit calls the name as a method of the object that
    // has it, or failing that as a global function; the slot under the arguments is spare.
    TryImplicit,        // u16 name, i32 target            -> [value]
    InvokeImplicit,     // u16 name, u16 global, u8 argc   spare args... -> result

    // Enums. EnumEntry names an instance as the entry with that ordinal; EnumSeal ends the list.
    EnumEntry,          // u16 name, u16 ordinal           class instance -> class
    EnumSeal,           //                                 class -> class
    EnumNext,           // u16 name, u16 ordinal           class -> class  (the entry made next has these already in its init)
    PendingEnum,        // u16 name                        -> the name, to be settled by the parameter it is passed to

    // Interfaces. Require names a member the implementing class has to supply; FinishClass,
    // at the end of every class, checks that it supplies all that its interfaces require.
    MarkInterface,      //                                 class -> class
    Require,            // u16 name                        class -> class
    FinishClass,        //                                 class -> class

    // Data classes and taking values apart. DataField adds a header field to the parts of a
    // data class; Component gives part `index` of a list or of a data object.
    DataField,          // u16 name                        class -> class  ("": a data class with no header fields)
    UnstableKey,        //                                 class -> class: its objects can change in what == sees (D26)
    Lazy,               //                                 closure -> a lazy value, worked out when first read (D37)
    Extend,             //                                 class interface -> class: fitted to the interface (D33)
    Bridge,             // u16 name                        class getter -> class: a getter an extend gives it (D33)
    Component,          // u8 index                        value -> part

    MissingArg,         // u16 parameter name              throws: the call left out an argument that has no default
    // A function calling itself as its last step (D29): the arguments become its parameters
    // and it starts again in the same frame, which counts the times.
    TailLoop,           // u8 count                        arguments ->
    // Before a bare name's lookup (GetImplicit and the rest): the object in a local to try first,
    // that of a block copied in place (D58, D59); flags 1: that one and no other (an inline
    // function's body, which runs on no object of the frame's).
    ImplicitFirst,      // u16 slot, u8 flags              ->
    // `block(obj, a)` or `obj.block(a)`: a function run on an object (D58), the object where a
    // method has `this`.
    CallOn,             // u8 argc                         callee object args -> result

    // A class's static values are worked out by a function of its own, attached with
    // ClassInit and run on first need: by EnsureClassInit, or by any use of a static member.
    ClassInit,          //                                 class closure -> class
    EnsureClassInit,    //                                 class -> class

    // An extension function: a method added to a type from outside. The u8 is a TypeId, with
    // kTypeNullable when null takes it too; kTypeInstance has the class under the function,
    // and 0 stands for every value.
    Extension,          // u16 name, u8 type               [class] closure ->
    MarkAbstract,       //                                 class -> class
    MarkValue,          // u8 made from its header alone   class -> class  (a value type, M6.6 part 2)
    // A member written `override` has to have something to take the place of.
    CheckOverride,      // u16 name, u8 flags              class -> class  (1: written override; 2: a field or property)
    // Invoke, where the last argument is a lambda written after the call: it goes to the last
    // parameter, and the parameters between the other arguments and it take their defaults.
    InvokeTrailing,     // u16 name, u8 argc               receiver args... lambda -> result
    SuperInvokeTrailing,  // u16 name, u8 argc             this args... lambda superclass -> result

    // Who may use a member. ClassNumber gives a class its number and its library's, which
    // the code written in it carries (FunctionProto::classIds). MemberAccess says what the
    // class declares of a member: the kind is 1 private, 2 protected or 3 internal, for the
    // code with that number (the class's, or for internal its library's); 0 open, whatever it
    // was above; 4 as it was above, where what was private above is open here. With 0x10 it
    // speaks of assigning only (`private set`). StaticAccess is the same for a static member.
    ClassNumber,        // u16 class number, u16 library   class -> class
    MemberAccess,       // u16 name, u16 number, u8 kind   class -> class
    StaticAccess,       // u16 name, u16 number, u8 kind   class -> class

    // GetProp, SetProp, Invoke and InvokeTrailing for a member the class declares itself,
    // reached through `this` by the class's own code (or a static member of the class, by
    // its name): that code may always use it, so who may is not looked at.
    GetOwn,             // u16 name, u16 cache             obj -> value
    SetOwn,             // u16 name, u16 cache             obj value -> value
    InvokeOwn,          // u16 name, u8 argc               receiver args... -> result
    InvokeOwnTrailing,  // u16 name, u8 argc               receiver args... lambda -> result

    // A name with several forms (overloads, D56): MakeForms gathers them into one value whose
    // proto is the nested function given, a set of forms (FunctionProto::chooser), and which
    // calls the form a call's arguments fit.
    MakeForms,          // u16 nested function, u8 count   forms... -> the set of them
    // The same for a method of a class: the names of its forms, each the method's name with the
    // signature of the form it is, are on the stack; the set goes under the method's name.
    MethodForms,        // u16 nested function, u8 count   class names... -> class
    StaticForms,        // u16 nested function, u8 count   class names... -> class  (the same for static methods)

    // `map[key, default = start]`: the entry, or `start` where the key is not there (L118).
    GetIndexOr,         //                                 map key start -> value

    // Two instructions in one (M6.5 part 4), the second's operand being a local or a constant:
    // `GetLocal x; Mul` is MulLocal x, `Const k; Add` is AddConst k. Two floats or two ints
    // are worked out at once; anything else goes the way the plain instruction does.
    AddLocal, SubLocal, MulLocal, DivLocal, ModLocal, LtLocal, LeLocal, GtLocal, GeLocal, EqLocal,  // u16 slot      a -> a op local
    AddConst, SubConst, MulConst, DivConst, ModConst, LtConst, LeConst, GtConst, GeConst, EqConst,  // u16 constant  a -> a op constant
    StoreLocal,         // u16 slot                        value ->  (SetLocal; Pop)
    CheckLocal,         // u16 slot, u16 name, u8 type     ->  (a parameter's declared type, not a class)

    // A `for` over a range written in place, `for i in 0..<n`, counted without a range or an
    // iterator being made (M6.5 part 4). RangeLoop takes the bounds (and the step, by Range's
    // flags) and leaves three locals: the next value, how many are left and the step.
    // RangeNext pushes the next value, as IterNext does, or jumps when none are left.
    RangeLoop,          // u8 flags                        start end [step] -> next left step
    RangeNext,          // u16 slot, i32 target            pushes the next value, or jumps when there are no more

    // Three instructions in one: `GetLocal a; GetLocal b; Mul` is MulLocals a b, and so on;
    // and `GetLocal a; SetLocal b; Pop` is CopyLocal a b.
    AddLocals, SubLocals, MulLocals, DivLocals, LtLocals, LeLocals, GtLocals, GeLocals, EqLocals,  // u16 slot, u16 slot  -> a op b
    CopyLocal,          // u16 from, u16 to                ->

    // An anonymous object (`object : Shape { ... }`), its class made once (M6.5 part 4).
    // ObjectClass pushes the class kept in the global and jumps over the code that makes it,
    // which StoreClass ends by keeping it there (0xFFFF: never kept, made every time). Then
    // the variables of the code around that its members use are pushed, each by Capture
    // (kind 0 a local, 1 an upvalue, 2 a capture of the object this code runs on, 3 the
    // superclass of the class `index` slots down), and NewObject makes the object with them.
    // Its members reach them with GetCaptured and SetCaptured, on the object they run on.
    ObjectClass,        // u16 global, i32 target          -> [class, at target]
    StoreClass,         // u16 global                      class -> class
    Capture,            // u16 index, u8 kind              -> captured variable
    NewObject,          // u8 count                        class captured... -> object (runs its init)
    GetCaptured,        // u16 capture, u16 name           -> value  (throws if it has no value yet)
    SetCaptured,        // u16 capture                     value -> value

    // Types when the program runs (M6.6 part 1). MakeType makes a type from a shape, a string
    // constant read left to right (shapeOperands): `?` makes what follows nullable; a any, i
    // int, f float, n Number, s string, b bool, y Bytes, r Range, F a function, L a list of
    // what follows, M a map of the two that follow, t Type, C<digit> the class on the stack with that
    // many type arguments after it, P a type on the stack. The classes and types it names are
    // on the stack in the order they come in the shape. A type made of nothing that changes is
    // made once, where it is written: CachedType pushes it and jumps over the code that makes
    // it, which CacheType ends by keeping it. A<digit> makes the type arguments of a call, the
    // types that follow.
    //
    // A generic function is given its type arguments by WithTypes, right before the call: the
    // frame the call pushes keeps them, or the object it makes, for a generic class. TypeArg
    // reads one of the frame's, and InstanceTypeArg one of an object's.
    MakeType,           // u16 shape                       operands... -> type
    CachedType,         // u16 cache, i32 target           -> [type, at target]
    CacheType,          // u16 cache                       type -> type
    TagCollection,      // u16 cache, i32 target           list-or-map -> list-or-map  (given the type kept, then at target; else on, to make it)
    TypeCollection,     //                                 list-or-map type -> list-or-map  (what it was made to hold)
    IsFull,             //                                 value type -> bool
    CastFull,           // u8 safe                         value type -> value  (throws on a mismatch, or with `safe` null)
    WithTypes,          //                                 type-arguments ->  (for the call that follows)
    TypeArg,            // u8 index                        -> type
    InstanceTypeArg,    // u8 index                        object class -> type  (of the class's own type parameters)
    SupertypeMaker,     //                                 class function -> class  (ObjClass::supertypeMakers)
    CheckResult,        // u16 name                        value type -> value  (ASTRA_CHECK_NATIVE_RESULTS: throws on a mismatch)
    // `pts.add(Point(1, 2))` (M6.6 part 3): where the list keeps its items packed and the value
    // type is made from its header alone, the arguments are written into a new item at the
    // end, and on at target with true; otherwise on, the stack as it was, to the call and the add.
    AddNew,             // u8 argc, i32 target             list class args... -> [true, at target]

    // A place a value is changed through (M6.6 part 4, value-lists.html section 8): a Ref to the
    // value where it is. An item of a list of values is changed where it is; a variable or a
    // field that holds a box is first given a box of its own, which only it holds.
    PlaceLocal,         // u16 slot                        -> place
    PlaceUpvalue,       // u16 index                       -> place
    PlaceGlobal,        // u16 global                      -> place
    PlaceField,         // u16 name                        object -> place
    PlaceIndex,         //                                 list index -> place

    // Read where it is (M6.6 part 4, value-lists.html section 6), where the compiler has found
    // that nothing changes the list meanwhile: an item of a list of values is pushed as a Ref
    // to it; anything else as GetIndex and IterNext give it. A function given a Ref it may not
    // keep as it runs takes a copy as it starts (OwnLocal).
    GetIndexRef,        //                                 list index -> item
    IterNextRef,        // u16 slot, i32 target            -> [item]  (as IterNext)
    // A field read where it is (M6.6 part 5): as GetProp and GetOwn, but a value kept inside its
    // object is given as a Ref into it, where the checker has shown nothing changes it meanwhile.
    GetPropRef,         // u16 name, u16 cache             obj -> value
    GetOwnRef,          // u16 name, u16 cache             obj -> value
    // `comp.bounds = Rect(...)` (M6.6 part 5): the field as where the result goes, a Ref into the
    // object where it keeps a value of a value type inside it, and otherwise null.
    FieldDestination,   // u16 name                        obj -> place
    // Right before a call whose result may stay where the function found it (M6.6 part 5): a
    // value it gives back that is a Ref into a list or an object is not copied on return.
    WithRefResult,      //                                 ->

    // A number's conversion, or one of Math's functions, called where the checker knows it is
    // the standard library's (M6.6 part 6): worked out in place, with no method looked up or
    // C++ function called. `which` is a NumericOp; it takes one value, or two for min and max.
    Numeric,            // u8 which                        value [value] -> result

    // M6.6 part 8: `>>>`, which brings in zeros (D44), and the int sum, difference and product
    // that wrap around (`&+`, `&-`, `&*`, D27), where `+`, `-` and `*` stop with an error.
    Ushr,               //                                 a b -> a >>> b
    AddWrap,            //                                 a b -> a &+ b
    SubWrap,            //                                 a b -> a &- b
    MulWrap,            //                                 a b -> a &* b
    OwnLocal,           // u16 slot                        ->

    // Results built where they go (M6.6 part 4, value-lists.html section 7). Right before a call,
    // WithDestinationAt gives it a place for its result, the one `depth` slots under the call's
    // own (`pts[i] = f(...)`), and WithFrameDestination passes on the frame's own, for a call
    // whose result it returns. A function that gives a value of a value type
    // (FunctionProto::typeArgs has kTakesDestination) keeps it for such calls of its own; making
    // a value from its header alone writes its fields there and gives the place.
    WithDestinationAt,  // u8 depth                        ->
    WithFrameDestination, //                               ->
    FromEnd,            //                                 k -> ^k
    // `for ((a, b) in ...)`: as IterNext, pushing the item's two parts; a map's key and value
    // with no entry made for them (D64, D73).
    IterNextPair,       // u16 slot, i32 target            -> a b

    // Reflection (M6.6 part 8, group 6, D48). ClassOf makes what a `#` was given the type of a
    // class, and pushes the function it is written in, whose rights to members the Class keeps:
    // `how` 0 a type, 1 a value, of whose class, 2 a class itself. ClassMeta gives a class the
    // nested function that describes its members for reflection, made into a function when
    // first asked for: `which` 0 its fields, 1 its methods and constructors (ObjClass::meta).
    ClassOf,            // u8 how                          operand -> type function
    ClassMeta,          // u16 nested function, u8 which   class -> class
    // An `atomic var` field changed in one step, by any core of a parallel section (D92):
    // `kind` 0 +=, 1 -=, 2 = max, 3 = min, giving the new value, or 4 compareAndSet, which takes
    // what is expected before the new value and gives whether it set it.
    AtomicField,        // u16 name, u8 kind               object [expected] value -> result
    // A parameter declared as a union holding one enum (`output : Stream | Path`): a bare name
    // that arrived for it becomes that enum's entry; anything else is left as it is.
    SettleEnum,         //                                 value class -> value
    // `grid[x, y]` (D133): a class's op [] or op []= with `count` indexes.
    IndexN,             // u8 count                        object index... -> value
    SetIndexN,          // u8 count                        object index... value -> result
    DupN,               // u8 count                        a b ... -> a b ... a b ...
    // Bundles of named arguments (D143): a map of names to values. BUNDLE_VALUES gives, for the
    // `count` names on the stack, each name and the bundle's value of it, for CALL_NAMED;
    // MERGE_BUNDLE is a new bundle of both, the second's values winning.
    BundleValues,       // u8 count                        bundle name... -> name value ...
    MergeBundle,        //                                 bundle bundle -> bundle

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
    kTypeBytes,
    kTypeType,      // a type, as the program runs (M6.6 part 1): `items.itemType`
    kTypeInstance,  // an instance of a particular class or of one of its subclasses
    kTypeChar,      // one character (D24)
    kTypeByte,      // 0 to 255 (D19)
    // Storage widths (D148): ints (a float for float32) that must fit where they are kept.
    kTypeInt16,
    kTypeInt32,
    kTypeUint16,
    kTypeUint32,
    kTypeFloat32,
    kTypeIndex,     // a position: an int from the start, or ^k from the end (D146)
    kTypeSet,
    // A list, map or set made by frozen(), which nobody may change (D26).
    kTypeFrozenList,
    kTypeFrozenMap,
    kTypeFrozenSet,
    kTypeCount_,
};

// For a frozen collection's type, the type it is a kind of (FrozenList, List); any other as it is.
inline uint8_t unfrozenTypeId(uint8_t id) {
    return id == kTypeFrozenList ? kTypeList : id == kTypeFrozenMap ? kTypeMap : id == kTypeFrozenSet ? kTypeSet : id;
}
// For a list's, map's or set's type, its frozen kind; 0 for any other.
inline uint8_t frozenTypeId(uint8_t id) {
    return id == kTypeList ? kTypeFrozenList : id == kTypeMap ? kTypeFrozenMap : id == kTypeSet ? kTypeFrozenSet : 0;
}

// Whether `id` names one of the types above other than a class's.
inline bool isBuiltinTypeId(uint8_t id) { return id >= kTypeInt && id < kTypeCount_ && id != kTypeInstance; }
// Whether a value of type `id` is an int kept at a narrower width: it must fit its range.
inline bool isIntStorage(uint8_t id) { return id >= kTypeInt16 && id <= kTypeUint32; }
// How a value of type `id` is kept narrow (D19, D24, D148), or Narrow::None.
inline Narrow narrowOf(uint8_t id) {
    switch (id) {
        case kTypeByte: return Narrow::Byte;
        case kTypeChar: return Narrow::Char;
        case kTypeInt16: return Narrow::Int16;
        case kTypeInt32: return Narrow::Int32;
        case kTypeUint16: return Narrow::Uint16;
        case kTypeUint32: return Narrow::Uint32;
        case kTypeFloat32: return Narrow::Float32;
        default: return Narrow::None;
    }
}

// The range of an int storage width.
inline void intStorageRange(uint8_t id, int64_t& low, int64_t& high) {
    switch (id) {
        case kTypeInt16: low = -32768; high = 32767; break;
        case kTypeInt32: low = -2147483648LL; high = 2147483647; break;
        case kTypeUint16: low = 0; high = 65535; break;
        default: low = 0; high = 4294967295LL; break;
    }
}

const uint8_t kTypeNullable = 0x80;  // `type?`: null is allowed as well
const uint8_t kTypeMask = 0x7F;
// Never the type of a value: the type arguments of a call, kept as one type (M6.6 part 1).
const uint8_t kTypeArguments = 0x7F;
// FunctionProto::typeArgs.
const uint8_t kTypeArgsOwn = 1;
const uint8_t kTypeArgsKept = 2;
// A function that gives a value of a value type, given where its result goes (M6.6 part 4).
const uint8_t kTakesDestination = 4;

// What Op::Numeric works out (M6.6 part 6), each as the standard library's function does.
enum class NumericOp : uint8_t {
    ToInt,       // Number.toInt(), toLong()
    ToFloat,     // Number.toFloat(), toDouble()
    RoundToInt,  // Number.roundToInt(), roundToLong()
    IsNaN,
    IsInfinite,
    IsFinite,
    Sqrt,        // Math.sqrt
    Floor,       // Math.floor, ceil and round: ints
    Ceil,
    Round,
    AbsInt,      // Math.abs, of an int and of a float
    AbsFloat,
    MinInt,      // Math.min and max of two ints, and of two floats
    MaxInt,
    MinFloat,
    MaxFloat,
    Count_
};
inline int numericArity(uint8_t which) {
    return which >= static_cast<uint8_t>(NumericOp::MinInt) && which <= static_cast<uint8_t>(NumericOp::MaxFloat) ? 2 : 1;
}

const int kFieldVal = 0x100;       // the field can be assigned only once
const int kFieldRequired = 0x200;  // the initializer has to assign it
const int kFieldOverride = 0x400;  // declared again, in place of the superclass's field of that name

// Maps a built-in type name to its TypeId; returns 0 for any other name.
uint8_t builtinTypeId(const std::string& name);

// "int", "string?", ... For kTypeInstance pass the class name.
std::string typeText(uint8_t type, const std::string& className);

struct UpvalueDesc {
    bool isLocal;    // captured from the enclosing function's locals (else from its upvalues)
    uint16_t index;
    // Taken from capture `index` of the anonymous object the enclosing function runs on, a
    // member of its class (M6.5 part 4; see Op::NewObject).
    bool fromCapture = false;
};

// In FunctionProto::outerReceivers: the entry is a capture of the anonymous object the
// function, a member of its class, runs on, not one of its upvalues.
constexpr uint16_t kCapturedReceiver = 0x8000;

// In a module's code, the instructions that make one of its top-level declarations (M6.5 part
// 5): run only to make that, and left out of a built program that never reaches it. A function
// or a class is reached by its global, `key` its slot; an extension function, which is found by
// its name, by any use of that name, `key` the constant that is its name.
struct DeclarationSpan {
    // Reflect and ReflectMethods: what describes a class's fields, or its methods, for
    // reflection (Op::ClassMeta), kept only where reflection reaches it (D50); `key` is the
    // global of the declaration the class is part of.
    enum Kind : uint8_t { Global, Extension, Reflect, ReflectMethods };
    uint32_t start;
    uint32_t end;
    uint16_t key;
    Kind kind = Global;
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

    // The `vararg` parameter (counted from 0), which receives the arguments from its position
    // on as a list, or -1. Parameters after it can only be passed by name.
    int varargIndex = -1;

    // Whether its frame keeps type arguments (M6.6 part 1): kTypeArgsOwn for a generic
    // function, given its own by WithTypes; kTypeArgsKept for a lambda or local function that
    // reads those of the function it is written in, or makes one that does, which its closure
    // keeps. Only such a function's frame has them: the others are called without.
    uint8_t typeArgs = 0;
    // Of a generic function declared inside another generic function (G1): how many type
    // arguments of the functions around it come first in its frame's, its own after them.
    uint8_t typeArgBase = 0;

    // Bit i is set when parameter i is declared with a class type, the only kind of parameter
    // that can turn a bare name into an enum entry. (Parameters past the 64th never do.)
    uint64_t classParams = 0;

    // Parameter names (interned), for calls that pass arguments by name. Empty for functions
    // that cannot be called that way.
    std::vector<ObjString*> paramNames;

    // Stack slots the function needs, counted from its frame base. Filled in by verifyFunction.
    int maxStack = 0;

    std::vector<uint8_t> code;
    std::vector<int> lines;                // source line of each byte in `code`
    std::vector<DeclarationSpan> spans;    // a module's: the code of each top-level declaration
    std::vector<Value> constants;          // ints, floats and interned strings
    std::vector<FunctionProto*> protos;    // nested functions
    std::vector<UpvalueDesc> upvalues;

    // For a lambda literal inside other lambda literals: the upvalues holding slot 0 of each
    // enclosing one, innermost first. That slot is the object the lambda was run on, if any.
    std::vector<uint16_t> outerReceivers;

    // The numbers of the classes the function is written inside of, innermost first, and
    // then of its library: a member that is private, protected or internal is open to the
    // function when one of these is among the numbers the member is marked with.
    std::vector<uint16_t> classIds;
    uint16_t ownClass = 0;  // the first of them, when the function is written in a class

    // A form of a name that has several (overloads, D56): what tells it from the others, the
    // types of its parameters as written, `(float, float)`; and those types for choosing a
    // form when the program runs, where the compiler could not: a TypeId with kTypeNullable
    // per parameter (0 untyped), and for kTypeInstance the class's name. Empty otherwise.
    std::string signature;
    std::vector<uint8_t> paramTypes;
    std::vector<ObjString*> paramClasses;
    // For a parameter of a function type, one more than the number of parameters that type
    // has, so that a lambda taking two tells `forEach { k, v -> }` from `forEach { entry -> }`;
    // 0 for any other.
    std::vector<uint8_t> paramCounts;

    // The set of a name's forms, made by MakeForms: it has no code of its own; calling it
    // calls the form the arguments fit (VM::chooseForm).
    bool chooser = false;

    // Where each GetProp, SetProp, GetOwn and SetOwn remembers the field it last found (M6.5
    // part 4), by its second operand. Each holds the layout of the object's class
    // (ObjClass::shape) in its high 32 bits and the field's place in bits 16 to 31, and for a
    // SetProp or SetOwn the field's type in bits 8 to 15; 0 when nothing is there yet.
    // Made empty by verifyFunction; not part of the program as it is written out.
    std::unique_ptr<std::atomic<uint64_t>[]> caches;
    uint32_t cacheCount = 0;
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

// For the compiler, part-way through a function: how many stack slots are in use where the
// code written so far ends, counted from the frame base, or -1 if that point is never reached.
int stackDepthAtEnd(FunctionProto& f, const Program& program);

void disassemble(const Program& program, std::ostream& out);

// How many classes and types a MakeType shape takes from the stack; -1 if it is malformed.
int shapeOperands(const std::string& shape);

// The length of the instruction at `at`, its operands included.
size_t instructionSize(const std::vector<uint8_t>& code, size_t at);

// An instruction's name, as the disassembly shows it.
const char* opName(Op op);

// Bytecode files. readProgram verifies everything it loads and throws std::runtime_error
// on a malformed file.
void writeProgram(const Program& program, std::ostream& out);
void readProgram(Program& program, VM& vm, const std::string& data);
bool looksLikeBytecode(const std::string& data);

// ---- A part of a program kept by itself (D145) ----
//
// A file's or a folder's modules are compiled once and kept, and put back into a later
// compile of the program. What their code says of the rest of the program goes by numbers
// that compile gave out: a global's slot, a module's index, and a class's or library's number
// (Op::ClassNumber, Op::MemberAccess, FunctionProto::classIds). renumber gives each of those
// to the function for its kind and puts in what it returns, all through a function and the
// functions nested in it; with functions that return what they are given it only lists them.
struct Renumbering {
    std::function<int(int)> global, module, number;
};
void renumber(FunctionProto& f, const Renumbering& r, VM& vm);

// The modules of such a part, by themselves: written, and read back into `program` (not yet
// verified, since their numbers are still the ones they were kept with).
void writeModules(const std::vector<const FunctionProto*>& modules, std::ostream& out);
std::vector<FunctionProto*> readModules(Program& program, VM& vm, const std::string& data);
// Verifies a module read back and renumbered, as readProgram does every module.
void verifyModule(FunctionProto& module, const Program& program);

// Takes out of a program the top-level declarations it can never reach, with their code
// (M6.5 part 5): for a built program, after it is compiled whole. `globalsByName` are the
// globals the VM looks up by their names, `membersByName` the members. Says what it took out.
struct LeftOut {
    size_t declarations = 0;
    size_t codeBefore = 0;  // the modules' own code, in bytes
    size_t codeAfter = 0;
};
LeftOut leaveOutUnreachable(Program& program, const std::vector<std::string>& globalsByName,
                            const std::vector<std::string>& membersByName);

// Whether any code of the program names one of these globals.
bool namesGlobal(const Program& program, const std::vector<std::string>& names);

}  // namespace astra
