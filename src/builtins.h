// Helpers shared by the files that implement the built-in functions and methods.
#pragma once

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
void appendUtf8(std::string& out, uint32_t cp);
std::vector<std::string> splitLines(const std::string& text);

ObjList* newList(VM& vm, std::vector<Value> items = {});

// Ordering used by sort() when no comparator is given.
bool defaultLess(VM& vm, const Value& a, const Value& b);

// The part of the standard library added for the Kotlin names: stdlib.cpp.
void registerStdlib(VM& vm);

}  // namespace astra
