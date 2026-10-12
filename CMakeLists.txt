cmake_minimum_required(VERSION 3.20)
project(Astra VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(astra
  src/lexer.cpp
  src/parser.cpp
  src/value.cpp
  src/bytecode.cpp
  src/compiler.cpp
  src/vm.cpp
  src/builtins.cpp
  src/gui.cpp
  src/main.cpp
)

target_compile_definitions(astra PRIVATE ASTRA_VERSION="${PROJECT_VERSION}")

if(MSVC)
  target_compile_options(astra PRIVATE /W4 /utf-8 /permissive-)
  target_compile_definitions(astra PRIVATE _CRT_SECURE_NO_WARNINGS)
  # The parser and compiler recurse over nested source; give them room.
  target_link_options(astra PRIVATE /STACK:16777216)
else()
  target_compile_options(astra PRIVATE -Wall -Wextra)
endif()
