#include "value.h"

#include <mutex>
#include <new>

#include <charconv>
#include <cmath>
#include <functional>

namespace astra {

uint32_t hashString(std::string_view s) {
    uint32_t hash = 2166136261u;  // FNV-1a
    for (unsigned char c : s) {
        hash ^= c;
        hash *= 16777619u;
    }
    return hash;
}

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
        default: return false;
    }
}
}  // namespace

// Whether the running thread may look into an object: always, except in a parallel section
// for one that another core owns.
static bool readableHere(const Value& v) { return v.as.obj->core == 0 || v.as.obj->core == currentCore; }

bool keysEqual(const Value& a, const Value& b, int depth) {
    if (!hasContentKey(a) || !hasContentKey(b) || depth >= kMaxKeyDepth) return valuesEqual(a, b);
    // What another core is working on is a key as itself only: its contents are not looked at.
    if (!readableHere(a) || !readableHere(b)) return valuesEqual(a, b);
    if (a.as.obj == b.as.obj) return true;
    if (a.as.obj->type != b.as.obj->type) return false;
    switch (a.as.obj->type) {
        case ObjType::List: {
            const std::vector<Value>& x = asList(a)->items;
            const std::vector<Value>& y = asList(b)->items;
            if (x.size() != y.size()) return false;
            for (size_t i = 0; i < x.size(); i++)
                if (!keysEqual(x[i], y[i], depth + 1)) return false;
            return true;
        }
        case ObjType::Range: {
            const ObjRange* x = asRange(a);
            const ObjRange* y = asRange(b);
            return x->start == y->start && x->end == y->end && x->step == y->step && x->inclusive == y->inclusive;
        }
        case ObjType::Bytes:
            return asBytes(a)->data == asBytes(b)->data;
        case ObjType::Instance: {
            const ObjInstance* x = asInstance(a);
            const ObjInstance* y = asInstance(b);
            if (x->cls != y->cls) return false;
            for (const ObjString* part : x->cls->components) {
                const Value* left = x->fields.find(part);
                const Value* right = y->fields.find(part);
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
    if (!hasContentKey(v) || depth >= kMaxKeyDepth || !readableHere(v)) return hashValue(v);
    auto mix = [](size_t seed, size_t next) { return seed ^ (next + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2)); };
    switch (v.as.obj->type) {
        case ObjType::List: {
            size_t h = 17;
            for (const Value& item : asList(v)->items) h = mix(h, keyHash(item, depth + 1));
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
        case ObjType::Instance: {
            const ObjInstance* instance = asInstance(v);
            size_t h = std::hash<const void*>()(instance->cls);
            for (const ObjString* part : instance->cls->components) {
                const Value* field = instance->fields.find(part);
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

int64_t ObjRange::count() const {
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
