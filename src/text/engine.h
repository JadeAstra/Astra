// The text engine: a font at a size, measuring, and drawing.
//
// Everything here is in device pixels, the pixels of the picture or screen being drawn on.
// A font is made for a size in logical pixels and a scale (1 on an ordinary screen, 1.5 or 2
// on a dense one); the two multiplied are how many device pixels its em is.
//
// Measuring and drawing go by one rule:
//
//   - A glyph's advance is its width in the font, scaled to the size in device pixels and
//     rounded to a whole device pixel.
//   - A line's width is the sum of its glyphs' advances, and each glyph is drawn where the
//     advances before it add up to. So measuring and drawing use the same whole numbers,
//     and a caret placed by measuring is where the text was drawn, exactly.
//   - Sizes in logical pixels are these divided by the scale. Rounding happens once, in
//     device pixels, never in logical ones: on a screen at 2x a glyph may be 13 device
//     pixels wide, which is 6.5 logical ones, and a line of them does not grow loose.
//
// Nothing is nudged between particular pairs of letters (no kerning) and letters are not
// joined into ligatures; scripts that need glyphs chosen and moved (Arabic, the Indic
// scripts) are part two of the engine.
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "face.h"
#include "fonts.h"
#include "raster.h"

namespace astra::text {

class Font;

// One glyph of a line of text, placed.
struct PlacedGlyph {
    Font* font = nullptr;    // the font asked for, or another one that has the character
    int glyph = 0;
    int x = 0;               // where its origin is, from the start of the line
    int advance = 0;
    uint32_t codepoint = 0;
    uint32_t offset = 0;     // where the character starts in the text, in bytes
};

struct FontRequest {
    float size = 12;        // logical pixels to the em
    float scale = 1;        // device pixels to a logical one
    int weight = 400;
    bool italic = false;
    bool hinting = true;
};

class Font {
public:
    // The font for a family and a style: FontBook::match on the installed fonts, so there
    // is always one unless the machine has no fonts at all (then null).
    static std::shared_ptr<Font> get(const std::string& family, const FontRequest& request);
    // The same for a font already chosen. Asking twice gives the same Font.
    static std::shared_ptr<Font> of(const Match& match, const FontRequest& request);

    const Face& face() const { return *face_; }
    const std::shared_ptr<Face>& sharedFace() const { return face_; }
    const FontRequest& request() const { return request_; }
    const Coords& coords() const { return coords_; }
    float pixelsPerEm() const { return ppem_; }
    bool fakeBold() const { return fakeBold_; }
    bool fakeItalic() const { return fakeItalic_; }

    // -- The line, in device pixels --
    int ascent() const { return ascent_; }      // from the baseline up
    int descent() const { return descent_; }    // from the baseline down
    int lineGap() const { return lineGap_; }    // extra between lines
    int height() const { return ascent_ + descent_ + lineGap_; }
    int xHeight() const { return xHeight_; }
    int capHeight() const { return capHeight_; }

    // -- Measuring --
    int advance(int glyph);
    // A character's width, in whichever font has it.
    int charWidth(uint32_t codepoint);
    int width(std::string_view utf8);
    // The glyphs of a line, placed. Returns the line's width.
    int place(std::string_view utf8, std::vector<PlacedGlyph>& out);
    // The font that has a character and its glyph there: this one, or another found for it.
    // The missing-glyph box of this font if no font has the character.
    Font* fontFor(uint32_t codepoint, int& glyph);

    // -- Drawing --
    // A glyph's picture. `foreground` (0xRRGGBBAA) matters only to a colour glyph that uses
    // the colour of the text for part of itself. The picture is kept; the reference is good
    // for as long as the font is.
    const GlyphImage& image(int glyph, uint32_t foreground = 0x000000FF);

private:
    Font() = default;

    std::shared_ptr<Face> face_;
    FontRequest request_;
    Coords coords_;
    float ppem_ = 0, unit_ = 0;  // unit_: device pixels for one unit of the font
    bool fakeBold_ = false, fakeItalic_ = false;
    int boldExtra_ = 0;
    int ascent_ = 0, descent_ = 0, lineGap_ = 0, xHeight_ = 0, capHeight_ = 0;
    // The hinting rule: where the x-height and the capital height are, and where they go.
    float xScaled_ = 0, capScaled_ = 0, xSnapped_ = 0, capSnapped_ = 0;

    std::mutex mutex_;
    std::vector<int> advances_;
    std::unordered_map<uint64_t, std::unique_ptr<GlyphImage>> images_;
    struct Other {
        std::shared_ptr<Font> font;  // null: this font
        int glyph = 0;
    };
    std::unordered_map<uint32_t, Other> others_;

    GlyphImage render(int glyph, uint32_t foreground) const;
};

// Characters that take no room and are not drawn: control characters, and the marks that
// only steer how other characters join or which way text runs.
bool invisible(uint32_t codepoint);

// The next character of UTF-8 text from `at`, which is moved past it. A byte that is not
// UTF-8 is read as U+FFFD.
uint32_t nextCodepoint(std::string_view text, size_t& at);

// Draws a line of text into a picture of `width` by `height` pixels, four bytes each (red,
// green, blue, alpha; the colours not multiplied by the alpha), with its start at `x` and
// its baseline at `baseline`. `color` is 0xRRGGBBAA. Returns the line's width.
int drawText(uint8_t* rgba, int width, int height, Font& font, std::string_view utf8, int x, int baseline, uint32_t color);

// One glyph's picture laid onto such a picture, its origin at (x, baseline).
void drawGlyph(uint8_t* rgba, int width, int height, const GlyphImage& image, int x, int baseline, uint32_t color);

}  // namespace astra::text
