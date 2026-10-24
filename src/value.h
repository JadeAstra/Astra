#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <atomic>
#include <bitset>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace astra {

class VM;
struct FunctionProto;

enum class ObjType : uint8_t {
    String, List, Map, Range, Closure, Upvalue, Native, Class, Instance, BoundMethod, BoundNative, Iterator, Bytes,
    Fiber,  // a task: a function that can set itself aside (ObjFiber, in vm.h)
    Resource,  // something the runtime holds for a program that is not made of values
    TypeInfo,  // a type, as the program has it when it runs (ObjTypeInfo)
    Value,     // a value of a value type, in a box (ObjValue)
    Set,       // a set, or a live view of a map's keys or entries (ObjSet)
    Lazy,      // what a lazy field, static or global works its value out with (ObjLazy, D37)
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
    // A frozen list, map, set or bytes (D26): nobody may change it, which VM::guardWrite asks
    // with the core. 1 when everything in it is a key that cannot change as well, 2 when it
    // holds something that can (an object of a class whose == reads a var).
    uint8_t frozen = 0;
    // Frozen data a worker was sent (D97): kept once outside every VM's heap, read in place by
    // each that holds it, and freed when the last lets go. No VM marks or sweeps it.
    bool outside = false;

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

    // One character, a Unicode code point, in `as.i` (D24): what `s[i]` gives.
    Char,
    // A number from 0 to 255 in `as.i` (D19): what an item of `Bytes` is.
    Byte,
    // `^k`, a position counted from the end, k (1 or more) in `as.i` (D146): what an `Index`
    // is that is not a plain int.
    FromEnd,
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
    // A lazy field, static or global with no value yet (D37): "no value" as every read tests
    // for, with what works it out (an ObjLazy). The first read puts the value in its place.
    static Value lazy(Obj* how) {
        Value v;
        v.kind = ValueKind::Undefined;
        v.as.obj = how;
        return v;
    }
    bool isLazy() const { return kind == ValueKind::Undefined && as.obj != nullptr; }
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
    static Value character(uint32_t c) {
        Value v;
        v.kind = ValueKind::Char;
        v.as.i = c;
        return v;
    }
    static Value byte(uint8_t b) {
        Value v;
        v.kind = ValueKind::Byte;
        v.as.i = b;
        return v;
    }
    static Value fromEnd(int64_t k) {
        Value v;
        v.kind = ValueKind::FromEnd;
        v.as.i = k;
        return v;
    }
    bool isFromEnd() const { return kind == ValueKind::FromEnd; }
    bool isChar() const { return kind == ValueKind::Char; }
    bool isByte() const { return kind == ValueKind::Byte; }

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

// The core the running thread is, in a parallel section; 0 on any other thread. For code
// that looks into objects without a VM at hand (the keys of a map).
extern thread_local uint8_t currentCore;
// What else the piece running on this thread may read: the numbers of the pieces that started
// its section (VM::readable_). Null outside a section inside a section.
extern thread_local const std::bitset<256>* currentReadable;

// A class's own == and hashCode, which maps and sets use for its objects and values as keys
// (D26): the VM running on this thread calls them. Each answers false where the class has
// none. refuseKey throws the error for a key that could change.
struct ObjClass;
struct KeyMethods {
    virtual bool hashOf(const ObjClass* cls, const Value& v, size_t& out) = 0;
    virtual bool equalOf(const ObjClass* cls, const Value& a, const Value& b, bool& out) = 0;
    [[noreturn]] virtual void refuseKey(const Value& key) = 0;

protected:
    ~KeyMethods() = default;
};
extern thread_local KeyMethods* keyMethods;

// How many characters (Unicode code points) a piece of UTF-8 text holds.
size_t countCharacters(std::string_view s);
// Adds character `c` to `out` as UTF-8.
void appendUtf8(std::string& out, uint32_t c);
// The hash of a string's characters taken one more character `c` in: a string's hash is the
// same whatever width it is kept at, and a char hashes as the string of it alone (D25).
inline uint32_t hashCharacter(uint32_t hash, uint32_t c) { return (hash ^ c) * 16777619u; }
constexpr uint32_t kEmptyStringHash = 2166136261u;  // FNV-1a over the characters

// Characters at one width, as a string is kept: 1, 2 or 4 bytes each (D20).
struct Characters {
    std::string units;  // `width` bytes a character, in the machine's byte order
    uint8_t width = 1;

    size_t size() const { return units.size() / width; }
    uint32_t operator[](size_t i) const {
        switch (width) {
            case 1: return static_cast<unsigned char>(units[i]);
            case 2: { uint16_t c; std::memcpy(&c, units.data() + 2 * i, 2); return c; }
            default: { uint32_t c; std::memcpy(&c, units.data() + 4 * i, 4); return c; }
        }
    }
    // Text at the narrowest width that holds all of it; text not valid as UTF-8 has U+FFFD in
    // place of each byte that is not.
    static Characters fromUtf8(std::string text);
    // Characters given at width `from`, kept at width `to` (at least as wide as any of them).
    static void widen(const char* in, size_t count, uint8_t from, uint8_t to, std::string& out);

    // Adds `count` characters given at width `from`, widening what is here if they need it:
    // text built from parts takes the width of its widest part.
    void append(const char* in, size_t count, uint8_t from) {
        if (from > width && units.empty()) {
            width = from;
        } else if (from > width) {
            std::string wider;
            wider.reserve((size() + count) * from);
            widen(units.data(), size(), width, from, wider);
            units = std::move(wider);
            width = from;
        }
        widen(in, count, from, width, units);
    }
    void append(const Characters& more) { append(more.units.data(), more.size(), more.width); }
    void appendUtf8Text(std::string_view text);
};

// A string is text: a sequence of characters, each one Unicode code point. It is kept at a
// fixed width, the narrowest its widest character needs, as Python keeps its strings (D20):
// 1 byte a character when all are below 256 (ASCII and Latin-1), 2 when all are below 65,536,
// 4 otherwise, so a character is reached by its number directly, and equal strings are kept
// alike and compare byte for byte. C++ code that wants UTF-8 (printing, files, the text
// engine, names) asks for utf8(): for ASCII text the characters themselves, for other text
// made when first asked for and kept.
struct ObjString final : Obj {
    const uint32_t hash;  // of the characters, whatever the width

    // Some class has a member of this name that not all code may use (1), or may assign to
    // (2); and some class has one that is private (4). Set and never cleared: a member of
    // any other name is reached without a look at who may use it.
    uint8_t restricted = 0;

    const uint8_t width;  // 1, 2 or 4 bytes a character
    const bool ascii;     // every character below 128: the characters are their own UTF-8
    const size_t length;  // in characters

    // From UTF-8 text.
    explicit ObjString(std::string utf8) : ObjString(make(Characters::fromUtf8(std::move(utf8)))) {}
    // From characters at some width; kept at the narrowest that holds them.
    explicit ObjString(Characters characters) : ObjString(make(std::move(characters))) {}
    // Two strings joined: kept at the wider one's width, which is the narrowest that holds
    // both, and hashed on from the first one's hash over the second's characters alone.
    ObjString(const ObjString* first, const ObjString* second) : ObjString(join(first, second)) {}
    ~ObjString() override { delete utf8_.load(std::memory_order_relaxed); }

    uint32_t at(size_t index) const {
        switch (width) {
            case 1: return static_cast<unsigned char>(units_[index]);
            case 2: { uint16_t c; std::memcpy(&c, units_.data() + 2 * index, 2); return c; }
            default: { uint32_t c; std::memcpy(&c, units_.data() + 4 * index, 4); return c; }
        }
    }
    // The characters, `width` bytes each.
    const std::string& units() const { return units_; }
    // Characters `begin` to `end` (not included), at the narrowest width that holds them.
    Characters slice(size_t begin, size_t end) const;

    const std::string& utf8() const {
        if (ascii) return units_;
        const std::string* made = utf8_.load(std::memory_order_acquire);
        return made != nullptr ? *made : makeUtf8();
    }
    // Where character `index` starts in the UTF-8 form; `index` may be the length, for the end.
    size_t utf8Offset(size_t index) const;
    // Where `part` first is at or after character `from` (last, at or before `from`, when
    // `last`), or -1.
    int64_t find(const ObjString* part, size_t from, bool last = false) const;

    // The memory the string holds beyond the object itself, for the collector's count.
    size_t heldBytes() const {
        const std::string* made = utf8_.load(std::memory_order_acquire);
        return units_.capacity() + (made != nullptr ? made->capacity() : 0);
    }

private:
    const std::string units_;
    // The UTF-8 form of text that is not all ASCII, once asked for. Several cores may ask at
    // the same moment: each makes it, and the first to store it wins.
    mutable std::atomic<const std::string*> utf8_{nullptr};

    struct Made {
        std::string units;
        uint32_t hash;
        uint8_t width;
        bool ascii;
        size_t length;
    };
    static Made make(Characters characters);
    static Made join(const ObjString* first, const ObjString* second);
    explicit ObjString(Made made);
    const std::string& makeUtf8() const;
};

inline void appendText(Characters& to, const ObjString* text) { to.append(text->units().data(), text->length, text->width); }

// Whether two strings hold the same characters.
inline bool sameText(const ObjString* a, const ObjString* b) {
    return a == b || (a->hash == b->hash && a->width == b->width && a->units() == b->units());
}
// Orders two strings by their characters' numbers (which is the order of their UTF-8 bytes).
int compareText(const ObjString* a, const ObjString* b);

// Language-level equality without operator overloading: numbers compare by value across
// int and float, strings by content, everything else by identity.
bool valuesEqual(const Value& a, const Value& b);
size_t hashValue(const Value& v);

// As keys of a map (and members of a set), lists, ranges, bytes and the objects of data
// classes are told apart by what they hold, as == tells them: [1, 2] finds what [1, 2] stored.
// A key must not be changed while it is in a map.
bool keysEqual(const Value& a, const Value& b, int depth = 0);
size_t keyHash(const Value& v, int depth = 0);
// Whether a value may be a new key of a map or a set: not one that can change (D26).
bool stableKey(const Value& v);

// Shortest text that reads back as the same double, always recognisable as a float ("3.0").
std::string formatFloat(double d);

// The string as a double-quoted literal with escapes.
std::string quoteString(std::string_view s);
// A char as it is written: 'a', '\n', '\''.
std::string quoteCharacter(uint32_t c);
// Whether `c` is a character: a code point that is not a surrogate's (D25).
inline bool isCharacter(int64_t c) { return c >= 0 && c <= 0x10FFFF && !(c >= 0xD800 && c <= 0xDFFF); }

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
struct ObjClosure;

// How a lazy value is worked out (D37): the function giving it, and whether a thread is at it.
// Kept by a Value::lazy in the place the value goes, until the first read puts it there.
struct ObjLazy final : Obj {
    ObjClosure* init = nullptr;
    uint8_t state = 0;     // 0 not worked out, 1 being worked out, 2 known
    uintptr_t owner = 0;   // state 1: the thread working it out
    int waiters = 0;       // state 1: the threads waiting for it, on its monitor
    Value value;           // state 2: what it gave, for a thread that waited for it
    ObjLazy() : Obj(ObjType::Lazy) {}
};

// A byte, a char or a number at a storage width (D19, D24, D148), kept narrower than a value
// is: in a packed list or a value type's field. Read, a storage width is an int (a float for
// Float32); a value that does not fit is an error where it is stored.
enum class Narrow : uint8_t { None, Byte, Char, Int16, Int32, Uint16, Uint32, Float32 };

inline uint32_t narrowSize(Narrow n) {
    return n == Narrow::Byte ? 1 : n == Narrow::Int16 || n == Narrow::Uint16 ? 2 : 4;
}

inline Value readNarrow(Narrow n, const uint8_t* at) {
    switch (n) {
        case Narrow::Byte: return Value::byte(*at);
        case Narrow::Char: {
            uint32_t c;
            std::memcpy(&c, at, 4);
            return Value::character(c);
        }
        case Narrow::Int16: {
            int16_t v;
            std::memcpy(&v, at, 2);
            return Value::integer(v);
        }
        case Narrow::Int32: {
            int32_t v;
            std::memcpy(&v, at, 4);
            return Value::integer(v);
        }
        case Narrow::Uint16: {
            uint16_t v;
            std::memcpy(&v, at, 2);
            return Value::integer(v);
        }
        case Narrow::Uint32: {
            uint32_t v;
            std::memcpy(&v, at, 4);
            return Value::integer(v);
        }
        case Narrow::Float32: {
            float f;
            std::memcpy(&f, at, 4);
            return Value::number(f);
        }
        default: return Value::null();
    }
}

// The range of a narrow int.
inline void narrowRange(Narrow n, int64_t& low, int64_t& high) {
    switch (n) {
        case Narrow::Byte: low = 0; high = 255; break;
        case Narrow::Int16: low = -32768; high = 32767; break;
        case Narrow::Int32: low = -2147483648LL; high = 2147483647; break;
        case Narrow::Uint16: low = 0; high = 65535; break;
        default: low = 0; high = 4294967295LL; break;
    }
}

// Whether `v` can be kept narrow: a byte as a byte (or an int from 0 to 255, from code that
// does not say its types), a char as a char, an int in the width's range, a float.
inline bool fitsNarrow(Narrow n, const Value& v) {
    switch (n) {
        case Narrow::Char: return v.isChar();
        case Narrow::Float32: return v.isFloat();
        case Narrow::Byte:
            if (v.isByte()) return true;
            [[fallthrough]];
        default: {
            if (!v.isInt()) return false;
            int64_t low, high;
            narrowRange(n, low, high);
            return v.as.i >= low && v.as.i <= high;
        }
    }
}

// Writes `v`, which fits, narrow at `at`; a float32 is the nearest 32-bit float.
inline void writeNarrow(Narrow n, uint8_t* at, const Value& v) {
    switch (n) {
        case Narrow::Byte: *at = static_cast<uint8_t>(v.as.i); break;
        case Narrow::Int16:
        case Narrow::Uint16: {
            const uint16_t bits = static_cast<uint16_t>(v.as.i);
            std::memcpy(at, &bits, 2);
            break;
        }
        case Narrow::Float32: {
            const float f = static_cast<float>(v.as.d);
            std::memcpy(at, &f, 4);
            break;
        }
        default: {
            const uint32_t bits = static_cast<uint32_t>(v.as.i);
            std::memcpy(at, &bits, 4);
            break;
        }
    }
}

// How a list keeps its items (M6.6 part 3, value-lists.html section 5): as 16-byte values, as
// today, for a list of objects, of `any` or of a type a value must say itself, and for a list
// made with nothing said of what it holds; or packed one after another at their own widths,
// for a list whose items are values: List<int>, List<float>, List<bool>, a value type's.
enum class ListStore : uint8_t {
    Values,  // `items`
    Ints,    // `block`, 8 bytes an item
    Floats,  // 8 bytes an item
    Bools,   // 1 byte an item
    Narrow,  // PackedItems::narrow, 1, 2 or 4 bytes an item: List<byte>, List<char>, List<int16>...
    Items,   // a value type's, in its layout, `width` bytes an item
};

// The items of a packed list.
struct PackedItems {
    ListStore store = ListStore::Values;
    bool nullable = false;          // with a bit per item for null (List<int?>, List<Point?>)
    uint32_t width = 0;             // the bytes an item takes
    size_t count = 0;               // the items
    ObjClass* itemClass = nullptr;  // ListStore::Items: the value type
    Narrow narrow = Narrow::None;   // ListStore::Narrow: what each item is
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

// A set (M6.6 part 8): each value once, in the order first added, two values the same one
// when a map would take them as one key. Or a live, read-only view of a map's keys or of its
// entries (D77): what the map holds as it is, nothing kept of its own.
struct ObjSet final : Obj {
    ValueMap items;              // the set's values, as keys; the values beside them unused
    ObjTypeInfo* type = nullptr; // `Set<string>`, as for a list
    ObjMap* viewOf = nullptr;    // a view: the map it shows
    bool entries = false;        // a view of the map's entries, as MapEntry values, not its keys
    ObjSet() : Obj(ObjType::Set) {}
    bool view() const { return viewOf != nullptr; }
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
    // An end counted from the end of what it is used on, `2..^1` (D146): `start` or `end` is
    // then k of ^k. Such a range is a slice's bounds, resolved by of(size); it has no numbers
    // of its own to loop over.
    const bool startFromEnd, endFromEnd;
    ObjRange(int64_t s, int64_t e, int64_t st, bool incl, bool sFromEnd = false, bool eFromEnd = false)
        : Obj(ObjType::Range), start(s), end(e), step(st), inclusive(incl), startFromEnd(sFromEnd), endFromEnd(eFromEnd) {}
    bool fromEnd() const { return startFromEnd || endFromEnd; }

    int64_t count() const;
    bool contains(int64_t i) const;
};

// A variable captured by a closure. Points into the VM stack while the variable is live
// there, and at its own `closed` slot afterwards.
struct ObjUpvalue final : Obj {
    Value* location;
    Value closed;
    ObjUpvalue* nextOpen = nullptr;
    // While it is open and points into the stack of a task: that task, which has to outlive it.
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
    // (`text.contains("a", ignoreCase = true)`); empty for the rest. The longest form's, and
    // every form's, for a method declared in several (`add(item)`, `add(index, item)`).
    std::vector<std::string> params;
    std::vector<std::vector<std::string>> forms;
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

    // Where an object of the class keeps the field's value, set when the first one is made
    // (VM::layFields, M6.6 part 6): in a slot of its own, a value; or, for an int, a float or a
    // bool whose type is written and is not null, as its 8-byte payload alone in the object's
    // inline part. A val, and a field its init must set, has a bit saying whether it has been
    // given a value yet (`tracked`); any other payload reads as 0, 0.0 or false until it is.
    // A byte, a char or a storage width is kept narrow (D148), `narrow`, after the 8-byte ones.
    enum PayloadKind : uint8_t { NoPayload, IntPayload, FloatPayload, BoolPayload, NarrowPayload };
    Narrow narrow = Narrow::None;
    int32_t slot = -1;
    int32_t payloadOffset = -1;
    uint16_t payloadBit = 0;
    PayloadKind payload = NoPayload;
    bool tracked = false;
    // The kind of value its payload is (ValueKind's Bool, Int and Float are 1, 2 and 3).
    uint8_t payloadValueKind() const { return payload == IntPayload ? 2 : payload == FloatPayload ? 3 : 1; }
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
        Narrow,  // a byte, a char or a storage width, `narrow`, at 1, 2 or 4 bytes (D148)
    };
    ObjString* name = nullptr;
    uint32_t offset = 0;
    Kind kind = Any;
    astra::Narrow narrow = astra::Narrow::None;  // Narrow: what it keeps
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
    // Keys (D26): whether `==` or `hashCode` is among the methods, 0 until asked, then 1 for
    // neither and 2 for them; and whether an object of the class can change in a way they see
    // (a data class with a var, an == reading one), which makes it no key. The checker works
    // that out and the compiler says so (Op::UnstableKey).
    uint8_t ownKeys = 0;
    bool unstableKey = false;
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

    // What reflection reads of the class (D38): the nested functions describing its fields (0)
    // and its methods and constructors (1), given by Op::ClassMeta, each made a function the
    // first time it is asked for. Null where it has none, as in a built program that does not
    // reflect on it (D50).
    const FunctionProto* metaProto[2] = {nullptr, nullptr};
    ObjClosure* meta[2] = {nullptr, nullptr};
    // What each gave, by the type of the class it was asked for (null for the class bare), in
    // VM::classDescription.
    std::unordered_map<const ObjTypeInfo*, Value> described[2];
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
    // How many classes had been finished when the layout was last tried and a field's value
    // type did not exist yet (G16): it is tried again once another class is.
    std::atomic<uint64_t> inlineTriedAt{~uint64_t{0}};

    // Where its objects keep their fields (M6.6 part 6, FieldDecl::slot): how many slots of
    // values, and how many bytes of payloads, with their bits, begin the inline part.
    std::atomic<bool> fieldsLaid{false};
    uint32_t slotCount = 0;
    uint32_t payloadBytes = 0;
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
    // `extend Person : Weighed` (D33): the interfaces an extend fitted this class to, with the
    // ones above them; and what came with them: the interfaces' own methods and getters, and
    // the extend's getters bridging names. Found from the class and the classes below it.
    std::vector<ObjClass*> extendedTo;
    Table extendMethods;
    Table extendGetters;
    std::vector<std::pair<ObjString*, ObjClass*>> required;

    explicit ObjClass(ObjString* n) : Obj(ObjType::Class), name(n) {}

    bool isSubclassOf(const ObjClass* other) const {
        for (const ObjClass* k = this; k != nullptr; k = k->super) {
            if (k == other) return true;
            for (const ObjClass* fitted : k->extendedTo)  // by an extend (D33)
                if (fitted == other) return true;
        }
        if (other != nullptr && other->isInterface)
            for (const ObjClass* implemented : interfaces)
                if (implemented == other) return true;
        return false;
    }

    // A method or getter an extend brought to this class or one above it (D33).
    const Value* extendMethod(const ObjString* key) const {
        for (const ObjClass* k = this; k != nullptr; k = k->super)
            if (const Value* v = k->extendMethods.find(key)) return v;
        return nullptr;
    }
    const Value* extendGetter(const ObjString* key) const {
        for (const ObjClass* k = this; k != nullptr; k = k->super)
            if (const Value* v = k->extendGetters.find(key)) return v;
        return nullptr;
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
    // Where its slots are, from the object's start: after its payloads (M6.6 part 6), which come
    // first so that each is at the same place in every object of its class.
    uint16_t slotsStart;

    ObjInstance(ObjClass* c, uint32_t count, uint16_t captures, bool withType, uint32_t inlineSlots, bool withValues,
                uint32_t payloadBytes)
        : Obj(ObjType::Instance), cls(c), shape(c->shape), fieldCount(count), captureCount(captures), typed(withType),
          inlined(withValues), slotsStart(static_cast<uint16_t>(sizeof(ObjInstance) + payloadBytes)) {
        if (payloadBytes != 0) std::memset(payloadData(), 0, payloadBytes);
        Value* at = fields();
        for (uint32_t i = 0; i < count + captures + (withType ? 1u : 0u); i++) new (at + i) Value(Value::undefined());
        if (inlineSlots != 0) std::memset(inlineData(), 0, inlineSlots * sizeof(Value));
    }

    // Its payloads, and the bits saying which have a value, right after the object's header.
    uint8_t* payloadData() { return reinterpret_cast<uint8_t*>(this + 1); }
    const uint8_t* payloadData() const { return reinterpret_cast<const uint8_t*>(this + 1); }
    // The bytes of its fields of value types, after its slots (M6.6 part 5).
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

    // `count` slots of values, and `bytes` of payloads (a multiple of 8).
    static void* operator new(size_t size, uint32_t count, uint32_t bytes) { return ::operator new(size + count * sizeof(Value) + bytes); }
    static void operator delete(void* p, uint32_t, uint32_t) { ::operator delete(p); }
    static void operator delete(void* p) { ::operator delete(p); }

    Value* fields() { return reinterpret_cast<Value*>(reinterpret_cast<uint8_t*>(this) + slotsStart); }
    const Value* fields() const { return reinterpret_cast<const Value*>(reinterpret_cast<const uint8_t*>(this) + slotsStart); }

    // How the field called `name` is kept in this object, or null if it has none of that name
    // (one declared after the object was made).
    const FieldDecl* declOf(const ObjString* name) const {
        const Value* at = cls->declaredIndex.find(name);
        if (at == nullptr) return nullptr;
        const FieldDecl& d = cls->declared[static_cast<size_t>(at->as.i)];
        if (d.payload == FieldDecl::NoPayload && (d.slot < 0 || static_cast<uint32_t>(d.slot) >= fieldCount)) return nullptr;
        return &d;
    }
    // Which slot the field called `name` is in, or -1 if the object has none of that name or
    // keeps it as a payload.
    int64_t fieldIndex(const ObjString* name) const {
        const FieldDecl* d = declOf(name);
        return d != nullptr && d->payload == FieldDecl::NoPayload ? d->slot : -1;
    }
    // The slot holding the field's value, or null if the object has no such field, it has no
    // value yet, or it is kept as a payload (read()).
    const Value* field(const ObjString* name) const {
        const int64_t i = fieldIndex(name);
        if (i < 0 || fields()[i].kind == ValueKind::Undefined) return nullptr;
        return fields() + i;
    }

    // A field kept as its payload alone (M6.6 part 6).
    bool payloadSet(const FieldDecl& d) const { return (payloadData()[d.payloadBit / 8] >> (d.payloadBit % 8) & 1) != 0; }
    Value payloadOf(const FieldDecl& d) const {
        const uint8_t* at = payloadData() + d.payloadOffset;
        switch (d.payload) {
            case FieldDecl::IntPayload: {
                int64_t i;
                std::memcpy(&i, at, sizeof i);
                return Value::integer(i);
            }
            case FieldDecl::FloatPayload: {
                double x;
                std::memcpy(&x, at, sizeof x);
                return Value::number(x);
            }
            case FieldDecl::BoolPayload: return Value::boolean(*at != 0);
            case FieldDecl::NarrowPayload: return readNarrow(d.narrow, at);
            default: return Value::null();
        }
    }
    // Its kind already checked.
    void setPayload(const FieldDecl& d, Value value) {
        if (d.payload == FieldDecl::NarrowPayload) {
            if (d.narrow == Narrow::Byte && value.isInt()) value = Value::byte(static_cast<uint8_t>(value.as.i));
            writeNarrow(d.narrow, payloadData() + d.payloadOffset, value);
            if (d.tracked) payloadData()[d.payloadBit / 8] |= static_cast<uint8_t>(1u << (d.payloadBit % 8));
            return;
        }
        // A bool as a whole 0 or 1, so that every payload is a value's 8 bytes of contents.
        const int64_t bits = d.payload == FieldDecl::BoolPayload ? (value.as.b ? 1 : 0) : value.as.i;
        std::memcpy(payloadData() + d.payloadOffset, &bits, 8);
        if (d.tracked) payloadData()[d.payloadBit / 8] |= static_cast<uint8_t>(1u << (d.payloadBit % 8));
    }

    // The field's value, however it is kept; false if the object has no such field or it has
    // no value yet.
    bool read(const ObjString* name, Value& out) const {
        const FieldDecl* d = declOf(name);
        if (d == nullptr) return false;
        if (d->payload != FieldDecl::NoPayload) {
            out = payloadOf(*d);  // 0, 0.0 or false until it is given a value
            return true;
        }
        const Value& slot = fields()[d->slot];
        if (slot.kind == ValueKind::Undefined) return false;
        out = slot;
        return true;
    }
    bool hasValue(const ObjString* name) const {
        const FieldDecl* d = declOf(name);
        if (d != nullptr && d->payload != FieldDecl::NoPayload) return !d->tracked || payloadSet(*d);
        Value ignored;
        return read(name, ignored);
    }
    // Gives the field a value, of its kind where it is a payload; false if the object has no
    // field of that name.
    bool setField(const ObjString* name, Value value) {
        const FieldDecl* d = declOf(name);
        if (d == nullptr) return false;
        if (d->payload != FieldDecl::NoPayload) setPayload(*d, value);
        else fields()[d->slot] = value;
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
inline ObjSet* asSet(const Value& v) { return static_cast<ObjSet*>(v.as.obj); }
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
