// The `astra` command: runs, compiles and disassembles Astra programs, and hosts the REPL.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "compiler.h"
#include "vm.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace astra;

namespace {

// Exit codes, after BSD sysexits.
const int kExitUsage = 64;
const int kExitCompileError = 65;
const int kExitNoInput = 66;
const int kExitRuntimeError = 70;
const int kExitCannotWrite = 73;

const char* const kUsage =
    "Astra " ASTRA_VERSION " - an object-oriented scripting language\n"
    "\n"
    "Usage:\n"
    "  astra                                 start the interactive prompt\n"
    "  astra <file> [args...]                run a source (.astra) or bytecode (.astc) file\n"
    "  astra run <file> [args...]            same as above\n"
    "  astra build <file> [-o <out.astc>]    compile a source file to a bytecode file\n"
    "  astra build <file> --exe [-o <app.exe>] [--windowed]\n"
    "                                        compile it into a program of its own;\n"
    "                                        --windowed gives it no console window\n"
    "  astra dis <file>                      list the bytecode of a source or bytecode file\n"
    "  astra --version\n"
    "  astra --help\n";

// Set when this executable is a built program rather than the astra command.
bool runningBundledProgram = false;

// The file this process was started from; empty if it cannot be found out.
std::filesystem::path executablePath() {
#ifdef _WIN32
    std::wstring buffer(1024, L'\0');
    for (;;) {
        DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size()) return std::filesystem::path(buffer.substr(0, length));
        buffer.resize(buffer.size() * 2);
    }
#else
    std::error_code ec;
    return std::filesystem::read_symlink("/proc/self/exe", ec);
#endif
}

// Where bundled libraries such as gui.astra live: a "lib" folder beside the executable, and
// nowhere else, so that a folder someone else can write to is never searched.
std::vector<std::string> libraryPaths() {
    std::filesystem::path exe = executablePath();
    std::vector<std::string> paths;
    if (!exe.empty()) paths.push_back((exe.parent_path() / "lib").string());
    return paths;
}

bool stdinIsTerminal() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(fileno(stdin)) != 0;
#endif
}

// Reports a problem that ends the program. A program built with --windowed has no console,
// so there the message goes in a box instead of being lost.
void reportFatal(const std::string& report) {
    std::cout.flush();
    std::cerr << report;

#ifdef _WIN32
    if (runningBundledProgram && GetConsoleWindow() == nullptr && GetFileType(GetStdHandle(STD_ERROR_HANDLE)) == FILE_TYPE_UNKNOWN) {
        std::wstring title = executablePath().stem().wstring();
        int size = MultiByteToWideChar(CP_UTF8, 0, report.data(), static_cast<int>(report.size()), nullptr, 0);
        std::wstring wide(static_cast<size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, report.data(), static_cast<int>(report.size()), wide.data(), size);
        MessageBoxW(nullptr, wide.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
    }
#endif
}

void reportUncaught(VM& vm, AstraThrow& thrown) {
    std::cout.flush();

    std::string text;
    try {
        // Error objects describe themselves ("Error: message"); anything else thrown is shown as a value.
        text = thrown.value.isObjType(ObjType::Instance) ? vm.stringify(thrown.value)
                                                         : "exception: " + vm.stringify(thrown.value);
    } catch (const AstraThrow&) {
        text = "exception (its toString() failed)";
    }

    std::string report = "Uncaught " + text + "\n";
    for (const std::string& line : thrown.trace) report += "  " + line + "\n";
    reportFatal(report);
}

// ---- Programs built into an executable ----
//
// `astra build --exe` makes a copy of this executable with the compiled program attached to
// the end, followed by the program's size and a marker. A copy that finds such a program on
// itself runs it instead of acting as the astra command.

const char kBundleMarker[8] = {'A', 'S', 'T', 'R', 'A', 'E', 'X', 'E'};
const size_t kBundleTrailer = 8 + sizeof kBundleMarker;  // program size, then the marker

// Reads a whole file as bytes; false if it cannot be opened.
bool readBytes(const std::filesystem::path& path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    out = buffer.str();
    return !file.bad();
}

// The size of the program attached to `image`, or -1 if there is none.
int64_t bundledProgramSize(const std::string& image) {
    if (image.size() < kBundleTrailer) return -1;
    const char* trailer = image.data() + image.size() - kBundleTrailer;
    if (std::memcmp(trailer + 8, kBundleMarker, sizeof kBundleMarker) != 0) return -1;

    uint64_t size = 0;
    for (int i = 0; i < 8; i++) size |= static_cast<uint64_t>(static_cast<unsigned char>(trailer[i])) << (8 * i);
    if (size > image.size() - kBundleTrailer) return -1;
    return static_cast<int64_t>(size);
}

// Checks only the last few bytes of the file, so an ordinary start of astra pays nothing.
bool hasBundledProgram(const std::filesystem::path& exe) {
    std::ifstream file(exe, std::ios::binary | std::ios::ate);
    if (!file) return false;
    std::streamoff size = file.tellg();
    if (size < static_cast<std::streamoff>(kBundleTrailer)) return false;
    char marker[sizeof kBundleMarker];
    file.seekg(size - static_cast<std::streamoff>(sizeof kBundleMarker));
    file.read(marker, sizeof marker);
    return file && std::memcmp(marker, kBundleMarker, sizeof marker) == 0;
}

// Runs the program attached to this executable. Every command-line argument is the program's.
int runBundled(const std::filesystem::path& exe, std::vector<std::string> scriptArgs) {
    runningBundledProgram = true;
    std::string image;
    int64_t size = readBytes(exe, image) ? bundledProgramSize(image) : -1;
    if (size < 0) {
        reportFatal(exe.filename().string() + ": the program built into this file is damaged\n");
        return kExitRuntimeError;
    }
    std::string bytecode = image.substr(image.size() - kBundleTrailer - static_cast<size_t>(size), static_cast<size_t>(size));
    image.clear();
    image.shrink_to_fit();

    VM vm;
    vm.scriptArgs = std::move(scriptArgs);
    Program program;
    try {
        readProgram(program, vm, bytecode);
    } catch (const std::runtime_error& e) {
        reportFatal(exe.filename().string() + ": the program built into this file is damaged (" + e.what() + ")\n");
        return kExitRuntimeError;
    }

    vm.load(program);
    try {
        vm.runModule(program.entry);
    } catch (AstraThrow& thrown) {
        reportUncaught(vm, thrown);
        return kExitRuntimeError;
    } catch (const AstraExit& exit) {
        return exit.code;
    }
    return 0;
}

#ifdef _WIN32
// Marks a Windows executable image as a windowed program, so that starting it opens no console.
bool makeWindowed(std::string& image) {
    if (image.size() < 0x40 || image[0] != 'M' || image[1] != 'Z') return false;
    uint32_t peOffset = 0;
    std::memcpy(&peOffset, image.data() + 0x3C, sizeof peOffset);
    const size_t subsystemAt = static_cast<size_t>(peOffset) + 4 + 20 + 68;  // signature, file header, into the optional header
    if (subsystemAt + 2 > image.size() || std::memcmp(image.data() + peOffset, "PE\0\0", 4) != 0) return false;
    image[subsystemAt] = 2;  // IMAGE_SUBSYSTEM_WINDOWS_GUI
    image[subsystemAt + 1] = 0;
    return true;
}
#endif

// Loads `path` into `program`, compiling it unless it is already bytecode.
// Returns 0 on success, otherwise the exit code after reporting the problem.
int loadProgram(const std::string& path, VM& vm, CompilerSession*& session, Program*& program,
                std::unique_ptr<CompilerSession>& sessionOwner, std::unique_ptr<Program>& programOwner) {
    std::string data;
    if (!readFile(path, data)) {
        std::cerr << "astra: cannot read '" << path << "'\n";
        return kExitNoInput;
    }

    try {
        if (looksLikeBytecode(data)) {
            programOwner = std::make_unique<Program>();
            readProgram(*programOwner, vm, data);
            program = programOwner.get();
            session = nullptr;
        } else {
            sessionOwner = std::make_unique<CompilerSession>(vm);
            sessionOwner->libraryPaths = libraryPaths();
            sessionOwner->compileFile(path);
            session = sessionOwner.get();
            program = &sessionOwner->program;
        }
    } catch (const CompileError& e) {
        std::cerr << e.format() << "\n";
        return kExitCompileError;
    } catch (const std::runtime_error& e) {
        std::cerr << "astra: " << path << ": " << e.what() << "\n";
        return kExitCompileError;
    }
    return 0;
}

int runFile(const std::string& path, std::vector<std::string> scriptArgs) {
    VM vm;
    vm.scriptArgs = std::move(scriptArgs);

    CompilerSession* session = nullptr;
    Program* program = nullptr;
    std::unique_ptr<CompilerSession> sessionOwner;
    std::unique_ptr<Program> programOwner;
    if (int failed = loadProgram(path, vm, session, program, sessionOwner, programOwner)) return failed;

    vm.load(*program);
    try {
        vm.runModule(program->entry);
    } catch (AstraThrow& thrown) {
        reportUncaught(vm, thrown);
        return kExitRuntimeError;
    } catch (const AstraExit& exit) {
        return exit.code;
    }
    return 0;
}

int buildFile(const std::string& path, std::string outPath, bool asExecutable, bool windowed) {
    std::string data;
    if (!readFile(path, data)) {
        std::cerr << "astra: cannot read '" << path << "'\n";
        return kExitNoInput;
    }
    if (looksLikeBytecode(data)) {
        std::cerr << "astra: '" << path << "' is already a bytecode file\n";
        return kExitUsage;
    }

#ifdef _WIN32
    const char* executableExtension = ".exe";
#else
    const char* executableExtension = "";
#endif
    if (outPath.empty())
        outPath = std::filesystem::path(path).replace_extension(asExecutable ? executableExtension : ".astc").string();
    if (asExecutable && std::filesystem::path(outPath) == std::filesystem::path(path)) outPath += ".out";

    VM vm;
    CompilerSession session(vm);
    session.libraryPaths = libraryPaths();
    try {
        session.compileFile(path);
    } catch (const CompileError& e) {
        std::cerr << e.format() << "\n";
        return kExitCompileError;
    }

    if (!asExecutable) {
        std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
        if (out) writeProgram(session.program, out);
        out.flush();
        if (!out) {
            std::cerr << "astra: cannot write '" << outPath << "'\n";
            return kExitCannotWrite;
        }
        return 0;
    }

    // A program of its own: this executable, with the compiled program attached.
    std::ostringstream compiled;
    writeProgram(session.program, compiled);
    std::string bytecode = compiled.str();

    std::string image;
    if (!readBytes(executablePath(), image)) {
        std::cerr << "astra: cannot read the astra executable to build from\n";
        return kExitCannotWrite;
    }
    int64_t existing = bundledProgramSize(image);
    if (existing >= 0) image.resize(image.size() - kBundleTrailer - static_cast<size_t>(existing));

    if (windowed) {
#ifdef _WIN32
        if (!makeWindowed(image)) {
            std::cerr << "astra: cannot make a windowed program from this executable\n";
            return kExitCannotWrite;
        }
#endif
    }

    char sizeBytes[8];
    for (int i = 0; i < 8; i++) sizeBytes[i] = static_cast<char>((static_cast<uint64_t>(bytecode.size()) >> (8 * i)) & 0xFF);

    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    out.write(image.data(), static_cast<std::streamsize>(image.size()));
    out.write(bytecode.data(), static_cast<std::streamsize>(bytecode.size()));
    out.write(sizeBytes, sizeof sizeBytes);
    out.write(kBundleMarker, sizeof kBundleMarker);
    out.flush();
    if (!out) {
        std::cerr << "astra: cannot write '" << outPath << "' (is that program still running?)\n";
        return kExitCannotWrite;
    }
    out.close();

#ifndef _WIN32
    std::error_code ec;
    std::filesystem::permissions(outPath, std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                                              std::filesystem::perms::others_exec, std::filesystem::perm_options::add, ec);
#endif
    return 0;
}

int disassembleFile(const std::string& path) {
    VM vm;
    CompilerSession* session = nullptr;
    Program* program = nullptr;
    std::unique_ptr<CompilerSession> sessionOwner;
    std::unique_ptr<Program> programOwner;
    if (int failed = loadProgram(path, vm, session, program, sessionOwner, programOwner)) return failed;

    disassemble(*program, std::cout);
    return 0;
}

bool isBlank(const std::string& s) { return s.find_first_not_of(" \t\r\n") == std::string::npos; }

int repl() {
    VM vm;
    CompilerSession session(vm);
    session.libraryPaths = libraryPaths();
    bool interactive = stdinIsTerminal();
    if (interactive) std::cout << "Astra " ASTRA_VERSION " - type .exit to quit\n";

    std::string buffer;
    for (;;) {
        if (interactive) std::cout << (buffer.empty() ? "> " : ". ") << std::flush;

        std::string line;
        if (!std::getline(std::cin, line)) break;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);  // byte order mark from a pipe
        if (buffer.empty() && (line == ".exit" || line == ".quit")) break;

        buffer += line;
        buffer += '\n';
        if (isBlank(buffer)) {
            buffer.clear();
            continue;
        }

        int module;
        try {
            module = session.compileSource(buffer, "<repl>", ".", true);
        } catch (const CompileError& e) {
            // Input that merely stops too early continues on the next line; a blank line gives up.
            if (e.atEof && !isBlank(line)) continue;
            std::cerr << e.format() << "\n";
            buffer.clear();
            continue;
        }
        buffer.clear();

        vm.load(session.program);
        try {
            Value result = vm.runModule(module);
            if (!result.isNull()) std::cout << vm.repr(result) << "\n";
        } catch (AstraThrow& thrown) {
            reportUncaught(vm, thrown);
        } catch (const AstraExit& exit) {
            return exit.code;
        }
        std::cout.flush();
    }
    return 0;
}

int dispatch(const std::vector<std::string>& args) {
    if (args.empty()) return repl();

    const std::string& command = args[0];
    if (command == "--help" || command == "-h" || command == "help") {
        std::cout << kUsage;
        return 0;
    }
    if (command == "--version" || command == "-v") {
        std::cout << "astra " ASTRA_VERSION "\n";
        return 0;
    }
    if (command == "repl") return repl();

    if (command == "run") {
        if (args.size() < 2) {
            std::cerr << "astra: 'run' needs a file\n";
            return kExitUsage;
        }
        return runFile(args[1], std::vector<std::string>(args.begin() + 2, args.end()));
    }

    if (command == "build") {
        std::string input, output;
        bool asExecutable = false, windowed = false;
        for (size_t i = 1; i < args.size(); i++) {
            if (args[i] == "-o" && i + 1 < args.size()) {
                output = args[++i];
            } else if (args[i] == "--exe") {
                asExecutable = true;
            } else if (args[i] == "--windowed") {
                asExecutable = windowed = true;
            } else if (input.empty() && !args[i].empty() && args[i][0] != '-') {
                input = args[i];
            } else {
                std::cerr << "astra: usage: astra build <file> [-o <out>] [--exe] [--windowed]\n";
                return kExitUsage;
            }
        }
        if (input.empty()) {
            std::cerr << "astra: 'build' needs a file\n";
            return kExitUsage;
        }
        // Asking for an .exe by name is asking for a program.
        if (std::filesystem::path(output).extension() == ".exe") asExecutable = true;
        return buildFile(input, output, asExecutable, windowed);
    }

    if (command == "dis") {
        if (args.size() != 2) {
            std::cerr << "astra: usage: astra dis <file>\n";
            return kExitUsage;
        }
        return disassembleFile(args[1]);
    }

    if (command.size() > 1 && command[0] == '-') {
        std::cerr << "astra: unknown option '" << command << "'\n" << kUsage;
        return kExitUsage;
    }

    return runFile(command, std::vector<std::string>(args.begin() + 1, args.end()));
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    static char outputBuffer[1 << 16];
    std::ios::sync_with_stdio(false);
    std::cout.rdbuf()->pubsetbuf(outputBuffer, sizeof outputBuffer);

    int code;
    try {
        std::vector<std::string> args(argv + 1, argv + argc);
        std::filesystem::path self = executablePath();
        code = !self.empty() && hasBundledProgram(self) ? runBundled(self, std::move(args)) : dispatch(args);
    } catch (const std::exception& e) {
        reportFatal(std::string("astra: internal error: ") + e.what() + "\n");
        code = kExitRuntimeError;
    }
    std::cout.flush();
    return code;
}
