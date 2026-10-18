cmake_minimum_required(VERSION 3.20)
# The version goes up with each milestone of docs/gui-plan.md: M1 is 0.1.0, M2 is 0.2.0,
# M3.5 is 0.3.5, M4 is 0.4.0, M5 is 0.5.0, M6 is 0.6.0.
project(Astra VERSION 0.6.0 LANGUAGES CXX)

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
  src/cache.cpp
  src/compiler.cpp
  src/vm.cpp
  src/builtins.cpp
  src/stdlib.cpp
  src/threads.cpp
  src/io.cpp
  src/worker.cpp
  src/parallel.cpp
  # Jade's native layer: windows, events and drawing with OpenGL (docs/gui-plan.md, section 4).
  src/jade/platform_win.cpp
  src/jade/gl.cpp
  src/jade/render.cpp
  src/jade/api.cpp
  # The text engine (docs/gui-plan.md, section 4), and pictures.
  src/text/face.cpp
  src/text/cff.cpp
  src/text/variation.cpp
  src/text/raster.cpp
  src/text/color.cpp
  src/text/bitmaps.cpp
  src/text/system.cpp
  src/text/fonts.cpp
  src/text/engine.cpp
  src/text/api.cpp
  src/image/png.cpp
  src/main.cpp
  src/checker.cpp
  # M0 spike: an OpenGL window and a text engine of our own, as the `Spike` class.
  src/spike/font.cpp
  src/spike/png.cpp
  src/spike/glwin.cpp
)
if(WIN32)
  target_link_libraries(astra PRIVATE opengl32 ws2_32 shell32)
endif()

target_compile_definitions(astra PRIVATE ASTRA_VERSION="${PROJECT_VERSION}")

# astra looks for libraries such as Jade only in the lib folder beside itself, so every
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
  # OpenGL and the shell are loaded when first used, not when astra starts: loading
  # opengl32.dll costs about 57 ms, which every script paid, window or not.
  target_link_libraries(astra PRIVATE delayimp)
  target_link_options(astra PRIVATE /DELAYLOAD:opengl32.dll /DELAYLOAD:shell32.dll)
else()
  target_compile_options(astra PRIVATE -Wall -Wextra)
endif()
