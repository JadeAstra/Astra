#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace astra {

class VM;
struct FunctionProto;

enum class ObjType : uint8_t {
    String, List, Map, Range, Closure, Upvalue, Native, Class, Instance, BoundMethod, BoundNative, Iterator,
};

// Header shared by every garbage-collected object.
struct Obj {
    Obj* next = nullptr;  // intrusive list of all objects, owned by the VM
    const ObjType type;
    bool marked = false;
    bool immortal = false;  // never collected (interned strings)

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

struct ObjString final : Obj {
    const std::string chars;
    const uint32_t hash;
    explicit ObjString(std::string s) : Obj(ObjType::String), chars(std::move(s)), hash(hashString(chars)) {}
};

// Language-level equality without operator overloading: numbers compare by value across
// int and float, strings by content, everything else by identity.
bool valuesEqual(const Value& a, const Value& b);
size_t hashValue(const Value& v);

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
        size_t operator()(const Value& v) const { return hashValue(v); }
    };
    struct Eq {
        bool operator()(const Value& a, const Value& b) const { return valuesEqual(a, b); }
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
    explicit ObjUpvalue(Value* slot) : Obj(ObjType::Upvalue), location(slot) {}
};

struct ObjClosure final : Obj {
    const FunctionProto* const proto;
    std::vector<ObjUpvalue*> upvalues;
    ObjClosure(const FunctionProto* p, size_t upvalueCount)
        : Obj(ObjType::Closure), proto(p), upvalues(upvalueCount, nullptr) {}
};

// Native functions receive their arguments as a pointer into the VM stack.
using NativeFn = Value (*)(VM& vm, int argc, Value* args);
using NativeMethodFn = Value (*)(VM& vm, Value self, int argc, Value* args);

// maxArgs of -1 means variadic.
struct ObjNative final : Obj {
    const std::string name;
    const int minArgs, maxArgs;
    const NativeFn fn;
    ObjNative(std::string n, int min, int max, NativeFn f)
        : Obj(ObjType::Native), name(std::move(n)), minArgs(min), maxArgs(max), fn(f) {}
};

struct NativeMethod {
    std::string name;
    int minArgs = 0;
    int maxArgs = 0;
    NativeMethodFn fn = nullptr;
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

struct ObjClass final : Obj {
    ObjString* const name;
    ObjClass* super = nullptr;
    ObjClosure* init = nullptr;
    Table methods;  // instance methods, including inherited ones (copied down at class creation)
    Table statics;  // static methods and values assigned on the class itself

    // Properties: `get name()` and `set name(value)`, including inherited ones.
    Table getters;
    Table setters;
    bool hasAccessors = false;

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

struct ObjInstance final : Obj {
    ObjClass* const cls;
    Table fields;
    bool constructing = false;  // from being made until its initializer has returned
    explicit ObjInstance(ObjClass* c) : Obj(ObjType::Instance), cls(c) {}
};

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

inline ObjString* asString(Value v) { return static_cast<ObjString*>(v.as.obj); }
inline ObjList* asList(Value v) { return static_cast<ObjList*>(v.as.obj); }
inline ObjMap* asMap(Value v) { return static_cast<ObjMap*>(v.as.obj); }
inline ObjRange* asRange(Value v) { return static_cast<ObjRange*>(v.as.obj); }
inline ObjClosure* asClosure(Value v) { return static_cast<ObjClosure*>(v.as.obj); }
inline ObjNative* asNative(Value v) { return static_cast<ObjNative*>(v.as.obj); }
inline ObjClass* asClass(Value v) { return static_cast<ObjClass*>(v.as.obj); }
inline ObjInstance* asInstance(Value v) { return static_cast<ObjInstance*>(v.as.obj); }
inline ObjBoundMethod* asBoundMethod(Value v) { return static_cast<ObjBoundMethod*>(v.as.obj); }
inline ObjBoundNative* asBoundNative(Value v) { return static_cast<ObjBoundNative*>(v.as.obj); }
inline ObjIterator* asIterator(Value v) { return static_cast<ObjIterator*>(v.as.obj); }

}  // namespace astra
