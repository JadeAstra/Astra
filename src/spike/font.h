// M0 spike: a text engine of our own. Reads a TrueType font file, measures by one rule and
// rasterises glyphs, with nothing asked of the system but the file. Portable C++.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace astra::spike {

// One rasterised glyph: an 8-bit coverage picture and where it sits. With the pen on the
// baseline at (x, y), y growing downwards, its top-left pixel goes at (x + left, y - top).
struct GlyphBitmap {
    int width = 0, height = 0;
    int left = 0, top = 0;
    std::vector<uint8_t> coverage;  // width * height, row by row
};

class Font {
public:
    // `faceIndex` picks a font out of a collection (.ttc). False if the file cannot be used.
    bool load(const std::string& path, int faceIndex = 0);
    const std::string& error() const { return error_; }

    int glyphIndex(uint32_t codepoint) const;  // 0, the "missing" glyph, if the font lacks it

    // Measuring, by one rule: a width in the font's units, scaled to the pixel size and
    // rounded to a whole pixel. The same on every system, since it uses only the file.
    int advance(int glyph, float pixelSize) const;
    int ascent(float pixelSize) const;
    int descent(float pixelSize) const;
    int lineHeight(float pixelSize) const;

    // `hint` snaps the baseline, the x-height and the capital height to whole pixels.
    GlyphBitmap rasterize(int glyph, float pixelSize, bool hint) const;

private:
    struct Point {
        float x, y;
        bool onCurve;
    };
    using Contour = std::vector<Point>;

    std::string data_;
    std::string error_;
    size_t glyf_ = 0, loca_ = 0, hmtx_ = 0, cmap4_ = 0, cmap12_ = 0;
    int unitsPerEm_ = 0, numGlyphs_ = 0, numHMetrics_ = 0;
    bool longLoca_ = false;
    int ascent_ = 0, descent_ = 0, lineGap_ = 0;  // font units; descent is positive
    int xHeight_ = 0, capHeight_ = 0;

    uint8_t u8(size_t at) const;
    uint16_t u16(size_t at) const;
    int16_t i16(size_t at) const;
    uint32_t u32(size_t at) const;
    bool fail(const std::string& why);

    bool glyphRange(int glyph, size_t& start, size_t& end) const;
    void outline(int glyph, std::vector<Contour>& out, int depth) const;
    float boundsTop(int glyph) const;
};

}  // namespace astra::spike
