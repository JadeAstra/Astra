#pragma once

// A script's compiled program, kept on this machine for its next run (D145, point 6). Running
// `astra something.astra` keeps the program it compiled, with what it was made from: each
// file's size, time and hash, the files of each folder it imported, and astra.exe's own size
// and time. A run with all of that unchanged loads the program and starts at once.
//
// The cache is this machine's own: %LOCALAPPDATA%\Astra\cache on Windows, ~/.cache/astra
// elsewhere, or the directory ASTRA_CACHE names. It is never beside the script. Entries are
// written whole or not at all, a damaged one counts as missing, and past 500 MB the entries
// used longest ago go first, so the cache can only ever cost a slower start.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace astra {

// Reads a whole file into `out`, in one read. False if it cannot be read.
bool readWholeFile(const std::filesystem::path& path, std::string& out);

// A file a program was compiled from: its text's hash, size and time when it was read.
struct SourceRead {
    std::string path;
    uint64_t hash = 0;
    uintmax_t size = 0;
    long long time = 0;
};

// A file's size and time, as the cache compares them. False if it cannot be looked at.
bool fileStamp(const std::string& path, uintmax_t& size, long long& time);

// The hash the cache keeps of a file's text (64-bit FNV-1a).
uint64_t textHash(const std::string& text);

// The program kept for the script at `path`, if nothing it was made from has changed since:
// its bytecode. False when there is none, or it no longer holds.
bool cachedProgram(const std::string& path, std::string& bytecode);

// Keeps a script's program for its next run. The writing is done on a thread of its own, as
// the program starts; finishKeeping waits for it before astra.exe ends.
void keepProgram(const std::string& path, const std::vector<SourceRead>& files, const std::vector<std::string>& folders,
                 std::string bytecode);
void finishKeeping();

}  // namespace astra
