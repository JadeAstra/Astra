// The `astra` command: runs, compiles and disassembles Astra programs, and hosts the shell.
// What a built program runs is in run.cpp; this, with the compiler, is in a part of the
// executable that `astra build` leaves out of what it writes (M6.5 part 5).

#include <algorithm>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "cache.h"
#include "checker.h"
#include "compiler.h"
#include "run.h"
#include "vm.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "section_compiler.h"  // last: this file's own code goes in its section (D161)

#ifdef _WIN32
// The C runtime's tables of what runs as the program starts: the
// initializers of C++ globals and of C globals. (Those of thread_local variables, run as each
// thread starts, are not public; such a variable in those files is made on first use too.)
extern "C" {
using StartFunction = void(__cdecl*)();
extern StartFunction __xc_a[], __xc_z[];
extern StartFunction __xi_a[], __xi_z[];
}
#endif

using namespace astra;

namespace {

// Reads a whole file as bytes; false if it cannot be opened.
bool readBytes(const std::filesystem::path& path, std::string& out) { return readWholeFile(path, out); }

// What of the start-up code of this executable is in one of the named sections, or nothing.
// A built program has them left out of its file, so nothing in them may run before main, nor
// as a thread starts (D161): each file of theirs makes its globals on first use.
std::string startupCodeIn(const std::vector<std::string>& names) {
#ifdef _WIN32
    const auto* base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    auto inNamed = [&](const void* code) -> std::string {
        const uintptr_t rva = reinterpret_cast<uintptr_t>(code) - reinterpret_cast<uintptr_t>(base);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++) {
            const IMAGE_SECTION_HEADER& s = sections[i];
            if (rva < s.VirtualAddress || rva >= s.VirtualAddress + s.Misc.VirtualSize) continue;
            char name[9] = {};
            std::memcpy(name, s.Name, 8);
            return std::find(names.begin(), names.end(), std::string(name)) != names.end() ? std::string(name) : std::string();
        }
        return {};
    };
    const std::pair<StartFunction*, StartFunction*> tables[] = {{__xc_a, __xc_z}, {__xi_a, __xi_z}};
    for (const auto& table : tables) {
        for (StartFunction* entry = table.first; entry < table.second; entry++) {
            if (*entry == nullptr) continue;
            std::string section = inNamed(reinterpret_cast<const void*>(*entry));
            if (!section.empty()) {
                char at[32];
                std::snprintf(at, sizeof at, "0x%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(*entry) - reinterpret_cast<uintptr_t>(base)));
                return "start-up code at " + std::string(at) + " in the image" + " (in the section " + section + ")" +
                       " (a global of a file there made as the program starts: make it on first use)";
            }
        }
    }
#else
    (void)names;
#endif
    return {};
}

// The build's check of the sections a built program leaves out (D161), on the link maps of
// astra.exe, of the core and the window linked alone, and of the core alone: every function
// those need is in astra.exe in a section they keep, not one left out. A template or inline
// function that files on both sides use is made once, from either; this catches the one made
// on the side left out. What is wrong, or nothing.
std::string checkMaps(const std::string& astraMap, const std::string& withWindowMap, const std::string& plainMap) {
    // A map's sections by their number, and its symbols with the number of the section each is in.
    struct Map {
        std::unordered_map<std::string, std::string> sectionNamed;  // "0003" -> ".astrac"
        std::unordered_map<std::string, std::string> sectionOf;     // symbol -> "0003"
    };
    auto read = [](const std::string& path, Map& map) {
        std::ifstream file(path);
        if (!file) return false;
        std::string line;
        bool symbols = false;
        while (std::getline(file, line)) {
            if (line.find("Publics by Value") != std::string::npos) symbols = true;
            std::istringstream words(line);
            std::string place, second, third;
            if (!(words >> place) || place.size() != 13 || place[4] != ':') continue;
            if (!(words >> second)) continue;
            const std::string number = place.substr(0, 4);
            if (!symbols) {
                // " 0003:00000000 000e5720H .astrac   CODE": the first part of a section names it
                if (words >> third && third[0] == '.' && place.substr(5) == "00000000") {
                    const std::string name = third.substr(0, third.find('$'));
                    if (!map.sectionNamed.count(number)) map.sectionNamed[number] = name;
                }
                continue;
            }
            if (second[0] == '$') continue;  // labels inside functions
            map.sectionOf.emplace(second, number);
        }
        return true;
    };
    Map astra, withWindow, plain;
    if (!read(astraMap, astra) || !read(withWindowMap, withWindow) || !read(plainMap, plain)) return "a link map cannot be read";
    auto check = [&](const Map& needs, const std::vector<std::string>& leftOut, const std::vector<std::string>& allowed) -> std::string {
        for (const auto& [symbol, number] : needs.sectionOf) {
            if (std::find(allowed.begin(), allowed.end(), symbol) != allowed.end()) continue;
            auto where = astra.sectionOf.find(symbol);
            if (where == astra.sectionOf.end()) continue;
            auto name = astra.sectionNamed.find(where->second);
            if (name == astra.sectionNamed.end()) continue;
            if (std::find(leftOut.begin(), leftOut.end(), name->second) != leftOut.end())
                return symbol + " is needed by what a built program keeps, but astra.exe has it in " + name->second;
        }
        return {};
    };
    if (std::string wrong = check(withWindow, {".astrac", ".astras"}, {}); !wrong.empty()) return wrong;
    // The core calls the window's two entry points only when its section is there.
    return check(plain, {".astrac", ".astras", ".astraj"},
                 {"?registerJade@astra@@YAXAEAVVM@1@@Z", "?registerText@astra@@YAXAEAVVM@1@@Z"});
}

// Leaves the bytes of the named sections out of an executable's image (M6.5 part 5, D161):
// each keeps its place in memory, empty, so no address moves; the sections after it in the
// file move up, and the debug directory's places in the file with them. False if the image is
// not one this can read.
bool leaveOutSections(std::string& image, const std::vector<std::string>& names) {
#ifdef _WIN32
    if (image.size() < sizeof(IMAGE_DOS_HEADER)) return false;
    auto* bytes = reinterpret_cast<unsigned char*>(image.data());
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > image.size())
        return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(bytes + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
    const unsigned count = nt->FileHeader.NumberOfSections;
    auto* sections = IMAGE_FIRST_SECTION(nt);
    if (reinterpret_cast<unsigned char*>(sections + count) > bytes + image.size()) return false;
    // A signed file's signature would no longer fit it; none is made here.
    if (nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY].Size != 0) return false;

    // Where the debug directory's entries are in the file, before anything moves.
    std::vector<IMAGE_DEBUG_DIRECTORY*> debug;
    const IMAGE_DATA_DIRECTORY& debugDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    for (unsigned i = 0; i < count && debugDir.Size != 0; i++) {
        const IMAGE_SECTION_HEADER& s = sections[i];
        if (debugDir.VirtualAddress < s.VirtualAddress || debugDir.VirtualAddress + debugDir.Size > s.VirtualAddress + s.SizeOfRawData)
            continue;
        const size_t at = s.PointerToRawData + (debugDir.VirtualAddress - s.VirtualAddress);
        for (size_t e = 0; e + sizeof(IMAGE_DEBUG_DIRECTORY) <= debugDir.Size; e += sizeof(IMAGE_DEBUG_DIRECTORY))
            debug.push_back(reinterpret_cast<IMAGE_DEBUG_DIRECTORY*>(bytes + at + e));
    }
    std::vector<uint32_t> debugAt;  // their places in the file, kept apart while the bytes move
    for (const IMAGE_DEBUG_DIRECTORY* entry : debug) debugAt.push_back(entry->PointerToRawData);

    // The sections to leave out, last in the file first, so that each cut leaves the earlier
    // ones where they are.
    std::vector<unsigned> cut;
    for (unsigned i = 0; i < count; i++) {
        char name[9] = {};
        std::memcpy(name, sections[i].Name, 8);
        if (std::find(names.begin(), names.end(), std::string(name)) != names.end() && sections[i].SizeOfRawData != 0) cut.push_back(i);
    }
    std::sort(cut.begin(), cut.end(), [&](unsigned a, unsigned b) { return sections[a].PointerToRawData > sections[b].PointerToRawData; });

    std::vector<uint32_t> placed(count);
    for (unsigned i = 0; i < count; i++) placed[i] = sections[i].PointerToRawData;
    std::vector<std::pair<uint32_t, uint32_t>> cuts;  // in the file as it was: where, and how much
    for (unsigned i : cut) {
        const uint32_t from = sections[i].PointerToRawData, size = sections[i].SizeOfRawData;
        if (static_cast<size_t>(from) + size > image.size()) return false;
        cuts.emplace_back(from, size);
        for (unsigned j = 0; j < count; j++)
            if (placed[j] > from) placed[j] -= size;
        for (uint32_t& at : debugAt)
            if (at > from) at -= size;
        placed[i] = 0;
    }
    // The headers first, while they are where they were; then the bytes.
    for (unsigned i = 0; i < count; i++) sections[i].PointerToRawData = placed[i];
    for (unsigned i : cut) sections[i].SizeOfRawData = 0;
    for (size_t e = 0; e < debug.size(); e++) debug[e]->PointerToRawData = debugAt[e];
    for (const auto& [from, size] : cuts) image.erase(from, size);
    return true;
#else
    (void)image;
    (void)names;
    return true;
#endif
}


const char* const kUsage =
    "Astra " ASTRA_VERSION " - an object-oriented scripting language\n"
    "\n"
    "Usage:\n"
    "  astra                                 start the interactive shell\n"
    "  astra <file> [args...]                run a source (.astra) or bytecode (.astc) file\n"
    "  astra run <file> [args...]            same as above\n"
    "  astra build <file> [-o <out.astc>]    compile a source file to a bytecode file\n"
    "  astra build <file> --exe [-o <app.exe>] [--windowed] [--icon <file.ico>]\n"
    "                                        compile it into a program of its own;\n"
    "                                        --windowed gives it no console window,\n"
    "                                        --icon gives it an icon\n"
    "  astra check <file>                    check a source file's types, running nothing\n"
    "  astra dis <file>                      list the bytecode of a source or bytecode file\n"
    "  astra --version\n"
    "  astra --help\n";

// Where the libraries that come with Astra, such as Jade, live: a "lib" folder beside the executable, and
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

// The pictures of an .ico file in the form an executable stores them: each picture as a
// resource of its own, and a directory that lists them.
struct IconResources {
    std::string directory;
    std::vector<std::string> pictures;
};

// Takes an .ico file apart; false if `ico` is not one.
bool readIcon(const std::string& ico, IconResources& out) {
    auto u16 = [&](size_t at) {
        return static_cast<uint32_t>(static_cast<unsigned char>(ico[at])) |
               static_cast<uint32_t>(static_cast<unsigned char>(ico[at + 1])) << 8;
    };
    auto u32 = [&](size_t at) { return u16(at) | u16(at + 2) << 16; };

    // The file starts with a reserved zero, the type (1 for icons, 2 for cursors) and the
    // number of pictures, followed by a 16-byte entry for each picture.
    const size_t kHeader = 6, kEntry = 16;
    if (ico.size() < kHeader || u16(0) != 0 || u16(2) != 1) return false;
    const size_t count = u16(4);
    if (count == 0 || ico.size() < kHeader + count * kEntry) return false;

    out.directory.assign(ico, 0, kHeader);
    for (size_t i = 0; i < count; i++) {
        const size_t entry = kHeader + i * kEntry;
        const uint64_t size = u32(entry + 8), offset = u32(entry + 12);
        if (size == 0 || offset + size > ico.size()) return false;
        out.pictures.push_back(ico.substr(static_cast<size_t>(offset), static_cast<size_t>(size)));

        // An entry in an executable is the one from the file with the picture's position in
        // the file replaced by the number of its resource.
        const size_t id = i + 1;
        out.directory.append(ico, entry, 12);
        out.directory.push_back(static_cast<char>(id & 0xFF));
        out.directory.push_back(static_cast<char>(id >> 8));
    }
    return true;
}

// Stores the icon in the executable at `exe`, as the icon Windows shows for the file. This
// rewrites the file, so it has to happen before the compiled program is attached.
bool storeIcon(const std::filesystem::path& exe, const IconResources& icon) {
    HANDLE update = BeginUpdateResourceW(exe.c_str(), FALSE);
    if (update == nullptr) return false;

    const WORD language = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
    auto store = [&](const wchar_t* type, size_t id, const std::string& data) {
        return UpdateResourceW(update, type, MAKEINTRESOURCEW(id), language, const_cast<char*>(data.data()),
                               static_cast<DWORD>(data.size())) != 0;
    };
    const wchar_t* const pictureType = MAKEINTRESOURCEW(3);     // RT_ICON
    const wchar_t* const directoryType = MAKEINTRESOURCEW(14);  // RT_GROUP_ICON
    bool stored = store(directoryType, 1, icon.directory);
    for (size_t i = 0; stored && i < icon.pictures.size(); i++) stored = store(pictureType, i + 1, icon.pictures[i]);

    return EndUpdateResourceW(update, stored ? FALSE : TRUE) != 0 && stored;
}
#endif

// Loads `path` into `program`, compiling it unless it is already bytecode.
// Returns 0 on success, otherwise the exit code after reporting the problem.
// Runs `work`, adding the time it takes to `total`.
template <typename Work>
static bool timed(double& total, Work work) {
    const auto start = std::chrono::steady_clock::now();
    const bool result = work();
    total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return result;
}

// A kept program, read as a bytecode file is; one that cannot be read counts as not kept.
static bool loadKept(VM& vm, const std::string& bytecode, std::unique_ptr<Program>& owner) {
    auto kept = std::make_unique<Program>();
    try {
        readProgram(*kept, vm, bytecode);
    } catch (const std::exception&) {
        return false;
    }
    owner = std::move(kept);
    return true;
}

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
        } else if (std::string kept; timed(compileTimes.cacheCheck, [&] { return cachedProgram(path, kept); }) &&
                                      timed(compileTimes.cacheRead, [&] { return loadKept(vm, kept, programOwner); })) {
            // Nothing it was made from has changed since it was kept: it starts at once (D145).
            program = programOwner.get();
            session = nullptr;
        } else {
            sessionOwner = std::make_unique<CompilerSession>(vm);
            sessionOwner->libraryPaths = libraryPaths();
            sessionOwner->compileFile(path);
            session = sessionOwner.get();
            program = &sessionOwner->program;
            const Program* made = program;
            keepProgram(path, session->sourcesRead(), session->foldersRead(), [made] {
                std::ostringstream image;
                writeProgram(*made, image);
                return image.str();
            });
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

// `astra check`: compiles the file and what it imports, then checks their types (M6.5).
// Each problem is printed as a compile error is; nothing runs.
int checkFile(const std::string& path, bool standardLibrary) {
    std::string data;
    if (!readFile(path, data)) {
        std::cerr << "astra: cannot read '" << path << "'\n";
        return kExitNoInput;
    }
    VM vm;
    Checker checker;
    checker.reportStandardLibrary = standardLibrary;
    CompilerSession session(vm, &checker);
    session.stopAtTypeErrors = false;  // every one is listed below
    session.libraryPaths = libraryPaths();
    try {
        session.compileFile(path);
    } catch (const CompileError& e) {
        std::cerr << e.format() << "\n";
        return kExitCompileError;
    }
    checker.run();
    for (const CheckDiagnostic& d : checker.diagnostics)
        std::cout << d.file << ":" << d.line << ":" << d.col << ": error: " << d.message << "\n";
    return checker.diagnostics.empty() ? 0 : kExitCompileError;
}

// With ASTRA_TIMES set: how long the program took to be ready to run, and its parts (D145).
static void showTimes(const char* what) {
    if (std::getenv("ASTRA_TIMES") == nullptr) return;
    const double ready = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - executableStarted).count();
#ifdef _WIN32
    // From when the system made the process, its loading included: the start a user waits for.
    FILETIME created, exited, kernel, user, now;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        GetSystemTimePreciseAsFileTime(&now);
        auto ticks = [](const FILETIME& t) { return (static_cast<unsigned long long>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
        std::fprintf(stderr, "astra: %s %.1f ms after the process was made\n", what, (ticks(now) - ticks(created)) / 10000.0);
    }
#endif
    if (compileTimes.cacheRead > 0)
        std::fprintf(stderr, "astra: %s %.1f ms after astra.exe started its work, from the program kept: its files looked at %.1f, "
                     "it read in %.1f\n", what, ready, compileTimes.cacheCheck, compileTimes.cacheRead);
    else
        std::fprintf(stderr, "astra: %s %.1f ms after astra.exe started its work: parsing %.1f, checking %.1f, compiling %.1f "
                     "(units put back %d, compiled %d; the standard library %s)\n",
                     what, ready, compileTimes.parse, compileTimes.check, compileTimes.compile, compileTimes.unitsReused,
                     compileTimes.unitsCompiled, compileTimes.preludeAttached ? "attached" : "compiled");
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
    showTimes("ready to run");
    // ASTRA_TIMES=compile stops here: the budgets are timed on programs that are not run.
    if (const char* times = std::getenv("ASTRA_TIMES"); times != nullptr && std::strcmp(times, "compile") == 0) {
        // What is kept is written as a program runs; here, with nothing to run, it is waited for.
        const auto waited = std::chrono::steady_clock::now();
        finishKeeping();
        std::fprintf(stderr, "astra: waited %.1f ms for the cache to be written\n",
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waited).count());
        return 0;
    }
    setSelfCommand({executablePath().string(), std::filesystem::absolute(path).string()});
    const int code = runProgram(vm, *program, program->entry);
    finishKeeping();
    return code;
}

int buildFile(const std::string& path, std::string outPath, bool asExecutable, bool windowed,
              const std::string& iconPath) {
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
    IconResources icon;
    if (!iconPath.empty()) {
        std::string ico;
        if (!readBytes(iconPath, ico)) {
            std::cerr << "astra: cannot read '" << iconPath << "'\n";
            return kExitNoInput;
        }
        if (!readIcon(ico, icon)) {
            std::cerr << "astra: '" << iconPath << "' is not an icon file; --icon takes an .ico file\n";
            return kExitUsage;
        }
    }
#else
    if (!iconPath.empty()) {
        std::cerr << "astra: --icon is only available on Windows\n";
        return kExitUsage;
    }
#endif

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
    showTimes("compiled");

    // What the program can never reach is left out of it (M6.5 part 5).
    const LeftOut leftOut = leaveOutUnreachable(session.program, VM::globalsFoundByName(), vm.membersFoundByName());
    if (std::getenv("ASTRA_TIMES") != nullptr)
        std::cerr << "astra: left out " << leftOut.declarations << " declarations; the modules' own code "
                  << leftOut.codeBefore << " bytes, now " << leftOut.codeAfter << "\n";

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
    stripPrelude(image);  // the program carries its own compiled copy of what it uses
    // What a built program never needs is left out of it: the compiler side and the standard
    // library's source always, and Jade's window with the text engine when nothing left in the
    // program names them (M6.5 part 5, D161).
    std::vector<std::string> unneeded{".astrac", ".astras"};
    if (!namesGlobal(session.program, {"__jade", "__text"})) unneeded.push_back(".astraj");
    if (const std::string inside = startupCodeIn(unneeded); !inside.empty()) {
        std::cerr << "astra: internal error: " << inside << "\n";
        return kExitCannotWrite;
    }
    if (!leaveOutSections(image, unneeded)) {
        std::cerr << "astra: cannot make a program from this executable (its file is not as expected)\n";
        return kExitCannotWrite;
    }

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

    auto writeRuntime = [&] {
        std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
        out.write(image.data(), static_cast<std::streamsize>(image.size()));
        out.flush();
        return static_cast<bool>(out);
    };
    if (!writeRuntime()) {
        std::cerr << "astra: cannot write '" << outPath << "' (is that program still running?)\n";
        return kExitCannotWrite;
    }

#ifdef _WIN32
    if (!iconPath.empty()) {
        // A virus scanner looking at the file just written can make this fail for a moment,
        // so it is tried a few times, each time on a fresh copy.
        bool stored = storeIcon(outPath, icon);
        for (int attempt = 0; !stored && attempt < 5; attempt++) {
            Sleep(200);
            stored = writeRuntime() && storeIcon(outPath, icon);
        }
        if (!stored) {
            std::error_code ignored;
            std::filesystem::remove(outPath, ignored);
            std::cerr << "astra: cannot put the icon into '" << outPath << "'\n";
            return kExitCannotWrite;
        }
    }
#endif

    std::ofstream out(outPath, std::ios::binary | std::ios::app);
    out.write(bytecode.data(), static_cast<std::streamsize>(bytecode.size()));
    out.write(sizeBytes, sizeof sizeBytes);
    out.write(kBundleMarker, sizeof kBundleMarker);
    out.flush();
    if (!out) {
        std::error_code ignored;
        std::filesystem::remove(outPath, ignored);
        std::cerr << "astra: cannot write '" << outPath << "'\n";
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
    finishKeeping();  // the cache, if it is being written, before the program ends
    return 0;
}

bool isBlank(const std::string& s) { return s.find_first_not_of(" \t\r\n") == std::string::npos; }

int shell() {
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
            module = session.compileSource(buffer, "<shell>", ".", true);
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
            vm.runEventLoop(false, 50);  // whatever has been posted and is ready, without waiting
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
    if (args.empty()) return shell();

    const std::string& command = args[0];
    if (command == "--help" || command == "-h" || command == "help") {
        std::cout << kUsage;
        return 0;
    }
    if (command == "--version" || command == "-v") {
        std::cout << "astra " ASTRA_VERSION "\n";
        return 0;
    }
    if (command == "shell") return shell();

    if (command == "run") {
        if (args.size() < 2) {
            std::cerr << "astra: 'run' needs a file\n";
            return kExitUsage;
        }
        return runFile(args[1], std::vector<std::string>(args.begin() + 2, args.end()));
    }

    if (command == "build") {
        std::string input, output, icon;
        bool asExecutable = false, windowed = false;
        for (size_t i = 1; i < args.size(); i++) {
            if (args[i] == "-o" && i + 1 < args.size()) {
                output = args[++i];
            } else if (args[i] == "--icon" && i + 1 < args.size()) {
                icon = args[++i];
                asExecutable = true;
            } else if (args[i] == "--exe") {
                asExecutable = true;
            } else if (args[i] == "--windowed") {
                asExecutable = windowed = true;
            } else if (input.empty() && !args[i].empty() && args[i][0] != '-') {
                input = args[i];
            } else {
                std::cerr << "astra: usage: astra build <file> [-o <out>] [--exe] [--windowed] [--icon <file.ico>]\n";
                return kExitUsage;
            }
        }
        if (input.empty()) {
            std::cerr << "astra: 'build' needs a file\n";
            return kExitUsage;
        }
        // Asking for an .exe by name is asking for a program.
        if (std::filesystem::path(output).extension() == ".exe") asExecutable = true;
        return buildFile(input, output, asExecutable, windowed, icon);
    }

    if (command == "--check-sections" && args.size() == 4) {
        // The build's check of what a built program leaves out (D161): nothing of it runs as the
        // program starts, and nothing a built program keeps is made there (the link maps).
        std::string wrong = startupCodeIn({".astrac", ".astras", ".astraj"});
        if (wrong.empty()) wrong = checkMaps(args[1], args[2], args[3]);
        if (wrong.empty()) return 0;
        std::cerr << "astra: " << wrong << "\n";
        return 1;
    }

    if (command == "--attach-prelude" && args.size() == 2) {
        // The build's last step: a copy of this executable with the standard library compiled
        // and attached (D145, point 3), which the build then puts in its place.
        compilingPrelude = true;
        VM vm;
        std::string image;
        try {
            CompilerSession session(vm);
            image = session.preludeImage();
        } catch (const CompileError& e) {
            std::cerr << e.format() << "\n";
            return kExitCompileError;
        }
        if (image.empty() || !attachPrelude(args[1], image)) {
            std::cerr << "astra: cannot attach the standard library to " << args[1] << "\n";
            return kExitCannotWrite;
        }
        return 0;
    }

    if (command == "check") {
        // `--std` reports what is found in the standard library as well, for working on it.
        bool standardLibrary = false;
        size_t at = 1;
        for (; at + 1 < args.size(); at++) {
            if (args[at] == "--std") standardLibrary = true;
            else break;
        }
        if (at + 1 != args.size()) {
            std::cerr << "astra: usage: astra check <file>\n";
            return kExitUsage;
        }
        return checkFile(args.back(), standardLibrary);
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

namespace astra {

int astraCommandMain(const std::vector<std::string>& args) { return dispatch(args); }

}  // namespace astra
