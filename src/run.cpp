// Running a compiled program to its end: all of the astra executable a built program uses
// (M6.5 part 5). See run.h.

#include "run.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace astra {

bool runningBundledProgram = false;
std::chrono::steady_clock::time_point executableStarted;

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

namespace {

void reportThreadError(VM& vm, AstraThrow& thrown, const std::string& threadName) {
    thrown.trace.push_back("in thread " + threadName);
    reportUncaught(vm, thrown);
}

}  // namespace

// Runs a loaded program to its end: the script itself, then the event loop for as long as
// there is something for it to wait for (a timer, work in progress, a thread that is not a
// daemon). Returns the exit code.
int runProgram(VM& vm, Program& program, int entry) {
    vm.onThreadError = reportThreadError;
    // A worker has a VM of its own, and loads its own copy of the program from these bytes.
    vm.programImage = [&program] {
        std::ostringstream image(std::ios::binary);
        writeProgram(program, image);
        return image.str();
    };
    vm.onThreadExit = [](int code) {
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(code);
    };

    int code = 0;
    try {
        vm.runModule(entry);
        vm.runEventLoop(true);
    } catch (AstraThrow& thrown) {
        reportUncaught(vm, thrown);
        code = kExitRuntimeError;
    } catch (const AstraExit& exit) {
        code = exit.code;
    }
    // A daemon thread, or a worker still waiting in a read, cannot be left running while the
    // VM is taken apart under it: the process ends here instead.
    if (vm.threadsAlive()) {
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(code);
    }
    return code;
}


// ---- Programs built into an executable ----

const char kBundleMarker[8] = {'A', 'S', 'T', 'R', 'A', 'E', 'X', 'E'};

namespace {

// The size of a program attached to a file that ends with `trailer`, or -1.
int64_t sizeFromTrailer(const char* trailer, uint64_t room) {
    if (std::memcmp(trailer + 8, kBundleMarker, sizeof kBundleMarker) != 0) return -1;
    uint64_t size = 0;
    for (int i = 0; i < 8; i++) size |= static_cast<uint64_t>(static_cast<unsigned char>(trailer[i])) << (8 * i);
    return size > room ? -1 : static_cast<int64_t>(size);
}

}  // namespace

int64_t bundledProgramSize(const std::string& image) {
    if (image.size() < kBundleTrailer) return -1;
    return sizeFromTrailer(image.data() + image.size() - kBundleTrailer, image.size() - kBundleTrailer);
}

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

int runBundled(const std::filesystem::path& exe, std::vector<std::string> scriptArgs) {
    runningBundledProgram = true;
    // Only the end of the file is read: the program and the trailer after it.
    std::string bytecode;
    {
        std::ifstream file(exe, std::ios::binary | std::ios::ate);
        const std::streamoff length = file ? static_cast<std::streamoff>(file.tellg()) : 0;
        char trailer[kBundleTrailer];
        int64_t size = -1;
        if (length >= static_cast<std::streamoff>(kBundleTrailer)) {
            file.seekg(length - static_cast<std::streamoff>(kBundleTrailer));
            if (file.read(trailer, sizeof trailer)) size = sizeFromTrailer(trailer, static_cast<uint64_t>(length) - kBundleTrailer);
        }
        if (size >= 0) {
            bytecode.resize(static_cast<size_t>(size));
            file.seekg(length - static_cast<std::streamoff>(kBundleTrailer) - static_cast<std::streamoff>(size));
            if (!file.read(bytecode.data(), static_cast<std::streamsize>(size))) size = -1;
        }
        if (size < 0) {
            reportFatal(exe.filename().string() + ": the program built into this file is damaged\n");
            return kExitRuntimeError;
        }
    }

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
    setSelfCommand({exe.string()});
    return runProgram(vm, program, program.entry);
}

bool windowCodePresent() {
#ifdef _WIN32
    // The section's header is still there when its bytes are left out, with none of them in the
    // file; the loaded image's headers are the file's.
    const auto* image = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++, section++)
        if (std::memcmp(section->Name, ".astraj", 8) == 0) return section->SizeOfRawData != 0;
#endif
    return true;
}

int startExecutable(int argc, char** argv, int (*command)(const std::vector<std::string>& args)) {
    executableStarted = std::chrono::steady_clock::now();
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
        if (!self.empty() && hasBundledProgram(self)) {
            code = runBundled(self, std::move(args));
        } else if (command != nullptr) {
            code = command(args);
        } else {
            reportFatal("astra: this executable holds no program and no astra command\n");
            code = kExitUsage;
        }
    } catch (const std::exception& e) {
        reportFatal(std::string("astra: internal error: ") + e.what() + "\n");
        code = kExitRuntimeError;
    }
    std::cout.flush();
    return code;
}

}  // namespace astra
