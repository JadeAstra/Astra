#include "cache.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace astra {

namespace fs = std::filesystem;

namespace {

constexpr const char* kMagic = "ASTRA-PROGRAM-CACHE 2";
constexpr uintmax_t kMostBytes = 500ull * 1024 * 1024;

std::thread writer;

fs::path cacheDirectory() {
    if (const char* chosen = std::getenv("ASTRA_CACHE"); chosen != nullptr && *chosen != '\0') return fs::path(chosen);
#ifdef _WIN32
    if (const char* local = std::getenv("LOCALAPPDATA"); local != nullptr && *local != '\0')
        return fs::path(local) / "Astra" / "cache";
#else
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && *xdg != '\0') return fs::path(xdg) / "astra";
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') return fs::path(home) / ".cache" / "astra";
#endif
    return {};
}

fs::path executable() {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return n == 0 ? fs::path() : fs::path(std::wstring(buffer, n));
#else
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec);
#endif
}

std::string hex(uint64_t v) {
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(v));
    return text;
}

// A file's or a folder's size and time, as the entry keeps them, in one look at it. A
// folder's time changes when a file is added to it, taken away or renamed.
bool stamp(const fs::path& path, uintmax_t& size, long long& time) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    const bool folder = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    size = folder ? 0 : (static_cast<uintmax_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    time = static_cast<long long>((static_cast<unsigned long long>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                                  data.ftLastWriteTime.dwLowDateTime);
    return true;
#else
    std::error_code ec;
    const fs::file_status status = fs::status(path, ec);
    if (ec) return false;
    size = fs::is_directory(status) ? 0 : fs::file_size(path, ec);
    if (ec) return false;
    const auto written = fs::last_write_time(path, ec);
    if (ec) return false;
    time = static_cast<long long>(written.time_since_epoch().count());
    return true;
#endif
}

// The .astra files directly in a folder, as one hash of their names.
uint64_t folderHash(const fs::path& folder) {
    std::error_code ec;
    std::vector<std::string> names;
    for (const fs::directory_entry& entry : fs::directory_iterator(folder, ec))
        if (entry.is_regular_file(ec) && entry.path().extension() == ".astra") names.push_back(entry.path().filename().string());
    std::sort(names.begin(), names.end());
    std::string joined;
    for (const std::string& name : names) joined += name + "\n";
    return textHash(joined);
}

// What astra.exe is: a program kept by another build of it does not hold.
std::string executableStamp() {
    uintmax_t size = 0;
    long long time = 0;
    if (!stamp(executable(), size, time)) return "?";
    return std::to_string(size) + " " + std::to_string(time);
}

fs::path entryFor(const std::string& script) {
    const fs::path dir = cacheDirectory();
    if (dir.empty()) return {};
    std::error_code ec;
    fs::path full = fs::weakly_canonical(fs::absolute(script, ec), ec);
    return dir / (hex(textHash(full.string())) + ".program");
}

bool readAll(const fs::path& path, std::string& out) { return readWholeFile(path, out); }

// Past the cache's size, the entries used longest ago go, and any temporary file left by a
// run that ended while writing.
void trim(const fs::path& dir) {
    std::error_code ec;
    struct Entry {
        fs::path path;
        uintmax_t size;
        fs::file_time_type used;
    };
    std::vector<Entry> entries;
    uintmax_t total = 0;
    const auto now = fs::file_time_type::clock::now();
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        const auto used = e.last_write_time(ec);
        if (e.path().extension() == ".tmp") {
            if (now - used > std::chrono::hours(1)) fs::remove(e.path(), ec);
            continue;
        }
        const uintmax_t size = e.file_size(ec);
        entries.push_back({e.path(), size, used});
        total += size;
    }
    if (total <= kMostBytes) return;
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.used < b.used; });
    for (const Entry& e : entries) {
        if (total <= kMostBytes) break;
        if (fs::remove(e.path, ec)) total -= e.size;
    }
}

void write(fs::path entry, std::string text) {
    std::error_code ec;
    fs::create_directories(entry.parent_path(), ec);
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
    fs::path temporary = entry;
    temporary += "." + std::to_string(pid) + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) {
            out.close();
            fs::remove(temporary, ec);
            return;
        }
    }
    fs::rename(temporary, entry, ec);  // whole or not at all
    if (ec) fs::remove(temporary, ec);
    trim(entry.parent_path());
}

}  // namespace

bool fileStamp(const std::string& path, uintmax_t& size, long long& time) { return stamp(path, size, time); }

bool readWholeFile(const fs::path& path, std::string& out) {
#ifdef _WIN32
    std::FILE* file = _wfopen(path.c_str(), L"rb");
#else
    std::FILE* file = std::fopen(path.c_str(), "rb");
#endif
    if (file == nullptr) return false;
    std::string text;
    if (std::fseek(file, 0, SEEK_END) == 0) {
        const long size = std::ftell(file);
        if (size > 0) {
            text.resize(static_cast<size_t>(size));
            std::fseek(file, 0, SEEK_SET);
            text.resize(std::fread(text.data(), 1, text.size(), file));
        }
    }
    // A file whose size cannot be told beforehand (a pipe) is read to its end in pieces.
    char piece[65536];
    for (size_t got; (got = std::fread(piece, 1, sizeof piece, file)) > 0;) text.append(piece, got);
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    if (failed) return false;
    out = std::move(text);
    return true;
}

uint64_t textHash(const std::string& text) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : text) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

// One thing an entry was made from, as the entry records it.
struct Source {
    bool folder = false;
    uintmax_t size = 0;
    long long time = 0;
    std::string hash, path;
};

// Whether it is as it was: by its size and time, or, where those differ, by its text (a file)
// or the files in it (a folder).
bool unchanged(const Source& s) {
    uintmax_t size = 0;
    long long time = 0;
    if (!stamp(s.path, size, time)) return false;
    if (size == s.size && time == s.time) return true;
    if (s.folder) return hex(folderHash(s.path)) == s.hash;
    std::string text;
    return size == s.size && readAll(s.path, text) && hex(textHash(text)) == s.hash;
}

bool cachedProgram(const std::string& path, std::string& bytecode) {
    const fs::path entry = entryFor(path);
    if (entry.empty()) return false;
    std::string text;
    if (!readAll(entry, text)) return false;
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line != kMagic) return false;
    if (!std::getline(in, line) || line != "exe " + executableStamp()) return false;
    std::vector<Source> sources;
    size_t at = 0, n = 0;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string kind;
        fields >> kind;
        Source s;
        if (kind == "file") {
            fields >> s.size >> s.time >> s.hash;
        } else if (kind == "folder") {
            s.folder = true;
            fields >> s.time >> s.hash;
        } else if (kind == "bytes") {
            fields >> n;
            at = static_cast<size_t>(in.tellg());
            break;
        } else {
            return false;
        }
        std::getline(fields >> std::ws, s.path);
        sources.push_back(std::move(s));
    }
    if (at == 0 || at == static_cast<size_t>(-1) || at + n != text.size()) return false;

    // Looking at a file takes the system a while, so they are looked at a few at a time.
    std::atomic<bool> holds{true};
    std::atomic<size_t> next{0};
    auto look = [&] {
        for (size_t i = next++; i < sources.size() && holds; i = next++)
            if (!unchanged(sources[i])) holds = false;
    };
    const size_t helpers = std::min<size_t>(sources.size() / 32, 3);
    std::vector<std::thread> threads;
    for (size_t i = 0; i < helpers; i++) threads.emplace_back(look);
    look();
    for (std::thread& t : threads) t.join();
    if (!holds) return false;

    bytecode = text.substr(at, n);
    // Used now: the last to go when the cache is trimmed.
    std::error_code ec;
    fs::last_write_time(entry, fs::file_time_type::clock::now(), ec);
    return true;
}

void keepProgram(const std::string& path, const std::vector<SourceRead>& files, const std::vector<std::string>& folders,
                 std::string bytecode) {
    const fs::path entry = entryFor(path);
    if (entry.empty()) return;
    std::string text = std::string(kMagic) + "\nexe " + executableStamp() + "\n";
    for (const SourceRead& f : files)
        text += "file " + std::to_string(f.size) + " " + std::to_string(f.time) + " " + hex(f.hash) + " " + f.path + "\n";
    for (const std::string& folder : folders) {
        uintmax_t size = 0;
        long long time = 0;
        if (!stamp(folder, size, time)) return;
        text += "folder " + std::to_string(time) + " " + hex(folderHash(folder)) + " " + folder + "\n";
    }
    text += "bytes " + std::to_string(bytecode.size()) + "\n";
    text += bytecode;
    finishKeeping();
    writer = std::thread(write, entry, std::move(text));
}

void finishKeeping() {
    if (writer.joinable()) writer.join();
}

}  // namespace astra
