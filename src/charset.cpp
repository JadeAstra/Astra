// Character sets (D127): text turned into bytes and back in UTF-8, Latin-1, ASCII and UTF-16.
// Astra's text is UTF-8 throughout; the other sets are for what is read from and written to
// files, processes and sockets that use them, and for the conversions between text and bytes.

#include "charset.h"

#include <cstring>

#include "vm.h"

namespace astra {

namespace {

// UTF-16 units, a byte at a time in the order the set says.
void putUnit16(std::string& out, uint32_t unit, bool bigEndian) {
    const char low = static_cast<char>(unit & 0xFF), high = static_cast<char>(unit >> 8);
    if (bigEndian) {
        out += high;
        out += low;
    } else {
        out += low;
        out += high;
    }
}

uint32_t unit16(std::string_view bytes, size_t at, bool bigEndian) {
    const auto b0 = static_cast<unsigned char>(bytes[at]), b1 = static_cast<unsigned char>(bytes[at + 1]);
    return bigEndian ? (b0 << 8) | b1 : (b1 << 8) | b0;
}

}  // namespace

std::string encodeText(std::string_view utf8, Charset cs) {
    if (cs == Charset::Utf8) return std::string(utf8);
    const Characters text = Characters::fromUtf8(std::string(utf8));
    std::string out;
    out.reserve(text.size() * (cs == Charset::Utf16Le || cs == Charset::Utf16Be ? 2 : 1));
    for (size_t i = 0; i < text.size(); i++) {
        const uint32_t c = text[i];
        switch (cs) {
            case Charset::Latin1: out += c < 0x100 ? static_cast<char>(c) : '?'; break;
            case Charset::Ascii: out += c < 0x80 ? static_cast<char>(c) : '?'; break;
            default: {
                const bool big = cs == Charset::Utf16Be;
                if (c < 0x10000) {
                    putUnit16(out, c, big);
                } else {
                    putUnit16(out, 0xD800 + ((c - 0x10000) >> 10), big);
                    putUnit16(out, 0xDC00 + ((c - 0x10000) & 0x3FF), big);
                }
            }
        }
    }
    return out;
}

std::string decodeText(std::string_view bytes, Charset cs) {
    if (cs == Charset::Utf8) return std::string(bytes);
    std::string out;
    out.reserve(bytes.size());
    if (cs == Charset::Latin1 || cs == Charset::Ascii) {
        for (const char b : bytes) {
            const auto c = static_cast<unsigned char>(b);
            appendUtf8(out, cs == Charset::Ascii && c >= 0x80 ? 0xFFFD : c);
        }
        return out;
    }
    const bool big = cs == Charset::Utf16Be;
    size_t at = 0;
    while (at + 1 < bytes.size()) {
        const uint32_t unit = unit16(bytes, at, big);
        at += 2;
        if (unit >= 0xD800 && unit <= 0xDBFF && at + 1 < bytes.size()) {
            const uint32_t next = unit16(bytes, at, big);
            if (next >= 0xDC00 && next <= 0xDFFF) {
                appendUtf8(out, 0x10000 + ((unit - 0xD800) << 10) + (next - 0xDC00));
                at += 2;
                continue;
            }
        }
        // A surrogate on its own is no character.
        appendUtf8(out, unit >= 0xD800 && unit <= 0xDFFF ? 0xFFFD : unit);
    }
    if (at < bytes.size()) appendUtf8(out, 0xFFFD);  // half a unit at the end
    return out;
}

size_t lineEnd(std::string_view bytes, size_t from, Charset cs, size_t& endSize) {
    if (cs != Charset::Utf16Le && cs != Charset::Utf16Be) {
        endSize = 1;
        return bytes.find('\n', from);
    }
    // UTF-16: a whole unit that is a line end, the units counted from `from`.
    endSize = 2;
    const bool big = cs == Charset::Utf16Be;
    for (size_t at = from; at + 1 < bytes.size(); at += 2)
        if (unit16(bytes, at, big) == '\n') return at;
    return std::string_view::npos;
}

std::string withoutCarriageReturn(std::string line, Charset cs) {
    if (cs == Charset::Utf16Le || cs == Charset::Utf16Be) {
        if (line.size() >= 2 && unit16(line, line.size() - 2, cs == Charset::Utf16Be) == '\r') line.resize(line.size() - 2);
    } else if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

Charset charsetOf(VM& vm, Value v) {
    if (v.isNull()) return Charset::Utf8;
    if (v.isObjType(ObjType::Instance)) {
        const ObjInstance* entry = asInstance(v);
        if (entry->cls->isEnum && entry->cls->name->utf8() == "Charset")
            if (const Value* ordinal = entry->field(vm.names.entryOrdinal); ordinal != nullptr && ordinal->isInt() && ordinal->as.i >= 0 && ordinal->as.i <= 4)
                return static_cast<Charset>(ordinal->as.i);
    }
    throw vm.runtimeError(Thrown::ClassCastException, "expected a Charset (Charset.utf8, latin1, ascii, utf16le or utf16be), got a value of type " + vm.typeName(v));
}

}  // namespace astra
