// The standard library: global functions, the Math and File classes, and the methods of
// the built-in types.

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>

#include "compiler.h"
#include "vm.h"

namespace astra {

namespace {

// ---- Helpers ----

// Clamps a possibly negative position into [0, length], as slice() bounds.
size_t clampPosition(int64_t position, size_t length) {
    int64_t n = static_cast<int64_t>(length);
    if (position < 0) position += n;
    return static_cast<size_t>(std::clamp<int64_t>(position, 0, n));
}

std::string_view trimmed(std::string_view s) {
    const char* whitespace = " \t\r\n\f\v";
    size_t begin = s.find_first_not_of(whitespace);
    if (begin == std::string_view::npos) return {};
    size_t end = s.find_last_not_of(whitespace);
    return s.substr(begin, end - begin + 1);
}

bool parseInt(std::string_view s, int64_t& out) {
    if (!s.empty() && s[0] == '+') s.remove_prefix(1);
    auto res = std::from_chars(s.data(), s.data() + s.size(), out);
    return !s.empty() && res.ec == std::errc() && res.ptr == s.data() + s.size();
}

bool parseFloat(std::string_view s, double& out) {
    if (!s.empty() && s[0] == '+') s.remove_prefix(1);
    auto res = std::from_chars(s.data(), s.data() + s.size(), out);
    return !s.empty() && res.ec == std::errc() && res.ptr == s.data() + s.size();
}

int64_t floatToInt(VM& vm, double d) {
    if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0))
        throw vm.runtimeError("number " + formatFloat(d) + " does not fit in an int");
    return static_cast<int64_t>(d);
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Length in bytes of the UTF-8 character starting at s[i].
size_t utf8Length(const std::string& s, size_t i) {
    size_t n = 1;
    while (i + n < s.size() && (static_cast<unsigned char>(s[i + n]) & 0xC0) == 0x80) n++;
    return n;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    for (;;) {
        size_t end = text.find('\n', start);
        std::string line = text.substr(start, end == std::string::npos ? end : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return lines;
}

ObjList* newList(VM& vm, std::vector<Value> items = {}) {
    ObjList* list = vm.alloc<ObjList>();
    list->items = std::move(items);
    vm.account(list->items.size() * sizeof(Value));
    return list;
}

// Ordering used by sort() when no comparator is given.
bool defaultLess(VM& vm, const Value& a, const Value& b) {
    if (a.isInt() && b.isInt()) return a.as.i < b.as.i;
    if (a.isNumber() && b.isNumber()) return a.toDouble() < b.toDouble();
    if (a.isString() && b.isString()) return asString(a)->chars < asString(b)->chars;
    throw vm.runtimeError("cannot compare " + vm.typeName(a) + " and " + vm.typeName(b) + " while sorting");
}

// Stable merge sort of items[lo, hi). Hand-written because the comparator is user code:
// it may throw or be inconsistent, which std::sort does not tolerate.
void mergeSort(std::vector<Value>& items, std::vector<Value>& scratch, size_t lo, size_t hi,
               const std::function<bool(const Value&, const Value&)>& less) {
    if (hi - lo < 2) return;
    size_t mid = lo + (hi - lo) / 2;
    mergeSort(items, scratch, lo, mid, less);
    mergeSort(items, scratch, mid, hi, less);

    size_t i = lo, j = mid, k = lo;
    while (i < mid && j < hi) scratch[k++] = less(items[j], items[i]) ? items[j++] : items[i++];
    while (i < mid) scratch[k++] = items[i++];
    while (j < hi) scratch[k++] = items[j++];
    std::copy(scratch.begin() + static_cast<std::ptrdiff_t>(lo), scratch.begin() + static_cast<std::ptrdiff_t>(hi),
              items.begin() + static_cast<std::ptrdiff_t>(lo));
}

int64_t integerPower(int64_t base, int64_t exponent) {
    uint64_t result = 1, b = static_cast<uint64_t>(base);
    for (uint64_t e = static_cast<uint64_t>(exponent); e != 0; e >>= 1) {
        if (e & 1) result *= b;
        b *= b;
    }
    return static_cast<int64_t>(result);
}

// ---- Global functions ----

void registerGlobals(VM& vm) {
    vm.defineNative("print", 0, -1, [](VM& vm, int argc, Value* args) -> Value {
        for (int i = 0; i < argc; i++) {
            if (i > 0) *vm.output << ' ';
            *vm.output << vm.stringify(args[i]);
        }
        *vm.output << '\n';
        return Value::null();
    });

    vm.defineNative("write", 0, -1, [](VM& vm, int argc, Value* args) -> Value {
        for (int i = 0; i < argc; i++) *vm.output << vm.stringify(args[i]);
        return Value::null();
    });

    vm.defineNative("input", 0, 1, [](VM& vm, int argc, Value* args) -> Value {
        if (argc > 0) *vm.output << vm.stringify(args[0]);
        vm.output->flush();
        std::string line;
        if (!std::getline(*vm.input, line)) return Value::null();
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Text piped in on Windows often starts with a UTF-8 byte order mark.
        if (line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
        return vm.str(std::move(line));
    });

    vm.defineNative("len", 1, 1, [](VM& vm, int, Value* args) -> Value {
        Value v = args[0];
        if (v.isString()) return Value::integer(static_cast<int64_t>(asString(v)->chars.size()));
        if (v.isObjType(ObjType::List)) return Value::integer(static_cast<int64_t>(asList(v)->items.size()));
        if (v.isObjType(ObjType::Map)) return Value::integer(static_cast<int64_t>(asMap(v)->map.size()));
        if (v.isObjType(ObjType::Range)) return Value::integer(asRange(v)->count());
        throw vm.runtimeError("a value of type " + vm.typeName(v) + " has no length");
    });

    vm.defineNative("str", 1, 1, [](VM& vm, int, Value* args) -> Value {
        if (args[0].isString()) return args[0];
        return vm.str(vm.stringify(args[0]));
    });

    vm.defineNative("int", 1, 1, [](VM& vm, int, Value* args) -> Value {
        Value v = args[0];
        if (v.isInt()) return v;
        if (v.isFloat()) return Value::integer(floatToInt(vm, std::trunc(v.as.d)));
        if (v.isBool()) return Value::integer(v.as.b ? 1 : 0);
        if (v.isString()) {
            std::string_view text = trimmed(asString(v)->chars);
            int64_t i;
            if (parseInt(text, i)) return Value::integer(i);
            double d;
            if (parseFloat(text, d) && std::isfinite(d)) return Value::integer(floatToInt(vm, std::trunc(d)));
            throw vm.runtimeError("cannot convert " + quoteString(asString(v)->chars) + " to an int");
        }
        throw vm.runtimeError("cannot convert a value of type " + vm.typeName(v) + " to an int");
    });

    vm.defineNative("float", 1, 1, [](VM& vm, int, Value* args) -> Value {
        Value v = args[0];
        if (v.isFloat()) return v;
        if (v.isInt()) return Value::number(static_cast<double>(v.as.i));
        if (v.isBool()) return Value::number(v.as.b ? 1.0 : 0.0);
        if (v.isString()) {
            double d;
            if (parseFloat(trimmed(asString(v)->chars), d)) return Value::number(d);
            throw vm.runtimeError("cannot convert " + quoteString(asString(v)->chars) + " to a float");
        }
        throw vm.runtimeError("cannot convert a value of type " + vm.typeName(v) + " to a float");
    });

    vm.defineNative("type", 1, 1, [](VM& vm, int, Value* args) -> Value {
        return Value::object(vm.intern(vm.typeName(args[0])));
    });

    vm.defineNative("clock", 0, 0, [](VM&, int, Value*) -> Value {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        return Value::number(std::chrono::duration<double>(now).count());
    });

    vm.defineNative("range", 1, 3, [](VM& vm, int argc, Value* args) -> Value {
        int64_t start = 0, end, step = 1;
        if (argc == 1) {
            end = vm.toInt(args[0], "range end");
        } else {
            start = vm.toInt(args[0], "range start");
            end = vm.toInt(args[1], "range end");
            if (argc == 3) step = vm.toInt(args[2], "range step");
        }
        if (step == 0) throw vm.runtimeError("range step cannot be zero");
        return Value::object(vm.alloc<ObjRange>(start, end, step, false));
    });

    // with(obj) { ... }: runs the block on obj, so that the names in it are obj's members.
    vm.defineNative("with", 2, 2, [](VM& vm, int, Value* args) -> Value {
        return vm.callOn(args[0], args[1], 0, nullptr);
    });

    vm.defineNative("assert", 1, 2, [](VM& vm, int argc, Value* args) -> Value {
        if (args[0].truthy()) return Value::null();
        throw vm.runtimeError(argc > 1 ? vm.stringify(args[1]) : "assertion failed");
    });

    vm.defineNative("args", 0, 0, [](VM& vm, int, Value*) -> Value {
        std::vector<Value> items;
        for (const std::string& arg : vm.scriptArgs) items.push_back(vm.str(arg));
        return Value::object(newList(vm, std::move(items)));
    });

    vm.defineNative("exit", 0, 1, [](VM& vm, int argc, Value* args) -> Value {
        throw AstraExit{argc > 0 ? static_cast<int>(vm.toInt(args[0], "exit code")) : 0};
    });

    vm.defineNative("chr", 1, 1, [](VM& vm, int, Value* args) -> Value {
        int64_t cp = vm.toInt(args[0], "code point");
        if (cp < 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            throw vm.runtimeError(std::to_string(cp) + " is not a valid Unicode code point");
        std::string s;
        appendUtf8(s, static_cast<uint32_t>(cp));
        return vm.str(std::move(s));
    });

    vm.defineNative("ord", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const std::string& s = vm.toStr(args[0], "ord() argument");
        if (s.empty()) throw vm.runtimeError("ord() of an empty string");
        unsigned char lead = static_cast<unsigned char>(s[0]);
        size_t n = utf8Length(s, 0);
        uint32_t cp = lead;
        if (n == 2) cp = lead & 0x1F;
        else if (n == 3) cp = lead & 0x0F;
        else if (n >= 4) cp = lead & 0x07;
        for (size_t i = 1; i < n && i < 4; i++) cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
        return Value::integer(cp);
    });
}

// ---- Math ----

void registerMath(VM& vm) {
    ObjClass* math = vm.alloc<ObjClass>(vm.intern("Math"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        math->statics.set(vm.intern(name),
                          Value::object(vm.alloc<ObjNative>(std::string("Math.") + name, minArgs, maxArgs, fn)));
    };

    math->statics.set(vm.intern("PI"), Value::number(3.14159265358979323846));
    math->statics.set(vm.intern("E"), Value::number(2.71828182845904523536));

#define ASTRA_MATH_UNARY(NAME, FN)                                           \
    def(NAME, 1, 1, [](VM& vm, int, Value* args) -> Value {                 \
        return Value::number(FN(vm.toNumber(args[0], "Math." NAME "() argument"))); \
    })
    ASTRA_MATH_UNARY("sqrt", std::sqrt);
    ASTRA_MATH_UNARY("sin", std::sin);
    ASTRA_MATH_UNARY("cos", std::cos);
    ASTRA_MATH_UNARY("tan", std::tan);
    ASTRA_MATH_UNARY("log", std::log);
    ASTRA_MATH_UNARY("exp", std::exp);
#undef ASTRA_MATH_UNARY

    def("abs", 1, 1, [](VM& vm, int, Value* args) -> Value {
        Value v = args[0];
        if (v.isInt()) return Value::integer(v.as.i < 0 ? static_cast<int64_t>(0 - static_cast<uint64_t>(v.as.i)) : v.as.i);
        return Value::number(std::fabs(vm.toNumber(v, "Math.abs() argument")));
    });

    def("floor", 1, 1, [](VM& vm, int, Value* args) -> Value {
        if (args[0].isInt()) return args[0];
        return Value::integer(floatToInt(vm, std::floor(vm.toNumber(args[0], "Math.floor() argument"))));
    });

    def("ceil", 1, 1, [](VM& vm, int, Value* args) -> Value {
        if (args[0].isInt()) return args[0];
        return Value::integer(floatToInt(vm, std::ceil(vm.toNumber(args[0], "Math.ceil() argument"))));
    });

    def("round", 1, 1, [](VM& vm, int, Value* args) -> Value {
        if (args[0].isInt()) return args[0];
        return Value::integer(floatToInt(vm, std::round(vm.toNumber(args[0], "Math.round() argument"))));
    });

    def("min", 1, -1, [](VM& vm, int argc, Value* args) -> Value {
        Value best = args[0];
        vm.toNumber(best, "Math.min() argument");
        for (int i = 1; i < argc; i++)
            if (vm.toNumber(args[i], "Math.min() argument") < best.toDouble()) best = args[i];
        return best;
    });

    def("max", 1, -1, [](VM& vm, int argc, Value* args) -> Value {
        Value best = args[0];
        vm.toNumber(best, "Math.max() argument");
        for (int i = 1; i < argc; i++)
            if (vm.toNumber(args[i], "Math.max() argument") > best.toDouble()) best = args[i];
        return best;
    });

    def("pow", 2, 2, [](VM& vm, int, Value* args) -> Value {
        if (args[0].isInt() && args[1].isInt() && args[1].as.i >= 0)
            return Value::integer(integerPower(args[0].as.i, args[1].as.i));
        return Value::number(std::pow(vm.toNumber(args[0], "Math.pow() base"), vm.toNumber(args[1], "Math.pow() exponent")));
    });

    def("random", 0, 0, [](VM& vm, int, Value*) -> Value {
        return Value::number(std::uniform_real_distribution<double>(0.0, 1.0)(vm.rng));
    });

    def("randomInt", 2, 2, [](VM& vm, int, Value* args) -> Value {
        int64_t lo = vm.toInt(args[0], "Math.randomInt() lower bound");
        int64_t hi = vm.toInt(args[1], "Math.randomInt() upper bound");
        if (lo > hi) throw vm.runtimeError("Math.randomInt() lower bound is greater than the upper bound");
        return Value::integer(std::uniform_int_distribution<int64_t>(lo, hi)(vm.rng));
    });

    def("seed", 1, 1, [](VM& vm, int, Value* args) -> Value {
        vm.rng.seed(static_cast<uint64_t>(vm.toInt(args[0], "Math.seed() argument")));
        return Value::null();
    });

    vm.defineGlobal("Math", Value::object(math));
}

// ---- File ----

void registerFile(VM& vm) {
    ObjClass* file = vm.alloc<ObjClass>(vm.intern("File"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        file->statics.set(vm.intern(name),
                          Value::object(vm.alloc<ObjNative>(std::string("File.") + name, minArgs, maxArgs, fn)));
    };

    def("read", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const std::string& path = vm.toStr(args[0], "file path");
        std::ifstream in(path, std::ios::binary);
        if (!in) throw vm.runtimeError("cannot read file '" + path + "'");
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return vm.str(std::move(text));
    });

    def("write", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const std::string& path = vm.toStr(args[0], "file path");
        std::string text = vm.stringify(args[1]);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out || !(out << text).flush()) throw vm.runtimeError("cannot write file '" + path + "'");
        return Value::null();
    });

    def("append", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const std::string& path = vm.toStr(args[0], "file path");
        std::string text = vm.stringify(args[1]);
        std::ofstream out(path, std::ios::binary | std::ios::app);
        if (!out || !(out << text).flush()) throw vm.runtimeError("cannot write file '" + path + "'");
        return Value::null();
    });

    def("exists", 1, 1, [](VM& vm, int, Value* args) -> Value {
        std::error_code ec;
        return Value::boolean(std::filesystem::exists(vm.toStr(args[0], "file path"), ec));
    });

    vm.defineGlobal("File", Value::object(file));
}

// ---- String methods ----

void registerStringMethods(VM& vm) {
    const BuiltinType T = BuiltinType::String;

    vm.defineMethod(T, "upper", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        std::string s = asString(self)->chars;
        for (char& c : s)
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        return vm.str(std::move(s));
    });

    vm.defineMethod(T, "lower", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        std::string s = asString(self)->chars;
        for (char& c : s)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return vm.str(std::move(s));
    });

    vm.defineMethod(T, "trim", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        return vm.str(std::string(trimmed(asString(self)->chars)));
    });

    // For text written over several lines in the source: trimIndent takes away the indentation
    // the lines share, and trimMargin everything up to and including a marker ("|" unless
    // another is given). Both drop a first and a last line that are blank.
    vm.defineMethod(T, "trimIndent", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        std::vector<std::string> lines = splitLines(asString(self)->chars);
        size_t indent = std::string::npos;
        for (const std::string& line : lines) {
            size_t first = line.find_first_not_of(" \t");
            if (first != std::string::npos) indent = std::min(indent, first);
        }
        std::string out;
        for (size_t i = 0; i < lines.size(); i++) {
            const bool blank = lines[i].find_first_not_of(" \t") == std::string::npos;
            if (blank && (i == 0 || i + 1 == lines.size())) continue;
            if (!out.empty() || i > 1 || (i == 1 && lines[0].find_first_not_of(" \t") != std::string::npos)) out += '\n';
            if (!blank) out += lines[i].substr(indent == std::string::npos ? 0 : indent);
        }
        return vm.str(std::move(out));
    });

    vm.defineMethod(T, "trimMargin", 0, 1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        const std::string marker = argc > 0 ? vm.toStr(args[0], "margin marker") : std::string("|");
        std::vector<std::string> lines = splitLines(asString(self)->chars);
        std::string out;
        bool any = false;
        for (size_t i = 0; i < lines.size(); i++) {
            const size_t first = lines[i].find_first_not_of(" \t");
            const bool blank = first == std::string::npos;
            if (blank && (i == 0 || i + 1 == lines.size())) continue;
            if (any) out += '\n';
            any = true;
            if (!blank && !marker.empty() && lines[i].compare(first, marker.size(), marker) == 0) out += lines[i].substr(first + marker.size());
            else out += lines[i];
        }
        return vm.str(std::move(out));
    });

    vm.defineMethod(T, "split", 0, 1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        const std::string& s = asString(self)->chars;
        std::vector<Value> parts;

        if (argc == 0) {
            // No separator: split on runs of whitespace.
            size_t i = 0;
            while (i < s.size()) {
                while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
                size_t begin = i;
                while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) i++;
                if (i > begin) parts.push_back(vm.str(s.substr(begin, i - begin)));
            }
        } else {
            const std::string& sep = vm.toStr(args[0], "separator");
            if (sep.empty()) {
                for (size_t i = 0; i < s.size();) {
                    size_t n = utf8Length(s, i);
                    parts.push_back(vm.str(s.substr(i, n)));
                    i += n;
                }
            } else {
                size_t begin = 0;
                for (;;) {
                    size_t at = s.find(sep, begin);
                    if (at == std::string::npos) break;
                    parts.push_back(vm.str(s.substr(begin, at - begin)));
                    begin = at + sep.size();
                }
                parts.push_back(vm.str(s.substr(begin)));
            }
        }
        return Value::object(newList(vm, std::move(parts)));
    });

    vm.defineMethod(T, "contains", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        return Value::boolean(asString(self)->chars.find(vm.toStr(args[0], "search text")) != std::string::npos);
    });

    vm.defineMethod(T, "startsWith", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        const std::string& s = asString(self)->chars;
        const std::string& prefix = vm.toStr(args[0], "prefix");
        return Value::boolean(s.compare(0, prefix.size(), prefix) == 0);
    });

    vm.defineMethod(T, "endsWith", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        const std::string& s = asString(self)->chars;
        const std::string& suffix = vm.toStr(args[0], "suffix");
        return Value::boolean(s.size() >= suffix.size() &&
                              s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0);
    });

    vm.defineMethod(T, "indexOf", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        size_t at = asString(self)->chars.find(vm.toStr(args[0], "search text"));
        return Value::integer(at == std::string::npos ? -1 : static_cast<int64_t>(at));
    });

    vm.defineMethod(T, "slice", 1, 2, [](VM& vm, Value self, int argc, Value* args) -> Value {
        const std::string& s = asString(self)->chars;
        size_t begin = clampPosition(vm.toInt(args[0], "slice start"), s.size());
        size_t end = argc > 1 ? clampPosition(vm.toInt(args[1], "slice end"), s.size()) : s.size();
        return vm.str(begin < end ? s.substr(begin, end - begin) : std::string());
    });

    vm.defineMethod(T, "replace", 2, 2, [](VM& vm, Value self, int, Value* args) -> Value {
        const std::string& s = asString(self)->chars;
        const std::string& from = vm.toStr(args[0], "text to replace");
        const std::string& to = vm.toStr(args[1], "replacement");
        if (from.empty()) return self;

        std::string result;
        size_t begin = 0;
        for (;;) {
            size_t at = s.find(from, begin);
            if (at == std::string::npos) break;
            result.append(s, begin, at - begin);
            result += to;
            begin = at + from.size();
        }
        result.append(s, begin, std::string::npos);
        return vm.str(std::move(result));
    });

    vm.defineMethod(T, "repeat", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        const std::string& s = asString(self)->chars;
        int64_t times = vm.toInt(args[0], "repeat count");
        if (times < 0) throw vm.runtimeError("repeat count cannot be negative");
        if (!s.empty() && static_cast<uint64_t>(times) > (uint64_t{1} << 30) / s.size())
            throw vm.runtimeError("repeated string would be too large");
        std::string result;
        result.reserve(s.size() * static_cast<size_t>(times));
        for (int64_t i = 0; i < times; i++) result += s;
        return vm.str(std::move(result));
    });
}

// ---- List methods ----

void registerListMethods(VM& vm) {
    const BuiltinType T = BuiltinType::List;

    vm.defineMethod(T, "push", 1, -1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        ObjList* list = asList(self);
        list->items.insert(list->items.end(), args, args + argc);
        vm.account(static_cast<size_t>(argc) * sizeof(Value));
        return Value::null();
    });

    vm.defineMethod(T, "pop", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        ObjList* list = asList(self);
        if (list->items.empty()) throw vm.runtimeError("pop() from an empty list");
        Value last = list->items.back();
        list->items.pop_back();
        return last;
    });

    vm.defineMethod(T, "insert", 2, 2, [](VM& vm, Value self, int, Value* args) -> Value {
        ObjList* list = asList(self);
        size_t at = clampPosition(vm.toInt(args[0], "insert position"), list->items.size());
        list->items.insert(list->items.begin() + static_cast<std::ptrdiff_t>(at), args[1]);
        vm.account(sizeof(Value));
        return Value::null();
    });

    vm.defineMethod(T, "removeAt", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        ObjList* list = asList(self);
        int64_t index = vm.toInt(args[0], "list index");
        int64_t n = static_cast<int64_t>(list->items.size());
        if (index < 0) index += n;
        if (index < 0 || index >= n)
            throw vm.runtimeError("list index " + std::to_string(args[0].as.i) + " is out of range (length " +
                                  std::to_string(n) + ")");
        Value removed = list->items[static_cast<size_t>(index)];
        list->items.erase(list->items.begin() + static_cast<std::ptrdiff_t>(index));
        return removed;
    });

    vm.defineMethod(T, "remove", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        std::vector<Value>& items = asList(self)->items;
        for (size_t i = 0; i < items.size(); i++) {
            if (vm.equals(items[i], args[0])) {
                items.erase(items.begin() + static_cast<std::ptrdiff_t>(i));
                return Value::boolean(true);
            }
        }
        return Value::boolean(false);
    });

    vm.defineMethod(T, "clear", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        asList(self)->items.clear();
        return Value::null();
    });

    vm.defineMethod(T, "contains", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        // By index: comparing may run an object's own == operator, which may change the list.
        const std::vector<Value>& items = asList(self)->items;
        for (size_t i = 0; i < items.size(); i++)
            if (vm.equals(items[i], args[0])) return Value::boolean(true);
        return Value::boolean(false);
    });

    vm.defineMethod(T, "indexOf", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        const std::vector<Value>& items = asList(self)->items;
        for (size_t i = 0; i < items.size(); i++)
            if (vm.equals(items[i], args[0])) return Value::integer(static_cast<int64_t>(i));
        return Value::integer(-1);
    });

    vm.defineMethod(T, "slice", 0, 2, [](VM& vm, Value self, int argc, Value* args) -> Value {
        const std::vector<Value>& items = asList(self)->items;
        size_t begin = argc > 0 ? clampPosition(vm.toInt(args[0], "slice start"), items.size()) : 0;
        size_t end = argc > 1 ? clampPosition(vm.toInt(args[1], "slice end"), items.size()) : items.size();
        std::vector<Value> part;
        if (begin < end)
            part.assign(items.begin() + static_cast<std::ptrdiff_t>(begin), items.begin() + static_cast<std::ptrdiff_t>(end));
        return Value::object(newList(vm, std::move(part)));
    });

    vm.defineMethod(T, "join", 0, 1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        ObjList* list = asList(self);
        std::string sep = argc > 0 ? vm.toStr(args[0], "separator") : std::string();
        std::string result;
        // Indexed and re-checked each time round: stringify may run a toString() that changes the list.
        for (size_t i = 0; i < list->items.size(); i++) {
            if (i > 0) result += sep;
            result += vm.stringify(list->items[i]);
        }
        return vm.str(std::move(result));
    });

    vm.defineMethod(T, "reverse", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        std::vector<Value>& items = asList(self)->items;
        std::reverse(items.begin(), items.end());
        return self;
    });

    vm.defineMethod(T, "sort", 0, 1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        ObjList* list = asList(self);
        // Sorts a copy, so a comparator that modifies the list cannot corrupt the sort.
        std::vector<Value> items = list->items;
        TempVector keepAlive(vm, items);
        std::vector<Value> scratch(items.size());

        std::function<bool(const Value&, const Value&)> less;
        if (argc == 0 || args[0].isNull()) {
            less = [&vm](const Value& a, const Value& b) { return defaultLess(vm, a, b); };
        } else {
            Value comparator = args[0];
            less = [&vm, comparator](const Value& a, const Value& b) {
                Value pair[2] = {a, b};
                Value order = vm.call(comparator, 2, pair);
                if (!order.isNumber()) throw vm.runtimeError("sort comparator must return a number, got " + vm.typeName(order));
                return order.toDouble() < 0;
            };
        }

        mergeSort(items, scratch, 0, items.size(), less);
        list->items = std::move(items);
        return self;
    });

    vm.defineMethod(T, "map", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        ObjList* list = asList(self);
        ObjList* result = newList(vm);
        TempRoot keepAlive(vm, Value::object(result));
        for (size_t i = 0; i < list->items.size(); i++) {
            Value item = list->items[i];
            result->items.push_back(vm.call(args[0], 1, &item));
        }
        vm.account(result->items.size() * sizeof(Value));
        return Value::object(result);
    });

    vm.defineMethod(T, "filter", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        ObjList* list = asList(self);
        ObjList* result = newList(vm);
        TempRoot keepAlive(vm, Value::object(result));
        for (size_t i = 0; i < list->items.size(); i++) {
            Value item = list->items[i];
            // Stored before the call so the item stays reachable even if the callback drops it from the list.
            result->items.push_back(item);
            if (!vm.call(args[0], 1, &item).truthy()) result->items.pop_back();
        }
        vm.account(result->items.size() * sizeof(Value));
        return Value::object(result);
    });

    vm.defineMethod(T, "reduce", 2, 2, [](VM& vm, Value self, int, Value* args) -> Value {
        ObjList* list = asList(self);
        TempRoot accumulator(vm, args[1]);
        for (size_t i = 0; i < list->items.size(); i++) {
            Value pair[2] = {accumulator.get(), list->items[i]};
            accumulator.set(vm.call(args[0], 2, pair));
        }
        return accumulator.get();
    });

    vm.defineMethod(T, "each", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        ObjList* list = asList(self);
        for (size_t i = 0; i < list->items.size(); i++) {
            Value item = list->items[i];
            vm.call(args[0], 1, &item);
        }
        return Value::null();
    });

    vm.defineMethod(T, "find", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        ObjList* list = asList(self);
        for (size_t i = 0; i < list->items.size(); i++) {
            TempRoot item(vm, list->items[i]);
            Value candidate = item.get();
            if (vm.call(args[0], 1, &candidate).truthy()) return item.get();
        }
        return Value::null();
    });
}

// ---- Map methods ----

void registerMapMethods(VM& vm) {
    const BuiltinType T = BuiltinType::Map;

    vm.defineMethod(T, "keys", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        std::vector<Value> keys;
        for (const ValueMap::Entry& entry : asMap(self)->map.entries())
            if (entry.live) keys.push_back(entry.key);
        return Value::object(newList(vm, std::move(keys)));
    });

    vm.defineMethod(T, "values", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        std::vector<Value> values;
        for (const ValueMap::Entry& entry : asMap(self)->map.entries())
            if (entry.live) values.push_back(entry.value);
        return Value::object(newList(vm, std::move(values)));
    });

    vm.defineMethod(T, "entries", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        std::vector<Value> pairs;
        for (const ValueMap::Entry& entry : asMap(self)->map.entries())
            if (entry.live) pairs.push_back(Value::object(newList(vm, {entry.key, entry.value})));
        return Value::object(newList(vm, std::move(pairs)));
    });

    vm.defineMethod(T, "has", 1, 1, [](VM&, Value self, int, Value* args) -> Value {
        return Value::boolean(asMap(self)->map.find(args[0]) != nullptr);
    });

    // The same under the name that `key in map` looks for.
    vm.defineMethod(T, "contains", 1, 1, [](VM&, Value self, int, Value* args) -> Value {
        return Value::boolean(asMap(self)->map.find(args[0]) != nullptr);
    });

    vm.defineMethod(T, "get", 1, 2, [](VM&, Value self, int argc, Value* args) -> Value {
        const Value* found = asMap(self)->map.find(args[0]);
        if (found != nullptr) return *found;
        return argc > 1 ? args[1] : Value::null();
    });

    vm.defineMethod(T, "remove", 1, 1, [](VM&, Value self, int, Value* args) -> Value {
        return Value::boolean(asMap(self)->map.remove(args[0]));
    });

    vm.defineMethod(T, "clear", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        asMap(self)->map.clear();
        return Value::null();
    });
}

// ---- Range methods ----

void registerRangeMethods(VM& vm) {
    const BuiltinType T = BuiltinType::Range;

    vm.defineMethod(T, "toList", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        ObjRange* range = asRange(self);
        int64_t count = range->count();
        if (count > 50000000) throw vm.runtimeError("range is too large to turn into a list");
        std::vector<Value> items;
        items.reserve(static_cast<size_t>(count));
        uint64_t next = static_cast<uint64_t>(range->start);
        for (int64_t i = 0; i < count; i++) {
            items.push_back(Value::integer(static_cast<int64_t>(next)));
            next += static_cast<uint64_t>(range->step);
        }
        return Value::object(newList(vm, std::move(items)));
    });

    vm.defineMethod(T, "contains", 1, 1, [](VM&, Value self, int, Value* args) -> Value {
        return Value::boolean(args[0].isInt() && asRange(self)->contains(args[0].as.i));
    });
}

}  // namespace

void registerBuiltins(VM& vm) {
    registerGlobals(vm);
    registerMath(vm);
    registerFile(vm);
    registerStringMethods(vm);
    registerListMethods(vm);
    registerMapMethods(vm);
    registerRangeMethods(vm);
    registerGui(vm);
    registerSpike(vm);
}

}  // namespace astra
