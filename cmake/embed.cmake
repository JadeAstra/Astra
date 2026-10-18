# Turns the standard library's Astra sources into a C++ file holding their text, so that
# astra.exe carries them inside itself.
#   cmake -DOUT=<file.cpp> -DFILES=<a.astra|b.astra|...> -P embed.cmake
cmake_minimum_required(VERSION 3.20)

string(REPLACE "|" ";" files "${FILES}")
set(out "// Generated from src/std/*.astra by cmake/embed.cmake. Do not edit.\n#include <cstddef>\nnamespace astra {\nextern const unsigned char kStdSource[];\nextern const size_t kStdSourceSize;\nextern const size_t kStdFileEnds[];\nextern const size_t kStdFileCount;\nconst unsigned char kStdSource[] = {\n")
# Where each file's text ends, so that the files can be parsed apart, a few at a time.
set(ends "")
set(offset 0)
list(LENGTH files count)
foreach(file ${files})
  file(READ "${file}" hex HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
  # A line end after each file, so that the last line of one cannot run into the next.
  string(APPEND out "${bytes}0x0a,\n")
  string(LENGTH "${hex}" digits)
  math(EXPR offset "${offset} + ${digits} / 2 + 1")
  string(APPEND ends "${offset},")
endforeach()
string(APPEND out "0x00};\nconst size_t kStdSourceSize = sizeof(kStdSource) - 1;\n")
string(APPEND out "const size_t kStdFileEnds[] = {${ends}};\nconst size_t kStdFileCount = ${count};\n}  // namespace astra\n")
file(WRITE "${OUT}" "${out}")
