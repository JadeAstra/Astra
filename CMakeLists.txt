cmake_minimum_required(VERSION 3.20)
# The version goes up with each milestone of docs/gui-plan.md: M1 is 0.1.0, M2 is 0.2.0,
# M3.5 is 0.3.5, M4 is 0.4.0, M5 is 0.5.0, M6 is 0.6.0, M6.5 is 0.6.5.
project(Astra VERSION 0.6.5 LANGUAGES CXX)

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

# The executable in three parts (M6.5 part 5, D161): what every program needs to run, Jade's
# window with the text engine and the image codecs, and the compiler side with the astra command.
# The last two are compiled into sections of their own (src/section_*.h), which `astra build`
# leaves out of a built program that does not need them.
add_library(astra_core OBJECT
  src/value.cpp
  src/bytecode.cpp
  src/vm.cpp
  src/builtins.cpp
  src/stdlib.cpp
  src/reflect.cpp
  src/threads.cpp
  src/io.cpp
  src/charset.cpp
  src/worker.cpp
  src/parallel.cpp
  src/run.cpp
)
add_library(astra_window OBJECT
  src/jade/platform_win.cpp
  src/jade/gl.cpp
  src/jade/render.cpp
  src/jade/api.cpp
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
)
add_library(astra_compiler OBJECT
  "${CMAKE_BINARY_DIR}/std_source.cpp"
  src/lexer.cpp
  src/parser.cpp
  src/checker.cpp
  src/cache.cpp
  src/compiler.cpp
  src/command.cpp
)
add_executable(astra src/main.cpp $<TARGET_OBJECTS:astra_core> $<TARGET_OBJECTS:astra_window> $<TARGET_OBJECTS:astra_compiler>)

# The proof that what a built program keeps needs nothing left out of it: the core and the
# window linked without the compiler side, and the core alone, the window's two entry points
# (which the core calls only when the window's section is there) standing in. Either fails to
# link if anything kept names what is left out. Neither is shipped.
add_executable(astra_proof src/proof_main.cpp $<TARGET_OBJECTS:astra_core> $<TARGET_OBJECTS:astra_window>)
add_executable(astra_proof_plain src/proof_main.cpp src/window_absent.cpp $<TARGET_OBJECTS:astra_core>)
foreach(proof astra_proof astra_proof_plain)
  set_target_properties(${proof} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/proof")
endforeach()
foreach(part astra astra_core astra_window astra_compiler astra_proof astra_proof_plain)
  target_compile_definitions(${part} PRIVATE ASTRA_VERSION="${PROJECT_VERSION}")
endforeach()
if(WIN32)
  foreach(linked astra astra_proof astra_proof_plain)
    target_link_libraries(${linked} PRIVATE opengl32 ws2_32 shell32)
  endforeach()
endif()

# astra looks for libraries such as Jade only in the lib folder beside itself, so every
# build puts a copy of lib/ next to the executable.
add_custom_target(astra_lib ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/lib" "$<TARGET_FILE_DIR:astra>/lib"
  COMMENT "Copying lib next to astra"
  VERBATIM)

if(MSVC)
  foreach(part astra astra_core astra_window astra_compiler astra_proof astra_proof_plain)
    target_compile_options(${part} PRIVATE /W4 /utf-8 /permissive-)
    target_compile_definitions(${part} PRIVATE _CRT_SECURE_NO_WARNINGS)
  endforeach()
  # The parser and compiler recurse over nested source; give them room.
  target_link_options(astra PRIVATE /STACK:16777216)
  # OpenGL and the shell are loaded when first used, not when astra starts: loading
  # opengl32.dll costs about 57 ms, which every script paid, window or not.
  target_link_libraries(astra PRIVATE delayimp)
  target_link_options(astra PRIVATE /DELAYLOAD:opengl32.dll /DELAYLOAD:shell32.dll)
else()
  foreach(part astra astra_core astra_window astra_compiler astra_proof astra_proof_plain)
    target_compile_options(${part} PRIVATE -Wall -Wextra)
  endforeach()
endif()

# The check of what a built program leaves out (D161): nothing of it runs as the program starts,
# and every function the proofs need is in astra.exe in a section a built program keeps.
if(MSVC)
  foreach(linked astra astra_proof astra_proof_plain)
    target_link_options(${linked} PRIVATE "/MAP:$<TARGET_FILE_DIR:${linked}>/${linked}.map")
  endforeach()
  add_custom_target(astra_check_sections ALL
    COMMAND $<TARGET_FILE:astra> --check-sections "$<TARGET_FILE_DIR:astra>/astra.map"
            "$<TARGET_FILE_DIR:astra_proof>/astra_proof.map" "$<TARGET_FILE_DIR:astra_proof_plain>/astra_proof_plain.map"
    DEPENDS astra astra_proof astra_proof_plain
    COMMENT "Checking what a built program leaves out"
    VERBATIM)
endif()

# The standard library, compiled once as the build's last step and attached to astra.exe, so
# that no script compiles it or checks it again (D145). Off, astra compiles it at each start.
option(ASTRA_ATTACH_PRELUDE "Attach the compiled standard library to astra.exe" ON)
if(ASTRA_ATTACH_PRELUDE)
  add_custom_command(TARGET astra POST_BUILD
    COMMAND $<TARGET_FILE:astra> --attach-prelude $<TARGET_FILE_DIR:astra>/astra-attached.exe
    COMMAND ${CMAKE_COMMAND} -E rename $<TARGET_FILE_DIR:astra>/astra-attached.exe $<TARGET_FILE:astra>
    COMMENT "Attaching the compiled standard library"
    VERBATIM)
endif()
