// The text engine: the fonts there are, and choosing among them.
//
// The system is asked for one thing, the font files that are installed. What they are
// called, what weights and styles each family has and which characters each font covers are
// read from the files themselves. A request for a font, "Segoe UI, bold, italic", is matched
// against those; a character the chosen font lacks is looked for in the others.
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "face.h"

namespace astra::text {

// ---- What is asked of the system (system.cpp) ----

std::vector<std::string> installedFontFiles();
// The families to try for "sansserif", "serif" and "monospaced".
std::vector<std::string> logicalFamily(const std::string& lowerName);
std::vector<std::string> fallbackFamilies();

// ---- The fonts ----

// What is known about one font without holding its file open.
struct FaceInfo {
    std::string path;
    int index = 0;
    std::string family, legacyFamily, style, fullName;
    int weight = 400, stretch = 5;
    bool italic = false, monospace = false, symbol = false, cubic = false, color = false;
    // A variable font covers every weight between two; any other has the one.
    bool variable = false;
    int minWeight = 400, maxWeight = 400;
    bool hasSlant = false;  // a variable font with an axis for italics
};

// A font chosen for a request.
struct Match {
    std::shared_ptr<Face> face;
    int which = -1;      // its place in FontBook::faces()
    int weight = 400;    // the weight to set a variable font to
    // What the family lacks and has to be made up: a bold by thickening, an italic by slanting.
    bool fakeBold = false, fakeItalic = false;

    explicit operator bool() const { return face != nullptr; }
};

class FontBook {
public:
    // The fonts installed on this machine, read the first time they are asked for.
    static FontBook& installed();

    FontBook() = default;

    // Adds the fonts of a file. How many it held; 0, with `error`, if it cannot be used.
    int addFile(const std::string& path, std::string& error);

    std::vector<FaceInfo> faces();
    // Every family, in alphabetical order.
    std::vector<std::string> families();
    bool hasFamily(const std::string& family);

    // The font itself, read when first wanted and kept. Null if its file has gone.
    std::shared_ptr<Face> face(int which);

    // The best font there is for a family and a style. The family may be a logical one
    // ("SansSerif", "Serif", "Monospaced", and Java's "Dialog"), or several separated by
    // commas, the first that is installed being used. One that is not installed gives the
    // system's sans-serif: there is always an answer, unless no font is installed at all.
    Match match(const std::string& family, int weight, bool italic, int stretch = 5);
    // The same, or nothing if the family is not installed.
    Match matchExactly(const std::string& family, int weight, bool italic, int stretch = 5);

    // A font that has a character, for text in a font that lacks it. Nothing if none has.
    Match fallback(uint32_t codepoint, int weight, bool italic);

private:
    std::recursive_mutex mutex_;
    std::vector<FaceInfo> faces_;
    std::vector<std::shared_ptr<Face>> loaded_;
    std::vector<bool> failed_;
    std::unordered_map<std::string, std::vector<int>> byFamily_;   // lower-case typographic family
    std::unordered_map<std::string, std::vector<int>> byLegacy_;   // lower-case four-style family and full name
    std::unordered_map<uint64_t, int> fallbacks_;                  // character and style to face, -1 for none
    std::vector<int> fallbackOrder_;
    bool fallbackOrderMade_ = false;

    void addFace(const std::shared_ptr<Face>& face, bool keep);
    Match choose(const std::vector<int>& candidates, int weight, bool italic, int stretch, bool legacy);
    Match matchOne(const std::string& lowerFamily, int weight, bool italic, int stretch);
};

std::string lowerCase(const std::string& s);

}  // namespace astra::text
