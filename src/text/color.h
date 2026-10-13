// The text engine: colour glyphs (emoji).
//
// An emoji font does not draw its glyphs in the colour of the text. In the format Windows'
// emoji font uses, a colour glyph is a recipe: other glyphs of the font stacked in layers,
// each filled with a colour from the font's palette (`COLR` version 0), or, in the newer
// version, a tree of "paints": fill this outline with that gradient, move it, turn it, lay
// it over that one (`COLR` version 1). This reads both and paints them into a picture.
#pragma once

#include <memory>

#include "face.h"
#include "raster.h"

namespace astra::text {

struct ColorTables {
    // Null if the font has no colour glyphs of a kind that is read.
    static std::unique_ptr<ColorTables> read(const Face& face);

    bool has(int glyph) const;

    // Paints a colour glyph. `scale` is pixels for each unit of the font, `foreground` the
    // colour of the text (0xRRGGBBAA), which a font may use for part of a glyph. An empty
    // picture if the glyph has no colour form.
    GlyphImage render(const Face& face, int glyph, const Coords& coords, float scale, uint32_t foreground) const;

    int version() const { return version_; }

private:
    Reader r_;
    size_t colr_ = 0, cpal_ = 0;
    int version_ = 0;
    // Version 0: layers of plain colour.
    size_t baseGlyphs_ = 0, layers_ = 0;
    int baseGlyphCount_ = 0, layerCount_ = 0;
    // Version 1: a tree of paints for each glyph.
    size_t paintGlyphs_ = 0, paintLayers_ = 0, clips_ = 0;
    // The palette in use.
    size_t colors_ = 0;
    int colorCount_ = 0;

    bool layersOf(int glyph, int& first, int& count) const;
    size_t paintOf(int glyph) const;
    bool clipBox(int glyph, float& minX, float& minY, float& maxX, float& maxY) const;

    struct Painter;
    friend struct Painter;
};

}  // namespace astra::text
