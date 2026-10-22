// The standard library: global functions, the Math and File classes, and the methods of
// the built-in types.

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>

#include "builtins.h"
#include "charset.h"
#include "run.h"
#include "vm.h"

namespace astra {

// A request for more memory than the machine could ever give, its memory and page file together,
// is refused at once, with the same error that running out gives; below that the allocator
// decides. (Asking anyway would also stop a program built with the address sanitizer, whose
// allocator does not fail in the ordinary way.)
void VM::checkRoomFor(uint64_t bytes) {
    static const uint64_t limit = [] {
        MEMORYSTATUSEX status{};
        status.dwLength = sizeof status;
        return GlobalMemoryStatusEx(&status) ? static_cast<uint64_t>(status.ullTotalPageFile) : UINT64_MAX;
    }();
    if (bytes > limit) throw outOfMemory();
}

// ---- Helpers (declared in builtins.h; stdlib.cpp uses them too) ----

// Clamps a possibly negative position into [0, length], as slice() bounds.
size_t clampPosition(int64_t position, size_t length) {
    return static_cast<size_t>(std::clamp<int64_t>(position, 0, static_cast<int64_t>(length)));
}

// Upper or lower case for all letters, not only the ASCII ones. The mapping is the
// operating system's for now; the text engine brings Unicode data of our own (M4).
std::string changeCase(const std::string& s, bool upper) {
    bool ascii = true;
    for (unsigned char c : s) ascii = ascii && c < 0x80;
    if (ascii) {
        std::string out = s;
        for (char& c : out) {
            if (upper && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
            if (!upper && c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return out;
    }
#ifdef _WIN32
    int wideLength = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(wideLength), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), wide.data(), wideLength);
    const DWORD how = upper ? LCMAP_UPPERCASE : LCMAP_LOWERCASE;
    int mappedLength = LCMapStringEx(LOCALE_NAME_INVARIANT, how, wide.data(), wideLength, nullptr, 0, nullptr, nullptr, 0);
    std::wstring mapped(static_cast<size_t>(mappedLength), L'\0');
    LCMapStringEx(LOCALE_NAME_INVARIANT, how, wide.data(), wideLength, mapped.data(), mappedLength, nullptr, nullptr, 0);
    int length = WideCharToMultiByte(CP_UTF8, 0, mapped.data(), mappedLength, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, mapped.data(), mappedLength, out.data(), length, nullptr, nullptr);
    return out;
#else
    std::string out = s;
    for (char& c : out) {
        if (upper && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        if (!upper && c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
#endif
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
        throw vm.runtimeError(Thrown::ArithmeticException, "number " + formatFloat(d) + " does not fit in an int");
    return static_cast<int64_t>(d);
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

ObjList* newList(VM& vm, std::vector<Value> items) {
    ObjList* list = vm.alloc<ObjList>();
    list->values() = std::move(items);
    vm.account(list->values().size() * sizeof(Value));
    return list;
}

// Ordering used by sort() when no comparator is given.
bool defaultLess(VM& vm, const Value& a, const Value& b) {
    if (a.isInt() && b.isInt()) return a.as.i < b.as.i;
    if (a.isNumber() && b.isNumber()) return a.toDouble() < b.toDouble();
    if (a.isString() && b.isString()) return compareText(asString(a), asString(b)) < 0;
    // Chars by their numbers; a byte as the int it holds.
    if (a.isChar() && b.isChar()) return a.as.i < b.as.i;
    if ((a.isByte() || a.isInt()) && (b.isByte() || b.isInt())) return a.as.i < b.as.i;
    if ((a.isByte() || a.isNumber()) && (b.isByte() || b.isNumber()))
        return (a.isFloat() ? a.as.d : static_cast<double>(a.as.i)) < (b.isFloat() ? b.as.d : static_cast<double>(b.as.i));
    // An object whose class says how it compares, with compareTo (Kotlin's Comparable): a Duration.
    if (const ObjClass* cls = classOf(a); cls != nullptr && cls->methods.find(vm.names.compareTo) != nullptr) {
        const Value method = vm.memberGet(a, vm.names.compareTo);
        TempRoot keep(vm, method);
        const Value order = vm.call(method, 1, &b);
        if (order.isInt()) return order.as.i < 0;
    }
    throw vm.runtimeError(Thrown::ClassCastException, "cannot compare " + vm.typeName(a) + " and " + vm.typeName(b) + " while sorting");
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

// An int to a power of 0 or more, an error where it does not fit in an int (D27).
int64_t integerPower(VM& vm, int64_t base, int64_t exponent) {
    int64_t result = 1, b = base;
    bool bOverflowed = false;
    for (int64_t e = exponent; e != 0; e >>= 1) {
        if (e & 1) {
            if (bOverflowed || mulOverflows(result, b, result))
                throw vm.runtimeError(Thrown::ArithmeticException, std::to_string(base) + " to the power " + std::to_string(exponent) + " does not fit in an int");
        }
        if (e > 1 && !bOverflowed) bOverflowed = mulOverflows(b, b, b);
    }
    return result;
}

namespace {

// ---- Global functions ----

void registerGlobals(VM& vm) {
    // __nameParams(type, name, params): what a built-in's parameters are called, from its
    // declaration in src/std/builtins.astra. The standard library calls it as it loads; the
    // type is "" for a function.
    vm.defineNative("__nameParams", 3, 3, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isString() || !args[1].isString() || !args[2].isObjType(ObjType::List)) throw vm.runtimeError(Thrown::InternalError, "__nameParams: bad arguments");
        std::vector<std::string> params;
        for (const Value& p : vm.listCopy(asList(args[2])))
            if (p.isString()) params.push_back(asString(p)->utf8());
        const std::string& type = asString(args[0])->utf8();
        BuiltinType builtin;
        if (type.empty()) {
            vm.nameFunctionParams(asString(args[1])->utf8(), params);
        } else if (VM::builtinTypeNamed(type, builtin)) {
            vm.nameParams(builtin, asString(args[1])->utf8(), params);
        } else if (ObjNative* member = vm.builtinStatic(type, asString(args[1])->utf8())) {
            // A function of an object the C++ provides (`Math.pow`).
            if (params.size() > member->params.size()) member->params = params;
        }
        return Value::null();
    });
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

    // Sends on whatever print and write have gathered. Output to a file or to another program
    // is otherwise sent in pieces, and at the latest when the program ends.
    vm.defineNative("flush", 0, 0, [](VM& vm, int, Value*) -> Value {
        vm.output->flush();
        return Value::null();
    });

    vm.defineNative("input", 0, 1, [](VM& vm, int argc, Value* args) -> Value {
        if (argc > 0) *vm.output << vm.stringify(args[0]);
        vm.output->flush();
        std::string line;
        bool got;
        {
            LockReleased released(vm);  // other threads go on while this one waits for a line
            got = static_cast<bool>(std::getline(*vm.input, line));
        }
        if (!got) return Value::null();
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Text piped in on Windows often starts with a UTF-8 byte order mark.
        if (line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
        return vm.str(std::move(line));
    });





    vm.defineNative("type", 1, 1, [](VM& vm, int, Value* args) -> Value {
        return Value::object(vm.intern(vm.typeName(args[0])));
    });

    vm.defineNative("clock", 0, 0, [](VM&, int, Value*) -> Value {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        return Value::number(std::chrono::duration<double>(now).count());
    });

    vm.defineNative("Range", 1, 3, [](VM& vm, int argc, Value* args) -> Value {
        int64_t start = 0, end, step = 1;
        if (argc == 1) {
            end = vm.toInt(args[0], "range end");
        } else {
            start = vm.toInt(args[0], "range start");
            end = vm.toInt(args[1], "range end");
            if (argc == 3) step = vm.toInt(args[2], "range step");
        }
        if (step == 0) throw vm.runtimeError(Thrown::IllegalArgumentException, "range step cannot be zero");
        return Value::object(vm.alloc<ObjRange>(start, end, step, false));
    });

    // Throws what it is given as it is: the standard library's, for a failure caught where it was
    // thrown and given again later, whose class the compiler cannot know (D35).
    vm.defineNative("__rethrow", 1, 1, [](VM&, int, Value* args) -> Value {
        AstraThrow thrown;
        thrown.value = args[0];
        throw thrown;
    });

    vm.defineNative("assert", 1, 2, [](VM& vm, int argc, Value* args) -> Value {
        if (args[0].truthy()) return Value::null();
        throw vm.runtimeError(Thrown::AssertionError, argc > 1 ? vm.stringify(args[1]) : "assertion failed");
    });

    vm.defineNative("args", 0, 0, [](VM& vm, int, Value*) -> Value {
        std::vector<Value> items;
        for (const std::string& arg : vm.scriptArgs) items.push_back(vm.str(arg));
        return Value::object(newList(vm, std::move(items)));
    });

    vm.defineNative("exit", 0, 1, [](VM& vm, int argc, Value* args) -> Value {
        throw AstraExit{argc > 0 ? static_cast<int>(vm.toInt(args[0], "exit code")) : 0};
    });

    // A value's own hash, as a map takes it, at 32 bits: what hashCode() gives for a value
    // whose class has none of its own (D26).
    vm.defineNative("__hashCode", 1, 1, [](VM&, int, Value* args) -> Value {
        const size_t one = args[0].isNull() ? 0 : keyHash(args[0]);
        return Value::integer(static_cast<int32_t>(static_cast<uint32_t>(one ^ (one >> 32))));
    });

    // The hash of several values together, as Java's Objects.hash works it out, wrapping at 32
    // bits inside, so a class's hash needs no `31 * h + ...` of its own (D148).
    vm.defineNative("hash", 0, -1, [](VM&, int argc, Value* args) -> Value {
        uint32_t result = 1;
        for (int i = 0; i < argc; i++) {
            const size_t one = args[i].isNull() ? 0 : keyHash(args[i]);
            result = 31 * result + static_cast<uint32_t>(one ^ (one >> 32));
        }
        return Value::integer(static_cast<int32_t>(result));
    });
}

// ---- Math ----

// A byte as the int it is, for the functions that keep an int an int; anything else as it is.
static Value asWideInt(Value v) { return v.isByte() ? Value::integer(v.as.i) : v; }

void registerMath(VM& vm) {
    ObjClass* math = vm.alloc<ObjClass>(vm.intern("Math"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        math->statics.set(vm.intern(name),
                          Value::object(vm.alloc<ObjNative>(std::string("Math.") + name, minArgs, maxArgs, fn)));
    };

    math->statics.set(vm.intern("pi"), Value::number(3.14159265358979323846));
    math->statics.set(vm.intern("e"), Value::number(2.71828182845904523536));

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
    ASTRA_MATH_UNARY("log10", std::log10);
    ASTRA_MATH_UNARY("log2", std::log2);
    ASTRA_MATH_UNARY("asin", std::asin);
    ASTRA_MATH_UNARY("acos", std::acos);
    ASTRA_MATH_UNARY("atan", std::atan);
    ASTRA_MATH_UNARY("trunc", std::trunc);
#undef ASTRA_MATH_UNARY

    def("abs", 1, 1, [](VM& vm, int, Value* args) -> Value {
        args[0] = asWideInt(args[0]);   // a byte is an int here
        Value v = args[0];
        if (v.isInt()) {
            if (v.as.i == INT64_MIN) throw vm.overflowError(Op::Sub, 0, v.as.i);  // (D27)
            return Value::integer(v.as.i < 0 ? -v.as.i : v.as.i);
        }
        return Value::number(std::fabs(vm.toNumber(v, "Math.abs() argument")));
    });

    def("floor", 1, 1, [](VM& vm, int, Value* args) -> Value {
        args[0] = asWideInt(args[0]);   // a byte is an int here
        if (args[0].isInt()) return args[0];
        return Value::integer(floatToInt(vm, std::floor(vm.toNumber(args[0], "Math.floor() argument"))));
    });

    def("ceil", 1, 1, [](VM& vm, int, Value* args) -> Value {
        args[0] = asWideInt(args[0]);   // a byte is an int here
        if (args[0].isInt()) return args[0];
        return Value::integer(floatToInt(vm, std::ceil(vm.toNumber(args[0], "Math.ceil() argument"))));
    });

    def("round", 1, 1, [](VM& vm, int, Value* args) -> Value {
        args[0] = asWideInt(args[0]);   // a byte is an int here
        if (args[0].isInt()) return args[0];
        return Value::integer(floatToInt(vm, std::round(vm.toNumber(args[0], "Math.round() argument"))));
    });

    // (Op::Numeric does min and max of two, and the rest, as these do: VM::numeric below.)
    def("min", 1, -1, [](VM& vm, int argc, Value* args) -> Value {
        Value best = args[0];
        vm.toNumber(best, "Math.min() argument");
        for (int i = 1; i < argc; i++)
            if (vm.toNumber(args[i], "Math.min() argument") < vm.toNumber(best, "Math.min() argument")) best = args[i];
        return best.isByte() ? Value::integer(best.as.i) : best;
    });

    def("max", 1, -1, [](VM& vm, int argc, Value* args) -> Value {
        Value best = args[0];
        vm.toNumber(best, "Math.max() argument");
        for (int i = 1; i < argc; i++)
            if (vm.toNumber(args[i], "Math.max() argument") > vm.toNumber(best, "Math.max() argument")) best = args[i];
        return best.isByte() ? Value::integer(best.as.i) : best;
    });

    def("pow", 2, 2, [](VM& vm, int, Value* args) -> Value {
        args[0] = asWideInt(args[0]);   // a byte is an int here
        args[1] = asWideInt(args[1]);
        if (args[0].isInt() && args[1].isInt() && args[1].as.i >= 0)
            return Value::integer(integerPower(vm, args[0].as.i, args[1].as.i));
        return Value::number(std::pow(vm.toNumber(args[0], "Math.pow() base"), vm.toNumber(args[1], "Math.pow() exponent")));
    });

    def("random", 0, 0, [](VM& vm, int, Value*) -> Value {
        return Value::number(std::uniform_real_distribution<double>(0.0, 1.0)(vm.rng));
    });

    def("randomInt", 2, 2, [](VM& vm, int, Value* args) -> Value {
        int64_t lo = vm.toInt(args[0], "Math.randomInt() lower bound");
        int64_t hi = vm.toInt(args[1], "Math.randomInt() upper bound");
        if (lo > hi) throw vm.runtimeError(Thrown::IllegalArgumentException, "Math.randomInt() lower bound is greater than the upper bound");
        return Value::integer(std::uniform_int_distribution<int64_t>(lo, hi)(vm.rng));
    });

    def("seed", 1, 1, [](VM& vm, int, Value* args) -> Value {
        vm.rng.seed(static_cast<uint64_t>(vm.toInt(args[0], "Math.seed() argument")));
        return Value::null();
    });

    def("atan2", 2, 2, [](VM& vm, int, Value* args) -> Value {
        return Value::number(std::atan2(vm.toNumber(args[0], "Math.atan2() argument"), vm.toNumber(args[1], "Math.atan2() argument")));
    });
    def("hypot", 2, 2, [](VM& vm, int, Value* args) -> Value {
        return Value::number(std::hypot(vm.toNumber(args[0], "Math.hypot() argument"), vm.toNumber(args[1], "Math.hypot() argument")));
    });
    def("sign", 1, 1, [](VM& vm, int, Value* args) -> Value {
        args[0] = asWideInt(args[0]);   // a byte is an int here
        if (args[0].isInt()) return Value::integer((args[0].as.i > 0) - (args[0].as.i < 0));
        const double d = vm.toNumber(args[0], "Math.sign() argument");
        return Value::number(d > 0 ? 1.0 : d < 0 ? -1.0 : d);
    });

    vm.defineGlobal("Math", Value::object(math));
}

// ---- File ----

// ---- String methods ----

void registerStringMethods(VM& vm) {
    const BuiltinType T = BuiltinType::String;

    vm.defineMethod(T, "upper", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        return vm.str(changeCase(asString(self)->utf8(), true));
    });

    vm.defineMethod(T, "lower", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        return vm.str(changeCase(asString(self)->utf8(), false));
    });

    vm.defineMethod(T, "trim", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        return vm.str(std::string(trimmed(asString(self)->utf8())));
    });

    // For text written over several lines in the source: trimIndent takes away the indentation
    // the lines share, and trimMargin everything up to and including a marker ("|" unless
    // another is given). Both drop a first and a last line that are blank.
    vm.defineMethod(T, "trimIndent", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        std::vector<std::string> lines = splitLines(asString(self)->utf8());
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
        std::vector<std::string> lines = splitLines(asString(self)->utf8());
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
        if (argc > 0 && args[0].isChar()) args[0] = vm.characterString(static_cast<uint32_t>(args[0].as.i));  // split(',') (D25)
        // On the characters themselves, each part at the width it needs (D20).
        const ObjString* text = asString(self);
        const size_t length = text->length;
        std::vector<Value> parts;

        if (argc == 0) {
            // No separator: split on runs of whitespace.
            auto space = [text](size_t i) {
                const uint32_t c = text->at(i);
                return c < 0x80 && std::isspace(static_cast<int>(c));
            };
            size_t i = 0;
            while (i < length) {
                while (i < length && space(i)) i++;
                size_t begin = i;
                while (i < length && !space(i)) i++;
                if (i > begin) parts.push_back(vm.str(text->slice(begin, i)));
            }
        } else {
            const ObjString* separator = vm.toText(args[0], "separator");
            if (separator->length == 0) {
                for (size_t i = 0; i < length; i++) parts.push_back(vm.characterString(text->at(i)));
            } else {
                size_t begin = 0;
                for (;;) {
                    const int64_t at = text->find(separator, begin);
                    if (at < 0) break;
                    parts.push_back(vm.str(text->slice(begin, static_cast<size_t>(at))));
                    begin = static_cast<size_t>(at) + separator->length;
                }
                parts.push_back(vm.str(text->slice(begin, length)));
            }
        }
        return Value::object(newList(vm, std::move(parts)));
    });


    vm.defineMethod(T, "repeat", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        const std::string& s = asString(self)->utf8();
        int64_t times = vm.toInt(args[0], "repeat count");
        if (times < 0) throw vm.runtimeError(Thrown::IllegalArgumentException, "repeat count cannot be negative");
        // As large as memory allows; a size past what can be counted cannot be asked for at all.
        if (!s.empty() && static_cast<uint64_t>(times) > SIZE_MAX / s.size()) throw vm.outOfMemory();
        vm.checkRoomFor(static_cast<uint64_t>(times) * s.size());
        std::string result;
        result.reserve(s.size() * static_cast<size_t>(times));
        for (int64_t i = 0; i < times; i++) result += s;
        return vm.str(std::move(result));
    });
}

// ---- List methods ----

void registerListMethods(VM& vm) {
    const BuiltinType T = BuiltinType::List;

    vm.defineMethod(T, "insert", 2, 2, [](VM& vm, Value self, int, Value* args) -> Value {
        ObjList* list = asList(self);
        const int64_t at = vm.toInt(args[0], "insert position");
        if (at < 0 || static_cast<uint64_t>(at) > list->size()) throw vm.outOfRange("insert position", args[0], list->size());
        vm.listInsert(list, static_cast<size_t>(at), &args[1], 1);
        return Value::null();
    });

    vm.defineMethod(T, "removeAt", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        ObjList* list = asList(self);
        size_t index;
        if (!vm.position(args[0], list->size(), index, "list index")) throw vm.outOfRange("list index", args[0], list->size());
        const Value removed = vm.listGet(list, index);
        vm.listErase(list, index, 1);
        return removed;
    });

    vm.defineMethod(T, "remove", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        ObjList* list = asList(self);
        for (size_t i = 0; i < list->size(); i++) {
            if (vm.equals(vm.listGet(list, i), args[0])) {
                vm.listErase(list, i, 1);
                return Value::boolean(true);
            }
        }
        return Value::boolean(false);
    });

    vm.defineMethod(T, "clear", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        vm.listClear(asList(self));
        return Value::null();
    });

    vm.defineMethod(T, "contains", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        // By index: comparing may run an object's own == operator, which may change the list.
        ObjList* list = asList(self);
        for (size_t i = 0; i < list->size(); i++)
            if (vm.equals(vm.listGet(list, i), args[0])) return Value::boolean(true);
        return Value::boolean(false);
    });

    vm.defineMethod(T, "indexOf", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        ObjList* list = asList(self);
        for (size_t i = 0; i < list->size(); i++)
            if (vm.equals(vm.listGet(list, i), args[0])) return Value::integer(static_cast<int64_t>(i));
        return Value::integer(-1);
    });


    // The items as text (Kotlin's joinToString, D74): a separator between them, a prefix and a
    // postfix around them, at most `limit` of them and then `truncated`, each as `transform`
    // makes it or as print shows it.
    vm.defineMethod(T, "join", 0, 6, [](VM& vm, Value self, int argc, Value* args) -> Value {
        ObjList* list = asList(self);
        auto given = [&](int i) { return i < argc && !args[i].isNull(); };
        // Copied before any call: running the transform may move the stack the arguments are on.
        const std::string separator = given(0) ? vm.toStr(args[0], "separator") : ", ";
        const std::string prefix = given(1) ? vm.toStr(args[1], "prefix") : "";
        const std::string postfix = given(2) ? vm.toStr(args[2], "postfix") : "";
        const int64_t limit = given(3) ? vm.toInt(args[3], "limit") : -1;
        const std::string truncated = given(4) ? vm.toStr(args[4], "truncated") : "...";
        const Value transform = given(5) ? args[5] : Value::null();
        std::string result = prefix;
        // Indexed and re-checked each time round: a toString() or the transform may change the list.
        for (size_t i = 0; i < list->size(); i++) {
            if (i > 0) result += separator;
            if (limit >= 0 && static_cast<int64_t>(i) >= limit) {
                result += truncated;
                break;
            }
            Value item = vm.listGet(list, i);
            TempRoot keep(vm, item);
            if (!transform.isNull()) item = vm.call(transform, 1, &item);
            TempRoot keepShown(vm, item);
            result += vm.stringify(item);
        }
        return vm.str(result + postfix);
    });

    vm.defineMethod(T, "reverse", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        ObjList* list = asList(self);
        for (size_t i = 0, j = list->size(); i + 1 < j; i++, j--) vm.listSwap(list, i, j - 1);
        return self;
    });

    vm.defineMethod(T, "sort", 0, 1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        ObjList* list = asList(self);
        // Sorts a copy, so a comparator that modifies the list cannot corrupt the sort. A packed
        // list's items are taken out once, sorted, and each written back once: the items move
        // once, however many comparisons there are (value-lists.html, section 9).
        std::vector<Value> items = vm.listCopy(list);
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
                if (!order.isNumber()) throw vm.runtimeError(Thrown::ClassCastException, "sort comparator must return a number, got " + vm.typeName(order));
                return order.toDouble() < 0;
            };
        }

        mergeSort(items, scratch, 0, items.size(), less);
        if (!list->packed()) {
            list->values() = std::move(items);
        } else {
            if (list->size() != items.size()) throw vm.runtimeError(Thrown::ConcurrentModificationException, "the list was changed while it was sorted");
            for (size_t i = 0; i < items.size(); i++) vm.listSet(list, i, items[i]);
        }
        return self;
    });

    // map, filter, reduce, each and find are in src/std/list.astra: a function given to a
    // method written in Astra runs as part of the program, where a task can be set aside.
}

// ---- Map methods ----

void registerMapMethods(VM& vm) {
    const BuiltinType T = BuiltinType::Map;





    // The same under the name that `key in map` looks for.
    // A whole entry, `"a" to 1` or a MapEntry, with its key and its value (D140); a key is
    // asked of the keys.
    vm.defineMethod(T, "contains", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        const Value entry = args[0];
        const bool whole = entry.isObjType(ObjType::Value) || entry.isSmall() || entry.isRef();
        if (!whole)
            throw vm.runtimeError(Thrown::IllegalArgumentException, "map.contains(...) asks for a whole entry, (key to value); for a key write map.keys.contains(" +
                                  vm.repr(entry) + "), or " + vm.repr(entry) + " in map.keys");
        const Value key = vm.componentOf(entry, 0);
        TempRoot keepKey(vm, key);
        const Value value = vm.componentOf(entry, 1);
        const Value* held = asMap(self)->map.find(key);
        return Value::boolean(held != nullptr && vm.equals(*held, value));
    });

    vm.defineMethod(T, "get", 1, 2, [](VM&, Value self, int argc, Value* args) -> Value {
        const Value* found = asMap(self)->map.find(args[0]);
        if (found != nullptr) return *found;
        return argc > 1 ? args[1] : Value::null();
    });

    // Gives the value that was there, or null.
    vm.defineMethod(T, "remove", 1, 1, [](VM&, Value self, int, Value* args) -> Value {
        const Value* found = asMap(self)->map.find(args[0]);
        if (found == nullptr) return Value::null();
        Value removed = *found;
        asMap(self)->map.remove(args[0]);
        return removed;
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
        if (static_cast<uint64_t>(count) > SIZE_MAX / sizeof(Value)) throw vm.outOfMemory();
        vm.checkRoomFor(static_cast<uint64_t>(count) * sizeof(Value));
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

// ---- bytes ----

// A byte given as one, or as an int from 0 to 255 by code that does not say its types.
uint8_t byteArgument(VM& vm, Value v) {
    if (v.isByte()) return static_cast<uint8_t>(v.as.i);
    if (!v.isInt() || v.as.i < 0 || v.as.i > 255) throw vm.runtimeError(Thrown::IllegalArgumentException, "a byte is a whole number from 0 to 255, not " + vm.stringify(v));
    return static_cast<uint8_t>(v.as.i);
}

// Where a number of `size` bytes starts in `data`, at `at`: an error past the end.
size_t binaryAt(VM& vm, const std::vector<uint8_t>& data, Value at, size_t size) {
    const int64_t i = vm.toInt(at, "position");
    if (i < 0 || static_cast<uint64_t>(i) > data.size() || data.size() - static_cast<size_t>(i) < size)
        throw vm.runtimeError(Thrown::IndexOutOfBoundsException, "a " + std::to_string(size * 8) + "-bit number at " + std::to_string(i) + " runs past the end of " +
                              std::to_string(data.size()) + " bytes");
    return static_cast<size_t>(i);
}

// `size` bytes from `data` at `at`, as one number, least significant byte first unless `big`.
uint64_t readBinary(const std::vector<uint8_t>& data, size_t at, size_t size, bool big) {
    uint64_t n = 0;
    for (size_t k = 0; k < size; k++) n |= static_cast<uint64_t>(data[at + (big ? size - 1 - k : k)]) << (8 * k);
    return n;
}

void writeBinary(std::vector<uint8_t>& data, size_t at, size_t size, bool big, uint64_t n) {
    for (size_t k = 0; k < size; k++) data[at + (big ? size - 1 - k : k)] = static_cast<uint8_t>(n >> (8 * k));
}

enum class Kind { Signed, Unsigned, Float };

template <size_t Size, Kind kind>
Value readNumber(VM& vm, Value self, int argc, Value* args) {
    const std::vector<uint8_t>& data = asBytes(self)->data;
    const uint64_t bits = readBinary(data, binaryAt(vm, data, args[0], Size), Size, argc > 1 && args[1].truthy());
    if constexpr (kind == Kind::Float) {
        float f;
        const uint32_t word = static_cast<uint32_t>(bits);
        std::memcpy(&f, &word, sizeof f);
        return Value::number(f);
    } else if constexpr (kind == Kind::Signed) {
        return Value::integer(Size == 2 ? static_cast<int16_t>(bits) : static_cast<int32_t>(bits));
    } else {
        return Value::integer(static_cast<int64_t>(bits));
    }
}

template <size_t Size, Kind kind>
Value writeNumber(VM& vm, Value self, int argc, Value* args) {
    std::vector<uint8_t>& data = asBytes(self)->data;
    const size_t at = binaryAt(vm, data, args[0], Size);
    uint64_t bits;
    if constexpr (kind == Kind::Float) {
        const float f = static_cast<float>(vm.toNumber(args[1], "number"));
        uint32_t word;
        std::memcpy(&word, &f, sizeof word);
        bits = word;
    } else {
        const int64_t n = vm.toInt(args[1], "number");
        const int64_t low = kind == Kind::Unsigned ? 0 : Size == 2 ? -32768 : -2147483648LL;
        const int64_t high = kind == Kind::Unsigned ? (Size == 2 ? 65535 : 4294967295LL) : Size == 2 ? 32767 : 2147483647;
        if (n < low || n > high)
            throw vm.runtimeError(Thrown::ArithmeticException, std::to_string(n) + " does not fit in " + std::to_string(Size * 8) + " bits " +
                                  (kind == Kind::Unsigned ? "unsigned" : "signed") + ", which hold " + std::to_string(low) + " to " +
                                  std::to_string(high));
        bits = static_cast<uint64_t>(n);
    }
    writeBinary(data, at, Size, argc > 2 && args[2].truthy(), bits);
    return self;
}

void registerBytes(VM& vm) {
    // bytes(16): sixteen zeros. bytes([1, 2, 255]) or bytes(other): a copy.
    vm.defineNative("Bytes", 1, 1, [](VM& vm, int, Value* args) -> Value {
        ObjBytes* made = vm.alloc<ObjBytes>();
        Value from = args[0];
        if (from.isInt()) {
            if (from.as.i < 0) throw vm.runtimeError(Thrown::IllegalArgumentException, "bytes() cannot make a negative number of bytes");
            vm.checkRoomFor(static_cast<uint64_t>(from.as.i));
            made->data.assign(static_cast<size_t>(from.as.i), 0);
        } else if (from.isObjType(ObjType::List)) {
            for (Value item : vm.listCopy(asList(from))) made->data.push_back(byteArgument(vm, item));
        } else if (from.isObjType(ObjType::Bytes)) {
            made->data = asBytes(from)->data;
        } else {
            throw vm.runtimeError(Thrown::IllegalArgumentException, "bytes() takes a size, a list of numbers or other bytes, not a value of type " + vm.typeName(from) +
                                  (from.isString() ? "; text becomes bytes with toBytes()" : ""));
        }
        vm.account(made->data.size());
        return Value::object(made);
    });

    const BuiltinType T = BuiltinType::Bytes;


    vm.defineMethod(T, "toList", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        std::vector<Value> items;
        for (uint8_t b : asBytes(self)->data) items.push_back(Value::byte(b));
        return Value::object(newList(vm, std::move(items)));
    });

    // The text these bytes spell, in UTF-8 or the character set given (D127).
    vm.defineMethod(T, "decodeToString", 0, 1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        const std::vector<uint8_t>& data = asBytes(self)->data;
        const Charset cs = charsetOf(vm, argc > 0 ? args[0] : Value::null());
        const std::string_view raw(reinterpret_cast<const char*>(data.data()), data.size());
        return vm.str(cs == Charset::Utf8 ? std::string(raw) : decodeText(raw, cs));
    });

    vm.defineMethod(T, "fill", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        const uint8_t value = byteArgument(vm, args[0]);
        std::vector<uint8_t>& data = asBytes(self)->data;
        std::fill(data.begin(), data.end(), value);
        return self;
    });

    vm.defineMethod(T, "indexOf", 1, 1, [](VM&, Value self, int, Value* args) -> Value {
        const std::vector<uint8_t>& data = asBytes(self)->data;
        if (args[0].isInt() || args[0].isByte())
            for (size_t i = 0; i < data.size(); i++)
                if (data[i] == args[0].as.i) return Value::integer(static_cast<int64_t>(i));
        return Value::integer(-1);
    });

    vm.defineMethod(T, "contains", 1, 1, [](VM&, Value self, int, Value* args) -> Value {
        const std::vector<uint8_t>& data = asBytes(self)->data;
        return Value::boolean((args[0].isInt() || args[0].isByte()) && std::find(data.begin(), data.end(), args[0].as.i) != data.end());
    });


    // 16- and 32-bit numbers in the bytes (D19, L0): read as an int (a float for float32) and
    // written, each at a position, least significant byte first unless `bigEndian`.
    vm.defineMethod(T, "int16", 1, 2, readNumber<2, Kind::Signed>);
    vm.defineMethod(T, "uint16", 1, 2, readNumber<2, Kind::Unsigned>);
    vm.defineMethod(T, "int32", 1, 2, readNumber<4, Kind::Signed>);
    vm.defineMethod(T, "uint32", 1, 2, readNumber<4, Kind::Unsigned>);
    vm.defineMethod(T, "float32", 1, 2, readNumber<4, Kind::Float>);
    vm.defineMethod(T, "putInt16", 2, 3, writeNumber<2, Kind::Signed>);
    vm.defineMethod(T, "putUint16", 2, 3, writeNumber<2, Kind::Unsigned>);
    vm.defineMethod(T, "putInt32", 2, 3, writeNumber<4, Kind::Signed>);
    vm.defineMethod(T, "putUint32", 2, 3, writeNumber<4, Kind::Unsigned>);
    vm.defineMethod(T, "putFloat32", 2, 3, writeNumber<4, Kind::Float>);

    // Text as bytes, in UTF-8 or the character set given (D127).
    vm.defineMethod(BuiltinType::String, "toBytes", 0, 1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        const Charset cs = charsetOf(vm, argc > 0 ? args[0] : Value::null());
        const std::string& utf8 = asString(self)->utf8();
        const std::string s = cs == Charset::Utf8 ? utf8 : encodeText(utf8, cs);
        ObjBytes* made = vm.alloc<ObjBytes>();
        made->data.assign(s.begin(), s.end());
        vm.account(made->data.size());
        return Value::object(made);
    });
}

void registerBuiltins(VM& vm) {
    registerGlobals(vm);
    registerMath(vm);
    registerStringMethods(vm);
    registerListMethods(vm);
    registerMapMethods(vm);
    registerRangeMethods(vm);
    registerBytes(vm);
    registerStdlib(vm);
    registerReflection(vm);
    registerThreads(vm);
    registerIo(vm);
    registerGrid(vm);
    registerWorkers(vm);
    registerParallel(vm);
    // A built program that reaches neither Jade nor the text engine has their code left out of
    // its file (M6.5 part 5): their names are then not there either.
    if (windowCodePresent()) {
        registerText(vm);
        // Windows belong to the main program: a worker has none.
        if (!vm.isWorker()) registerJade(vm);
    }

    // For parallel sections: which methods change what they are called on, and which
    // built-in functions wait, or work on something the whole program has one of.
    vm.markMutating(BuiltinType::List, {"insert", "removeAt", "remove", "clear", "reverse", "sort", "add", "addAll", "set",
                                        "removeFirst", "removeLast", "removeFirstOrNull", "removeLastOrNull", "fill", "sortBy",
                                        "sortByDescending", "replaceAll"});
    vm.markMutating(BuiltinType::Map, {"remove", "clear", "put", "set"});
    vm.markMutating(BuiltinType::Set, {"add", "remove", "clear", "addAll", "removeAll", "retainAll"});
    // Given an item of a list of values where it is, `add` copies it in at once (M6.6 part 5).
    vm.markTakesPlaces(BuiltinType::List, {"add"});
    vm.markMutating(BuiltinType::Bytes, {"fill", "putInt16", "putUint16", "putInt32", "putUint32", "putFloat32"});
    for (const char* global : {"__thread", "__loop", "__io", "__file", "__worker", "input", "exit"})
        vm.markNotParallel(global);
    vm.markNotParallel("__task", {"arity"});      // how many parameters a function has: map.forEach asks
}

// Op::Numeric (M6.6 part 6): a number's conversion or one of Math's functions, giving what the
// standard library's own gives, errors included; the compiler chose it for the arguments' types.
Value VM::numeric(uint8_t which, const Value* args) {
    const Value a = args[0].isByte() ? Value::integer(args[0].as.i) : args[0];   // a byte is an int here
    switch (static_cast<NumericOp>(which)) {
        case NumericOp::ToInt: return a.isInt() ? a : Value::integer(floatToInt(*this, std::trunc(toNumber(a, "the number"))));
        case NumericOp::ToFloat: return Value::number(toNumber(a, "the number"));
        case NumericOp::RoundToInt:
            if (a.isInt()) return a;
            if (std::isnan(toNumber(a, "the number"))) throw runtimeError(Thrown::ArithmeticException, "cannot round NaN to a whole number");
            return Value::integer(floatToInt(*this, std::floor(a.as.d + 0.5)));
        case NumericOp::IsNaN: return Value::boolean(a.isFloat() && std::isnan(a.as.d));
        case NumericOp::IsInfinite: return Value::boolean(a.isFloat() && std::isinf(a.as.d));
        case NumericOp::IsFinite: return Value::boolean(a.isInt() || std::isfinite(toNumber(a, "the number")));
        case NumericOp::Sqrt: return Value::number(std::sqrt(toNumber(a, "Math.sqrt() argument")));
        case NumericOp::Floor:
            return a.isInt() ? a : Value::integer(floatToInt(*this, std::floor(toNumber(a, "Math.floor() argument"))));
        case NumericOp::Ceil:
            return a.isInt() ? a : Value::integer(floatToInt(*this, std::ceil(toNumber(a, "Math.ceil() argument"))));
        case NumericOp::Round:
            return a.isInt() ? a : Value::integer(floatToInt(*this, std::round(toNumber(a, "Math.round() argument"))));
        case NumericOp::AbsInt:
        case NumericOp::AbsFloat:
            if (a.isInt()) {
                if (a.as.i == INT64_MIN) throw overflowError(Op::Sub, 0, a.as.i);  // (D27)
                return Value::integer(a.as.i < 0 ? -a.as.i : a.as.i);
            }
            return Value::number(std::fabs(toNumber(a, "Math.abs() argument")));
        case NumericOp::MinInt:
        case NumericOp::MinFloat: {
            const Value b = args[1].isByte() ? Value::integer(args[1].as.i) : args[1];
            const double x = toNumber(a, "Math.min() argument"), y = toNumber(b, "Math.min() argument");
            return y < x ? b : a;
        }
        case NumericOp::MaxInt:
        case NumericOp::MaxFloat: {
            const Value b = args[1].isByte() ? Value::integer(args[1].as.i) : args[1];
            const double x = toNumber(a, "Math.max() argument"), y = toNumber(b, "Math.max() argument");
            return y > x ? b : a;
        }
        default: throw runtimeError(Thrown::InternalError, "invalid bytecode: unknown numeric operation");
    }
}

}  // namespace astra
