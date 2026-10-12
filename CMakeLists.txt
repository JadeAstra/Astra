cmake_minimum_required(VERSION 3.20)
project(Astra VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Link the C++ runtime into the executable: astra.exe, and the programs that
# `astra build --exe` makes from it, then run on machines without Visual Studio's runtime.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

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
  # M0 spike: an OpenGL window and a text engine of our own, as the `Spike` class.
  src/spike/font.cpp
  src/spike/png.cpp
  src/spike/glwin.cpp
)
if(WIN32)
  target_link_libraries(astra PRIVATE opengl32)
endif()

target_compile_definitions(astra PRIVATE ASTRA_VERSION="${PROJECT_VERSION}")

# astra looks for libraries such as gui.astra only in the lib folder beside itself, so every
# build puts a copy of lib/ next to the executable.
add_custom_target(astra_lib ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/lib" "$<TARGET_FILE_DIR:astra>/lib"
  COMMENT "Copying lib next to astra"
  VERBATIM)

if(MSVC)
  target_compile_options(astra PRIVATE /W4 /utf-8 /permissive-)
  target_compile_definitions(astra PRIVATE _CRT_SECURE_NO_WARNINGS)
  # The parser and compiler recurse over nested source; give them room.
  target_link_options(astra PRIVATE /STACK:16777216)
else()
  target_compile_options(astra PRIVATE -Wall -Wextra)
endif()
