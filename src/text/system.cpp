// The text engine: the two things asked of the system. Which font files are installed, and
// a file's bytes. Everything else about text is the same code on every system; this file is
// the part written once for each.

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>

#include "fonts.h"

namespace astra::text {

namespace {

bool fontFileName(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".ttf" || ext == ".otf" || ext == ".ttc" || ext == ".otc";
}

std::string utf8Of(const std::filesystem::path& path) {
    const std::u8string s = path.u8string();
    return std::string(s.begin(), s.end());
}

std::filesystem::path pathOf(const std::string& utf8) { return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())); }

void addFolder(const std::filesystem::path& folder, std::vector<std::string>& out, std::set<std::string>& seen, bool recurse) {
    std::error_code ec;
    auto add = [&](const std::filesystem::directory_entry& entry) {
        if (!entry.is_regular_file(ec) || !fontFileName(entry.path())) return;
        std::string path = utf8Of(entry.path());
        std::string key = path;
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (seen.insert(key).second) out.push_back(std::move(path));
    };
    if (recurse) {
        for (std::filesystem::recursive_directory_iterator it(folder, std::filesystem::directory_options::skip_permission_denied, ec), end; !ec && it != end;
             it.increment(ec))
            add(*it);
    } else {
        for (std::filesystem::directory_iterator it(folder, std::filesystem::directory_options::skip_permission_denied, ec), end; !ec && it != end;
             it.increment(ec))
            add(*it);
    }
}

#ifdef _WIN32

struct MappedFile final : FontData {
    HANDLE file = INVALID_HANDLE_VALUE, mapping = nullptr;
    ~MappedFile() override {
        if (data != nullptr) UnmapViewOfFile(data);
        if (mapping != nullptr) CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
};

std::filesystem::path folderFromEnv(const wchar_t* variable, const wchar_t* rest) {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(variable, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    return std::filesystem::path(buffer) / rest;
}

#else

struct LoadedFile final : FontData {
    std::vector<uint8_t> bytes;
};

#endif

}  // namespace

std::shared_ptr<const FontData> openFontFile(const std::string& path) {
#ifdef _WIN32
    auto out = std::make_shared<MappedFile>();
    // Shared for everything, deleting included: a font being in use here does not stop it
    // from being replaced or removed.
    out->file = CreateFileW(pathOf(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out->file == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(out->file, &size) || size.QuadPart < 12 || size.QuadPart > (LONGLONG{1} << 31)) return nullptr;
    out->mapping = CreateFileMappingW(out->file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (out->mapping == nullptr) return nullptr;
    out->data = static_cast<const uint8_t*>(MapViewOfFile(out->mapping, FILE_MAP_READ, 0, 0, 0));
    if (out->data == nullptr) return nullptr;
    out->size = static_cast<size_t>(size.QuadPart);
    return out;
#else
    std::ifstream file(pathOf(path), std::ios::binary | std::ios::ate);
    if (!file) return nullptr;
    const std::streamoff size = file.tellg();
    if (size < 12 || size > (std::streamoff{1} << 31)) return nullptr;
    auto out = std::make_shared<LoadedFile>();
    out->bytes.resize(static_cast<size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(out->bytes.data()), size);
    if (!file) return nullptr;
    out->data = out->bytes.data();
    out->size = out->bytes.size();
    return out;
#endif
}

std::vector<std::string> installedFontFiles() {
    std::vector<std::string> out;
    std::set<std::string> seen;
#ifdef _WIN32
    // Fonts for everyone, and the ones this user installed for themselves.
    addFolder(folderFromEnv(L"WINDIR", L"Fonts"), out, seen, false);
    addFolder(folderFromEnv(L"LOCALAPPDATA", L"Microsoft\\Windows\\Fonts"), out, seen, false);
#elif defined(__APPLE__)
    addFolder("/System/Library/Fonts", out, seen, true);
    addFolder("/Library/Fonts", out, seen, true);
    if (const char* home = std::getenv("HOME")) addFolder(std::filesystem::path(home) / "Library/Fonts", out, seen, true);
#else
    addFolder("/usr/share/fonts", out, seen, true);
    addFolder("/usr/local/share/fonts", out, seen, true);
    if (const char* home = std::getenv("HOME")) {
        addFolder(std::filesystem::path(home) / ".local/share/fonts", out, seen, true);
        addFolder(std::filesystem::path(home) / ".fonts", out, seen, true);
    }
#endif
    std::sort(out.begin(), out.end());
    return out;
}

// The families to try, in order, for each of the names that mean "the system's own font of
// this kind". The first that is installed is used.
std::vector<std::string> logicalFamily(const std::string& name) {
#ifdef _WIN32
    if (name == "sansserif") return {"Segoe UI", "Tahoma", "Arial"};
    if (name == "serif") return {"Times New Roman", "Georgia", "Cambria"};
    if (name == "monospaced") return {"Consolas", "Cascadia Mono", "Courier New", "Lucida Console"};
#elif defined(__APPLE__)
    if (name == "sansserif") return {".AppleSystemUIFont", "SF Pro", "SF Pro Text", "Helvetica Neue", "Helvetica"};
    if (name == "serif") return {"Times New Roman", "Times"};
    if (name == "monospaced") return {"SF Mono", "Menlo", "Monaco"};
#else
    if (name == "sansserif") return {"Noto Sans", "DejaVu Sans", "Liberation Sans", "Cantarell", "Ubuntu"};
    if (name == "serif") return {"Noto Serif", "DejaVu Serif", "Liberation Serif"};
    if (name == "monospaced") return {"Noto Sans Mono", "DejaVu Sans Mono", "Liberation Mono", "Ubuntu Mono"};
#endif
    return {};
}

// Where to look for a character the chosen font lacks, before looking through every font
// there is: the system's fonts for other scripts, symbols and emoji, best first.
std::vector<std::string> fallbackFamilies() {
#ifdef _WIN32
    return {"Segoe UI",       "Segoe UI Emoji",  "Segoe UI Symbol",    "Segoe UI Historic", "Microsoft YaHei", "Yu Gothic UI",
            "Yu Gothic",      "Malgun Gothic",   "Microsoft JhengHei", "Nirmala UI",        "Leelawadee UI",   "Ebrima",
            "Gadugi",         "Javanese Text",   "Myanmar Text",       "Mongolian Baiti",   "Microsoft Himalaya", "Microsoft Yi Baiti",
            "Microsoft New Tai Lue", "Microsoft Tai Le", "Microsoft PhagsPa", "Sylfaen",    "Cambria Math",    "SimSun",
            "SimSun-ExtB",    "MingLiU-ExtB",    "Arial",              "Tahoma",            "Times New Roman"};
#elif defined(__APPLE__)
    return {"Helvetica Neue", "Apple Color Emoji", "Apple Symbols", "PingFang SC", "Hiragino Sans", "Apple SD Gothic Neo", "Geeza Pro", "Arial Unicode MS"};
#else
    return {"Noto Sans", "DejaVu Sans", "Noto Color Emoji", "Noto Sans Symbols", "Noto Sans Symbols 2", "Noto Sans CJK SC", "Noto Sans CJK JP",
            "Noto Sans CJK KR", "Noto Sans Arabic", "Noto Sans Hebrew", "Noto Sans Devanagari", "FreeSans", "Unifont"};
#endif
}

}  // namespace astra::text
