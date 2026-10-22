// The text engine: turning an outline into a picture.
#pragma once

#include <cstdint>
#include <vector>

#include "face.h"

namespace astra::text {

// A small picture of one glyph and where it sits. With the pen on the baseline at (x, y),
// y growing downwards, its top-left pixel goes at (x + left, y - top).
//
// An ordinary glyph is coverage: one byte for each pixel, saying how much of the pixel the
// glyph covers, to be painted in whatever colour the text has. A colour glyph (an emoji) has
// colours of its own: four bytes for each pixel, red, green, blue and alpha, with the colours
// already multiplied by the alpha.
struct GlyphImage {
    int width = 0, height = 0;
    int left = 0, top = 0;
    bool color = false;
    std::vector<uint8_t> pixels;

    bool empty() const { return width == 0 || height == 0; }
};

// How finely curves are cut into straight pieces: a glyph's, small, to within about a tenth of
// a pixel; a shape's, which may be large, about ten times as closely, so that an oval's pixels
// add up to its area.
constexpr float kGlyphFineness = 1.0f;
constexpr float kShapeFineness = 9.0f;

// Fills an outline that is already in pixels (y still growing upwards), with exact-area
// antialiasing: each pixel's value is the share of it that lies inside the outline.
GlyphImage rasterize(const Path& path, float fineness = kGlyphFineness);

// The same into a box given beforehand, which a colour glyph's layers share: `left` and
// `top` say where the box sits, as in GlyphImage. Adds nothing if the path is empty.
void rasterizeInto(const Path& path, int left, int top, int width, int height, std::vector<uint8_t>& coverage,
                   float fineness = kGlyphFineness);

}  // namespace astra::text
