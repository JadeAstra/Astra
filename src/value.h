#pragma once

#include <cstddef>
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
};

// An Astra runtime value: null, bool, 64-bit int, double, or a pointer to a heap object.
struct Value {
    ValueKind kind;
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

struct ObjList final : Obj {
    std::vector<Value> items;
    ObjList() : Obj(ObjType::List) {}
};

struct ObjMap final : Obj {
    ValueMap map;
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
};

// Gives each layout of a class's fields a number of its own, never given again (M6.5 part 4).
uint32_t newShape();

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

    ObjInstance(ObjClass* c, uint32_t count, uint16_t captures)
        : Obj(ObjType::Instance), cls(c), shape(c->shape), fieldCount(count), captureCount(captures) {
        Value* at = fields();
        for (uint32_t i = 0; i < count + captures; i++) new (at + i) Value(Value::undefined());
    }

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
inline ObjBoundMethod* asBoundMethod(const Value& v) { return static_cast<ObjBoundMethod*>(v.as.obj); }
inline ObjBoundNative* asBoundNative(const Value& v) { return static_cast<ObjBoundNative*>(v.as.obj); }
inline ObjIterator* asIterator(const Value& v) { return static_cast<ObjIterator*>(v.as.obj); }

}  // namespace astra
