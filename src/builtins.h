// Helpers shared by the files that implement the built-in functions and methods.
#pragma once

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>  // _mul128
#endif
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "vm.h"

namespace astra {

// Clamps a possibly negative position into [0, length], as slice() bounds.
size_t clampPosition(int64_t position, size_t length);

// Upper or lower case for all letters, not only the ASCII ones.
std::string changeCase(const std::string& s, bool upper);

std::string_view trimmed(std::string_view s);
bool parseInt(std::string_view s, int64_t& out);
bool parseFloat(std::string_view s, double& out);
int64_t floatToInt(VM& vm, double d);

// An int sum, difference or product that leaves the 64 bits an int has (D27): true where it
// does, its result wrapped around in `r` either way.
inline bool addOverflows(int64_t a, int64_t b, int64_t& r) {
#if defined(__clang__) || defined(__GNUC__)
    return __builtin_add_overflow(a, b, &r);
#else
    r = static_cast<int64_t>(static_cast<uint64_t>(a) + static_cast<uint64_t>(b));
    return ((a ^ r) & (b ^ r)) < 0;
#endif
}
inline bool subOverflows(int64_t a, int64_t b, int64_t& r) {
#if defined(__clang__) || defined(__GNUC__)
    return __builtin_sub_overflow(a, b, &r);
#else
    r = static_cast<int64_t>(static_cast<uint64_t>(a) - static_cast<uint64_t>(b));
    return ((a ^ b) & (a ^ r)) < 0;
#endif
}
inline bool mulOverflows(int64_t a, int64_t b, int64_t& r) {
#if defined(__clang__) || defined(__GNUC__)
    return __builtin_mul_overflow(a, b, &r);
#else
    int64_t high;
    r = _mul128(a, b, &high);
    return high != (r >> 63);
#endif
}
void appendUtf8(std::string& out, uint32_t cp);
std::vector<std::string> splitLines(const std::string& text);

ObjList* newList(VM& vm, std::vector<Value> items = {});

// Ordering used by sort() when no comparator is given.
bool defaultLess(VM& vm, const Value& a, const Value& b);

// The part of the standard library added for the Kotlin names: stdlib.cpp.
void registerStdlib(VM& vm);

// Reflection, what Class and the rest read of the classes (D38): reflect.cpp.
void registerReflection(VM& vm);

}  // namespace astra
