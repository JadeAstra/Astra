# Turns the standard library's Astra sources into a C++ file holding their text, so that
# astra.exe carries them inside itself.
#   cmake -DOUT=<file.cpp> -DFILES=<a.astra|b.astra|...> -P embed.cmake
cmake_minimum_required(VERSION 3.20)

string(REPLACE "|" ";" files "${FILES}")
set(out "// Generated from src/std/*.astra by cmake/embed.cmake. Do not edit.\n#include <cstddef>\nnamespace astra {\nextern const unsigned char kStdSource[];\nextern const size_t kStdSourceSize;\nconst unsigned char kStdSource[] = {\n")
foreach(file ${files})
  file(READ "${file}" hex HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
  # A line end after each file, so that the last line of one cannot run into the next.
  string(APPEND out "${bytes}0x0a,\n")
endforeach()
string(APPEND out "0x00};\nconst size_t kStdSourceSize = sizeof(kStdSource) - 1;\n}  // namespace astra\n")
file(WRITE "${OUT}" "${out}")
