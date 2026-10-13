// The text engine: colour glyphs that are pictures.
//
// Some emoji fonts do not describe their glyphs as shapes at all. They carry a small PNG
// picture of each, at one size or a few: Android's and Linux's emoji font in the tables
// `CBLC` and `CBDT`, Apple's in `sbix`. A glyph is drawn by taking the picture nearest in
// size and scaling it to the size wanted.
#pragma once

#include <memory>
#include <vector>

#include "face.h"
#include "raster.h"

namespace astra::text {

struct BitmapGlyphs {
    // Null if the font has no glyphs of this kind.
    static std::unique_ptr<BitmapGlyphs> read(const Face& face);

    bool has(int glyph) const;

    // The glyph's picture for a size (`ppem`: pixels to the em). Empty if it has none.
    GlyphImage render(int glyph, float ppem) const;

private:
    struct Strike {
        size_t at = 0;  // CBLC: its BitmapSize record. sbix: the strike itself.
        int ppem = 0;
    };
    // Where a glyph's picture is in a strike, and how it sits: in the strike's own pixels.
    struct Found {
        size_t data = 0, length = 0;
        int bearingX = 0, bearingY = 0;  // of the picture's left edge and its top edge, from the origin
        bool fromBottom = false;         // bearingY is of the bottom edge instead (sbix)
    };

    Reader r_;
    bool sbix_ = false;
    size_t location_ = 0, data_ = 0;  // CBLC and CBDT, or sbix in the first
    int glyphCount_ = 0;
    std::vector<Strike> strikes_;

    bool find(const Strike& strike, int glyph, Found& out, int depth = 0) const;
};

// Scales a picture of four bytes a pixel (colours multiplied by alpha) to another size.
void scalePicture(const std::vector<uint8_t>& from, int fromWidth, int fromHeight, std::vector<uint8_t>& to, int toWidth, int toHeight);

}  // namespace astra::text
