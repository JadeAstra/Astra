#include "value.h"

#include <mutex>
#include <new>

#include <charconv>
#include <cmath>
#include <cstring>
#include <functional>

namespace astra {

std::atomic<ObjClass*> smallClasses[kMaxSmallClasses];

uint32_t newShape() {
    static std::atomic<uint32_t> next{1};
    uint32_t shape;
    do shape = next.fetch_add(1, std::memory_order_relaxed);
    while (shape == 0);  // 0 is what a place that has found nothing yet remembers
    return shape;
}

uint32_t hashString(std::string_view s) {
    uint32_t hash = 2166136261u;  // FNV-1a
    for (unsigned char c : s) {
        hash ^= c;
        hash *= 16777619u;
    }
    return hash;
}

bool valueBytesEqual(const ValueLayout& layout, const uint8_t* x, const uint8_t* y, int depth);

bool valuesEqual(const Value& a, const Value& b) {
    if (a.kind == b.kind) {
        switch (a.kind) {
            case ValueKind::Null:
            case ValueKind::Undefined:
                return true;
            case ValueKind::Bool:
                return a.as.b == b.as.b;
            case ValueKind::Int:
                return a.as.i == b.as.i;
            case ValueKind::Float:
                return a.as.d == b.as.d;
            case ValueKind::Small: {
                // Two values of one small value type, field by field (M6.6 part 5).
                if (a.refIndex != b.refIndex) return false;
                const ObjClass* cls = smallClassOf(a);
                return valueBytesEqual(*cls->layout.load(std::memory_order_acquire), smallBytes(a), smallBytes(b), 0);
            }
            case ValueKind::Obj:
                if (a.as.obj == b.as.obj) return true;
                if (a.as.obj->type == ObjType::String && b.as.obj->type == ObjType::String) {
                    const ObjString* x = asString(a);
                    const ObjString* y = asString(b);
                    return x->hash == y->hash && x->chars == y->chars;
                }
                return false;
        }
        return false;
    }

    if (a.kind == ValueKind::Int && b.kind == ValueKind::Float) return static_cast<double>(a.as.i) == b.as.d;
    if (a.kind == ValueKind::Float && b.kind == ValueKind::Int) return a.as.d == static_cast<double>(b.as.i);
    return false;
}

size_t hashValue(const Value& v) {
    switch (v.kind) {
        case ValueKind::Bool:
            return v.as.b ? 11 : 7;
        case ValueKind::Int:
            return std::hash<int64_t>()(v.as.i);
        case ValueKind::Float: {
            // Integral floats must hash like the equal int so 1 and 1.0 are the same map key.
            double d = v.as.d;
            if (d >= -9.2e18 && d <= 9.2e18 && std::floor(d) == d)
                return std::hash<int64_t>()(static_cast<int64_t>(d));
            return std::hash<double>()(d);
        }
        case ValueKind::Obj:
            if (v.as.obj->type == ObjType::String) return asString(v)->hash;
            return std::hash<const Obj*>()(v.as.obj);
        case ValueKind::Small: return keyHash(v, 0);
        default:
            return 0;
    }
}

namespace {
const int kMaxKeyDepth = 32;

// The objects whose contents make them what they are as a key.
bool hasContentKey(const Value& v) {
    if (!v.isObj()) return false;
    switch (v.as.obj->type) {
        case ObjType::List:
        case ObjType::Range:
        case ObjType::Bytes: return true;
        case ObjType::Instance: return asInstance(v)->cls->isData;
        case ObjType::Value: return true;
        default: return false;
    }
}

Value objectAt(const uint8_t* at) {
    Obj* obj;
    std::memcpy(&obj, at, sizeof obj);
    return obj != nullptr ? Value::object(obj) : Value::null();
}

Value valueAt(const uint8_t* at) {
    Value v;
    std::memcpy(&v, at, sizeof v);
    return v;
}

double floatAt(const uint8_t* at) {
    double d;
    std::memcpy(&d, at, sizeof d);
    return d;
}
}  // namespace

bool keysEqual(const Value& a, const Value& b, int depth);
size_t keyHash(const Value& v, int depth);

// The fields of two values of one value type, compared as keys are, from their bytes in the
// type's layout (M6.6 part 2); a value held in place in another, field by field in turn.
bool valueBytesEqual(const ValueLayout& layout, const uint8_t* x, const uint8_t* y, int depth) {
    for (const ValueField& field : layout.fields) {
        const uint8_t* p = x + field.offset;
        const uint8_t* q = y + field.offset;
        switch (field.kind) {
            case ValueField::Int:
                if (std::memcmp(p, q, 8) != 0) return false;
                break;
            case ValueField::Bool:
                if (*p != *q) return false;
                break;
            case ValueField::Float:
                if (!(floatAt(p) == floatAt(q))) return false;
                break;
            case ValueField::Object:
                if (!keysEqual(objectAt(p), objectAt(q), depth + 1)) return false;
                break;
            case ValueField::Any:
                if (!keysEqual(valueAt(p), valueAt(q), depth + 1)) return false;
                break;
            case ValueField::Nested:
                if (!valueBytesEqual(*field.cls->layout.load(std::memory_order_acquire), p, q, depth + 1)) return false;
                break;
        }
    }
    return true;
}

namespace {
size_t mixHash(size_t seed, size_t next) { return seed ^ (next + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2)); }

size_t valueBytesHash(const ValueLayout& layout, const uint8_t* x, int depth) {
    size_t h = 23;
    for (const ValueField& field : layout.fields) {
        const uint8_t* p = x + field.offset;
        switch (field.kind) {
            case ValueField::Int: {
                int64_t i;
                std::memcpy(&i, p, sizeof i);
                h = mixHash(h, hashValue(Value::integer(i)));
                break;
            }
            case ValueField::Bool: h = mixHash(h, *p ? 11 : 7); break;
            case ValueField::Float: h = mixHash(h, hashValue(Value::number(floatAt(p) == 0.0 ? 0.0 : floatAt(p)))); break;
            case ValueField::Object: h = mixHash(h, keyHash(objectAt(p), depth + 1)); break;
            case ValueField::Any: h = mixHash(h, keyHash(valueAt(p), depth + 1)); break;
            case ValueField::Nested:
                h = mixHash(h, valueBytesHash(*field.cls->layout.load(std::memory_order_acquire), p, depth + 1));
                break;
        }
    }
    return h;
}
}  // namespace

namespace {
// An item of a list as a key sees it (M6.6 part 3): its value, or, for a value type's item,
// where its bytes are, whether the list keeps it packed or in a box.
struct KeyItem {
    Value value;
    const uint8_t* bytes = nullptr;
    const ObjClass* cls = nullptr;
};

KeyItem keyItem(const ObjList* list, size_t i) {
    if (!list->packed()) {
        const Value v = list->values()[i];
        if (v.isObjType(ObjType::Value)) return KeyItem{v, asValue(v)->data(), asValue(v)->cls};
        return KeyItem{v};
    }
    if (list->pack->nullable && (list->pack->nulls[i / 64] >> (i % 64) & 1) != 0) return KeyItem{Value::null()};
    const uint8_t* at = list->pack->block.data() + i * list->pack->width;
    switch (list->pack->store) {
        case ListStore::Ints: {
            int64_t v;
            std::memcpy(&v, at, sizeof v);
            return KeyItem{Value::integer(v)};
        }
        case ListStore::Floats: return KeyItem{Value::number(floatAt(at))};
        case ListStore::Bools: return KeyItem{Value::boolean(*at != 0)};
        default: return KeyItem{Value::null(), at, list->pack->itemClass};
    }
}

// A value of a value type in any of its forms: a box, one kept in the value itself (whose
// bytes are in `v`, which has to outlive them), or a Ref; its class and where its bytes are.
bool valueForm(const Value& v, const ObjClass*& cls, const uint8_t*& bytes) {
    if (v.kind == ValueKind::Small) {
        cls = smallClassOf(v);
        bytes = smallBytes(v);
        return true;
    }
    if (v.kind == ValueKind::Ref) {
        bytes = refBytes(v);
        if (bytes == nullptr) return false;
        cls = refClass(v);
        return true;
    }
    if (v.isObjType(ObjType::Value)) {
        cls = asValue(v)->cls;
        bytes = asValue(v)->data();
        return true;
    }
    return false;
}

bool keyItemsEqual(const KeyItem& a, const KeyItem& b, int depth) {
    const ObjClass* ca = a.cls;
    const ObjClass* cb = b.cls;
    const uint8_t* pa = a.bytes;
    const uint8_t* pb = b.bytes;
    if (ca == nullptr && a.value.isSmall()) valueForm(a.value, ca, pa);
    if (cb == nullptr && b.value.isSmall()) valueForm(b.value, cb, pb);
    if (ca != nullptr || cb != nullptr)
        return ca == cb && valueBytesEqual(*ca->layout.load(std::memory_order_acquire), pa, pb, depth + 1);
    return keysEqual(a.value, b.value, depth + 1);
}

size_t keyItemHash(const KeyItem& a, int depth) {
    if (a.cls != nullptr)
        return mixHash(std::hash<const void*>()(a.cls), valueBytesHash(*a.cls->layout.load(std::memory_order_acquire), a.bytes, depth));
    return keyHash(a.value, depth + 1);
}
}  // namespace

// Whether the running thread may look into an object: always, except in a parallel section
// for one that another core owns.
static bool readableHere(const Value& v) { return v.as.obj->core == 0 || v.as.obj->core == currentCore; }

bool keysEqual(const Value& a, const Value& b, int depth) {
    // Values of value types, whatever form each is in (M6.6 part 5).
    if ((a.isSmall() || b.isSmall() || a.isRef() || b.isRef()) && depth < kMaxKeyDepth) {
        const ObjClass *ca = nullptr, *cb = nullptr;
        const uint8_t *pa = nullptr, *pb = nullptr;
        if (!valueForm(a, ca, pa) || !valueForm(b, cb, pb)) return false;
        return ca == cb && valueBytesEqual(*ca->layout.load(std::memory_order_acquire), pa, pb, depth);
    }
    if (!hasContentKey(a) || !hasContentKey(b) || depth >= kMaxKeyDepth) return valuesEqual(a, b);
    // What another core is working on is a key as itself only: its contents are not looked at.
    if (!readableHere(a) || !readableHere(b)) return valuesEqual(a, b);
    if (a.as.obj == b.as.obj) return true;
    if (a.as.obj->type != b.as.obj->type) return false;
    switch (a.as.obj->type) {
        case ObjType::List: {
            const ObjList* x = asList(a);
            const ObjList* y = asList(b);
            if (x->size() != y->size()) return false;
            for (size_t i = 0; i < x->size(); i++)
                if (!keyItemsEqual(keyItem(x, i), keyItem(y, i), depth)) return false;
            return true;
        }
        case ObjType::Range: {
            const ObjRange* x = asRange(a);
            const ObjRange* y = asRange(b);
            return x->start == y->start && x->end == y->end && x->step == y->step && x->inclusive == y->inclusive;
        }
        case ObjType::Bytes:
            return asBytes(a)->data == asBytes(b)->data;
        case ObjType::Value: {
            const ObjValue* x = asValue(a);
            const ObjValue* y = asValue(b);
            return x->cls == y->cls && valueBytesEqual(*x->cls->layout.load(std::memory_order_acquire), x->data(), y->data(), depth);
        }
        case ObjType::Instance: {
            const ObjInstance* x = asInstance(a);
            const ObjInstance* y = asInstance(b);
            if (x->cls != y->cls) return false;
            for (const ObjString* part : x->cls->components) {
                const Value* left = x->field(part);
                const Value* right = y->field(part);
                if (!keysEqual(left != nullptr ? *left : Value::null(), right != nullptr ? *right : Value::null(), depth + 1))
                    return false;
            }
            return true;
        }
        default:
            return false;
    }
}

size_t keyHash(const Value& v, int depth) {
    if (v.isSmall() || v.isRef()) {
        const ObjClass* cls = nullptr;
        const uint8_t* bytes = nullptr;
        if (!valueForm(v, cls, bytes)) return 0;
        return mixHash(std::hash<const void*>()(cls), valueBytesHash(*cls->layout.load(std::memory_order_acquire), bytes, depth));
    }
    if (!hasContentKey(v) || depth >= kMaxKeyDepth || !readableHere(v)) return hashValue(v);
    auto mix = [](size_t seed, size_t next) { return seed ^ (next + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2)); };
    switch (v.as.obj->type) {
        case ObjType::List: {
            size_t h = 17;
            const ObjList* list = asList(v);
            for (size_t i = 0; i < list->size(); i++) h = mix(h, keyItemHash(keyItem(list, i), depth));
            return h;
        }
        case ObjType::Range: {
            const ObjRange* r = asRange(v);
            return mix(mix(std::hash<int64_t>()(r->start), std::hash<int64_t>()(r->end)), std::hash<int64_t>()(r->step) + (r->inclusive ? 1 : 0));
        }
        case ObjType::Bytes: {
            size_t h = 31;
            for (uint8_t b : asBytes(v)->data) h = h * 131 + b;
            return h;
        }
        case ObjType::Value: {
            const ObjValue* box = asValue(v);
            return mix(std::hash<const void*>()(box->cls), valueBytesHash(*box->cls->layout.load(std::memory_order_acquire), box->data(), depth));
        }
        case ObjType::Instance: {
            const ObjInstance* instance = asInstance(v);
            size_t h = std::hash<const void*>()(instance->cls);
            for (const ObjString* part : instance->cls->components) {
                const Value* field = instance->field(part);
                h = mix(h, field != nullptr ? keyHash(*field, depth + 1) : 0);
            }
            return h;
        }
        default:
            return hashValue(v);
    }
}

std::string formatFloat(double d) {
    if (std::isnan(d)) return "nan";
    if (std::isinf(d)) return d < 0 ? "-inf" : "inf";
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof buf, d);
    std::string s(buf, res.ptr);
    if (s.find_first_of(".e") == std::string::npos) s += ".0";
    return s;
}

std::string quoteString(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            case '\0': out += "\\0"; break;
            default: out += c; break;
        }
    }
    out += '"';
    return out;
}

// ---- Table ----

void Table::set(ObjString* key, Value value) {
    if ((count_ + 1) * 4 > capacity_ * 3) grow();

    uint32_t mask = capacity_ - 1;
    uint32_t i = key->hash & mask;
    for (;;) {
        Entry& e = entries_[i];
        if (e.key == key) {
            e.value = value;
            return;
        }
        if (e.key == nullptr) {
            e.key = key;
            e.value = value;
            count_++;
            return;
        }
        i = (i + 1) & mask;
    }
}

void Table::grow() {
    uint32_t oldCapacity = capacity_;
    Entry* old = entries_;

    // The new entries first: if there is no memory for them, the table is left as it was.
    Entry* fresh = new Entry[oldCapacity == 0 ? 4 : oldCapacity * 2];
    capacity_ = oldCapacity == 0 ? 4 : oldCapacity * 2;
    entries_ = fresh;
    count_ = 0;

    for (uint32_t i = 0; i < oldCapacity; i++)
        if (old[i].key != nullptr) set(old[i].key, old[i].value);
    delete[] old;
}

void Table::addAll(const Table& other) {
    for (uint32_t i = 0; i < other.capacity_; i++)
        if (other.entries_[i].key != nullptr) set(other.entries_[i].key, other.entries_[i].value);
}

void Table::addMissing(const Table& other) {
    for (uint32_t i = 0; i < other.capacity_; i++)
        if (other.entries_[i].key != nullptr && find(other.entries_[i].key) == nullptr)
            set(other.entries_[i].key, other.entries_[i].value);
}

size_t countCharacters(std::string_view s) {
    size_t count = 0;
    for (unsigned char c : s) count += (c & 0xC0) != 0x80;  // every byte that is not a continuation
    return count;
}

thread_local uint8_t currentCore = 0;

void ObjString::index() const {
    if (indexed_.load(std::memory_order_acquire)) return;
    // A string is shared between cores, and two may ask for a position in it at the same
    // moment: one of them makes the table and the other waits for it.
    static std::mutex making;
    std::lock_guard<std::mutex> hold(making);
    if (indexed_.load(std::memory_order_relaxed)) return;
    std::vector<uint32_t> starts;
    starts.reserve(length + 1);
    for (size_t i = 0; i < chars.size(); i++)
        if ((static_cast<unsigned char>(chars[i]) & 0xC0) != 0x80) starts.push_back(static_cast<uint32_t>(i));
    starts.push_back(static_cast<uint32_t>(chars.size()));
    starts_ = std::move(starts);
    indexed_.store(true, std::memory_order_release);
}

size_t ObjString::byteAt(size_t at) const {
    if (isAscii()) return at;
    index();
    return starts_[at < starts_.size() ? at : starts_.size() - 1];
}

size_t ObjString::charAt(size_t offset) const {
    if (isAscii()) return offset;
    index();
    return static_cast<size_t>(std::lower_bound(starts_.begin(), starts_.end(), static_cast<uint32_t>(offset)) - starts_.begin());
}

// ---- ValueMap ----

const Value* ValueMap::find(const Value& key) const {
    auto it = index_.find(key);
    return it == index_.end() ? nullptr : &entries_[it->second].value;
}

void ValueMap::set(const Value& key, const Value& value) {
    auto it = index_.find(key);
    if (it != index_.end()) {
        entries_[it->second].value = value;
        return;
    }
    // The entry first, then its place in the index; if the index cannot grow, the entry goes
    // again, so that the two never disagree.
    entries_.push_back(Entry{key, value, true});
    try {
        index_.emplace(key, static_cast<uint32_t>(entries_.size() - 1));
    } catch (...) {
        entries_.pop_back();
        throw;
    }
}

bool ValueMap::remove(const Value& key) {
    auto it = index_.find(key);
    if (it == index_.end()) return false;

    entries_[it->second] = Entry{};
    index_.erase(it);

    // Squeeze out removed entries once they outnumber the live ones. Built aside and then
    // swapped in; without the memory to build it, the map simply stays as it is.
    if (entries_.size() > 32 && entries_.size() > index_.size() * 2) {
        try {
            std::vector<Entry> compacted;
            compacted.reserve(index_.size());
            decltype(index_) rebuilt;
            rebuilt.reserve(index_.size());
            for (const Entry& e : entries_) {
                if (!e.live) continue;
                rebuilt.emplace(e.key, static_cast<uint32_t>(compacted.size()));
                compacted.push_back(e);
            }
            entries_ = std::move(compacted);
            index_ = std::move(rebuilt);
        } catch (const std::bad_alloc&) {
        }
    }
    return true;
}

void ValueMap::clear() {
    entries_.clear();
    index_.clear();
}

// ---- ObjRange ----

int64_t ObjRange::count() const { return rangeCount(start, end, step, inclusive); }

int64_t rangeCount(int64_t start, int64_t end, int64_t step, bool inclusive) {
    // Unsigned arithmetic: the span of a range can exceed INT64_MAX.
    uint64_t span;
    if (step > 0) {
        if (end < start) return 0;
        span = static_cast<uint64_t>(end) - static_cast<uint64_t>(start);
    } else {
        if (end > start) return 0;
        span = static_cast<uint64_t>(start) - static_cast<uint64_t>(end);
    }
    uint64_t magnitude = step > 0 ? static_cast<uint64_t>(step) : 0 - static_cast<uint64_t>(step);
    uint64_t n = inclusive ? span / magnitude + 1 : (span + magnitude - 1) / magnitude;
    return n > static_cast<uint64_t>(INT64_MAX) ? INT64_MAX : static_cast<int64_t>(n);
}

bool ObjRange::contains(int64_t i) const {
    if (step > 0) {
        if (i < start || (inclusive ? i > end : i >= end)) return false;
    } else {
        if (i > start || (inclusive ? i < end : i <= end)) return false;
    }
    uint64_t offset = step > 0 ? static_cast<uint64_t>(i) - static_cast<uint64_t>(start)
                               : static_cast<uint64_t>(start) - static_cast<uint64_t>(i);
    uint64_t magnitude = step > 0 ? static_cast<uint64_t>(step) : 0 - static_cast<uint64_t>(step);
    return offset % magnitude == 0;
}

}  // namespace astra
