// The text engine: the cubic outlines of .otf fonts (the `CFF ` table).
//
// These fonts do not store a glyph's outline as points. They store a small program for each
// glyph, in a stack language with thirty-odd operators, which draws the outline when it is
// run: "move here, a line by this much, a curve by these". Programs call shared pieces
// (subroutines), and carry hints for a rasteriser that we do not use and step over.
//
// A variable font of this kind has the table `CFF2` instead: the same language, with one
// operator more, "blend", which gives a number together with how much it changes towards
// each end of the font's axes.
#pragma once

#include <string>
#include <vector>

#include "face.h"

namespace astra::text {

struct CffFont {
    // False, with `error` saying why, if the table cannot be used.
    bool load(const Reader& file, size_t offset, size_t length, std::string& error);

    // Adds the glyph's outline to `path`, in the font's units. False if there is none.
    // `coords` says where on a variable font's axes; it is not looked at for any other.
    bool outline(int glyph, const Coords& coords, Path& path) const;

    bool variable() const { return cff2_; }

private:
    // A list of byte strings, as the format stores its names, programs and subroutines.
    struct Index {
        size_t count = 0;
        size_t offsets = 0;   // where the list of positions starts
        int offSize = 1;
        size_t data = 0;      // what the positions are counted from
        size_t end = 0;       // the first byte after the whole index
    };

    Reader r_;
    size_t base_ = 0, limit_ = 0;
    Index charStrings_, globalSubrs_;
    // One per "font dict": a font for many scripts splits its glyphs among several, each
    // with subroutines of its own.
    std::vector<Index> localSubrs_;
    size_t fdSelect_ = 0;
    int fdSelectFormat_ = -1;
    // The variable kind of table (CFF2), and what it adds.
    bool cff2_ = false;
    size_t variationStore_ = 0;
    std::vector<int> vsindexes_;  // one per font dict: the set of regions its glyphs blend by

    bool readIndex(size_t at, Index& out) const;
    bool item(const Index& index, size_t i, size_t& start, size_t& end) const;
    int fontDictOf(int glyph) const;
    bool readPrivate(size_t dictStart, size_t dictEnd, Index& subrs, int& vsindex) const;

    struct Run;
    bool run(Run& state, size_t start, size_t end, int depth) const;
};

}  // namespace astra::text
