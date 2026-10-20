// The native part of the standard library's Kotlin names: what is called often enough to
// matter, and what Astra code cannot do for itself (reading a file, formatting a number,
// telling a letter from a digit). The rest is written in Astra, in src/std/*.astra.

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>

#include "builtins.h"
#include "vm.h"

namespace astra {

namespace {

namespace fs = std::filesystem;

// ---- Helpers ----

// Whether optional argument `i` was passed. Null counts as left out: it is what a parameter
// skipped on the way to a named one is given.
bool given(int argc, const Value* args, int i) { return i < argc && !args[i].isNull(); }

bool flag(int argc, const Value* args, int i) { return given(argc, args, i) && args[i].truthy(); }

std::string fold(const std::string& s) { return changeCase(s, false); }

// The byte at which character `index` of `s` starts; the end if there are not that many.
size_t offsetOfCharacter(std::string_view s, size_t index) {
    size_t i = 0;
    for (; i < s.size() && index > 0; index--) {
        i++;
        while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) i++;
    }
    return i;
}

int compareNumbers(Value a, Value b) {
    if (a.isInt() && b.isInt()) return (a.as.i > b.as.i) - (a.as.i < b.as.i);
    const double x = a.toDouble(), y = b.toDouble();
    return (x > y) - (x < y);
}

// How two values are ordered: numbers by size, strings by their characters, and null before
// everything else. Negative, zero or positive, as a comparator gives.
int compareValues(VM& vm, Value a, Value b) {
    if (a.isNull() || b.isNull()) return (b.isNull() ? 1 : 0) - (a.isNull() ? 1 : 0);
    if (a.isNumber() && b.isNumber()) return compareNumbers(a, b);
    if (a.isString() && b.isString()) {
        return compareText(asString(a), asString(b));
    }
    if (a.isBool() && b.isBool()) return static_cast<int>(a.as.b) - static_cast<int>(b.as.b);
    // Chars by their numbers; a byte as the int it holds.
    if (a.isChar() && b.isChar()) return (a.as.i > b.as.i) - (a.as.i < b.as.i);
    if (a.isByte()) a = Value::integer(a.as.i);
    if (b.isByte()) b = Value::integer(b.as.i);
    if (a.isNumber() && b.isNumber()) return compareNumbers(a, b);
    throw vm.runtimeError("cannot compare " + vm.typeName(a) + " and " + vm.typeName(b));
}

// An int to a power of 0 or more, an error where it does not fit in an int (D27).
int64_t integerPower(VM& vm, int64_t base, int64_t exponent) {
    int64_t result = 1, b = base;
    bool bOverflowed = false;
    for (int64_t e = exponent; e != 0; e >>= 1) {
        if (e & 1) {
            if (bOverflowed || mulOverflows(result, b, result))
                throw vm.runtimeError(std::to_string(base) + " to the power " + std::to_string(exponent) + " does not fit in an int");
        }
        if (e > 1 && !bOverflowed) bOverflowed = mulOverflows(b, b, b);
    }
    return result;
}

// ---- Strings ----

bool parseWhole(std::string_view s, int radix, int64_t& out) {
    if (!s.empty() && s[0] == '+') s.remove_prefix(1);
    if (s.empty()) return false;
    auto result = std::from_chars(s.data(), s.data() + s.size(), out, radix);
    return result.ec == std::errc() && result.ptr == s.data() + s.size();
}

// An int's 64 bits, for the unsigned helpers (D44).
uint64_t wholeOnly(VM& vm, Value v, const char* what) {
    if (!v.isInt()) throw vm.runtimeError(std::string(what) + " is for ints, not " + vm.typeName(v));
    return static_cast<uint64_t>(v.as.i);
}

int radixOf(VM& vm, int argc, const Value* args, int i) {
    if (!given(argc, args, i)) return 10;
    const int64_t radix = vm.toInt(args[i], "radix");
    if (radix < 2 || radix > 36) throw vm.runtimeError("a radix is from 2 to 36, not " + std::to_string(radix));
    return static_cast<int>(radix);
}

enum class CharacterClass { Letter, Digit, LetterOrDigit, Whitespace, Upper, Lower };

bool asciiIs(unsigned char c, CharacterClass which) {
    const bool upper = c >= 'A' && c <= 'Z', lower = c >= 'a' && c <= 'z', digit = c >= '0' && c <= '9';
    switch (which) {
        case CharacterClass::Letter: return upper || lower;
        case CharacterClass::Digit: return digit;
        case CharacterClass::LetterOrDigit: return upper || lower || digit;
        case CharacterClass::Whitespace: return c == ' ' || (c >= '\t' && c <= '\r');
        case CharacterClass::Upper: return upper;
        case CharacterClass::Lower: return lower;
    }
    return false;
}

// Whether every character of `s` is of the kind asked about; false for no characters at all.
// Like upper() and lower(), the answer for letters outside ASCII is the operating system's
// until the text engine brings Unicode data of our own (M4).
bool charactersAre(const std::string& s, CharacterClass which) {
    if (s.empty()) return false;
    bool ascii = true;
    for (unsigned char c : s) ascii = ascii && c < 0x80;
    if (ascii) {
        for (unsigned char c : s)
            if (!asciiIs(c, which)) return false;
        return true;
    }
#ifdef _WIN32
    const int length = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), wide.data(), length);
    std::vector<WORD> kinds(static_cast<size_t>(length));
    if (!GetStringTypeW(CT_CTYPE1, wide.data(), length, kinds.data())) return false;
    for (WORD kind : kinds) {
        bool is = false;
        switch (which) {
            case CharacterClass::Letter: is = (kind & C1_ALPHA) != 0; break;
            case CharacterClass::Digit: is = (kind & C1_DIGIT) != 0; break;
            case CharacterClass::LetterOrDigit: is = (kind & (C1_ALPHA | C1_DIGIT)) != 0; break;
            case CharacterClass::Whitespace: is = (kind & C1_SPACE) != 0; break;
            case CharacterClass::Upper: is = (kind & C1_UPPER) != 0; break;
            case CharacterClass::Lower: is = (kind & C1_LOWER) != 0; break;
        }
        if (!is) return false;
    }
    return true;
#else
    return false;
#endif
}

// Thousands separators for the digits of a whole number: 1234567 -> 1,234,567.
std::string grouped(const std::string& digits) {
    std::string out;
    for (size_t i = 0; i < digits.size(); i++) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out;
}

// "%d items, %.2f each".format(3, 1.5): the patterns of Kotlin's (Java's) String.format.
//   %[index$][flags][width][.precision]conversion
// Flags: - left-justify, 0 pad with zeros, + always a sign, space for a plus sign, and
// , thousands separators. Conversions: d x X o (whole numbers), f e E g G (fractions),
// s S (anything, as print shows it), c (a character), b (true or false), %% and %n.
std::string formatText(VM& vm, const std::string& pattern, int argc, const Value* args) {
    std::string out;
    int next = 0;
    const size_t size = pattern.size();
    auto digit = [&](size_t at) { return at < size && pattern[at] >= '0' && pattern[at] <= '9'; };

    for (size_t i = 0; i < size; i++) {
        if (pattern[i] != '%') {
            out += pattern[i];
            continue;
        }
        size_t at = i + 1;
        if (at < size && pattern[at] == '%') {
            out += '%';
            i = at;
            continue;
        }
        if (at < size && pattern[at] == 'n') {
            out += '\n';
            i = at;
            continue;
        }

        int index = -1;
        {
            size_t p = at;
            int number = 0;
            while (digit(p) && number < 100000) number = number * 10 + (pattern[p++] - '0');
            if (p > at && p < size && pattern[p] == '$') {
                index = number - 1;
                at = p + 1;
            }
        }
        bool left = false, zero = false, plus = false, space = false, comma = false;
        for (bool more = true; more && at < size;) {
            switch (pattern[at]) {
                case '-': left = true; break;
                case '0': zero = true; break;
                case '+': plus = true; break;
                case ' ': space = true; break;
                case ',': comma = true; break;
                default: more = false; break;
            }
            if (more) at++;
        }
        size_t width = 0;
        while (digit(at) && width < 100000) width = width * 10 + static_cast<size_t>(pattern[at++] - '0');
        int precision = -1;
        if (at < size && pattern[at] == '.') {
            at++;
            precision = 0;
            while (digit(at) && precision < 100000) precision = precision * 10 + (pattern[at++] - '0');
        }
        if (at >= size) throw vm.runtimeError("format: the pattern ends in the middle of a '%' item");
        const char conversion = pattern[at];
        i = at;

        if (index < 0) index = next++;
        if (index >= argc)
            throw vm.runtimeError("format: the pattern asks for more values than the " + std::to_string(argc) + " given");
        const Value value = args[index];

        std::string sign, body;
        bool numeric = false;
        switch (conversion) {
            case 'd': {
                if (!value.isNumber() || (value.isFloat() && value.as.d != std::floor(value.as.d)))
                    throw vm.runtimeError("format: %d takes a whole number, not " + vm.stringify(value) +
                                          "; %f and %s take any number");
                const int64_t n = value.isInt() ? value.as.i : floatToInt(vm, value.as.d);
                const uint64_t magnitude = n < 0 ? uint64_t{0} - static_cast<uint64_t>(n) : static_cast<uint64_t>(n);
                body = std::to_string(magnitude);
                if (comma) body = grouped(body);
                sign = n < 0 ? "-" : plus ? "+" : space ? " " : "";
                numeric = true;
                break;
            }
            case 'x':
            case 'X':
            case 'o': {
                if (!value.isInt()) throw vm.runtimeError(std::string("format: %") + conversion + " takes a whole number, not " + vm.stringify(value));
                char buffer[32];
                auto result = std::to_chars(buffer, buffer + sizeof(buffer), static_cast<uint64_t>(value.as.i), conversion == 'o' ? 8 : 16);
                body.assign(buffer, result.ptr);
                if (conversion == 'X')
                    for (char& c : body) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                numeric = true;
                break;
            }
            case 'f':
            case 'e':
            case 'E':
            case 'g':
            case 'G': {
                if (!value.isNumber()) throw vm.runtimeError(std::string("format: %") + conversion + " takes a number, not " + vm.stringify(value));
                const double d = value.toDouble();
                sign = std::signbit(d) && !std::isnan(d) ? "-" : plus ? "+" : space ? " " : "";
                if (std::isnan(d) || std::isinf(d)) {
                    body = std::isnan(d) ? "NaN" : "Infinity";
                    zero = false;
                    break;
                }
                const char spec[] = {'%', '.', '*', conversion, '\0'};
                const int digits = precision < 0 ? 6 : std::min(precision, 340);
                const int needed = std::snprintf(nullptr, 0, spec, digits, std::fabs(d));
                body.resize(static_cast<size_t>(needed) + 1);
                std::snprintf(body.data(), body.size(), spec, digits, std::fabs(d));
                body.resize(static_cast<size_t>(needed));
                if (comma && conversion == 'f') {
                    const size_t point = body.find('.');
                    body = grouped(body.substr(0, point)) + (point == std::string::npos ? "" : body.substr(point));
                }
                numeric = true;
                break;
            }
            case 's':
            case 'S':
                body = vm.stringify(value);
                if (precision >= 0) body.resize(offsetOfCharacter(body, static_cast<size_t>(precision)));
                if (conversion == 'S') body = changeCase(body, true);
                break;
            case 'c':
                if (value.isChar()) {
                    appendUtf8(body, static_cast<uint32_t>(value.as.i));
                } else if (value.isInt()) {
                    if (value.as.i < 0 || value.as.i > 0x10FFFF) throw vm.runtimeError("format: %c takes a character or its code, not " + vm.stringify(value));
                    appendUtf8(body, static_cast<uint32_t>(value.as.i));
                } else if (value.isString()) {
                    body = asString(value)->utf8();
                } else {
                    throw vm.runtimeError("format: %c takes a character or its code, not " + vm.stringify(value));
                }
                break;
            case 'b':
                body = value.truthy() ? "true" : "false";
                break;
            default:
                throw vm.runtimeError(std::string("format: '%") + conversion + "' is not a kind of item; use d, x, f, e, g, s, c or b, and %% for a percent sign");
        }

        const size_t length = sign.size() + countCharacters(body);
        const std::string padding(length < width ? width - length : 0, zero && numeric && !left ? '0' : ' ');
        if (left) out += sign + body + padding;
        else if (zero && numeric) out += sign + padding + body;
        else out += padding + sign + body;
    }
    return out;
}

// A char given where a string goes, as Kotlin's overloads taking a Char have it (D25): made
// the string of it alone, in its place on the stack, where the collector sees it.
void charAsText(VM& vm, Value& v) {
    if (v.isChar()) v = vm.characterString(static_cast<uint32_t>(v.as.i));
}

void registerStringMethods(VM& vm) {
    const BuiltinType T = BuiltinType::String;

    vm.defineMethod(T, "contains", 1, 2, [](VM& vm, Value self, int argc, Value* args) -> Value {
        charAsText(vm, args[0]);
        const std::string& s = asString(self)->utf8();
        const std::string& other = vm.toStr(args[0], "search text");
        if (flag(argc, args, 1)) return Value::boolean(fold(s).find(fold(other)) != std::string::npos);
        return Value::boolean(s.find(other) != std::string::npos);
    });

    vm.defineMethod(T, "startsWith", 1, 2, [](VM& vm, Value self, int argc, Value* args) -> Value {
        charAsText(vm, args[0]);
        const std::string& prefix = vm.toStr(args[0], "prefix");
        if (flag(argc, args, 1)) {
            const std::string s = fold(asString(self)->utf8()), p = fold(prefix);
            return Value::boolean(s.compare(0, p.size(), p) == 0);
        }
        return Value::boolean(asString(self)->utf8().compare(0, prefix.size(), prefix) == 0);
    });

    vm.defineMethod(T, "endsWith", 1, 2, [](VM& vm, Value self, int argc, Value* args) -> Value {
        charAsText(vm, args[0]);
        const bool ignoreCase = flag(argc, args, 1);
        const std::string s = ignoreCase ? fold(asString(self)->utf8()) : asString(self)->utf8();
        const std::string suffix = ignoreCase ? fold(vm.toStr(args[0], "suffix")) : vm.toStr(args[0], "suffix");
        return Value::boolean(s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0);
    });

    // Where `other` first appears at or after `startIndex`, or -1.
    vm.defineMethod(T, "indexOf", 1, 3, [](VM& vm, Value self, int argc, Value* args) -> Value {
        charAsText(vm, args[0]);
        const ObjString* text = asString(self);
        const ObjString* part = vm.toText(args[0], "search text");
        const size_t start = given(argc, args, 1) ? clampPosition(std::max<int64_t>(vm.toInt(args[1], "start index"), 0), text->length) : 0;
        if (flag(argc, args, 2)) {
            const std::string& other = part->utf8();
            // Changing case keeps the number of characters, so a position in one is a position in the other.
            const std::string s = fold(text->utf8());
            const size_t at = s.find(fold(other), offsetOfCharacter(s, start));
            return Value::integer(at == std::string::npos ? -1 : static_cast<int64_t>(countCharacters(std::string_view(s).substr(0, at))));
        }
        return Value::integer(text->find(part, start));
    });

    // Where `other` last appears, starting at or before `startIndex`, or -1.
    vm.defineMethod(T, "lastIndexOf", 1, 3, [](VM& vm, Value self, int argc, Value* args) -> Value {
        charAsText(vm, args[0]);
        const ObjString* text = asString(self);
        const ObjString* part = vm.toText(args[0], "search text");
        int64_t start = given(argc, args, 1) ? vm.toInt(args[1], "start index") : static_cast<int64_t>(text->length);
        if (start < 0) return Value::integer(-1);
        const size_t from = std::min(static_cast<size_t>(start), text->length);
        if (flag(argc, args, 2)) {
            const std::string& other = part->utf8();
            const std::string s = fold(text->utf8());
            const size_t at = s.rfind(fold(other), offsetOfCharacter(s, from));
            return Value::integer(at == std::string::npos ? -1 : static_cast<int64_t>(countCharacters(std::string_view(s).substr(0, at))));
        }
        return Value::integer(text->find(part, from, true));
    });

    vm.defineMethod(T, "replace", 2, 3, [](VM& vm, Value self, int argc, Value* args) -> Value {
        charAsText(vm, args[0]);
        charAsText(vm, args[1]);
        const ObjString* text = asString(self);
        const std::string& s = text->utf8();
        const std::string& from = vm.toStr(args[0], "text to replace");
        const std::string& to = vm.toStr(args[1], "replacement");
        if (from.empty()) return self;

        std::string result;
        size_t begin = 0;
        if (flag(argc, args, 2)) {
            // Found in the text with its case folded, by character position, and cut out of the original.
            const std::string folded = fold(s), target = fold(from);
            const size_t targetLength = countCharacters(target);
            size_t searchFrom = 0, charsBefore = 0;
            for (;;) {
                const size_t at = folded.find(target, searchFrom);
                if (at == std::string::npos) break;
                charsBefore += countCharacters(std::string_view(folded).substr(searchFrom, at - searchFrom));
                const size_t cut = text->utf8Offset(charsBefore);
                result.append(s, begin, cut - begin);
                result += to;
                charsBefore += targetLength;
                begin = text->utf8Offset(charsBefore);
                searchFrom = at + target.size();
            }
        } else {
            for (;;) {
                const size_t at = s.find(from, begin);
                if (at == std::string::npos) break;
                result.append(s, begin, at - begin);
                result += to;
                begin = at + from.size();
            }
        }
        result.append(s, begin, std::string::npos);
        return vm.str(std::move(result));
    });

    vm.defineMethod(T, "equals", 1, 2, [](VM&, Value self, int argc, Value* args) -> Value {
        if (!args[0].isString()) return Value::boolean(false);
        if (flag(argc, args, 1)) return Value::boolean(fold(asString(self)->utf8()) == fold(asString(args[0])->utf8()));
        return Value::boolean(sameText(asString(self), asString(args[0])));
    });

    vm.defineMethod(T, "compareTo", 1, 2, [](VM& vm, Value self, int argc, Value* args) -> Value {
        const ObjString* other = vm.toText(args[0], "text to compare with");
        if (!flag(argc, args, 1)) return Value::integer(compareText(asString(self), other));
        const int order = fold(asString(self)->utf8()).compare(fold(other->utf8()));
        return Value::integer((order > 0) - (order < 0));
    });



    vm.defineMethod(T, "isEmpty", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        return Value::boolean(asString(self)->utf8().empty());
    });
    vm.defineMethod(T, "isNotEmpty", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        return Value::boolean(!asString(self)->utf8().empty());
    });
    vm.defineMethod(T, "isBlank", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        return Value::boolean(trimmed(asString(self)->utf8()).empty());
    });
    vm.defineMethod(T, "isNotBlank", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        return Value::boolean(!trimmed(asString(self)->utf8()).empty());
    });

    vm.defineMethod(T, "trimStart", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        const std::string& s = asString(self)->utf8();
        const size_t begin = s.find_first_not_of(" \t\r\n\f\v");
        return vm.str(begin == std::string::npos ? std::string() : s.substr(begin));
    });
    vm.defineMethod(T, "trimEnd", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        const std::string& s = asString(self)->utf8();
        const size_t end = s.find_last_not_of(" \t\r\n\f\v");
        return vm.str(end == std::string::npos ? std::string() : s.substr(0, end + 1));
    });

    // The lines of the text, without their line ends.
    vm.defineMethod(T, "lines", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        ObjList* list = newList(vm);
        TempRoot keep(vm, Value::object(list));
        for (std::string& line : splitLines(asString(self)->utf8())) list->values().push_back(vm.str(std::move(line)));
        vm.account(list->values().size() * sizeof(Value));
        return Value::object(list);
    });

    auto pad = [](VM& vm, Value self, int argc, Value* args, bool atStart) -> Value {
        const ObjString* text = asString(self);
        const int64_t length = vm.toInt(args[0], "padded length");
        if (given(argc, args, 1)) charAsText(vm, args[1]);  // padStart(5, ' ') (D25)
        const std::string with = given(argc, args, 1) ? vm.toStr(args[1], "padding character") : std::string(" ");
        if (countCharacters(with) != 1) throw vm.runtimeError("padding is done with a single character, not " + vm.stringify(args[1]));
        if (length <= static_cast<int64_t>(text->length)) return self;
        // As large as memory allows: the room is asked for at once, so a size too large fails at once.
        const uint64_t missing = static_cast<uint64_t>(length) - text->length;
        if (missing > SIZE_MAX / with.size()) throw vm.outOfMemory();
        vm.checkRoomFor(missing * with.size());
        std::string padding;
        padding.reserve(static_cast<size_t>(missing) * with.size());
        for (int64_t i = static_cast<int64_t>(text->length); i < length; i++) padding += with;
        return vm.str(atStart ? padding + text->utf8() : text->utf8() + padding);
    };
    static const auto padText = pad;
    vm.defineMethod(T, "padStart", 1, 2, [](VM& vm, Value self, int argc, Value* args) -> Value {
        return padText(vm, self, argc, args, true);
    });
    vm.defineMethod(T, "padEnd", 1, 2, [](VM& vm, Value self, int argc, Value* args) -> Value {
        return padText(vm, self, argc, args, false);
    });

    // The characters in the opposite order.
    vm.defineMethod(T, "reversed", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        const ObjString* text = asString(self);
        Characters out{std::string(text->units().size(), '\0'), text->width};
        const size_t width = text->width;
        for (size_t i = 0; i < text->length; i++)
            std::memcpy(&out.units[i * width], text->units().data() + (text->length - 1 - i) * width, width);
        return vm.str(std::move(out));
    });

    // Text to number. toInt() and toDouble() are errors for text that is not one; the
    // ...OrNull forms give null instead.
    vm.defineMethod(T, "toInt", 0, 1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        int64_t n;
        if (!parseWhole(asString(self)->utf8(), radixOf(vm, argc, args, 0), n))
            throw vm.runtimeError(vm.stringify(self).insert(0, "\"") + "\" is not a whole number");
        return Value::integer(n);
    });
    vm.defineMethod(T, "toIntOrNull", 0, 1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        int64_t n;
        return parseWhole(asString(self)->utf8(), radixOf(vm, argc, args, 0), n) ? Value::integer(n) : Value::null();
    });
    // Text to an int's 64 bits read as unsigned (D44): "18446744073709551615" is -1.
    vm.defineMethod(T, "parseUnsigned", 0, 1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        std::string_view text = asString(self)->utf8();
        if (!text.empty() && text[0] == '+') text.remove_prefix(1);
        uint64_t n = 0;
        auto result = std::from_chars(text.data(), text.data() + text.size(), n, radixOf(vm, argc, args, 0));
        if (text.empty() || result.ec != std::errc() || result.ptr != text.data() + text.size())
            throw vm.runtimeError(vm.stringify(self).insert(0, "\"") + "\" is not a whole number from 0 to 18446744073709551615");
        return Value::integer(static_cast<int64_t>(n));
    });
    vm.defineMethod(T, "toFloat", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        double d;
        if (!parseFloat(asString(self)->utf8(), d)) throw vm.runtimeError(vm.stringify(self).insert(0, "\"") + "\" is not a number");
        return Value::number(d);
    });
    vm.defineMethod(T, "toFloatOrNull", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        double d;
        return parseFloat(asString(self)->utf8(), d) ? Value::number(d) : Value::null();
    });

    vm.defineMethod(T, "format", 0, -1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        return vm.str(formatText(vm, asString(self)->utf8(), argc, args));
    });

#define ASTRA_CHARACTER_TEST(NAME, WHICH)                                              \
    vm.defineMethod(T, NAME, 0, 0, [](VM&, Value self, int, Value*) -> Value {          \
        return Value::boolean(charactersAre(asString(self)->utf8(), CharacterClass::WHICH)); \
    })
    ASTRA_CHARACTER_TEST("isLetter", Letter);
    ASTRA_CHARACTER_TEST("isDigit", Digit);
    ASTRA_CHARACTER_TEST("isLetterOrDigit", LetterOrDigit);
    ASTRA_CHARACTER_TEST("isWhitespace", Whitespace);
    ASTRA_CHARACTER_TEST("isUpperCase", Upper);
    ASTRA_CHARACTER_TEST("isLowerCase", Lower);
#undef ASTRA_CHARACTER_TEST
}

// ---- Numbers ----

void registerNumberMethods(VM& vm) {
    const BuiltinType T = BuiltinType::Number;

    // The number itself if it lies from `minimum` to `maximum`, and otherwise the nearer of the two.
    vm.defineMethod(T, "coerceIn", 1, 2, [](VM& vm, Value self, int argc, Value* args) -> Value {
        Value low = args[0], high = argc > 1 ? args[1] : Value::null();
        if (argc == 1 && low.isObjType(ObjType::Range)) {
            const ObjRange* range = asRange(low);
            if (range->count() == 0) throw vm.runtimeError("coerceIn: the range is empty");
            const int64_t first = range->start, last = range->start + (range->count() - 1) * range->step;
            low = Value::integer(std::min(first, last));
            high = Value::integer(std::max(first, last));
        }
        if (!low.isNumber() || !high.isNumber()) throw vm.runtimeError("coerceIn takes a minimum and a maximum, or a range");
        if (compareNumbers(low, high) > 0)
            throw vm.runtimeError("coerceIn: the maximum " + vm.stringify(high) + " is less than the minimum " + vm.stringify(low));
        if (compareNumbers(self, low) < 0) return low;
        if (compareNumbers(self, high) > 0) return high;
        return self;
    });

    vm.defineMethod(T, "coerceAtLeast", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        vm.toNumber(args[0], "minimum");
        return compareNumbers(self, args[0]) < 0 ? args[0] : self;
    });
    vm.defineMethod(T, "coerceAtMost", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        vm.toNumber(args[0], "maximum");
        return compareNumbers(self, args[0]) > 0 ? args[0] : self;
    });

    // To the nearest whole number; a half goes up, as in Kotlin.
    auto round = [](VM& vm, Value self, int, Value*) -> Value {
        if (self.isInt()) return self;
        if (std::isnan(self.as.d)) throw vm.runtimeError("cannot round NaN to a whole number");
        return Value::integer(floatToInt(vm, std::floor(self.as.d + 0.5)));
    };
    vm.defineMethod(T, "roundToInt", 0, 0, round);

    // The whole part: what is after the point is dropped.
    auto whole = [](VM& vm, Value self, int, Value*) -> Value {
        return self.isInt() ? self : Value::integer(floatToInt(vm, std::trunc(self.as.d)));
    };
    vm.defineMethod(T, "toInt", 0, 0, whole);

    auto fraction = [](VM&, Value self, int, Value*) -> Value { return Value::number(self.toDouble()); };
    vm.defineMethod(T, "toFloat", 0, 0, fraction);

    // An int as a byte (D19): an error past 0 to 255; toByteWrapped() keeps the low 8 bits.
    vm.defineMethod(T, "toByte", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        const int64_t n = static_cast<int64_t>(wholeOnly(vm, self, "toByte"));
        if (n < 0 || n > 255)
            throw vm.runtimeError(std::to_string(n) + " does not fit in a byte, which holds 0 to 255; toByteWrapped() keeps the low 8 bits");
        return Value::byte(static_cast<uint8_t>(n));
    });
    vm.defineMethod(T, "toByteWrapped", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        return Value::byte(static_cast<uint8_t>(wholeOnly(vm, self, "toByteWrapped")));
    });
    // The low bits of an int, as the storage widths keep them (D148), read back as an int.
    vm.defineMethod(T, "toInt16Wrapped", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        return Value::integer(static_cast<int16_t>(wholeOnly(vm, self, "toInt16Wrapped")));
    });
    vm.defineMethod(T, "toUint16Wrapped", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        return Value::integer(static_cast<uint16_t>(wholeOnly(vm, self, "toUint16Wrapped")));
    });
    vm.defineMethod(T, "toInt32Wrapped", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        return Value::integer(static_cast<int32_t>(wholeOnly(vm, self, "toInt32Wrapped")));
    });
    vm.defineMethod(T, "toUint32Wrapped", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        return Value::integer(static_cast<uint32_t>(wholeOnly(vm, self, "toUint32Wrapped")));
    });
    // For 32-bit work ported from Kotlin (D148): the low 32 bits as a signed number, and
    // rotations of them, each giving a signed 32-bit number as Kotlin's Int does.
    vm.defineMethod(T, "wrap32", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        return Value::integer(static_cast<int32_t>(wholeOnly(vm, self, "wrap32")));
    });
    vm.defineMethod(T, "rotateLeft32", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        const uint32_t bits = static_cast<uint32_t>(wholeOnly(vm, self, "rotateLeft32"));
        const unsigned count = static_cast<unsigned>(vm.toInt(args[0], "rotation") & 31);
        return Value::integer(static_cast<int32_t>(count == 0 ? bits : (bits << count) | (bits >> (32 - count))));
    });
    vm.defineMethod(T, "rotateRight32", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        const uint32_t bits = static_cast<uint32_t>(wholeOnly(vm, self, "rotateRight32"));
        const unsigned count = static_cast<unsigned>(vm.toInt(args[0], "rotation") & 31);
        return Value::integer(static_cast<int32_t>(count == 0 ? bits : (bits >> count) | (bits << (32 - count))));
    });

    // An int's 64 bits read as unsigned (D44): there are no unsigned types, but these.
    vm.defineMethod(T, "toUnsignedString", 0, 1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        const uint64_t n = wholeOnly(vm, self, "toUnsignedString");
        char buffer[72];
        auto result = std::to_chars(buffer, buffer + sizeof(buffer), n, radixOf(vm, argc, args, 0));
        return vm.str(std::string(buffer, result.ptr));
    });
    vm.defineMethod(T, "compareUnsigned", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        const uint64_t a = wholeOnly(vm, self, "compareUnsigned"), b = wholeOnly(vm, args[0], "compareUnsigned");
        return Value::integer(a < b ? -1 : a > b ? 1 : 0);
    });
    vm.defineMethod(T, "divideUnsigned", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        const uint64_t a = wholeOnly(vm, self, "divideUnsigned"), b = wholeOnly(vm, args[0], "divideUnsigned");
        if (b == 0) throw vm.runtimeError("division by zero");
        return Value::integer(static_cast<int64_t>(a / b));
    });
    vm.defineMethod(T, "remainderUnsigned", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        const uint64_t a = wholeOnly(vm, self, "remainderUnsigned"), b = wholeOnly(vm, args[0], "remainderUnsigned");
        if (b == 0) throw vm.runtimeError("division by zero");
        return Value::integer(static_cast<int64_t>(a % b));
    });

    // toString(), or toString(radix) for a whole number in another base.
    vm.defineMethod(T, "toString", 0, 1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        if (!given(argc, args, 0)) return vm.str(vm.stringify(self));
        const int radix = radixOf(vm, argc, args, 0);
        if (!self.isInt()) throw vm.runtimeError("toString(radix) is for whole numbers, not " + vm.stringify(self));
        const uint64_t magnitude = self.as.i < 0 ? uint64_t{0} - static_cast<uint64_t>(self.as.i) : static_cast<uint64_t>(self.as.i);
        char buffer[72];
        auto result = std::to_chars(buffer, buffer + sizeof(buffer), magnitude, radix);
        return vm.str((self.as.i < 0 ? "-" : "") + std::string(buffer, result.ptr));
    });

    vm.defineMethod(T, "pow", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        if (self.isInt() && args[0].isInt() && args[0].as.i >= 0) return Value::integer(integerPower(vm, self.as.i, args[0].as.i));
        return Value::number(std::pow(self.toDouble(), vm.toNumber(args[0], "exponent")));
    });

    // The remainder that is never negative for a positive divisor: (-1).mod(5) is 4.
    vm.defineMethod(T, "mod", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        if (self.isInt() && args[0].isInt()) {
            const int64_t divisor = args[0].as.i;
            if (divisor == 0) throw vm.runtimeError("division by zero");
            if (divisor == -1) return Value::integer(0);
            const int64_t remainder = self.as.i % divisor;
            return Value::integer(remainder != 0 && ((remainder < 0) != (divisor < 0)) ? remainder + divisor : remainder);
        }
        const double divisor = vm.toNumber(args[0], "divisor");
        const double remainder = std::fmod(self.toDouble(), divisor);
        return Value::number(remainder != 0 && ((remainder < 0) != (divisor < 0)) ? remainder + divisor : remainder);
    });

    vm.defineMethod(T, "compareTo", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        vm.toNumber(args[0], "number to compare with");
        return Value::integer(compareNumbers(self, args[0]));
    });

    vm.defineMethod(T, "isNaN", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        return Value::boolean(self.isFloat() && std::isnan(self.as.d));
    });
    vm.defineMethod(T, "isInfinite", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        return Value::boolean(self.isFloat() && std::isinf(self.as.d));
    });
    vm.defineMethod(T, "isFinite", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        return Value::boolean(self.isInt() || std::isfinite(self.as.d));
    });
}

// A char's methods (D24): what kind of character it is, and the same letter in the other case.
void registerCharMethods(VM& vm) {
    constexpr BuiltinType T = BuiltinType::Char;
#define CHARACTER_CLASS(NAME, WHICH)                                                                          \
    vm.defineMethod(T, NAME, 0, 0, [](VM&, Value self, int, Value*) -> Value {                                 \
        std::string text;                                                                                     \
        appendUtf8(text, static_cast<uint32_t>(self.as.i));                                                   \
        return Value::boolean(charactersAre(text, CharacterClass::WHICH));                                    \
    });
    CHARACTER_CLASS("isLetter", Letter)
    CHARACTER_CLASS("isDigit", Digit)
    CHARACTER_CLASS("isLetterOrDigit", LetterOrDigit)
    CHARACTER_CLASS("isWhitespace", Whitespace)
    CHARACTER_CLASS("isUpperCase", Upper)
    CHARACTER_CLASS("isLowerCase", Lower)
#undef CHARACTER_CLASS
    // The same letter in the other case, where it is one character; the char itself otherwise.
    vm.defineMethod(T, "upper", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        std::string text;
        appendUtf8(text, static_cast<uint32_t>(self.as.i));
        const Characters result = Characters::fromUtf8(changeCase(text, true));
        return result.size() == 1 ? Value::character(result[0]) : self;
    });
    vm.defineMethod(T, "lower", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        std::string text;
        appendUtf8(text, static_cast<uint32_t>(self.as.i));
        const Characters result = Characters::fromUtf8(changeCase(text, false));
        return result.size() == 1 ? Value::character(result[0]) : self;
    });
    // The number a digit stands for: '7' is 7, and with a radix past 10, 'f' is 15.
    vm.defineMethod(T, "digitToInt", 0, 1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        const int radix = radixOf(vm, argc, args, 0);
        const int64_t c = self.as.i;
        const int64_t digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 99;
        if (digit >= radix) throw vm.runtimeError(quoteCharacter(static_cast<uint32_t>(c)) + " is not a digit" + (radix == 10 ? std::string() : " in base " + std::to_string(radix)));
        return Value::integer(digit);
    });
}

// An Index's (D146): the position it means in `size` items, ^k being size - k. An int is one
// too, a position from the start, so of() is on ints as well, for an Index that holds one.
void registerIndexMethods(VM& vm) {
    vm.defineMethod(BuiltinType::Index, "of", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        return Value::integer(vm.toInt(args[0], "size") - self.as.i);
    });
    vm.defineMethod(BuiltinType::Number, "of", 1, 1, [](VM& vm, Value self, int, Value*) -> Value {
        return Value::integer(vm.toInt(self, "position"));
    });
    // A range's bounds in `size` items: one with an end counted from the end made plain.
    vm.defineMethod(BuiltinType::Range, "of", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        const ObjRange* range = asRange(self);
        const int64_t size = vm.toInt(args[0], "size");
        return Value::object(vm.alloc<ObjRange>(range->startFromEnd ? size - range->start : range->start,
                                                range->endFromEnd ? size - range->end : range->end, range->step, range->inclusive));
    });
}

// A byte's methods (D19): the int it holds.
void registerByteMethods(VM& vm) {
    vm.defineMethod(BuiltinType::Byte, "toInt", 0, 0, [](VM&, Value self, int, Value*) -> Value { return Value::integer(self.as.i); });
}

// Int.maxValue and the like. Astra has one whole-number type and one for fractions; the
// four names are Kotlin's, so that a limit means the same number as it does there.
void registerNumberConstants(VM&) {
    // Astra's are `int.maxValue` and the like, which the compiler writes in place (D76); Kotlin's
    // holders, Int, Long, Double and Float, are errors saying so.
}

// ---- Lists ----

// A position in a list for get, set and subList: unlike list[i], it does not count from the
// end, so that a position before the start is told apart from the last item.
bool exactIndex(Value index, size_t size, size_t& out) {
    if (!index.isInt() || index.as.i < 0 || static_cast<uint64_t>(index.as.i) >= size) return false;
    out = static_cast<size_t>(index.as.i);
    return true;
}

AstraThrow outOfRange(VM& vm, const char* what, Value index, size_t size) {
    return vm.runtimeError(std::string(what) + ": index " + vm.stringify(index) + " is out of range (size " + std::to_string(size) + ")");
}

// Sorts positions [lo, hi) by `less`, stably: merge sort, so that what `less` does cannot
// leave the sort in a state std::sort does not allow.
template <typename Less>
void sortPositions(std::vector<size_t>& order, std::vector<size_t>& scratch, size_t lo, size_t hi, const Less& less) {
    if (hi - lo < 2) return;
    const size_t mid = lo + (hi - lo) / 2;
    sortPositions(order, scratch, lo, mid, less);
    sortPositions(order, scratch, mid, hi, less);
    size_t i = lo, j = mid, k = lo;
    while (i < mid && j < hi) scratch[k++] = less(order[j], order[i]) ? order[j++] : order[i++];
    while (i < mid) scratch[k++] = order[i++];
    while (j < hi) scratch[k++] = order[j++];
    std::copy(scratch.begin() + static_cast<std::ptrdiff_t>(lo), scratch.begin() + static_cast<std::ptrdiff_t>(hi),
              order.begin() + static_cast<std::ptrdiff_t>(lo));
}

// Item `i` of a list as a function given it is handed it: an item of a list of values where it is,
// a Ref, which the function copies itself if it may change the list while it runs (M6.6 part 4).
Value itemWhereItIs(VM& vm, ObjList* list, size_t i) {
    if (list->store() == ListStore::Items && !(list->pack->nullable && (list->pack->nulls[i / 64] >> (i % 64) & 1)))
        return Value::ref(list, static_cast<uint32_t>(i), 0, 0);
    return vm.listGet(list, i);
}

// sortBy { key } and sortByDescending { key } (M6.6 part 3, value-lists.html section 9): each
// item's key is worked out once, the positions are sorted by the keys, and then each item is
// moved once, to where it goes, however large it is and however many comparisons there are.
Value sortByKey(VM& vm, Value self, Value selector, bool descending) {
    ObjList* list = asList(self);
    const size_t n = list->size();
    std::vector<Value> keys;
    keys.reserve(n);
    TempVector keepKeys(vm, keys);
    for (size_t i = 0; i < n && i < list->size(); i++) {
        Value item = itemWhereItIs(vm, list, i);  // M6.6 part 4: no copy of a value to work out its key
        TempRoot keepItem(vm, item);
        keys.push_back(vm.call(selector, 1, &item));
    }
    if (list->size() != n) throw vm.runtimeError("the list was changed while it was sorted");
    std::vector<size_t> order(n), scratch(n);
    for (size_t i = 0; i < n; i++) order[i] = i;
    bool ints = true;
    for (const Value& key : keys) ints = ints && key.isInt();
    if (ints) {
        sortPositions(order, scratch, 0, n, [&](size_t a, size_t b) {
            return descending ? keys[b].as.i < keys[a].as.i : keys[a].as.i < keys[b].as.i;
        });
    } else {
        sortPositions(order, scratch, 0, n, [&](size_t a, size_t b) {
            return descending ? compareValues(vm, keys[b], keys[a]) < 0 : compareValues(vm, keys[a], keys[b]) < 0;
        });
    }
    vm.listArrange(list, order);
    return Value::null();
}

void registerListMethods(VM& vm) {
    const BuiltinType T = BuiltinType::List;

    vm.defineMethod(T, "sortBy", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value { return sortByKey(vm, self, args[0], false); });

    // replaceAll { it.moved() } (value-lists.html section 9): every item replaced by what the
    // function gives for it. An item of a list of values is handed over where it is, and is
    // where the result goes: a value made from its header alone is built there (section 7).
    vm.defineMethod(T, "replaceAll", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        ObjList* list = asList(self);
        const Value transform = args[0];
        for (size_t i = 0; i < list->size(); i++) {
            Value item = itemWhereItIs(vm, list, i);
            TempRoot keepItem(vm, item);
            if (item.isRef()) vm.pendingDestination_ = item;
            const Value made = vm.call(transform, 1, &item);
            vm.pendingDestination_ = Value::null();
            TempRoot keepMade(vm, made);
            if (i >= list->size()) throw vm.runtimeError("the list was made shorter while its items were replaced");
            vm.listSet(list, i, made);
        }
        return Value::null();
    });
    vm.defineMethod(T, "sortByDescending", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        return sortByKey(vm, self, args[0], true);
    });

    // add(item) at the end, or add(index, item) before the item at `index`.
    vm.defineMethod(T, "add", 1, 2, [](VM& vm, Value self, int argc, Value* args) -> Value {
        ObjList* list = asList(self);
        if (argc == 1) {
            vm.listAdd(list, args[0]);  // at the end, where nothing moves: a Ref is read as it is copied
        } else {
            args[1] = vm.boxed(args[1]);  // copied before the items after it move
            if (!args[0].isInt() || args[0].as.i < 0 || static_cast<uint64_t>(args[0].as.i) > list->size())
                throw outOfRange(vm, "add", args[0], list->size());
            vm.listInsert(list, static_cast<size_t>(args[0].as.i), &args[1], 1);
        }
        return Value::boolean(true);
    });

    // addAll(items) at the end, or addAll(index, items). `items` is anything a for loop takes.
    vm.defineMethod(T, "addAll", 1, 2, [](VM& vm, Value self, int argc, Value* args) -> Value {
        ObjList* list = asList(self);
        size_t at = list->size();
        if (argc == 2) {
            if (!args[0].isInt() || args[0].as.i < 0 || static_cast<uint64_t>(args[0].as.i) > list->size())
                throw outOfRange(vm, "addAll", args[0], list->size());
            at = static_cast<size_t>(args[0].as.i);
        }
        std::vector<Value> added;
        vm.collect(args[argc - 1], added);
        at = std::min(at, list->size());  // an iterator() may have changed the list
        TempVector keep(vm, added);
        vm.listInsert(list, at, added.data(), added.size());
        return Value::boolean(!added.empty());
    });

    vm.defineMethod(T, "isEmpty", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        return Value::boolean(asList(self)->size() == 0);
    });
    vm.defineMethod(T, "isNotEmpty", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        return Value::boolean(asList(self)->size() != 0);
    });

    vm.defineMethod(T, "get", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        ObjList* list = asList(self);
        size_t i;
        if (!exactIndex(args[0], list->size(), i)) throw outOfRange(vm, "get", args[0], list->size());
        return vm.listGet(list, i);
    });
    vm.defineMethod(T, "getOrNull", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        ObjList* list = asList(self);
        size_t i;
        return exactIndex(args[0], list->size(), i) ? vm.listGet(list, i) : Value::null();
    });
    // Gives the item that was there.
    vm.defineMethod(T, "set", 2, 2, [](VM& vm, Value self, int, Value* args) -> Value {
        ObjList* list = asList(self);
        size_t i;
        if (!exactIndex(args[0], list->size(), i)) throw outOfRange(vm, "set", args[0], list->size());
        const Value before = vm.listGet(list, i);
        vm.listSet(list, i, args[1]);
        return before;
    });

    auto removeEnd = [](VM& vm, Value self, bool first, const char* name, bool orNull) -> Value {
        ObjList* list = asList(self);
        if (list->size() == 0) {
            if (orNull) return Value::null();
            throw vm.runtimeError(std::string(name) + "() from an empty list");
        }
        const size_t at = first ? 0 : list->size() - 1;
        const Value removed = vm.listGet(list, at);
        vm.listErase(list, at, 1);
        return removed;
    };
    static const auto removeAtEnd = removeEnd;
    vm.defineMethod(T, "removeFirst", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        return removeAtEnd(vm, self, true, "removeFirst", false);
    });
    vm.defineMethod(T, "removeLast", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        return removeAtEnd(vm, self, false, "removeLast", false);
    });
    vm.defineMethod(T, "removeFirstOrNull", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        return removeAtEnd(vm, self, true, "removeFirstOrNull", true);
    });
    vm.defineMethod(T, "removeLastOrNull", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        return removeAtEnd(vm, self, false, "removeLastOrNull", true);
    });

    // A new list holding the same items, of the same type and kept the same way.
    auto copy = [](VM& vm, Value self, int, Value*) -> Value {
        ObjList* list = asList(self);
        if (!list->packed()) return Value::object(newList(vm, list->values()));
        ObjList* made = vm.listLike(list);
        vm.listCopyRange(made, list, 0, list->size());
        return Value::object(made);
    };
    vm.defineMethod(T, "toList", 0, 0, copy);
    vm.defineMethod(T, "toOpenList", 0, 0, copy);
    // A copy nobody may change, which can be a key (D26).
    vm.defineMethod(T, "frozen", 0, 0, [](VM& vm, Value self, int, Value*) -> Value { return vm.frozenCopy(self); });


    // A new list in the opposite order; reverse() turns the list itself round.
    vm.defineMethod(T, "reversed", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        ObjList* list = asList(self);
        ObjList* made = list->packed() ? vm.listLike(list) : newList(vm);
        vm.listCopyRange(made, list, 0, list->size());
        for (size_t i = 0, j = made->size(); i + 1 < j; i++, j--) vm.listSwap(made, i, j - 1);
        return Value::object(made);
    });

    vm.defineMethod(T, "lastIndexOf", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        ObjList* list = asList(self);
        for (size_t i = list->size(); i > 0; i--) {
            if (i > list->size()) continue;  // an == operator may have shortened the list
            if (vm.equals(vm.listGet(list, i - 1), args[0])) return Value::integer(static_cast<int64_t>(i - 1));
        }
        return Value::integer(-1);
    });

    vm.defineMethod(T, "fill", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        ObjList* list = asList(self);
        for (size_t i = 0; i < list->size(); i++) vm.listSet(list, i, args[0]);
        return Value::null();
    });
}

// ---- Maps ----

// A set's own (M6.6 part 8); what a set does besides is in set.astra. A view of a map's keys
// or entries (D77) takes all that only reads.
void registerSetMethods(VM& vm) {
    const BuiltinType T = BuiltinType::Set;
    vm.defineMethod(T, "frozen", 0, 0, [](VM& vm, Value self, int, Value*) -> Value { return vm.frozenCopy(self); });
    // Set(), or Set(from): the items of a list, a range, another set, a string or a map.
    vm.defineNative("Set", 0, 1, [](VM& vm, int argc, Value* args) -> Value {
        ObjSet* set = vm.newSet(nullptr);
        TempRoot keep(vm, Value::object(set));
        if (argc > 0 && !args[0].isNull())
            for (const Value& item : vm.itemsOf(args[0], "Set()")) vm.setAdd(set, item);
        return Value::object(set);
    });
    vm.defineMethod(T, "add", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        return Value::boolean(vm.setAdd(asSet(self), args[0]));  // true if it was not there
    });
    vm.defineMethod(T, "remove", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        return Value::boolean(vm.setRemove(asSet(self), args[0]));  // true if it was there
    });
    vm.defineMethod(T, "contains", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        return Value::boolean(vm.setContains(asSet(self), args[0]));
    });
    vm.defineMethod(T, "isEmpty", 0, 0, [](VM& vm, Value self, int, Value*) -> Value { return Value::boolean(vm.setSize(asSet(self)) == 0); });
    vm.defineMethod(T, "isNotEmpty", 0, 0, [](VM& vm, Value self, int, Value*) -> Value { return Value::boolean(vm.setSize(asSet(self)) != 0); });
    vm.defineMethod(T, "clear", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        ObjSet* set = asSet(self);
        if (set->view()) throw vm.viewChanged(set);
        set->items.clear();
        return Value::null();
    });
    vm.defineMethod(T, "addAll", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        ObjSet* set = asSet(self);
        if (set->view()) throw vm.viewChanged(set);
        bool changed = false;
        for (const Value& item : vm.itemsOf(args[0], "addAll()")) changed = vm.setAdd(set, item) || changed;
        return Value::boolean(changed);
    });
    // removeAll and retainAll take a collection, or a condition each item is given.
    auto removing = [](VM& vm, Value self, Value what, bool keepMatches) -> Value {
        ObjSet* set = asSet(self);
        if (set->view()) throw vm.viewChanged(set);
        const bool condition = what.isObjType(ObjType::Closure) || what.isObjType(ObjType::BoundMethod) || what.isObjType(ObjType::Native) ||
                               what.isObjType(ObjType::BoundNative);
        ObjSet* from = nullptr;
        if (!condition) {
            from = vm.newSet(nullptr);
            TempRoot keepFrom(vm, Value::object(from));
            for (const Value& item : vm.itemsOf(what, keepMatches ? "retainAll()" : "removeAll()")) vm.setAdd(from, item);
        }
        TempRoot keepFrom(vm, from != nullptr ? Value::object(from) : Value::null());
        std::vector<Value> items = vm.setItems(set);
        TempVector keep(vm, items);
        bool changed = false;
        for (Value& item : items) {
            const bool matches = condition ? vm.call(what, 1, &item).truthy() : vm.setContains(from, item);
            if (matches != keepMatches) changed = vm.setRemove(set, item) || changed;
        }
        return Value::boolean(changed);
    };
    static const auto removeBy = removing;
    vm.defineMethod(T, "removeAll", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value { return removeBy(vm, self, args[0], false); });
    vm.defineMethod(T, "retainAll", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value { return removeBy(vm, self, args[0], true); });
    vm.defineMethod(T, "toList", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        ObjSet* set = asSet(self);
        ObjList* list = vm.alloc<ObjList>();
        TempRoot keep(vm, Value::object(list));
        list->values() = vm.setItems(set);
        vm.account(list->values().size() * sizeof(Value));
        if (set->type != nullptr && !set->type->args.empty()) vm.typeList(list, vm.typeOf(kTypeList, false, nullptr, {set->type->args[0]}));
        return Value::object(list);
    });
    auto end = [](VM& vm, Value self, bool last) -> Value {
        const std::vector<Value> items = vm.setItems(asSet(self));
        if (items.empty()) throw vm.runtimeError(std::string(last ? "last()" : "first()") + " of an empty set");
        return last ? items.back() : items.front();
    };
    static const auto endOf = end;
    vm.defineMethod(T, "first", 0, 0, [](VM& vm, Value self, int, Value*) -> Value { return endOf(vm, self, false); });
    vm.defineMethod(T, "last", 0, 0, [](VM& vm, Value self, int, Value*) -> Value { return endOf(vm, self, true); });
}

void registerMapMethods(VM& vm) {
    const BuiltinType T = BuiltinType::Map;



    // put and set store a value under a key; put gives the value that was there, or null.
    vm.defineMethod(T, "put", 2, 2, [](VM& vm, Value self, int, Value* args) -> Value {
        ObjMap* map = asMap(self);
        const Value* found = map->map.find(args[0]);
        Value before = found != nullptr ? *found : Value::null();
        if (found == nullptr) vm.account(2 * sizeof(Value));
        map->map.set(args[0], args[1]);
        return before;
    });
    vm.defineMethod(T, "set", 2, 2, [](VM& vm, Value self, int, Value* args) -> Value {
        ObjMap* map = asMap(self);
        if (map->map.find(args[0]) == nullptr) vm.account(2 * sizeof(Value));
        map->map.set(args[0], args[1]);
        return Value::null();
    });

    vm.defineMethod(T, "isEmpty", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        return Value::boolean(asMap(self)->map.size() == 0);
    });
    vm.defineMethod(T, "isNotEmpty", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        return Value::boolean(asMap(self)->map.size() != 0);
    });

    // The value under a key that has to be there.
    vm.defineMethod(T, "getValue", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        const Value* found = asMap(self)->map.find(args[0]);
        if (found == nullptr) throw vm.runtimeError("getValue: the map has no key " + vm.stringify(args[0]));
        return *found;
    });
    vm.defineMethod(T, "getOrDefault", 2, 2, [](VM&, Value self, int, Value* args) -> Value {
        const Value* found = asMap(self)->map.find(args[0]);
        return found != nullptr ? *found : args[1];
    });

    // A new map with the same keys and values.
    auto copy = [](VM& vm, Value self, int, Value*) -> Value {
        ObjMap* made = vm.alloc<ObjMap>();
        for (const ValueMap::Entry& entry : asMap(self)->map.entries())
            if (entry.live) made->map.set(entry.key, entry.value);
        vm.account(made->map.size() * 2 * sizeof(Value));
        return Value::object(made);
    };
    vm.defineMethod(T, "toMap", 0, 0, copy);
    vm.defineMethod(T, "toOpenMap", 0, 0, copy);
    vm.defineMethod(T, "frozen", 0, 0, [](VM& vm, Value self, int, Value*) -> Value { return vm.frozenCopy(self); });

}

// ---- Ranges ----

void registerRangeMethods(VM& vm) {
    const BuiltinType T = BuiltinType::Range;

    vm.defineMethod(T, "first", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        const ObjRange* range = asRange(self);
        if (range->count() == 0) throw vm.runtimeError("first() of an empty range");
        return Value::integer(range->start);
    });
    vm.defineMethod(T, "last", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        const ObjRange* range = asRange(self);
        if (range->count() == 0) throw vm.runtimeError("last() of an empty range");
        return Value::integer(range->start + (range->count() - 1) * range->step);
    });
    vm.defineMethod(T, "isEmpty", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        return Value::boolean(asRange(self)->count() == 0);
    });
}

// ---- Global functions ----

void registerGlobals(VM& vm) {
    vm.defineNative("compareValues", 2, 2, [](VM& vm, int, Value* args) -> Value {
        return Value::integer(compareValues(vm, args[0], args[1]));
    });

    // The largest and the smallest of the values given.
    vm.defineNative("maxOf", 1, -1, [](VM& vm, int argc, Value* args) -> Value {
        Value best = args[0];
        for (int i = 1; i < argc; i++)
            if (compareValues(vm, args[i], best) > 0) best = args[i];
        return best;
    });
    vm.defineNative("minOf", 1, -1, [](VM& vm, int argc, Value* args) -> Value {
        Value best = args[0];
        for (int i = 1; i < argc; i++)
            if (compareValues(vm, args[i], best) < 0) best = args[i];
        return best;
    });
}

// ---- Files ----

// A path as the program wrote it is UTF-8, whatever the system's own encoding is.
fs::path toPath(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

std::string fromPath(const fs::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

bool isSeparator(char c) {
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

// A path without the separators it ends in, unless it is a root ("/", "C:\").
std::string_view withoutTrailingSeparators(std::string_view path) {
    while (path.size() > 1 && isSeparator(path.back()) && !(path.size() == 3 && path[1] == ':')) path.remove_suffix(1);
    return path;
}

// Where the last name of a path starts.
size_t lastNameStart(std::string_view path) {
    size_t at = path.size();
    while (at > 0 && !isSeparator(path[at - 1]) && !(at == 2 && path[1] == ':')) at--;
    return at;
}

// A whole file read or written in one go. The work itself touches nothing of the VM, so it
// can be done with the lock released, or by a worker while a flow is set aside (VM::blocking).
struct FileJob {
    std::string path;
    std::string data;
    std::string error;
};

void readWhole(FileJob& job) {
    std::ifstream in(toPath(job.path), std::ios::binary);
    if (!in) {
        job.error = "cannot read file '" + job.path + "'";
        return;
    }
    job.data.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeWhole(FileJob& job, bool append) {
    std::ofstream out(toPath(job.path), std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    if (!out || !out.write(job.data.data(), static_cast<std::streamsize>(job.data.size())).flush()) job.error = "cannot write file '" + job.path + "'";
}

Value readFile(VM& vm, Value path, bool asBytes) {
    auto job = std::make_shared<FileJob>();
    job->path = vm.toStr(path, "file path");
    return vm.blocking([job] { readWhole(*job); }, [job, asBytes](VM& vm) -> Value {
        if (!job->error.empty()) throw vm.errorOf("IOException", job->error);
        if (!asBytes) return vm.str(std::move(job->data));
        ObjBytes* made = vm.alloc<ObjBytes>();
        made->data.assign(job->data.begin(), job->data.end());
        vm.account(made->data.size());
        return Value::object(made);
    });
}

Value writeFile(VM& vm, Value path, Value content, bool bytesOnly, bool append) {
    auto job = std::make_shared<FileJob>();
    job->path = vm.toStr(path, "file path");
    if (content.isObjType(ObjType::Bytes)) {
        job->data.assign(asBytes(content)->data.begin(), asBytes(content)->data.end());
    } else if (bytesOnly) {
        throw vm.runtimeError("writeBytes takes bytes, not a value of type " + vm.typeName(content) + (content.isString() ? "; text is written with writeText" : ""));
    } else {
        job->data = vm.stringify(content);
    }
    return vm.blocking([job, append] { writeWhole(*job, append); }, [job](VM& vm) -> Value {
        if (!job->error.empty()) throw vm.errorOf("IOException", job->error);
        return Value::null();
    });
}

// What the File class (src/std/file.astra) is built on. The name starts with two
// underscores, which only the standard library's own source can use.
void registerFiles(VM& vm) {
    ObjClass* file = vm.alloc<ObjClass>(vm.intern("File"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        file->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("File.") + name, minArgs, maxArgs, fn)));
    };

    def("read", 1, 1, [](VM& vm, int, Value* args) -> Value { return readFile(vm, args[0], false); });
    def("write", 2, 2, [](VM& vm, int, Value* args) -> Value { return writeFile(vm, args[0], args[1], false, false); });
    def("append", 2, 2, [](VM& vm, int, Value* args) -> Value { return writeFile(vm, args[0], args[1], false, true); });
    def("readBytes", 1, 1, [](VM& vm, int, Value* args) -> Value { return readFile(vm, args[0], true); });
    def("writeBytes", 2, 2, [](VM& vm, int, Value* args) -> Value { return writeFile(vm, args[0], args[1], true, false); });
    def("appendBytes", 2, 2, [](VM& vm, int, Value* args) -> Value { return writeFile(vm, args[0], args[1], true, true); });

    def("exists", 1, 1, [](VM& vm, int, Value* args) -> Value {
        std::error_code ec;
        return Value::boolean(fs::exists(toPath(vm.toStr(args[0], "file path")), ec));
    });
    def("isFile", 1, 1, [](VM& vm, int, Value* args) -> Value {
        std::error_code ec;
        return Value::boolean(fs::is_regular_file(toPath(vm.toStr(args[0], "file path")), ec));
    });
    def("isDirectory", 1, 1, [](VM& vm, int, Value* args) -> Value {
        std::error_code ec;
        return Value::boolean(fs::is_directory(toPath(vm.toStr(args[0], "file path")), ec));
    });
    // In bytes; 0 for a file that is not there.
    def("size", 1, 1, [](VM& vm, int, Value* args) -> Value {
        std::error_code ec;
        const auto size = fs::file_size(toPath(vm.toStr(args[0], "file path")), ec);
        return Value::integer(ec ? 0 : static_cast<int64_t>(size));
    });
    // When the file was last written, in milliseconds since the start of 1970; 0 if it is not there.
    def("lastModified", 1, 1, [](VM& vm, int, Value* args) -> Value {
        std::error_code ec;
        const auto time = fs::last_write_time(toPath(vm.toStr(args[0], "file path")), ec);
        if (ec) return Value::integer(0);
        const auto system = std::chrono::clock_cast<std::chrono::system_clock>(time);
        return Value::integer(std::chrono::duration_cast<std::chrono::milliseconds>(system.time_since_epoch()).count());
    });
    // A file, or a folder with nothing in it. True if it was there and is gone.
    def("delete", 1, 1, [](VM& vm, int, Value* args) -> Value {
        std::error_code ec;
        return Value::boolean(fs::remove(toPath(vm.toStr(args[0], "file path")), ec) && !ec);
    });
    def("deleteRecursively", 1, 1, [](VM& vm, int, Value* args) -> Value {
        std::error_code ec;
        const auto removed = fs::remove_all(toPath(vm.toStr(args[0], "file path")), ec);
        return Value::boolean(!ec && removed > 0);
    });
    // True if the folder is there afterwards.
    def("mkdirs", 1, 1, [](VM& vm, int, Value* args) -> Value {
        std::error_code ec;
        const fs::path path = toPath(vm.toStr(args[0], "file path"));
        fs::create_directories(path, ec);
        return Value::boolean(fs::is_directory(path, ec));
    });
    def("rename", 2, 2, [](VM& vm, int, Value* args) -> Value {
        std::error_code ec;
        fs::rename(toPath(vm.toStr(args[0], "file path")), toPath(vm.toStr(args[1], "new path")), ec);
        return Value::boolean(!ec);
    });
    def("copy", 3, 3, [](VM& vm, int, Value* args) -> Value {
        std::error_code ec;
        const std::string& from = vm.toStr(args[0], "file path");
        const std::string& to = vm.toStr(args[1], "new path");
        const bool copied = fs::copy_file(toPath(from), toPath(to),
                                          args[2].truthy() ? fs::copy_options::overwrite_existing : fs::copy_options::none, ec);
        if (ec || !copied)
            throw vm.runtimeError("cannot copy '" + from + "' to '" + to + "'" + (ec ? ": " + ec.message() : ": it is already there"));
        return Value::null();
    });
    // The names in a folder, in order, or null if it is not a folder.
    def("list", 1, 1, [](VM& vm, int, Value* args) -> Value {
        std::error_code ec;
        std::vector<std::string> names;
        fs::directory_iterator it(toPath(vm.toStr(args[0], "file path")), ec);
        if (ec) return Value::null();
        for (const fs::directory_iterator end; it != end; it.increment(ec)) {
            if (ec) break;
            names.push_back(fromPath(it->path().filename()));
        }
        std::sort(names.begin(), names.end());
        ObjList* list = newList(vm);
        TempRoot keep(vm, Value::object(list));
        for (std::string& name : names) list->values().push_back(vm.str(std::move(name)));
        vm.account(list->values().size() * sizeof(Value));
        return Value::object(list);
    });
    def("absolute", 1, 1, [](VM& vm, int, Value* args) -> Value {
        std::error_code ec;
        const fs::path path = toPath(vm.toStr(args[0], "file path"));
        const fs::path absolute = fs::absolute(path, ec);
        return vm.str(fromPath(ec ? path : absolute.lexically_normal()));
    });

    // The last name of a path, the path of the folder it is in (null if the path says none),
    // and a path with a name added.
    def("name", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const std::string_view path = withoutTrailingSeparators(vm.toStr(args[0], "file path"));
        return vm.str(std::string(path.substr(lastNameStart(path))));
    });
    def("parent", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const std::string_view path = withoutTrailingSeparators(vm.toStr(args[0], "file path"));
        const size_t at = lastNameStart(path);
        if (at == 0 || at == path.size()) return Value::null();
        return vm.str(std::string(withoutTrailingSeparators(path.substr(0, at))));
    });
    def("join", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const std::string& base = vm.toStr(args[0], "file path");
        const std::string& child = vm.toStr(args[1], "file name");
        if (base.empty()) return args[1];
        if (isSeparator(base.back())) return vm.str(base + child);
        return vm.str(base + static_cast<char>(fs::path::preferred_separator) + child);
    });
    file->statics.set(vm.intern("separator"), vm.str(std::string(1, static_cast<char>(fs::path::preferred_separator))));

    vm.defineGlobal("__file", Value::object(file));
}

}  // namespace

void registerStdlib(VM& vm) {
    registerGlobals(vm);
    registerStringMethods(vm);
    registerNumberMethods(vm);
    registerCharMethods(vm);
    registerByteMethods(vm);
    registerIndexMethods(vm);
    registerNumberConstants(vm);
    registerListMethods(vm);
    registerMapMethods(vm);
    registerSetMethods(vm);
    registerRangeMethods(vm);
    registerFiles(vm);
}

}  // namespace astra
