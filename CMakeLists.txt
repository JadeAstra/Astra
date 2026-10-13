cmake_minimum_required(VERSION 3.20)
# The version goes up with each milestone of docs/gui-plan.md: M1 is 0.1.0, M2 is 0.2.0.
project(Astra VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Link the C++ runtime into the executable: astra.exe, and the programs that
# `astra build --exe` makes from it, then run on machines without Visual Studio's runtime.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The standard library is written in Astra (src/std/*.astra) and carried inside astra.exe.
file(GLOB ASTRA_STD CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/std/*.astra")
list(SORT ASTRA_STD)
string(REPLACE ";" "|" ASTRA_STD_ARG "${ASTRA_STD}")
add_custom_command(
  OUTPUT "${CMAKE_BINARY_DIR}/std_source.cpp"
  COMMAND ${CMAKE_COMMAND} "-DOUT=${CMAKE_BINARY_DIR}/std_source.cpp" "-DFILES=${ASTRA_STD_ARG}"
          -P "${CMAKE_SOURCE_DIR}/cmake/embed.cmake"
  DEPENDS ${ASTRA_STD} "${CMAKE_SOURCE_DIR}/cmake/embed.cmake"
  COMMENT "Packing the standard library"
  VERBATIM)

add_executable(astra
  "${CMAKE_BINARY_DIR}/std_source.cpp"
  src/lexer.cpp
  src/parser.cpp
  src/value.cpp
  src/bytecode.cpp
  src/compiler.cpp
  src/vm.cpp
  src/builtins.cpp
  src/stdlib.cpp
  src/threads.cpp
  src/io.cpp
  src/worker.cpp
  src/gui.cpp
  src/main.cpp
  # M0 spike: an OpenGL window and a text engine of our own, as the `Spike` class.
  src/spike/font.cpp
  src/spike/png.cpp
  src/spike/glwin.cpp
)
if(WIN32)
  target_link_libraries(astra PRIVATE opengl32 ws2_32)
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
