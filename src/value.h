#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace astra {

class VM;
struct FunctionProto;

enum class ObjType : uint8_t {
    String, List, Map, Range, Closure, Upvalue, Native, Class, Instance, BoundMethod, BoundNative, Iterator, Bytes,
    Fiber,  // a flow: a function that can set itself aside (ObjFiber, in vm.h)
    Resource,  // something the runtime holds for a program that is not made of values
    TypeInfo,  // a type, as the program has it when it runs (ObjTypeInfo)
    Value,     // a value of a value type, in a box (ObjValue)
};

// Header shared by every garbage-collected object.
struct Obj {
    Obj* next = nullptr;  // intrusive list of all objects, owned by the VM
    const ObjType type;
    bool marked = false;
    bool immortal = false;  // never collected (interned strings)
    // In a parallel section: the core that owns the object, which alone may look into it and
    // change it. 0 for an object that is shared, which every core may read and none may change.
    // Outside a section every object is 0.
    uint8_t core = 0;

    explicit Obj(ObjType t) : type(t) {}
    virtual ~Obj() = default;
};

enum class ValueKind : uint8_t {
    Null,
    Bool,
    Int,
    Float,
    Obj,
    Undefined,  // a global slot that has not been defined yet; never visible to programs

    // A bare name passed as an argument that may be an enum entry: which enum is only known
    // once it reaches a parameter declared as one. Holds the name (as.obj, an ObjString) and
    // never gets further than the start of the function called.
    EnumName,

    // A value of a value type where it is kept (M6.6 part 4, value-lists.html section 6): in
    // an item of a list of values, or in a box, held by as.obj; the item's position in
    // `refIndex`, and where the value is within it in `refOffset`, `refDepth` values deep.
    // Only ever on a thread's stack: storing it anywhere else stores a box with a copy.
    Ref,

    // A value of a small value type kept in the value itself (M6.6 part 5, value-lists.html
    // section 4): one of at most 8 bytes that holds no object, its fields in `as`, its type's
    // number in `refIndex` (smallClasses). Kept anywhere a value is, with no box.
    Small,
};

// An Astra runtime value: null, bool, 64-bit int, double, or a pointer to a heap object.
struct Value {
    ValueKind kind;
    uint8_t refDepth;    // a Ref: how many values deep in the item it is (0: the item itself)
    uint16_t refOffset;  // a Ref: where in the item, in bytes
    uint32_t refIndex;   // a Ref into a list: the item's position
    union {
        bool b;
        int64_t i;
        double d;
        Obj* obj;
    } as;

    Value() : kind(ValueKind::Null) { as.i = 0; }

    static Value null() { return Value(); }
    static Value enumName(Obj* name) {
        Value v;
        v.kind = ValueKind::EnumName;
        v.as.obj = name;
        return v;
    }
    static Value undefined() {
        Value v;
        v.kind = ValueKind::Undefined;
        return v;
    }
    static Value boolean(bool b) {
        Value v;
        v.kind = ValueKind::Bool;
        v.as.b = b;
        return v;
    }
    // A value made in place, where it is to be kept: no copy through a temporary (M6.5 part 4).
    void setInt(int64_t i) {
        kind = ValueKind::Int;
        as.i = i;
    }
    void setFloat(double d) {
        kind = ValueKind::Float;
        as.d = d;
    }
    void setBool(bool b) {
        kind = ValueKind::Bool;
        as.i = 0;
        as.b = b;
    }

    static Value integer(int64_t i) {
        Value v;
        v.kind = ValueKind::Int;
        v.as.i = i;
        return v;
    }
    static Value number(double d) {
        Value v;
        v.kind = ValueKind::Float;
        v.as.d = d;
        return v;
    }
    static Value object(Obj* o) {
        Value v;
        v.kind = ValueKind::Obj;
        v.as.obj = o;
        return v;
    }
    static Value ref(Obj* holder, uint32_t index, uint16_t offset, uint8_t depth) {
        Value v;
        v.kind = ValueKind::Ref;
        v.as.obj = holder;
        v.refIndex = index;
        v.refOffset = offset;
        v.refDepth = depth;
        return v;
    }
    bool isRef() const { return kind == ValueKind::Ref; }
    static Value inSlot(uint32_t type, uint64_t bits) {
        Value v;
        v.kind = ValueKind::Small;
        v.refDepth = 0;
        v.refOffset = 0;
        v.refIndex = type;
        v.as.i = static_cast<int64_t>(bits);
        return v;
    }
    bool isSmall() const { return kind == ValueKind::Small; }

    bool isNull() const { return kind == ValueKind::Null; }
    bool isBool() const { return kind == ValueKind::Bool; }
    bool isInt() const { return kind == ValueKind::Int; }
    bool isFloat() const { return kind == ValueKind::Float; }
    bool isNumber() const { return kind == ValueKind::Int || kind == ValueKind::Float; }
    bool isObj() const { return kind == ValueKind::Obj; }
    bool isObjType(ObjType t) const { return kind == ValueKind::Obj && as.obj->type == t; }
    bool isString() const { return isObjType(ObjType::String); }

    // Numeric value as a double. Only valid when isNumber().
    double toDouble() const { return kind == ValueKind::Int ? static_cast<double>(as.i) : as.d; }

    // Only null and false are falsy.
    bool truthy() const { return !(kind == ValueKind::Null || (kind == ValueKind::Bool && !as.b)); }
};

uint32_t hashString(std::string_view s);

// The core the running thread is, in a parallel section; 0 on any other thread. For code
// that looks into objects without a VM at hand (the keys of a map).
extern thread_local uint8_t currentCore;

// How many characters (Unicode code points) a piece of UTF-8 text holds.
size_t countCharacters(std::string_view s);

// A string is text: a sequence of characters, each one Unicode code point. It is kept as
// UTF-8, and every position a program sees counts characters, never bytes.
struct ObjString final : Obj {
    const std::string chars;
    const uint32_t hash;

    // Some class has a member of this name that not all code may use (1), or may assign to
    // (2); and some class has one that is private (4). Set and never cleared: a member of
    // any other name is reached without a look at who may use it.
    uint8_t restricted = 0;

    const size_t length;  // in characters

    explicit ObjString(std::string s)
        : Obj(ObjType::String), chars(std::move(s)), hash(hashString(chars)), length(countCharacters(chars)) {}

    bool isAscii() const { return length == chars.size(); }

    // The byte at which character `index` starts; `index` may be the length, for the end.
    size_t byteAt(size_t index) const;
    // The number of characters before byte `offset`, which is where a character starts.
    size_t charAt(size_t offset) const;
    // Character `index`, as the bytes that spell it.
    std::string_view character(size_t index) const {
        size_t begin = byteAt(index);
        return std::string_view(chars).substr(begin, byteAt(index + 1) - begin);
    }

private:
    // For text that is not all ASCII: where each character starts, and the end. Made the
    // first time a position is asked for, which several cores may do at once.
    mutable std::vector<uint32_t> starts_;
    mutable std::atomic<bool> indexed_{false};
    void index() const;
};

// Language-level equality without operator overloading: numbers compare by value across
// int and float, strings by content, everything else by identity.
bool valuesEqual(const Value& a, const Value& b);
size_t hashValue(const Value& v);

// As keys of a map (and members of a set), lists, ranges, bytes and the objects of data
// classes are told apart by what they hold, as == tells them: [1, 2] finds what [1, 2] stored.
// A key must not be changed while it is in a map.
bool keysEqual(const Value& a, const Value& b, int depth = 0);
size_t keyHash(const Value& v, int depth = 0);

// Shortest text that reads back as the same double, always recognisable as a float ("3.0").
std::string formatFloat(double d);

// The string as a double-quoted literal with escapes.
std::string quoteString(std::string_view s);

// Hash table keyed by interned strings (compared by pointer). Used for fields, methods and statics.
class Table {
public:
    struct Entry {
        ObjString* key = nullptr;
        Value value;
    };

    Table() = default;
    ~Table() { delete[] entries_; }
    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;

    void clear() {
        delete[] entries_;
        entries_ = nullptr;
        capacity_ = 0;
        count_ = 0;
    }

    const Value* find(const ObjString* key) const {
        if (count_ == 0) return nullptr;
        uint32_t mask = capacity_ - 1;
        uint32_t i = key->hash & mask;
        for (;;) {
            const Entry& e = entries_[i];
            if (e.key == key) return &e.value;
            if (e.key == nullptr) return nullptr;
            i = (i + 1) & mask;
        }
    }

    void set(ObjString* key, Value value);
    void addAll(const Table& other);
    void addMissing(const Table& other);  // only the keys this table does not have

    uint32_t size() const { return count_; }
    uint32_t capacity() const { return capacity_; }
    const Entry* entries() const { return entries_; }

private:
    Entry* entries_ = nullptr;
    uint32_t capacity_ = 0;
    uint32_t count_ = 0;

    void grow();
};

// Insertion-ordered hash map from Value to Value: the storage behind Astra maps.
class ValueMap {
public:
    struct Entry {
        Value key;
        Value value;
        bool live = false;
    };

    const Value* find(const Value& key) const;
    void set(const Value& key, const Value& value);
    bool remove(const Value& key);
    void clear();
    size_t size() const { return index_.size(); }

    // Includes removed entries (live == false); iterate by index and skip those.
    const std::vector<Entry>& entries() const { return entries_; }

private:
    struct Hash {
        size_t operator()(const Value& v) const { return keyHash(v); }
    };
    struct Eq {
        bool operator()(const Value& a, const Value& b) const { return keysEqual(a, b); }
    };

    std::vector<Entry> entries_;
    std::unordered_map<Value, uint32_t, Hash, Eq> index_;
};

struct ObjTypeInfo;
struct ObjClass;

// How a list keeps its items (M6.6 part 3, value-lists.html section 5): as 16-byte values, as
// today, for a list of objects, of `any` or of a type a value must say itself, and for a list
// made with nothing said of what it holds; or packed one after another at their own widths,
// for a list whose items are values: List<int>, List<float>, List<bool>, a value type's.
enum class ListStore : uint8_t {
    Values,  // `items`
    Ints,    // `block`, 8 bytes an item
    Floats,  // 8 bytes an item
    Bools,   // 1 byte an item
    Items,   // a value type's, in its layout, `width` bytes an item
};

// The items of a packed list.
struct PackedItems {
    ListStore store = ListStore::Values;
    bool nullable = false;          // with a bit per item for null (List<int?>, List<Point?>)
    uint32_t width = 0;             // the bytes an item takes
    size_t count = 0;               // the items
    ObjClass* itemClass = nullptr;  // ListStore::Items: the value type
    std::vector<uint8_t> block;     // `count` items of `width` bytes, one after another
    std::vector<uint64_t> nulls;    // nullable: a bit per item, set where the item is null
};

struct ObjList final : Obj {
    // What the list was made to hold, `List<int>` (M6.6 part 1); null where that was not said,
    // as for a list made before types were kept, which takes anything.
    ObjTypeInfo* type = nullptr;
    // A packed list's items, apart from the list, which most lists do not need (M6.6 part 3).
    std::unique_ptr<PackedItems> pack;

    ObjList() : Obj(ObjType::List) {}

    bool packed() const { return pack != nullptr; }
    ListStore store() const { return pack != nullptr ? pack->store : ListStore::Values; }
    size_t size() const { return pack != nullptr ? pack->count : items_.size(); }
    // The items of a list kept as 16-byte values (not packed()); VM::listGet and the rest work
    // on any list.
    std::vector<Value>& values() { return items_; }
    const std::vector<Value>& values() const { return items_; }

private:
    std::vector<Value> items_;
};

struct ObjMap final : Obj {
    ValueMap map;
    ObjTypeInfo* type = nullptr;  // `Map<string, int>`, as for a list
    ObjMap() : Obj(ObjType::Map) {}
};

// Binary data: a fixed number of bytes, each a number from 0 to 255, that can be changed in place.
struct ObjBytes final : Obj {
    std::vector<uint8_t> data;
    ObjBytes() : Obj(ObjType::Bytes) {}
};

// Something the runtime holds for a program that is not made of values: a picture on the
// graphics card, say. A program can only keep it and hand it back. When the collector finds
// it unreachable, `release` is called with its number, on whichever thread is collecting.
struct ObjResource final : Obj {
    const char* const kind;
    const int64_t id;
    void (*const release)(int64_t);
    ObjResource(const char* k, int64_t i, void (*r)(int64_t)) : Obj(ObjType::Resource), kind(k), id(i), release(r) {}
    ~ObjResource() override {
        if (release != nullptr) release(id);
    }
};

// How many values a range from `start` to `end` by `step` has (a negative step counts down), at
// most INT64_MAX.
int64_t rangeCount(int64_t start, int64_t end, int64_t step, bool inclusive);

struct ObjRange final : Obj {
    const int64_t start, end, step;
    const bool inclusive;
    ObjRange(int64_t s, int64_t e, int64_t st, bool incl)
        : Obj(ObjType::Range), start(s), end(e), step(st), inclusive(incl) {}

    int64_t count() const;
    bool contains(int64_t i) const;
};

// A variable captured by a closure. Points into the VM stack while the variable is live
// there, and at its own `closed` slot afterwards.
struct ObjUpvalue final : Obj {
    Value* location;
    Value closed;
    ObjUpvalue* nextOpen = nullptr;
    // While it is open and points into the stack of a flow: that flow, which has to outlive it.
    Obj* owner = nullptr;
    explicit ObjUpvalue(Value* slot) : Obj(ObjType::Upvalue), location(slot) {}
};

struct ObjClosure : Obj {
    const FunctionProto* const proto;
    std::vector<ObjUpvalue*> upvalues;
    // The type arguments of the call it was made in (M6.6 part 1): a lambda written in a generic
    // function reads the function's own. Null where there were none.
    const ObjTypeInfo* typeArgs = nullptr;
    ObjClosure(const FunctionProto* p, size_t upvalueCount)
        : Obj(ObjType::Closure), proto(p), upvalues(upvalueCount, nullptr) {}
};

// The forms of a name that has several (overloads, D56), as one function. Its proto is a set
// of forms (FunctionProto::chooser); calling it calls the form the arguments fit.
struct ObjForms final : ObjClosure {
    std::vector<ObjClosure*> forms;
    explicit ObjForms(const FunctionProto* p) : ObjClosure(p, 0) {}
};

// Native functions receive their arguments as a pointer into the VM stack.
using NativeFn = Value (*)(VM& vm, int argc, Value* args);
using NativeMethodFn = Value (*)(VM& vm, Value self, int argc, Value* args);

// maxArgs of -1 means variadic.
struct ObjNative final : Obj {
    const std::string name;
    const int minArgs, maxArgs;
    const NativeFn fn;
    // False for a function that waits, or works on something there is one of for the whole
    // program: it is not called from a parallel section.
    bool parallelSafe = true;
    // What its parameters are called, from its declaration (src/std/builtins.astra), for a
    // call that names them; empty for one that takes none by name.
    std::vector<std::string> params;
    ObjNative(std::string n, int min, int max, NativeFn f)
        : Obj(ObjType::Native), name(std::move(n)), minArgs(min), maxArgs(max), fn(f) {}
};

struct NativeMethod {
    std::string name;
    int minArgs = 0;
    int maxArgs = 0;
    NativeMethodFn fn = nullptr;
    // What its parameters are called, for the methods that take arguments by name
    // (`text.contains("a", ignoreCase = true)`); empty for the rest.
    std::vector<std::string> params;
    // It changes the value it is called on: in a parallel section, a core's own only.
    bool mutates = false;
    // It copies a value of a value type it is given at once, so it may be given one where it is
    // kept, a Ref, rather than a box made for it (M6.6 part 5).
    bool takesPlaces = false;
};

struct ObjClass;

// A field declared in a class body (`val x : int`).
struct FieldDecl {
    ObjString* name = nullptr;
    uint8_t type = 0;       // a TypeId, optionally with kTypeNullable; 0 means untyped
    bool isVal = false;     // can be assigned only once

    // Declared again in a subclass with `override`. Such a val is given a value twice while
    // its object is made, first by the superclass and then by the subclass.
    bool overrides = false;

    // For a class-typed field: the class is looked up on first use, by calling `typeThunk`,
    // so that a field may name a class that is declared later in the program.
    ObjClosure* typeThunk = nullptr;
    ObjClass* typeClass = nullptr;

    // A field of a value type kept inside its object (M6.6 part 5): where its bytes are in the
    // object's inline part; -1 for any other field.
    int32_t inlineOffset = -1;
};

// Gives each layout of a class's fields a number of its own, never given again (M6.5 part 4).
uint32_t newShape();

// ---- Value types (M6.6 part 2, value-lists.html) ----

// Where a field of a value type is in the type's fixed layout, which is the same wherever the
// value is kept: in a box now, and in an item of a list of values or inside an object later.
struct ValueField {
    enum Kind : uint8_t {
        Int,     // 8 bytes, an int64_t
        Float,   // 8 bytes, a double
        Bool,    // 1 byte
        Object,  // 8 bytes, a pointer to an object, null for null: a string, a list, an object of a class...
        Any,     // 16 bytes, a Value: for a type a value must say itself (any, Number, int?, a type parameter)
        Nested,  // another value type's fields, in place, at its own size
    };
    ObjString* name = nullptr;
    uint32_t offset = 0;
    Kind kind = Any;
    uint8_t type = 0;           // as FieldDecl::type, for the check of a value stored there
    ObjClass* cls = nullptr;    // the class a class-typed field is declared with; for Nested, the value type
};

struct ValueLayout {
    uint32_t size = 0;                // a multiple of the alignment
    uint32_t align = 1;               // 8, but for a type of bools alone
    std::vector<ValueField> fields;   // in the order of the class's `declared`, index for index
    std::vector<uint32_t> objects;    // where the Object fields are, nested types' included, for the collector
    std::vector<uint32_t> values;     // and the Any fields
};

// The value types kept in the value itself (ValueKind::Small), by number; each VM keeps its
// own alive, and a class takes its number out when it goes.
constexpr uint32_t kMaxSmallClasses = 1u << 16;
extern std::atomic<ObjClass*> smallClasses[kMaxSmallClasses];

struct ObjClass final : Obj {
    ObjString* const name;

    // The layout of the class's fields, as a number: an object made now has its fields where
    // `declared` says, and is marked with this. A place in the code that has found a field
    // remembers the number and where the field was, and finds it there again at once in an
    // object with the same number. It changes whenever `declared` grows.
    uint32_t shape = newShape();
    ObjClass* super = nullptr;
    ObjClosure* init = nullptr;
    Table methods;  // instance methods, including inherited ones (copied down at class creation)
    Table statics;  // static methods and values assigned on the class itself

    // Properties: `get name()` and `set name(value)`, including inherited ones.
    Table getters;
    Table setters;
    bool hasAccessors = false;

    // Members written `private`, `protected` or `internal`, inherited ones too, and static
    // members written so, which are not inherited. A name leads to a mark, which says what
    // numbers the code using the member has to carry one of; the numbers are in `accessIds`
    // (see "Who may use a member" in vm.cpp, and Op::MemberAccess).
    Table accessMarks;
    Table staticAccessMarks;
    std::vector<uint16_t> accessIds;
    int number = 0;              // the class's own number, and its library's
    int library = 0;
    bool accessAllOwn = true;    // the class's own code may use every member its objects have,
    bool accessSharedOwn = true; // or at least every one that is protected or internal
    // One bit for each name in `accessMarks`, by the low six bits of its hash: a name whose
    // bit is clear is certainly not among them, which is known without looking it up.
    uint64_t accessBits = 0;

    std::vector<FieldDecl> declared;        // declared fields, including inherited ones
    Table declaredIndex;                    // field name -> index into `declared`
    std::vector<ObjString*> ownRequired;    // fields this class's own initializer must assign
    bool anyRequired = false;               // this class or an ancestor has required fields
    std::vector<ObjClosure*> fieldInits;    // run on every new instance, superclass first

    // The supertypes written with type arguments (`class Pair<A> : Base<List<A>>()`, M6.6 part
    // 1): for each, a function that makes it from this class's own type arguments, which it is
    // given as a generic function is; and what they made, by the type of this class they were
    // made for (null for an object made without one), in VM::supertypesOf.
    std::vector<ObjClosure*> supertypeMakers;
    std::unordered_map<const ObjTypeInfo*, std::vector<ObjTypeInfo*>> supertypesMade;

    // Static fields declared with `static val` / `static var` (their values are in `statics`),
    // and static properties.
    std::vector<FieldDecl> staticDeclared;
    Table staticIndex;                      // name -> index into `staticDeclared`
    Table staticGetters;
    Table staticSetters;

    // An enum: its instances are its entries, each with a name and an ordinal, and once they
    // are made no more can be.
    bool isEnum = false;
    bool enumSealed = false;
    ObjString* nextEntry = nullptr;  // the name of the entry about to be made, which it has from the start
    int nextOrdinal = 0;

    // Interfaces. A class lists every interface it implements, its superclass's and those its
    // interfaces extend included. `required` are the members an interface leaves for the
    // class to supply, each with the interface that asks for it.
    // A data class: the header fields its objects are compared, printed and taken apart by.
    bool isData = false;
    std::vector<ObjString*> components;

    // A value type (`value Point(var x : int, var y : int)`, M6.6 part 2): its values have no
    // identity and never change once made; they are kept in its fixed layout, worked out from
    // its fields when the first one is made (VM::layoutOf), when every class it names exists.
    // It is a data class as well.
    bool isValue = false;
    bool madeFromHeader = false;  // a value type with no init, init blocks or fields of its body
    uint32_t smallId = 0;         // kept in the value itself: its number in smallClasses; 0 if not

    // The fields of value types an object of this class keeps inside itself (M6.6 part 5,
    // value-lists.html section 4), worked out when the first one is made: each one's place in
    // the object's inline part, after its other slots, and its type; and how many slots of a
    // value's size the part takes.
    struct InlineField {
        uint32_t offset;
        uint32_t size;
        ObjClass* cls;
    };
    std::vector<InlineField> inlineFields;
    uint32_t inlineSlots = 0;
    std::atomic<bool> inlineLaid{false};
    const InlineField* inlineFieldAt(uint32_t offset) const {
        for (const InlineField& f : inlineFields)
            if (offset >= f.offset && offset < f.offset + f.size) return &f;
        return nullptr;
    }
    ~ObjClass() override {
        if (smallId != 0) smallClasses[smallId].store(nullptr, std::memory_order_release);
    }
    std::atomic<const ValueLayout*> layout{nullptr};
    std::unique_ptr<ValueLayout> layoutOwned;

    // Works out the class's static fields and, for an enum, its entries. It runs once, the
    // first time a static member is asked for or when the program reaches the class's
    // declaration, and is null from then on.
    ObjClosure* staticInit = nullptr;

    Table extensions;  // functions added from outside with `fun Name.function()`: looked for after the members
    bool isAbstract = false;  // never created itself; what it leaves unwritten its subclasses supply
    bool isInterface = false;
    std::vector<ObjClass*> interfaces;
    std::vector<std::pair<ObjString*, ObjClass*>> required;

    explicit ObjClass(ObjString* n) : Obj(ObjType::Class), name(n) {}

    bool isSubclassOf(const ObjClass* other) const {
        for (const ObjClass* k = this; k != nullptr; k = k->super)
            if (k == other) return true;
        if (other != nullptr && other->isInterface)
            for (const ObjClass* implemented : interfaces)
                if (implemented == other) return true;
        return false;
    }

    const Value* findStatic(const ObjString* key) const {
        for (const ObjClass* k = this; k != nullptr; k = k->super)
            if (const Value* v = k->statics.find(key)) return v;
        return nullptr;
    }
};

// A type as the program has it when it runs (M6.6 part 1): what a list or a map was made to
// hold, a generic class's or function's type arguments. There is one of each type, made the
// first time it is needed and kept for as long as the VM (VM::typeOf), so two types are the
// same exactly when they are the same object.
struct ObjTypeInfo final : Obj {
    const uint8_t id;      // a TypeId (bytecode.h), or 0 for any
    const bool nullable;
    ObjClass* const cls;   // for an instance of a class
    const std::vector<ObjTypeInfo*> args;  // List: the items; Map: the keys and the values; a class: its type arguments
    // For a List type: whether a list of it keeps its items packed (M6.6 part 3), worked out the
    // first time one is made; every core works out the same.
    std::atomic<int8_t> packs{-1};
    ObjTypeInfo(uint8_t i, bool n, ObjClass* c, std::vector<ObjTypeInfo*> a)
        : Obj(ObjType::TypeInfo), id(i), nullable(n), cls(c), args(std::move(a)) {}
};

// An object of a class. Its fields are kept in the order of the class's `declared` (M6.5 part 4),
// right after it in the same block of memory; one that has not been given a value yet holds
// Undefined. Made with VM::newInstance, which sets aside the room for them.
struct ObjInstance final : Obj {
    ObjClass* const cls;
    const uint32_t shape;       // the class's layout when the object was made
    const uint32_t fieldCount;  // and the number of fields it has, which never changes
    bool constructing = false;  // from being made until its initializer has returned
    // An anonymous object (`object : Shape { ... }`): the variables of the code around it that
    // its members use, each an ObjUpvalue, kept after the fields (M6.5 part 4). Its class is
    // made once, so they cannot be in its methods' closures.
    const uint16_t captureCount;
    // An object of a generic class made with its type arguments known (`Box<int>(1)`, M6.6
    // part 1) keeps its type after the captures; one made without them does not, and is read
    // as of any type arguments.
    const bool typed;
    // Made with room for its class's fields of value types inside it (M6.6 part 5).
    const bool inlined;

    ObjInstance(ObjClass* c, uint32_t count, uint16_t captures, bool withType, uint32_t inlineSlots)
        : Obj(ObjType::Instance), cls(c), shape(c->shape), fieldCount(count), captureCount(captures), typed(withType),
          inlined(inlineSlots != 0) {
        Value* at = fields();
        for (uint32_t i = 0; i < count + captures + (withType ? 1u : 0u); i++) new (at + i) Value(Value::undefined());
        if (inlineSlots != 0) std::memset(inlineData(), 0, inlineSlots * sizeof(Value));
    }

    // The bytes of its fields of value types, after its other slots.
    uint8_t* inlineData() { return reinterpret_cast<uint8_t*>(fields() + fieldCount + captureCount + (typed ? 1u : 0u)); }
    const uint8_t* inlineData() const {
        return reinterpret_cast<const uint8_t*>(fields() + fieldCount + captureCount + (typed ? 1u : 0u));
    }

    // Its type, `Box<int>`, or null where it was made without one.
    const ObjTypeInfo* type() const {
        return typed ? static_cast<const ObjTypeInfo*>(fields()[fieldCount + captureCount].as.obj) : nullptr;
    }
    void setType(ObjTypeInfo* type) { fields()[fieldCount + captureCount] = Value::object(type); }

    // Capture `k`, where the code that uses it was compiled for this object's class.
    ObjUpvalue* captured(uint32_t k) const { return static_cast<ObjUpvalue*>(fields()[fieldCount + k].as.obj); }

    static void* operator new(size_t size, uint32_t count) { return ::operator new(size + count * sizeof(Value)); }
    static void operator delete(void* p, uint32_t) { ::operator delete(p); }
    static void operator delete(void* p) { ::operator delete(p); }

    Value* fields() { return reinterpret_cast<Value*>(this + 1); }
    const Value* fields() const { return reinterpret_cast<const Value*>(this + 1); }

    // Where the field called `name` is, or -1 if the object has none of that name.
    int64_t fieldIndex(const ObjString* name) const {
        const Value* at = cls->declaredIndex.find(name);
        return at != nullptr && at->as.i < fieldCount ? at->as.i : -1;
    }
    // The field's value, or null if the object has no such field or it has no value yet.
    const Value* field(const ObjString* name) const {
        const int64_t i = fieldIndex(name);
        if (i < 0 || fields()[i].kind == ValueKind::Undefined) return nullptr;
        return fields() + i;
    }
    // Gives the field a value; false if the object has no field of that name.
    bool setField(const ObjString* name, Value value) {
        const int64_t i = fieldIndex(name);
        if (i < 0) return false;
        fields()[i] = value;
        return true;
    }
};
static_assert(sizeof(ObjInstance) % alignof(Value) == 0, "an object's fields follow it");

// A value of a value type in a box (value-lists.html, section 4): its class, and after it its
// fields in the class's layout. A box never changes once its initializer has returned, so it
// may be shared: `val b = a` copies the reference.
struct ObjValue final : Obj {
    ObjClass* const cls;
    bool constructing = false;  // from being made until its initializer has returned

    explicit ObjValue(ObjClass* c) : Obj(ObjType::Value), cls(c) {}

    static void* operator new(size_t size, uint32_t bytes) { return ::operator new(size + bytes); }
    static void operator delete(void* p, uint32_t) { ::operator delete(p); }
    static void operator delete(void* p) { ::operator delete(p); }

    uint8_t* data() { return reinterpret_cast<uint8_t*>(this + 1); }
    const uint8_t* data() const { return reinterpret_cast<const uint8_t*>(this + 1); }
};
static_assert(sizeof(ObjValue) % alignof(Value) == 0, "a value's fields follow it");

// The value type of what a Ref points at: the holder's item type, and from there `refDepth`
// values in, the one whose place `refOffset` is in.
inline ObjClass* refClass(const Value& v) {
    ObjClass* cls;
    uint32_t base = 0;
    if (v.as.obj->type == ObjType::Instance) {
        // A field of a value type inside an object (M6.6 part 5): the one the offset is in.
        const ObjClass::InlineField* field = static_cast<const ObjInstance*>(v.as.obj)->cls->inlineFieldAt(v.refOffset);
        cls = field->cls;
        base = field->offset;
    } else {
        cls = v.as.obj->type == ObjType::List ? static_cast<const ObjList*>(v.as.obj)->pack->itemClass
                                              : static_cast<const ObjValue*>(v.as.obj)->cls;
    }
    for (int d = 0; d < v.refDepth; d++) {
        const ValueLayout* layout = cls->layout.load(std::memory_order_acquire);
        for (const ValueField& field : layout->fields) {
            if (field.kind != ValueField::Nested) continue;
            const uint32_t start = base + field.offset;
            if (v.refOffset >= start && v.refOffset < start + field.cls->layout.load(std::memory_order_acquire)->size) {
                base = start;
                cls = field.cls;
                break;
            }
        }
    }
    return cls;
}

// The class of the value a Ref of depth 0 points at, a list's item or a box's value, without
// looking further; null for a list that keeps no values now.
inline ObjClass* refHolderClass(const Value& v) {
    if (v.as.obj->type == ObjType::List) {
        const ObjList* list = static_cast<const ObjList*>(v.as.obj);
        return list->pack != nullptr ? list->pack->itemClass : nullptr;
    }
    if (v.as.obj->type == ObjType::Instance) return static_cast<const ObjInstance*>(v.as.obj)->cls->inlineFieldAt(v.refOffset)->cls;
    return static_cast<const ObjValue*>(v.as.obj)->cls;
}

// The class of a value kept in the value itself.
inline ObjClass* smallClassOf(const Value& v) { return smallClasses[v.refIndex].load(std::memory_order_acquire); }
inline const uint8_t* smallBytes(const Value& v) { return reinterpret_cast<const uint8_t*>(&v.as); }

// Where the bytes of what a Ref points at are; null for an item that is no longer in its list.
inline uint8_t* refBytes(const Value& v) {
    if (v.as.obj->type == ObjType::List) {
        const ObjList* list = static_cast<const ObjList*>(v.as.obj);
        if (list->pack == nullptr || v.refIndex >= list->pack->count) return nullptr;
        return const_cast<uint8_t*>(list->pack->block.data()) + static_cast<size_t>(v.refIndex) * list->pack->width + v.refOffset;
    }
    if (v.as.obj->type == ObjType::Instance) return static_cast<ObjInstance*>(v.as.obj)->inlineData() + v.refOffset;
    return static_cast<ObjValue*>(v.as.obj)->data() + v.refOffset;
}

// The class of an object of a class or of a value of a value type, where it is kept or where it
// is pointed at; null for anything else.
inline ObjClass* classOf(const Value& v) {
    if (v.kind == ValueKind::Ref) return refClass(v);
    if (v.kind == ValueKind::Small) return smallClassOf(v);
    if (!v.isObj()) return nullptr;
    if (v.as.obj->type == ObjType::Instance) return static_cast<const ObjInstance*>(v.as.obj)->cls;
    if (v.as.obj->type == ObjType::Value) return static_cast<const ObjValue*>(v.as.obj)->cls;
    return nullptr;
}

struct ObjBoundMethod final : Obj {
    const Value receiver;
    ObjClosure* const method;
    ObjBoundMethod(Value r, ObjClosure* m) : Obj(ObjType::BoundMethod), receiver(r), method(m) {}
};

struct ObjBoundNative final : Obj {
    const Value receiver;
    const NativeMethod* const method;
    ObjBoundNative(Value r, const NativeMethod* m) : Obj(ObjType::BoundNative), receiver(r), method(m) {}
};

// State of a for-in loop over `source` (a list, map, string or range).
struct ObjIterator final : Obj {
    const Value source;
    size_t index = 0;
    int64_t nextValue = 0;  // ranges: next value to yield
    int64_t remaining = 0;  // ranges: values left
    explicit ObjIterator(Value s) : Obj(ObjType::Iterator), source(s) {}
};

inline ObjString* asString(const Value& v) { return static_cast<ObjString*>(v.as.obj); }
inline ObjList* asList(const Value& v) { return static_cast<ObjList*>(v.as.obj); }
inline ObjMap* asMap(const Value& v) { return static_cast<ObjMap*>(v.as.obj); }
inline ObjRange* asRange(const Value& v) { return static_cast<ObjRange*>(v.as.obj); }
inline ObjBytes* asBytes(const Value& v) { return static_cast<ObjBytes*>(v.as.obj); }
inline ObjClosure* asClosure(const Value& v) { return static_cast<ObjClosure*>(v.as.obj); }
inline ObjNative* asNative(const Value& v) { return static_cast<ObjNative*>(v.as.obj); }
inline ObjClass* asClass(const Value& v) { return static_cast<ObjClass*>(v.as.obj); }
inline ObjInstance* asInstance(const Value& v) { return static_cast<ObjInstance*>(v.as.obj); }
inline ObjValue* asValue(const Value& v) { return static_cast<ObjValue*>(v.as.obj); }
inline ObjBoundMethod* asBoundMethod(const Value& v) { return static_cast<ObjBoundMethod*>(v.as.obj); }
inline ObjBoundNative* asBoundNative(const Value& v) { return static_cast<ObjBoundNative*>(v.as.obj); }
inline ObjIterator* asIterator(const Value& v) { return static_cast<ObjIterator*>(v.as.obj); }

}  // namespace astra
