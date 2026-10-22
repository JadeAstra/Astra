// Character sets (D127): what charset.cpp does, for the C++ that reads and writes text.
#pragma once

#include <string>
#include <string_view>

#include "value.h"

namespace astra {

class VM;

// In the order of the entries of the standard library's enum Charset.
enum class Charset : uint8_t { Utf8, Latin1, Ascii, Utf16Le, Utf16Be };

// UTF-8 text as bytes in the set; a character the set has no place for is '?', as in Java.
std::string encodeText(std::string_view utf8, Charset cs);
// Bytes in the set as UTF-8 text; what does not spell a character is U+FFFD, as for UTF-8.
std::string decodeText(std::string_view bytes, Charset cs);
// Where the next line end is in bytes in the set, from `from`, and how many bytes it takes.
size_t lineEnd(std::string_view bytes, size_t from, Charset cs, size_t& endSize);
// A line in the set without the carriage return it may end in.
std::string withoutCarriageReturn(std::string line, Charset cs);
// A Charset given to a built-in function; UTF-8 for null.
Charset charsetOf(VM& vm, Value v);

}  // namespace astra
