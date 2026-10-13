// The text engine, part one: reading a font file.
//
// A Face is one font out of a TrueType or OpenType file (.ttf, .otf, or one member of a .ttc
// collection): which glyph a character maps to, how wide each glyph is, the outline of each
// glyph, and the font's names and measurements. Everything is read from the file's bytes by
// code of our own; nothing is asked of the system. A Face is not changed after it is loaded,
// so any number of threads may use one at once.
//
// Both kinds of outline are read: the quadratic curves of TrueType (the `glyf` table) and
// the cubic curves of the .otf kind (`CFF `), which are a small program per glyph (cff.cpp).
// A variable font holds every weight and width in one file; `coords` says which instance is
// wanted, and outlines, widths and measurements are adjusted for it (variation.cpp).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace astra::text {

// An outline: straight lines and curves, in the font's units with y growing upwards.
struct Path {
    enum Verb : uint8_t { Move, Line, Quad, Cubic, Close };
    std::vector<uint8_t> verbs;
    std::vector<float> points;  // x, y pairs: 1 for Move and Line, 2 for Quad, 3 for Cubic

    void moveTo(float x, float y) {
        verbs.push_back(Move);
        points.insert(points.end(), {x, y});
    }
    void lineTo(float x, float y) {
        verbs.push_back(Line);
        points.insert(points.end(), {x, y});
    }
    void quadTo(float cx, float cy, float x, float y) {
        verbs.push_back(Quad);
        points.insert(points.end(), {cx, cy, x, y});
    }
    void cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y) {
        verbs.push_back(Cubic);
        points.insert(points.end(), {c1x, c1y, c2x, c2y, x, y});
    }
    void close() { verbs.push_back(Close); }
    bool empty() const { return verbs.empty(); }
    void clear() {
        verbs.clear();
        points.clear();
    }
    // x' = a*x + c*y + e,  y' = b*x + d*y + f
    void transform(float a, float b, float c, float d, float e, float f);
    // False if the path has no points.
    bool bounds(float& minX, float& minY, float& maxX, float& maxY) const;
};

// The bytes of a font file, read big-endian. A read outside the file gives 0, so a damaged
// font yields wrong shapes at worst and never a read out of bounds.
class Reader {
public:
    Reader() = default;
    Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    size_t size() const { return size_; }
    const uint8_t* data() const { return data_; }
    bool has(size_t at, size_t count) const { return at <= size_ && count <= size_ - at; }

    uint8_t u8(size_t at) const { return at < size_ ? data_[at] : 0; }
    int8_t i8(size_t at) const { return static_cast<int8_t>(u8(at)); }
    uint16_t u16(size_t at) const { return static_cast<uint16_t>((u8(at) << 8) | u8(at + 1)); }
    int16_t i16(size_t at) const { return static_cast<int16_t>(u16(at)); }
    uint32_t u24(size_t at) const { return (static_cast<uint32_t>(u8(at)) << 16) | u16(at + 1); }
    uint32_t u32(size_t at) const { return (static_cast<uint32_t>(u16(at)) << 16) | u16(at + 2); }
    int32_t i32(size_t at) const { return static_cast<int32_t>(u32(at)); }
    float f2dot14(size_t at) const { return static_cast<float>(i16(at)) / 16384.0f; }
    float fixed(size_t at) const { return static_cast<float>(i32(at)) / 65536.0f; }

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
};

constexpr uint32_t tag(const char (&s)[5]) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(s[0])) << 24) | (static_cast<uint32_t>(static_cast<unsigned char>(s[1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(s[2])) << 8) | static_cast<unsigned char>(s[3]);
}

// One axis a variable font can be varied along: weight, width, optical size and so on.
struct Axis {
    uint32_t tag = 0;
    float minimum = 0, defaultValue = 0, maximum = 0;
    std::string name;
};

// A place on a variable font's axes, one number per axis from -1 to 1 (0 is the default).
// Empty for a font that is not variable, or for its default instance.
using Coords = std::vector<float>;

// The bytes of a font file, held however they were got: read into memory, or the file
// itself mapped into memory, so that only the parts looked at are read from the disk.
struct FontData {
    const uint8_t* data = nullptr;
    size_t size = 0;
    FontData() = default;
    FontData(const FontData&) = delete;
    FontData& operator=(const FontData&) = delete;
    virtual ~FontData() = default;
};

// A file's bytes; null if it cannot be read (system.cpp).
std::shared_ptr<const FontData> openFontFile(const std::string& path);

struct CffFont;        // cff.cpp
struct Variations;     // variation.cpp
struct ColorTables;    // color.cpp
struct BitmapGlyphs;   // bitmaps.cpp

class Face {
public:
    // Loads face `index` of a file. Null, with `error` saying why, if it cannot be used.
    static std::shared_ptr<Face> load(const std::string& path, int index, std::string& error);
    // The same from bytes already in memory, which the face keeps.
    static std::shared_ptr<Face> fromData(std::shared_ptr<const FontData> data, int index, const std::string& path, std::string& error);
    static std::shared_ptr<Face> fromBytes(std::vector<uint8_t> bytes, int index, const std::string& path, std::string& error);
    // How many faces a file holds: 1, or more for a collection, or 0 if it is not a font.
    static int count(const std::string& path);

    ~Face();

    // -- What it is called and what style it is --
    const std::string& path() const { return path_; }
    int index() const { return index_; }
    const std::string& family() const { return family_; }        // "Segoe UI"
    // The family as programs that know only four styles see it: "Segoe UI Semibold".
    const std::string& legacyFamily() const { return legacyFamily_; }
    const std::string& style() const { return style_; }          // "Bold Italic"
    const std::string& fullName() const { return fullName_; }
    int weight() const { return weight_; }                       // 100 to 900; 400 is regular, 700 bold
    int stretch() const { return stretch_; }                     // 1 to 9; 5 is normal width
    bool italic() const { return italic_; }
    bool monospace() const { return monospace_; }
    bool symbol() const { return symbol_; }                      // a symbol font: its characters are private ones
    bool cubic() const { return cff_ != nullptr; }

    // -- Characters and glyphs --
    int glyphCount() const { return glyphCount_; }
    // The glyph for a character; 0, the "missing" glyph, if the font has none.
    int glyphIndex(uint32_t codepoint) const;
    bool has(uint32_t codepoint) const { return glyphIndex(codepoint) != 0; }

    // -- Measurements, in the font's units --
    int unitsPerEm() const { return unitsPerEm_; }
    float ascent(const Coords& coords = {}) const;      // above the baseline
    float descent(const Coords& coords = {}) const;     // below it, as a positive number
    float lineGap(const Coords& coords = {}) const;
    float xHeight(const Coords& coords = {}) const;
    float capHeight(const Coords& coords = {}) const;
    float advance(int glyph, const Coords& coords = {}) const;

    // -- Outlines --
    // Adds the glyph's outline to `path`. False for a glyph with none (a space, or a glyph
    // that is a picture). The outline is for the instance `coords` names.
    bool outline(int glyph, const Coords& coords, Path& path) const;

    // -- Variable fonts --
    const std::vector<Axis>& axes() const { return axes_; }
    bool variable() const { return !axes_.empty(); }
    // From values on the axes as a designer states them (weight 600, width 87.5) to Coords.
    // `values` is one number per axis, in the order of axes().
    Coords normalize(const std::vector<float>& values) const;
    // Coords for a weight (100 to 900) and a width in percent, on the axes the font has.
    Coords instance(float weight, float widthPercent = 100, float opticalSize = 0) const;

    // -- For the other parts of the engine --
    const Reader& bytes() const { return file_; }
    const std::shared_ptr<const FontData>& data() const { return owned_; }
    // Where a table is in the file; false if the font has none of that name.
    bool table(uint32_t name, size_t& offset, size_t& length) const;
    const ColorTables* color() const { return color_.get(); }
    const BitmapGlyphs* bitmaps() const { return bitmaps_.get(); }
    // Whether it has glyphs with colours of their own (emoji), of either kind.
    bool hasColor() const { return color_ != nullptr || bitmaps_ != nullptr; }

private:
    Face() = default;
    bool read(std::string& error);
    void readNames(size_t nameTable);
    void readCharacterMap(size_t cmap);
    bool glyfRange(int glyph, size_t& start, size_t& end) const;
    // `widthChange`, if asked for, is how much wider the glyph is in the instance.
    bool glyfOutline(int glyph, const Coords& coords, Path& path, int depth, float* widthChange = nullptr) const;
    float metric(uint32_t which, float base, const Coords& coords) const;

    std::shared_ptr<const FontData> owned_;
    Reader file_;
    std::string path_;
    int index_ = 0;
    size_t directory_ = 0;

    std::string family_, style_, fullName_, legacyFamily_;
    int weight_ = 400, stretch_ = 5;
    bool italic_ = false, monospace_ = false, symbol_ = false;

    int unitsPerEm_ = 0, glyphCount_ = 0, hMetricCount_ = 0;
    bool longLoca_ = false;
    int ascent_ = 0, descent_ = 0, lineGap_ = 0, xHeight_ = 0, capHeight_ = 0;
    size_t glyf_ = 0, glyfLength_ = 0, loca_ = 0, hmtx_ = 0, hmtxLength_ = 0;
    size_t cmap4_ = 0, cmap12_ = 0, cmap13_ = 0, cmap6_ = 0, cmap0_ = 0;

    std::vector<Axis> axes_;
    std::unique_ptr<CffFont> cff_;
    std::unique_ptr<Variations> variations_;
    std::unique_ptr<ColorTables> color_;
    std::unique_ptr<BitmapGlyphs> bitmaps_;

    friend struct Variations;
};

}  // namespace astra::text
