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
#include <functional>
#include <string>
#include <vector>

namespace astra {

// Reads a whole file into `out`, in one read. False if it cannot be read.
bool readWholeFile(const std::filesystem::path& path, std::string& out);

// The standard library compiled when astra.exe was built (D145, point 3), attached to the end
// of the file with its size and a marker: read from there without reading the rest, made into
// a copy of the file with it attached, and taken off a file's image (for a built program,
// which carries what it needs of it in its own code).
bool attachedPrelude(std::string& image);
bool attachPrelude(const std::filesystem::path& copy, const std::string& image);
void stripPrelude(std::string& file);

// A file a program was compiled from: its text's hash, size and time when it was read.
struct SourceRead {
    std::string path;
    uint64_t hash = 0;
    uintmax_t size = 0;
    long long time = 0;
};

// A folder whose .astra files a program took: its time, and the hash of their names, when
// they were listed.
struct FolderRead {
    std::string path;
    long long time = 0;
    uint64_t hash = 0;
};

// A file's or folder's size and time, as the cache compares them. False if it cannot be
// looked at.
bool fileStamp(const std::string& path, uintmax_t& size, long long& time);

// The hash the cache keeps of a folder's .astra files: of their names, in name order.
uint64_t namesHash(std::vector<std::string> names);

// The hash the cache keeps of a file's text (64-bit FNV-1a).
uint64_t textHash(const std::string& text);

// The program kept for the script at `path`, if nothing it was made from has changed since:
// its bytecode. False when there is none, or it no longer holds.
bool cachedProgram(const std::string& path, std::string& bytecode);

// Keeps a script's program for its next run. `bytecode` makes the program's bytes; it is
// called, and the entry written, on a thread of its own, as the program starts, so the
// program must not change meanwhile (a running program never changes its code).
// finishKeeping waits for that thread before astra.exe ends.
void keepProgram(const std::string& path, std::vector<SourceRead> files, std::vector<FolderRead> folders,
                 std::function<std::string()> bytecode);
void finishKeeping();

// A unit of a program kept compiled (D145, points 1 and 2): a file's modules, or a folder's.
// It is kept with its own files' texts and summaries, by hash, and with the summary of every
// file its compile depended on (what it imports, near and far, and the other files of its
// folder): what each declares, with what was learned of it from bodies. It is put back into a
// compile whose own files are the same, and whose files it depends on show the same summaries;
// the compiler checks that, and what `image` holds is its own.
struct KeptFile {
    std::string path;
    uint64_t text = 0, summary = 0;
};
struct KeptUnit {
    std::vector<KeptFile> own;   // its files, text and summary
    std::vector<KeptFile> uses;  // what it depends on, summary only
    std::string image;
};
bool keptUnit(const std::string& unit, KeptUnit& out);
// Keeps a unit, written with the program's entry by the same thread (`image` is called there).
void keepUnit(const std::string& unit, KeptUnit entry, std::function<std::string()> image);

}  // namespace astra
