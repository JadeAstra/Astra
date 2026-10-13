// The text engine: variable fonts.
//
// A variable font is one file that holds a whole family: every weight from thin to black,
// and often every width and optical size too. It stores one set of outlines and, for each
// of its axes, how far every point moves between one end of the axis and the other. An
// instance, "weight 600" say, is made by moving the points part of the way.
//
// This reads the tables that say so: the axes and how values on them are scaled (`fvar`,
// `avar`), the movement of each glyph's points (`gvar`), and the change in glyph widths
// (`HVAR`) and in the font's measurements (`MVAR`).
#pragma once

#include <memory>
#include <vector>

#include "face.h"

namespace astra::text {

struct Variations {
    // Null if the font is not variable. Fills in the face's list of axes.
    static std::unique_ptr<Variations> read(const Face& face, std::vector<Axis>& axes);

    Coords normalize(const std::vector<float>& values) const;

    bool hasGlyphData() const { return gvar_ != 0; }
    bool hasAdvanceTable() const { return hvar_ != 0; }
    // Moves a glyph's points for an instance. `xs` and `ys` hold the points and, after
    // them, four more that stand for the glyph's edges (the second minus the first is its
    // width). `ends` is the last point of each contour, or empty for a glyph made of other
    // glyphs, whose "points" are the places of its parts.
    void moveGlyphPoints(int glyph, const Coords& coords, std::vector<float>& xs, std::vector<float>& ys, const std::vector<int>& ends) const;

    // How much wider a glyph is in an instance, and how much a measurement of the font
    // (named by a four-letter tag) changes. In the font's units.
    float advanceDelta(int glyph, const Coords& coords) const;
    float metricDelta(uint32_t which, const Coords& coords) const;

private:
    Reader file_;
    const Face* face_ = nullptr;
    std::vector<Axis> axes_;
    size_t avar_ = 0, gvar_ = 0, gvarLength_ = 0, hvar_ = 0, mvar_ = 0;

    // A set of deltas kept apart from the data it applies to, as HVAR and MVAR have them.
    float storeDelta(size_t store, uint32_t outer, uint32_t inner, const Coords& coords) const;
};

// For one set of a store of deltas (an "ItemVariationStore" at `store`): how many regions of
// the axes the set has, and how much each of them counts at a place on the axes. These are
// what a cubic glyph program's "blend" multiplies its deltas by.
size_t regionCount(const Reader& r, size_t store, uint32_t set);
std::vector<float> regionScalars(const Reader& r, size_t store, uint32_t set, const Coords& coords);

}  // namespace astra::text
