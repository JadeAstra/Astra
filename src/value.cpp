#include "value.h"

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

    capacity_ = oldCapacity == 0 ? 4 : oldCapacity * 2;
    entries_ = new Entry[capacity_];
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
    index_.emplace(key, static_cast<uint32_t>(entries_.size()));
    entries_.push_back(Entry{key, value, true});
}

bool ValueMap::remove(const Value& key) {
    auto it = index_.find(key);
    if (it == index_.end()) return false;

    entries_[it->second] = Entry{};
    index_.erase(it);

    // Squeeze out removed entries once they outnumber the live ones.
    if (entries_.size() > 32 && entries_.size() > index_.size() * 2) {
        std::vector<Entry> compacted;
        compacted.reserve(index_.size());
        index_.clear();
        for (const Entry& e : entries_) {
            if (!e.live) continue;
            index_.emplace(e.key, static_cast<uint32_t>(compacted.size()));
            compacted.push_back(e);
        }
        entries_ = std::move(compacted);
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
