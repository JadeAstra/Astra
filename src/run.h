#pragma once

// Running a compiled program to its end: all of the astra executable a built program uses
// (M6.5 part 5). The astra command itself is in command.cpp, in a part of the executable
// that `astra build` leaves out of what it writes.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "bytecode.h"
#include "vm.h"

namespace astra {

// Exit codes, after BSD sysexits.
constexpr int kExitUsage = 64;
constexpr int kExitCompileError = 65;
constexpr int kExitNoInput = 66;
constexpr int kExitRuntimeError = 70;
constexpr int kExitCannotWrite = 73;

// Set when this executable is a built program rather than the astra command.
extern bool runningBundledProgram;

// The file this process was started from; empty if it cannot be found out.
std::filesystem::path executablePath();

// Reports a problem that ends the program. A program built with --windowed has no console,
// so there the message goes in a box instead of being lost.
void reportFatal(const std::string& report);
// Reports an error nothing caught, with where it was thrown.
void reportUncaught(VM& vm, AstraThrow& thrown);

// Runs a loaded program to its end: the script itself, then the event loop for as long as
// there is something for it to wait for. Returns the exit code.
int runProgram(VM& vm, Program& program, int entry);

// ---- Programs built into an executable ----
//
// `astra build --exe` makes a copy of this executable with the compiled program attached to
// the end, followed by the program's size and a marker. A copy that finds such a program on
// itself runs it instead of acting as the astra command.

extern const char kBundleMarker[8];
constexpr size_t kBundleTrailer = 8 + sizeof kBundleMarker;  // program size, then the marker

// The size of the program attached to `image`, or -1 if there is none.
int64_t bundledProgramSize(const std::string& image);
// Whether a program is attached to the file; reads only its last bytes.
bool hasBundledProgram(const std::filesystem::path& exe);
// Runs the program attached to this executable. Every command-line argument is the program's.
int runBundled(const std::filesystem::path& exe, std::vector<std::string> scriptArgs);

// When this executable began its work (main's first line).
extern std::chrono::steady_clock::time_point executableStarted;

// What main does (main.cpp): runs the program built into this executable if there is one, and
// otherwise `command`, the astra command (command.cpp), or with none says so. A built program
// has no command: what it is made of is left out of its file, and it is never called there.
int startExecutable(int argc, char** argv, int (*command)(const std::vector<std::string>& args));

// The astra command, given the command line's arguments (command.cpp).
int astraCommandMain(const std::vector<std::string>& args);

// Whether the part of this executable with Jade's window, drawing and the text engine is in
// its file: a built program that reaches neither has it left out (M6.5 part 5).
bool windowCodePresent();

}  // namespace astra
