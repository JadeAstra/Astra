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
        const int order = asString(a)->chars.compare(asString(b)->chars);
        return (order > 0) - (order < 0);
    }
    if (a.isBool() && b.isBool()) return static_cast<int>(a.as.b) - static_cast<int>(b.as.b);
    throw vm.runtimeError("cannot compare " + vm.typeName(a) + " and " + vm.typeName(b));
}

// How many parameters a function takes, where that can be told.
int parameterCount(Value function) {
    if (function.isObjType(ObjType::Closure)) return asClosure(function)->proto->arity;
    if (function.isObjType(ObjType::BoundMethod)) return asBoundMethod(function)->method->proto->arity;
    return 1;
}

int64_t integerPower(int64_t base, int64_t exponent) {
    uint64_t result = 1, b = static_cast<uint64_t>(base);
    for (uint64_t e = static_cast<uint64_t>(exponent); e != 0; e >>= 1) {
        if (e & 1) result *= b;
        b *= b;
    }
    return static_cast<int64_t>(result);
}

// ---- Strings ----

bool parseWhole(std::string_view s, int radix, int64_t& out) {
    if (!s.empty() && s[0] == '+') s.remove_prefix(1);
    if (s.empty()) return false;
    auto result = std::from_chars(s.data(), s.data() + s.size(), out, radix);
    return result.ec == std::errc() && result.ptr == s.data() + s.size();
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
                if (value.isInt()) {
                    if (value.as.i < 0 || value.as.i > 0x10FFFF) throw vm.runtimeError("format: %c takes a character or its code, not " + vm.stringify(value));
                    appendUtf8(body, static_cast<uint32_t>(value.as.i));
                } else if (value.isString()) {
                    body = asString(value)->chars;
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

void registerStringMethods(VM& vm) {
    const BuiltinType T = BuiltinType::String;

    vm.defineMethod(T, "contains", 1, 2, [](VM& vm, Value self, int argc, Value* args) -> Value {
        const std::string& s = asString(self)->chars;
        const std::string& other = vm.toStr(args[0], "search text");
        if (flag(argc, args, 1)) return Value::boolean(fold(s).find(fold(other)) != std::string::npos);
        return Value::boolean(s.find(other) != std::string::npos);
    });
    vm.nameParams(T, "contains", {"other", "ignoreCase"});

    vm.defineMethod(T, "startsWith", 1, 2, [](VM& vm, Value self, int argc, Value* args) -> Value {
        const std::string& prefix = vm.toStr(args[0], "prefix");
        if (flag(argc, args, 1)) {
            const std::string s = fold(asString(self)->chars), p = fold(prefix);
            return Value::boolean(s.compare(0, p.size(), p) == 0);
        }
        return Value::boolean(asString(self)->chars.compare(0, prefix.size(), prefix) == 0);
    });
    vm.nameParams(T, "startsWith", {"prefix", "ignoreCase"});

    vm.defineMethod(T, "endsWith", 1, 2, [](VM& vm, Value self, int argc, Value* args) -> Value {
        const bool ignoreCase = flag(argc, args, 1);
        const std::string s = ignoreCase ? fold(asString(self)->chars) : asString(self)->chars;
        const std::string suffix = ignoreCase ? fold(vm.toStr(args[0], "suffix")) : vm.toStr(args[0], "suffix");
        return Value::boolean(s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0);
    });
    vm.nameParams(T, "endsWith", {"suffix", "ignoreCase"});

    // Where `other` first appears at or after `startIndex`, or -1.
    vm.defineMethod(T, "indexOf", 1, 3, [](VM& vm, Value self, int argc, Value* args) -> Value {
        const ObjString* text = asString(self);
        const std::string& other = vm.toStr(args[0], "search text");
        const size_t start = given(argc, args, 1) ? clampPosition(std::max<int64_t>(vm.toInt(args[1], "start index"), 0), text->length) : 0;
        if (flag(argc, args, 2)) {
            // Changing case keeps the number of characters, so a position in one is a position in the other.
            const std::string s = fold(text->chars);
            const size_t at = s.find(fold(other), offsetOfCharacter(s, start));
            return Value::integer(at == std::string::npos ? -1 : static_cast<int64_t>(countCharacters(std::string_view(s).substr(0, at))));
        }
        const size_t at = text->chars.find(other, text->byteAt(start));
        return Value::integer(at == std::string::npos ? -1 : static_cast<int64_t>(text->charAt(at)));
    });
    vm.nameParams(T, "indexOf", {"other", "startIndex", "ignoreCase"});

    // Where `other` last appears, starting at or before `startIndex`, or -1.
    vm.defineMethod(T, "lastIndexOf", 1, 3, [](VM& vm, Value self, int argc, Value* args) -> Value {
        const ObjString* text = asString(self);
        const std::string& other = vm.toStr(args[0], "search text");
        int64_t start = given(argc, args, 1) ? vm.toInt(args[1], "start index") : static_cast<int64_t>(text->length);
        if (start < 0) return Value::integer(-1);
        const size_t from = std::min(static_cast<size_t>(start), text->length);
        if (flag(argc, args, 2)) {
            const std::string s = fold(text->chars);
            const size_t at = s.rfind(fold(other), offsetOfCharacter(s, from));
            return Value::integer(at == std::string::npos ? -1 : static_cast<int64_t>(countCharacters(std::string_view(s).substr(0, at))));
        }
        const size_t at = text->chars.rfind(other, text->byteAt(from));
        return Value::integer(at == std::string::npos ? -1 : static_cast<int64_t>(text->charAt(at)));
    });
    vm.nameParams(T, "lastIndexOf", {"other", "startIndex", "ignoreCase"});

    vm.defineMethod(T, "replace", 2, 3, [](VM& vm, Value self, int argc, Value* args) -> Value {
        const ObjString* text = asString(self);
        const std::string& s = text->chars;
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
                const size_t cut = text->byteAt(charsBefore);
                result.append(s, begin, cut - begin);
                result += to;
                charsBefore += targetLength;
                begin = text->byteAt(charsBefore);
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
    vm.nameParams(T, "replace", {"oldValue", "newValue", "ignoreCase"});

    vm.defineMethod(T, "equals", 1, 2, [](VM&, Value self, int argc, Value* args) -> Value {
        if (!args[0].isString()) return Value::boolean(false);
        if (flag(argc, args, 1)) return Value::boolean(fold(asString(self)->chars) == fold(asString(args[0])->chars));
        return Value::boolean(asString(self)->chars == asString(args[0])->chars);
    });
    vm.nameParams(T, "equals", {"other", "ignoreCase"});

    vm.defineMethod(T, "compareTo", 1, 2, [](VM& vm, Value self, int argc, Value* args) -> Value {
        const std::string& other = vm.toStr(args[0], "text to compare with");
        const int order = flag(argc, args, 1) ? fold(asString(self)->chars).compare(fold(other)) : asString(self)->chars.compare(other);
        return Value::integer((order > 0) - (order < 0));
    });
    vm.nameParams(T, "compareTo", {"other", "ignoreCase"});

    // substring(start, end) or substring(range). Unlike slice, a position outside the text is an error.
    vm.defineMethod(T, "substring", 1, 2, [](VM& vm, Value self, int argc, Value* args) -> Value {
        const ObjString* text = asString(self);
        const int64_t length = static_cast<int64_t>(text->length);
        int64_t begin, end = length;
        if (args[0].isObjType(ObjType::Range)) {
            const ObjRange* range = asRange(args[0]);
            if (range->step != 1) throw vm.runtimeError("substring() takes a range that counts up by one");
            begin = range->start;
            end = range->start + range->count();
        } else {
            begin = vm.toInt(args[0], "substring start");
            if (given(argc, args, 1)) end = vm.toInt(args[1], "substring end");
        }
        if (begin < 0 || end > length || begin > end)
            throw vm.runtimeError("substring(" + std::to_string(begin) + ", " + std::to_string(end) +
                                  ") is out of range for a string of length " + std::to_string(length));
        const size_t from = text->byteAt(static_cast<size_t>(begin));
        return vm.str(text->chars.substr(from, text->byteAt(static_cast<size_t>(end)) - from));
    });
    vm.nameParams(T, "substring", {"startIndex", "endIndex"});

    vm.defineMethod(T, "lowercase", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        return vm.str(changeCase(asString(self)->chars, false));
    });
    vm.defineMethod(T, "uppercase", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        return vm.str(changeCase(asString(self)->chars, true));
    });

    vm.defineMethod(T, "isEmpty", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        return Value::boolean(asString(self)->chars.empty());
    });
    vm.defineMethod(T, "isNotEmpty", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        return Value::boolean(!asString(self)->chars.empty());
    });
    vm.defineMethod(T, "isBlank", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        return Value::boolean(trimmed(asString(self)->chars).empty());
    });
    vm.defineMethod(T, "isNotBlank", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        return Value::boolean(!trimmed(asString(self)->chars).empty());
    });

    vm.defineMethod(T, "trimStart", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        const std::string& s = asString(self)->chars;
        const size_t begin = s.find_first_not_of(" \t\r\n\f\v");
        return vm.str(begin == std::string::npos ? std::string() : s.substr(begin));
    });
    vm.defineMethod(T, "trimEnd", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        const std::string& s = asString(self)->chars;
        const size_t end = s.find_last_not_of(" \t\r\n\f\v");
        return vm.str(end == std::string::npos ? std::string() : s.substr(0, end + 1));
    });

    // The lines of the text, without their line ends.
    vm.defineMethod(T, "lines", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        ObjList* list = newList(vm);
        TempRoot keep(vm, Value::object(list));
        for (std::string& line : splitLines(asString(self)->chars)) list->items.push_back(vm.str(std::move(line)));
        vm.account(list->items.size() * sizeof(Value));
        return Value::object(list);
    });

    auto pad = [](VM& vm, Value self, int argc, Value* args, bool atStart) -> Value {
        const ObjString* text = asString(self);
        const int64_t length = vm.toInt(args[0], "padded length");
        const std::string with = given(argc, args, 1) ? vm.toStr(args[1], "padding character") : std::string(" ");
        if (countCharacters(with) != 1) throw vm.runtimeError("padding is done with a single character, not " + vm.stringify(args[1]));
        if (length <= static_cast<int64_t>(text->length)) return self;
        if (length > (int64_t{1} << 28)) throw vm.runtimeError("padded string would be too large");
        std::string padding;
        for (int64_t i = static_cast<int64_t>(text->length); i < length; i++) padding += with;
        return vm.str(atStart ? padding + text->chars : text->chars + padding);
    };
    static const auto padText = pad;
    vm.defineMethod(T, "padStart", 1, 2, [](VM& vm, Value self, int argc, Value* args) -> Value {
        return padText(vm, self, argc, args, true);
    });
    vm.nameParams(T, "padStart", {"length", "padChar"});
    vm.defineMethod(T, "padEnd", 1, 2, [](VM& vm, Value self, int argc, Value* args) -> Value {
        return padText(vm, self, argc, args, false);
    });
    vm.nameParams(T, "padEnd", {"length", "padChar"});

    // The characters in the opposite order.
    vm.defineMethod(T, "reversed", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        const ObjString* text = asString(self);
        std::string out;
        out.reserve(text->chars.size());
        for (size_t i = text->length; i > 0; i--) out += text->character(i - 1);
        return vm.str(std::move(out));
    });

    // Text to number. toInt() and toDouble() are errors for text that is not one; the
    // ...OrNull forms give null instead.
    vm.defineMethod(T, "toInt", 0, 1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        int64_t n;
        if (!parseWhole(asString(self)->chars, radixOf(vm, argc, args, 0), n))
            throw vm.runtimeError(vm.stringify(self).insert(0, "\"") + "\" is not a whole number");
        return Value::integer(n);
    });
    vm.defineMethod(T, "toIntOrNull", 0, 1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        int64_t n;
        return parseWhole(asString(self)->chars, radixOf(vm, argc, args, 0), n) ? Value::integer(n) : Value::null();
    });
    vm.defineMethod(T, "toDouble", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        double d;
        if (!parseFloat(asString(self)->chars, d)) throw vm.runtimeError(vm.stringify(self).insert(0, "\"") + "\" is not a number");
        return Value::number(d);
    });
    vm.defineMethod(T, "toDoubleOrNull", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        double d;
        return parseFloat(asString(self)->chars, d) ? Value::number(d) : Value::null();
    });

    vm.defineMethod(T, "format", 0, -1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        return vm.str(formatText(vm, asString(self)->chars, argc, args));
    });

#define ASTRA_CHARACTER_TEST(NAME, WHICH)                                              \
    vm.defineMethod(T, NAME, 0, 0, [](VM&, Value self, int, Value*) -> Value {          \
        return Value::boolean(charactersAre(asString(self)->chars, CharacterClass::WHICH)); \
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
    vm.nameParams(T, "coerceIn", {"minimumValue", "maximumValue"});

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
    vm.defineMethod(T, "roundToLong", 0, 0, round);

    // The whole part: what is after the point is dropped.
    auto whole = [](VM& vm, Value self, int, Value*) -> Value {
        return self.isInt() ? self : Value::integer(floatToInt(vm, std::trunc(self.as.d)));
    };
    vm.defineMethod(T, "toInt", 0, 0, whole);
    vm.defineMethod(T, "toLong", 0, 0, whole);

    auto fraction = [](VM&, Value self, int, Value*) -> Value { return Value::number(self.toDouble()); };
    vm.defineMethod(T, "toDouble", 0, 0, fraction);
    vm.defineMethod(T, "toFloat", 0, 0, fraction);

    // The character with this code.
    vm.defineMethod(T, "toChar", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        const int64_t code = vm.toInt(self, "character code");
        if (code < 0 || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
            throw vm.runtimeError(std::to_string(code) + " is not a valid Unicode code point");
        std::string s;
        appendUtf8(s, static_cast<uint32_t>(code));
        return vm.str(std::move(s));
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
        if (self.isInt() && args[0].isInt() && args[0].as.i >= 0) return Value::integer(integerPower(self.as.i, args[0].as.i));
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

// Int.maxValue and the like. Astra has one whole-number type and one for fractions; the
// four names are Kotlin's, so that a limit means the same number as it does there.
void registerNumberConstants(VM& vm) {
    auto object = [&](const char* name, std::initializer_list<std::pair<const char*, Value>> members) {
        ObjClass* holder = vm.alloc<ObjClass>(vm.intern(name));
        for (const auto& member : members) holder->statics.set(vm.intern(member.first), member.second);
        vm.defineGlobal(name, Value::object(holder));
    };
    const double infinity = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();

    object("Int", {{"maxValue", Value::integer(2147483647)}, {"minValue", Value::integer(-2147483647 - 1)}});
    object("Long", {{"maxValue", Value::integer(std::numeric_limits<int64_t>::max())},
                    {"minValue", Value::integer(std::numeric_limits<int64_t>::min())}});
    object("Double", {{"maxValue", Value::number(std::numeric_limits<double>::max())},
                      {"minValue", Value::number(std::numeric_limits<double>::denorm_min())},
                      {"positiveInfinity", Value::number(infinity)},
                      {"negativeInfinity", Value::number(-infinity)},
                      {"nan", Value::number(nan)}});
    object("Float", {{"maxValue", Value::number(static_cast<double>(std::numeric_limits<float>::max()))},
                     {"minValue", Value::number(static_cast<double>(std::numeric_limits<float>::denorm_min()))},
                     {"positiveInfinity", Value::number(infinity)},
                     {"negativeInfinity", Value::number(-infinity)},
                     {"nan", Value::number(nan)}});
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

// first, last and their ...OrNull forms: the item at one end, or with a condition the
// nearest to that end that meets it.
Value endItem(VM& vm, Value self, int argc, Value* args, bool fromStart, const char* name, bool orNull) {
    ObjList* list = asList(self);
    if (!given(argc, args, 0)) {
        if (!list->items.empty()) return fromStart ? list->items.front() : list->items.back();
        if (orNull) return Value::null();
        throw vm.runtimeError(std::string(name) + "() of an empty list");
    }
    for (size_t n = 0; n < list->items.size(); n++) {
        const size_t i = fromStart ? n : list->items.size() - 1 - n;
        TempRoot item(vm, list->items[i]);
        Value candidate = item.get();
        if (vm.call(args[0], 1, &candidate).truthy()) return item.get();
    }
    if (orNull) return Value::null();
    throw vm.runtimeError(std::string(name) + "(): no item of the list meets the condition");
}

void registerListMethods(VM& vm) {
    const BuiltinType T = BuiltinType::List;

    // add(item) at the end, or add(index, item) before the item at `index`.
    vm.defineMethod(T, "add", 1, 2, [](VM& vm, Value self, int argc, Value* args) -> Value {
        ObjList* list = asList(self);
        if (argc == 1) {
            list->items.push_back(args[0]);
        } else {
            if (!args[0].isInt() || args[0].as.i < 0 || static_cast<uint64_t>(args[0].as.i) > list->items.size())
                throw outOfRange(vm, "add", args[0], list->items.size());
            list->items.insert(list->items.begin() + static_cast<std::ptrdiff_t>(args[0].as.i), args[1]);
        }
        vm.account(sizeof(Value));
        return Value::boolean(true);
    });

    // addAll(items) at the end, or addAll(index, items). `items` is anything a for loop takes.
    vm.defineMethod(T, "addAll", 1, 2, [](VM& vm, Value self, int argc, Value* args) -> Value {
        ObjList* list = asList(self);
        size_t at = list->items.size();
        if (argc == 2) {
            if (!args[0].isInt() || args[0].as.i < 0 || static_cast<uint64_t>(args[0].as.i) > list->items.size())
                throw outOfRange(vm, "addAll", args[0], list->items.size());
            at = static_cast<size_t>(args[0].as.i);
        }
        std::vector<Value> added;
        vm.collect(args[argc - 1], added);
        at = std::min(at, list->items.size());  // an iterator() may have changed the list
        list->items.insert(list->items.begin() + static_cast<std::ptrdiff_t>(at), added.begin(), added.end());
        vm.account(added.size() * sizeof(Value));
        return Value::boolean(!added.empty());
    });

    vm.defineMethod(T, "isEmpty", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        return Value::boolean(asList(self)->items.empty());
    });
    vm.defineMethod(T, "isNotEmpty", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        return Value::boolean(!asList(self)->items.empty());
    });

    vm.defineMethod(T, "first", 0, 1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        return endItem(vm, self, argc, args, true, "first", false);
    });
    vm.defineMethod(T, "last", 0, 1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        return endItem(vm, self, argc, args, false, "last", false);
    });
    vm.defineMethod(T, "firstOrNull", 0, 1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        return endItem(vm, self, argc, args, true, "firstOrNull", true);
    });
    vm.defineMethod(T, "lastOrNull", 0, 1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        return endItem(vm, self, argc, args, false, "lastOrNull", true);
    });

    vm.defineMethod(T, "get", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        const std::vector<Value>& items = asList(self)->items;
        size_t i;
        if (!exactIndex(args[0], items.size(), i)) throw outOfRange(vm, "get", args[0], items.size());
        return items[i];
    });
    vm.defineMethod(T, "getOrNull", 1, 1, [](VM&, Value self, int, Value* args) -> Value {
        const std::vector<Value>& items = asList(self)->items;
        size_t i;
        return exactIndex(args[0], items.size(), i) ? items[i] : Value::null();
    });
    // Gives the item that was there.
    vm.defineMethod(T, "set", 2, 2, [](VM& vm, Value self, int, Value* args) -> Value {
        std::vector<Value>& items = asList(self)->items;
        size_t i;
        if (!exactIndex(args[0], items.size(), i)) throw outOfRange(vm, "set", args[0], items.size());
        Value before = items[i];
        items[i] = args[1];
        return before;
    });

    auto removeEnd = [](VM& vm, Value self, bool first, const char* name, bool orNull) -> Value {
        std::vector<Value>& items = asList(self)->items;
        if (items.empty()) {
            if (orNull) return Value::null();
            throw vm.runtimeError(std::string(name) + "() from an empty list");
        }
        Value removed = first ? items.front() : items.back();
        if (first) items.erase(items.begin());
        else items.pop_back();
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

    // A new list holding the same items.
    auto copy = [](VM& vm, Value self, int, Value*) -> Value { return Value::object(newList(vm, asList(self)->items)); };
    vm.defineMethod(T, "toList", 0, 0, copy);
    vm.defineMethod(T, "toMutableList", 0, 0, copy);

    // The items from `fromIndex` up to but not including `toIndex`, as a new list.
    vm.defineMethod(T, "subList", 2, 2, [](VM& vm, Value self, int, Value* args) -> Value {
        const std::vector<Value>& items = asList(self)->items;
        const int64_t from = vm.toInt(args[0], "subList start"), to = vm.toInt(args[1], "subList end");
        if (from < 0 || to > static_cast<int64_t>(items.size()) || from > to)
            throw vm.runtimeError("subList(" + std::to_string(from) + ", " + std::to_string(to) + ") is out of range for a list of size " +
                                  std::to_string(items.size()));
        return Value::object(newList(vm, std::vector<Value>(items.begin() + static_cast<std::ptrdiff_t>(from),
                                                              items.begin() + static_cast<std::ptrdiff_t>(to))));
    });
    vm.nameParams(T, "subList", {"fromIndex", "toIndex"});

    // A new list in the opposite order; reverse() turns the list itself round.
    vm.defineMethod(T, "reversed", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        const std::vector<Value>& items = asList(self)->items;
        return Value::object(newList(vm, std::vector<Value>(items.rbegin(), items.rend())));
    });

    vm.defineMethod(T, "lastIndexOf", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        const std::vector<Value>& items = asList(self)->items;
        for (size_t i = items.size(); i > 0; i--) {
            if (i > items.size()) continue;  // an == operator may have shortened the list
            if (vm.equals(items[i - 1], args[0])) return Value::integer(static_cast<int64_t>(i - 1));
        }
        return Value::integer(-1);
    });

    vm.defineMethod(T, "fill", 1, 1, [](VM&, Value self, int, Value* args) -> Value {
        std::vector<Value>& items = asList(self)->items;
        std::fill(items.begin(), items.end(), args[0]);
        return Value::null();
    });
}

// ---- Maps ----

void registerMapMethods(VM& vm) {
    const BuiltinType T = BuiltinType::Map;

    vm.defineMethod(T, "containsKey", 1, 1, [](VM&, Value self, int, Value* args) -> Value {
        return Value::boolean(asMap(self)->map.find(args[0]) != nullptr);
    });

    vm.defineMethod(T, "containsValue", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        const ObjMap* map = asMap(self);
        // By index: comparing may run an == operator that changes the map.
        for (size_t i = 0; i < map->map.entries().size(); i++) {
            const ValueMap::Entry& entry = map->map.entries()[i];
            if (entry.live && vm.equals(entry.value, args[0])) return Value::boolean(true);
        }
        return Value::boolean(false);
    });

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
    vm.defineMethod(T, "toMutableMap", 0, 0, copy);

    // forEach { entry -> } or forEach { key, value -> }: the function says which by how
    // many parameters it has.
    vm.defineMethod(T, "forEach", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        ObjMap* map = asMap(self);
        const bool pairs = parameterCount(args[0]) == 2;
        // By index, and looked at afresh each time round: the function may change the map.
        for (size_t i = 0; i < map->map.entries().size(); i++) {
            if (!map->map.entries()[i].live) continue;
            Value pair[2] = {map->map.entries()[i].key, map->map.entries()[i].value};
            TempRoot key(vm, pair[0]);
            TempRoot value(vm, pair[1]);
            if (pairs) {
                vm.call(args[0], 2, pair);
            } else {
                TempRoot entry(vm, vm.makeEntry(pair[0], pair[1]));
                Value argument = entry.get();
                vm.call(args[0], 1, &argument);
            }
        }
        return Value::null();
    });
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

std::string readWhole(VM& vm, const std::string& path) {
    std::ifstream in(toPath(path), std::ios::binary);
    if (!in) throw vm.runtimeError("cannot read file '" + path + "'");
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeWhole(VM& vm, const std::string& path, const char* data, size_t size, bool append) {
    std::ofstream out(toPath(path), std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    if (!out || !out.write(data, static_cast<std::streamsize>(size)).flush()) throw vm.runtimeError("cannot write file '" + path + "'");
}

const std::vector<uint8_t>& bytesArgument(VM& vm, Value v) {
    if (!v.isObjType(ObjType::Bytes))
        throw vm.runtimeError("writeBytes takes bytes, not a value of type " + vm.typeName(v) + (v.isString() ? "; text is written with writeText" : ""));
    return asBytes(v)->data;
}

// What the File class (src/std/file.astra) is built on. The name starts with two
// underscores, which only the standard library's own source can use.
void registerFiles(VM& vm) {
    ObjClass* file = vm.alloc<ObjClass>(vm.intern("File"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        file->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("File.") + name, minArgs, maxArgs, fn)));
    };

    def("read", 1, 1, [](VM& vm, int, Value* args) -> Value { return vm.str(readWhole(vm, vm.toStr(args[0], "file path"))); });
    def("write", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const std::string text = vm.stringify(args[1]);
        writeWhole(vm, vm.toStr(args[0], "file path"), text.data(), text.size(), false);
        return Value::null();
    });
    def("append", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const std::string text = vm.stringify(args[1]);
        writeWhole(vm, vm.toStr(args[0], "file path"), text.data(), text.size(), true);
        return Value::null();
    });
    def("readBytes", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const std::string data = readWhole(vm, vm.toStr(args[0], "file path"));
        ObjBytes* made = vm.alloc<ObjBytes>();
        made->data.assign(data.begin(), data.end());
        vm.account(made->data.size());
        return Value::object(made);
    });
    def("writeBytes", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const std::vector<uint8_t>& data = bytesArgument(vm, args[1]);
        writeWhole(vm, vm.toStr(args[0], "file path"), reinterpret_cast<const char*>(data.data()), data.size(), false);
        return Value::null();
    });
    def("appendBytes", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const std::vector<uint8_t>& data = bytesArgument(vm, args[1]);
        writeWhole(vm, vm.toStr(args[0], "file path"), reinterpret_cast<const char*>(data.data()), data.size(), true);
        return Value::null();
    });

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
        for (std::string& name : names) list->items.push_back(vm.str(std::move(name)));
        vm.account(list->items.size() * sizeof(Value));
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
    registerNumberConstants(vm);
    registerListMethods(vm);
    registerMapMethods(vm);
    registerRangeMethods(vm);
    registerFiles(vm);
}

}  // namespace astra
