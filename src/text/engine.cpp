// A font at a size, measuring and drawing: see engine.h.

#include "engine.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <map>
#include <tuple>

#include "bitmaps.h"
#include "color.h"

#include "../section_window.h"  // last: this file's own code goes in its section (D161)

namespace astra::text {

namespace {

int roundHalfUp(float v) { return static_cast<int>(std::floor(v + 0.5f)); }

struct FontKey {
    const Face* face;
    int size64, scale64, weight;
    bool fakeBold, fakeItalic, hinting;
    bool operator<(const FontKey& o) const {
        return std::tie(face, size64, scale64, weight, fakeBold, fakeItalic, hinting) <
               std::tie(o.face, o.size64, o.scale64, o.weight, o.fakeBold, o.fakeItalic, o.hinting);
    }
};

std::mutex& cacheMutex() {
    static std::mutex m;
    return m;
}

std::map<FontKey, std::shared_ptr<Font>>& cache() {
    static auto* fonts = new std::map<FontKey, std::shared_ptr<Font>>();  // never torn down: threads may still draw at exit
    return *fonts;
}

}  // namespace

bool invisible(uint32_t cp) {
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return true;
    if (cp == 0xAD || cp == 0x34F || cp == 0x61C || cp == 0x180E || cp == 0xFEFF) return true;
    if (cp >= 0x200B && cp <= 0x200F) return true;   // zero-width space and joiners, direction marks
    if (cp >= 0x202A && cp <= 0x202E) return true;   // direction embeddings
    if (cp >= 0x2060 && cp <= 0x206F) return true;   // word joiner, invisible operators, isolates
    if (cp >= 0xFE00 && cp <= 0xFE0F) return true;   // variation selectors
    if (cp >= 0xE0000 && cp <= 0xE0FFF) return true; // tags and more variation selectors
    return false;
}

uint32_t nextCodepoint(std::string_view text, size_t& at) {
    const auto byte = [&](size_t i) { return static_cast<uint8_t>(text[i]); };
    const uint8_t first = byte(at++);
    if (first < 0x80) return first;
    int more = 0;
    uint32_t cp = 0, least = 0;
    if ((first & 0xE0) == 0xC0) {
        more = 1;
        cp = first & 0x1F;
        least = 0x80;
    } else if ((first & 0xF0) == 0xE0) {
        more = 2;
        cp = first & 0x0F;
        least = 0x800;
    } else if ((first & 0xF8) == 0xF0) {
        more = 3;
        cp = first & 0x07;
        least = 0x10000;
    } else {
        return 0xFFFD;
    }
    for (int i = 0; i < more; i++) {
        if (at >= text.size() || (byte(at) & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (byte(at++) & 0x3F);
    }
    if (cp < least || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) return 0xFFFD;
    return cp;
}

std::shared_ptr<Font> Font::get(const std::string& family, const FontRequest& request) {
    const Match match = FontBook::installed().match(family, request.weight, request.italic);
    if (!match) return nullptr;
    return of(match, request);
}

std::shared_ptr<Font> Font::of(const Match& match, const FontRequest& asked) {
    if (!match) return nullptr;
    FontRequest request = asked;
    request.size = std::clamp(request.size, 1.0f, 2000.0f);
    request.scale = std::clamp(request.scale, 0.25f, 16.0f);
    const int weight = match.face->variable() ? match.weight : match.face->weight();
    const FontKey key{match.face.get(), roundHalfUp(request.size * 64), roundHalfUp(request.scale * 64), weight, match.fakeBold, match.fakeItalic, request.hinting};

    std::lock_guard<std::mutex> hold(cacheMutex());
    auto found = cache().find(key);
    if (found != cache().end()) return found->second;

    std::shared_ptr<Font> font(new Font());
    Font& f = *font;
    f.face_ = match.face;
    f.request_ = request;
    f.request_.weight = weight;
    f.fakeBold_ = match.fakeBold;
    f.fakeItalic_ = match.fakeItalic;
    f.ppem_ = request.size * request.scale;
    f.unit_ = f.ppem_ / static_cast<float>(f.face_->unitsPerEm());
    // A variable font is set to the weight asked for, and to the cut meant for this size if
    // it has several (an "optical size" axis, which is in points: three quarters of a pixel).
    if (f.face_->variable()) f.coords_ = f.face_->instance(static_cast<float>(match.weight), 100, request.size * 0.75f);
    f.boldExtra_ = f.fakeBold_ ? std::max(1, roundHalfUp(f.ppem_ / 24.0f)) : 0;

    f.ascent_ = roundHalfUp(f.face_->ascent(f.coords_) * f.unit_);
    f.descent_ = roundHalfUp(f.face_->descent(f.coords_) * f.unit_);
    f.lineGap_ = std::max(0, roundHalfUp(f.face_->lineGap(f.coords_) * f.unit_));

    // The hinting rule. The baseline is on a pixel edge already. The x-height and the
    // capital height are moved to the nearest whole pixel and what lies between is stretched
    // to fit, so the flat tops and bottoms of letters fall on pixel edges and stay sharp.
    // Nothing is moved sideways, and what hangs below the baseline is left as it is.
    f.xScaled_ = f.face_->xHeight(f.coords_) * f.unit_;
    f.capScaled_ = f.face_->capHeight(f.coords_) * f.unit_;
    f.xSnapped_ = std::max(1.0f, std::round(f.xScaled_));
    f.capSnapped_ = std::max(f.xSnapped_ + 1.0f, std::round(f.capScaled_));
    const bool fits = request.hinting && f.xScaled_ > 0 && f.capScaled_ > f.xScaled_;
    if (!fits) {
        f.xSnapped_ = f.xScaled_;
        f.capSnapped_ = f.capScaled_;
    }
    f.xHeight_ = roundHalfUp(f.xSnapped_);
    f.capHeight_ = roundHalfUp(f.capSnapped_);
    f.advances_.assign(static_cast<size_t>(f.face_->glyphCount()), INT_MIN);

    cache()[key] = font;
    return font;
}

int Font::advance(int glyph) {
    if (glyph < 0 || glyph >= face_->glyphCount()) return 0;
    std::lock_guard<std::mutex> hold(mutex_);
    int& known = advances_[static_cast<size_t>(glyph)];
    if (known == INT_MIN) {
        const float units = face_->advance(glyph, coords_);
        known = std::max(0, roundHalfUp(units * unit_));
        if (known > 0) known += boldExtra_;
    }
    return known;
}

Font* Font::fontFor(uint32_t codepoint, int& glyph) {
    glyph = face_->glyphIndex(codepoint);
    if (glyph != 0) return this;
    {
        std::lock_guard<std::mutex> hold(mutex_);
        auto known = others_.find(codepoint);
        if (known != others_.end()) {
            glyph = known->second.glyph;
            return known->second.font != nullptr ? known->second.font.get() : this;
        }
    }
    // Looked for with this font's lock released: the font found may be asked for its own.
    Other other;
    const Match match = FontBook::installed().fallback(codepoint, request_.weight >= 600 || fakeBold_ ? 700 : request_.weight, face_->italic() || fakeItalic_);
    if (match && match.face.get() != face_.get()) {
        other.font = of(match, request_);
        other.glyph = other.font != nullptr ? other.font->face().glyphIndex(codepoint) : 0;
        if (other.glyph == 0) other.font = nullptr;
    }
    std::lock_guard<std::mutex> hold(mutex_);
    others_[codepoint] = other;
    glyph = other.glyph;
    return other.font != nullptr ? other.font.get() : this;
}

int Font::charWidth(uint32_t codepoint) {
    if (invisible(codepoint)) return 0;
    int glyph = 0;
    Font* font = fontFor(codepoint, glyph);
    return font->advance(glyph);
}

int Font::width(std::string_view utf8) {
    int total = 0;
    for (size_t at = 0; at < utf8.size();) total += charWidth(nextCodepoint(utf8, at));
    return total;
}

int Font::place(std::string_view utf8, std::vector<PlacedGlyph>& out) {
    int x = 0;
    for (size_t at = 0; at < utf8.size();) {
        PlacedGlyph placed;
        placed.offset = static_cast<uint32_t>(at);
        placed.codepoint = nextCodepoint(utf8, at);
        if (invisible(placed.codepoint)) continue;
        placed.font = fontFor(placed.codepoint, placed.glyph);
        placed.x = x;
        placed.advance = placed.font->advance(placed.glyph);
        x += placed.advance;
        out.push_back(placed);
    }
    return x;
}

const GlyphImage& Font::image(int glyph, uint32_t foreground) {
    const ColorTables* color = face_->color();
    const bool colored = color != nullptr && color->has(glyph);
    const uint64_t key = static_cast<uint64_t>(static_cast<uint32_t>(glyph)) | (colored ? static_cast<uint64_t>(foreground) << 32 : 0);
    {
        std::lock_guard<std::mutex> hold(mutex_);
        auto known = images_.find(key);
        if (known != images_.end()) return *known->second;
    }
    auto made = std::make_unique<GlyphImage>(render(glyph, foreground));
    std::lock_guard<std::mutex> hold(mutex_);
    auto& slot = images_[key];
    if (slot == nullptr) slot = std::move(made);  // another thread may have made the same meanwhile
    return *slot;
}

GlyphImage Font::render(int glyph, uint32_t foreground) const {
    if (const ColorTables* color = face_->color(); color != nullptr && color->has(glyph)) {
        GlyphImage image = color->render(*face_, glyph, coords_, unit_, foreground);
        if (!image.empty()) return image;
    }
    if (const BitmapGlyphs* pictures = face_->bitmaps(); pictures != nullptr) {
        GlyphImage image = pictures->render(glyph, ppem_);
        if (!image.empty()) return image;
    }
    Path path;
    if (!face_->outline(glyph, coords_, path)) return GlyphImage();

    const float xScaled = xScaled_, capScaled = capScaled_, xSnapped = xSnapped_, capSnapped = capSnapped_;
    const bool fit = xSnapped != xScaled || capSnapped != capScaled;
    const float slant = fakeItalic_ ? 0.2f : 0.0f;
    for (size_t i = 0; i + 1 < path.points.size(); i += 2) {
        float y = path.points[i + 1] * unit_;
        if (fit && y > 0) {
            if (y <= xScaled) y = y * xSnapped / xScaled;
            else if (y <= capScaled) y = xSnapped + (y - xScaled) * (capSnapped - xSnapped) / (capScaled - xScaled);
            else y = capSnapped + (y - capScaled);
        }
        path.points[i] = path.points[i] * unit_ + y * slant;
        path.points[i + 1] = y;
    }
    GlyphImage image = rasterize(path);
    if (boldExtra_ > 0 && !image.empty()) {
        // A bold the family does not have: every stroke made thicker to the right.
        GlyphImage wide = image;
        wide.width = image.width + boldExtra_;
        wide.pixels.assign(static_cast<size_t>(wide.width) * static_cast<size_t>(wide.height), 0);
        for (int row = 0; row < image.height; row++) {
            const uint8_t* from = &image.pixels[static_cast<size_t>(row) * static_cast<size_t>(image.width)];
            uint8_t* to = &wide.pixels[static_cast<size_t>(row) * static_cast<size_t>(wide.width)];
            for (int col = 0; col < image.width; col++)
                for (int k = 0; k <= boldExtra_; k++) to[col + k] = std::max(to[col + k], from[col]);
        }
        return wide;
    }
    return image;
}

void drawGlyph(uint8_t* rgba, int width, int height, const GlyphImage& image, int x, int baseline, uint32_t color) {
    if (image.empty()) return;
    const int left = x + image.left, top = baseline - image.top;
    const int fromCol = std::max(0, -left), toCol = std::min(image.width, width - left);
    const int fromRow = std::max(0, -top), toRow = std::min(image.height, height - top);
    if (fromCol >= toCol || fromRow >= toRow) return;
    const uint32_t red = (color >> 24) & 0xFF, green = (color >> 16) & 0xFF, blue = (color >> 8) & 0xFF, alpha = color & 0xFF;

    for (int row = fromRow; row < toRow; row++) {
        uint8_t* d = rgba + (static_cast<size_t>(top + row) * static_cast<size_t>(width) + static_cast<size_t>(left + fromCol)) * 4;
        for (int col = fromCol; col < toCol; col++, d += 4) {
            // The glyph's colour here, multiplied by its alpha, and that alpha: of 255.
            uint32_t sr, sg, sb, sa;
            if (image.color) {
                const uint8_t* s = &image.pixels[(static_cast<size_t>(row) * static_cast<size_t>(image.width) + static_cast<size_t>(col)) * 4];
                if (s[3] == 0) continue;
                sr = s[0] * alpha / 255;
                sg = s[1] * alpha / 255;
                sb = s[2] * alpha / 255;
                sa = s[3] * alpha / 255;
            } else {
                const uint32_t coverage = image.pixels[static_cast<size_t>(row) * static_cast<size_t>(image.width) + static_cast<size_t>(col)];
                if (coverage == 0) continue;
                sa = (coverage * alpha + 127) / 255;
                if (sa != 0 && (sa == 255 || d[3] == 0)) {
                    // Nothing shows through, or there is nothing underneath: the colour itself.
                    d[0] = static_cast<uint8_t>(red);
                    d[1] = static_cast<uint8_t>(green);
                    d[2] = static_cast<uint8_t>(blue);
                    d[3] = static_cast<uint8_t>(sa);
                    continue;
                }
                sr = (red * sa + 127) / 255;
                sg = (green * sa + 127) / 255;
                sb = (blue * sa + 127) / 255;
            }
            if (sa == 0) continue;
            const uint32_t da = d[3];
            if (sa == 255 || da == 0) {
                // Nothing shows through, or there is nothing underneath.
                d[0] = static_cast<uint8_t>(sa == 255 ? sr : sr * 255 / sa);
                d[1] = static_cast<uint8_t>(sa == 255 ? sg : sg * 255 / sa);
                d[2] = static_cast<uint8_t>(sa == 255 ? sb : sb * 255 / sa);
                d[3] = static_cast<uint8_t>(sa);
                continue;
            }
            // Source over, on colours not multiplied by their alpha.
            const uint32_t under = da * (255 - sa);                 // of 255 * 255
            const uint32_t outA = sa * 255 + under;                 // of 255 * 255
            d[0] = static_cast<uint8_t>(std::min<uint32_t>(255, (sr * 255 * 255 + d[0] * under + outA / 2) / outA));
            d[1] = static_cast<uint8_t>(std::min<uint32_t>(255, (sg * 255 * 255 + d[1] * under + outA / 2) / outA));
            d[2] = static_cast<uint8_t>(std::min<uint32_t>(255, (sb * 255 * 255 + d[2] * under + outA / 2) / outA));
            d[3] = static_cast<uint8_t>((outA + 127) / 255);
        }
    }
}

int drawText(uint8_t* rgba, int width, int height, Font& font, std::string_view utf8, int x, int baseline, uint32_t color) {
    std::vector<PlacedGlyph> glyphs;
    const int total = font.place(utf8, glyphs);
    for (const PlacedGlyph& g : glyphs) {
        if (x + g.x > width + 4096 || x + g.x + g.advance < -4096) continue;  // far outside
        drawGlyph(rgba, width, height, g.font->image(g.glyph, color), x + g.x, baseline, color);
    }
    return total;
}

}  // namespace astra::text
