// The `astra` command: runs, compiles and disassembles Astra programs, and hosts the REPL.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
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
    "  astra dis <file>                      list the bytecode of a source or bytecode file\n"
    "  astra --version\n"
    "  astra --help\n";

// Where bundled libraries such as gui.astra live: a "lib" folder beside the executable, or
// beside its parent folder (the layout of a source checkout, with the binary in build/).
std::vector<std::string> libraryPaths() {
    std::filesystem::path exe;
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) exe = std::filesystem::path(buffer);
#else
    std::error_code ec;
    exe = std::filesystem::read_symlink("/proc/self/exe", ec);
#endif
    std::vector<std::string> paths;
    if (!exe.empty()) {
        std::filesystem::path dir = exe.parent_path();
        paths.push_back((dir / "lib").string());
        paths.push_back((dir.parent_path() / "lib").string());
    }
    return paths;
}

bool stdinIsTerminal() {

#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(fileno(stdin)) != 0;
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

    std::cerr << "Uncaught " << text << "\n";
    for (const std::string& line : thrown.trace) std::cerr << "  " << line << "\n";
}

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

int buildFile(const std::string& path, std::string outPath) {
    std::string data;
    if (!readFile(path, data)) {
        std::cerr << "astra: cannot read '" << path << "'\n";
        return kExitNoInput;
    }
    if (looksLikeBytecode(data)) {
        std::cerr << "astra: '" << path << "' is already a bytecode file\n";
        return kExitUsage;
    }

    if (outPath.empty()) outPath = std::filesystem::path(path).replace_extension(".astc").string();

    VM vm;
    CompilerSession session(vm);
    session.libraryPaths = libraryPaths();
    try {
        session.compileFile(path);
    } catch (const CompileError& e) {
        std::cerr << e.format() << "\n";
        return kExitCompileError;
    }

    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    if (out) writeProgram(session.program, out);
    out.flush();
    if (!out) {
        std::cerr << "astra: cannot write '" << outPath << "'\n";
        return kExitCannotWrite;
    }
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
        for (size_t i = 1; i < args.size(); i++) {
            if (args[i] == "-o" && i + 1 < args.size()) {
                output = args[++i];
            } else if (input.empty() && args[i] != "-o") {
                input = args[i];
            } else {
                std::cerr << "astra: usage: astra build <file> [-o <out.astc>]\n";
                return kExitUsage;
            }
        }
        if (input.empty()) {
            std::cerr << "astra: 'build' needs a file\n";
            return kExitUsage;
        }
        return buildFile(input, output);
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
        code = dispatch(std::vector<std::string>(argv + 1, argv + argc));
    } catch (const std::exception& e) {
        std::cout.flush();
        std::cerr << "astra: internal error: " << e.what() << "\n";
        code = kExitRuntimeError;
    }
    std::cout.flush();
    return code;
}
