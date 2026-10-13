// Colour glyphs that are pictures: see bitmaps.h.

#include "bitmaps.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "../image/png.h"

namespace astra::text {

void scalePicture(const std::vector<uint8_t>& from, int fromWidth, int fromHeight, std::vector<uint8_t>& to, int toWidth, int toHeight) {
    to.assign(static_cast<size_t>(toWidth) * static_cast<size_t>(toHeight) * 4, 0);
    if (fromWidth <= 0 || fromHeight <= 0 || toWidth <= 0 || toHeight <= 0) return;
    // Each pixel of the result is the average of the part of the original it covers, which
    // is exact when making a picture smaller. Made larger, a pixel covers less than one of
    // the original's and the result is that one: enough here, where pictures are made smaller.
    const double stepX = static_cast<double>(fromWidth) / toWidth, stepY = static_cast<double>(fromHeight) / toHeight;
    for (int y = 0; y < toHeight; y++) {
        const double y0 = y * stepY, y1 = (y + 1) * stepY;
        for (int x = 0; x < toWidth; x++) {
            const double x0 = x * stepX, x1 = (x + 1) * stepX;
            double sum[4] = {0, 0, 0, 0}, area = 0;
            for (int sy = static_cast<int>(y0); sy < fromHeight && sy < y1; sy++) {
                const double h = std::min<double>(sy + 1, y1) - std::max<double>(sy, y0);
                if (h <= 0) continue;
                for (int sx = static_cast<int>(x0); sx < fromWidth && sx < x1; sx++) {
                    const double w = std::min<double>(sx + 1, x1) - std::max<double>(sx, x0);
                    if (w <= 0) continue;
                    const uint8_t* s = &from[(static_cast<size_t>(sy) * static_cast<size_t>(fromWidth) + static_cast<size_t>(sx)) * 4];
                    for (int k = 0; k < 4; k++) sum[k] += s[k] * w * h;
                    area += w * h;
                }
            }
            if (area <= 0) continue;
            uint8_t* d = &to[(static_cast<size_t>(y) * static_cast<size_t>(toWidth) + static_cast<size_t>(x)) * 4];
            for (int k = 0; k < 4; k++) d[k] = static_cast<uint8_t>(std::clamp(sum[k] / area + 0.5, 0.0, 255.0));
        }
    }
}

std::unique_ptr<BitmapGlyphs> BitmapGlyphs::read(const Face& face) {
    auto out = std::unique_ptr<BitmapGlyphs>(new BitmapGlyphs());
    const Reader& r = face.bytes();
    out->r_ = r;
    out->glyphCount_ = face.glyphCount();
    size_t length = 0;
    if (face.table(tag("CBLC"), out->location_, length) && face.table(tag("CBDT"), out->data_, length)) {
        const uint32_t sizes = std::min<uint32_t>(r.u32(out->location_ + 4), 64);
        for (uint32_t i = 0; i < sizes; i++) {
            const size_t record = out->location_ + 8 + 48 * static_cast<size_t>(i);
            const int ppem = r.u8(record + 45);
            if (ppem > 0 && r.u8(record + 46) == 32) out->strikes_.push_back(Strike{record, ppem});  // 32 bits a pixel: colour
        }
    } else if (face.table(tag("sbix"), out->location_, length)) {
        out->sbix_ = true;
        const uint32_t strikes = std::min<uint32_t>(r.u32(out->location_ + 4), 64);
        for (uint32_t i = 0; i < strikes; i++) {
            const size_t at = out->location_ + r.u32(out->location_ + 8 + 4 * static_cast<size_t>(i));
            const int ppem = r.u16(at);
            if (ppem > 0) out->strikes_.push_back(Strike{at, ppem});
        }
    }
    if (out->strikes_.empty()) return nullptr;
    std::sort(out->strikes_.begin(), out->strikes_.end(), [](const Strike& a, const Strike& b) { return a.ppem < b.ppem; });
    return out;
}

bool BitmapGlyphs::find(const Strike& strike, int glyph, Found& out, int depth) const {
    if (glyph < 0 || glyph >= glyphCount_) return false;
    if (sbix_) {
        const size_t offsets = strike.at + 4 + 4 * static_cast<size_t>(glyph);
        const size_t start = strike.at + r_.u32(offsets), end = strike.at + r_.u32(offsets + 4);
        if (end < start + 8 || !r_.has(start, end - start)) return false;
        const uint32_t kind = r_.u32(start + 4);
        if (kind == tag("dupe")) return depth < 4 && find(strike, r_.u16(start + 8), out, depth + 1);  // the same picture as another glyph
        if (kind != tag("png ")) return false;
        out.bearingX = r_.i16(start);
        out.bearingY = r_.i16(start + 2);
        out.fromBottom = true;
        out.data = start + 8;
        out.length = end - start - 8;
        return true;
    }

    // CBLC: the strike lists ranges of glyphs, each range with its own way of saying where
    // each glyph's picture is in CBDT.
    const size_t array = location_ + r_.u32(strike.at);
    const uint32_t ranges = r_.u32(strike.at + 8);
    for (uint32_t i = 0; i < ranges && i < 65536; i++) {
        const size_t range = array + 8 * static_cast<size_t>(i);
        const int first = r_.u16(range), last = r_.u16(range + 2);
        if (glyph < first || glyph > last) continue;
        const size_t sub = array + r_.u32(range + 4);
        const int indexFormat = r_.u16(sub), imageFormat = r_.u16(sub + 2);
        const size_t images = data_ + r_.u32(sub + 4);
        const size_t nth = static_cast<size_t>(glyph - first);
        size_t start = 0, end = 0, sharedMetrics = 0;
        switch (indexFormat) {
            case 1:
                start = images + r_.u32(sub + 8 + 4 * nth);
                end = images + r_.u32(sub + 12 + 4 * nth);
                break;
            case 2:
                start = images + static_cast<size_t>(r_.u32(sub + 8)) * nth;
                end = start + r_.u32(sub + 8);
                sharedMetrics = sub + 12;
                break;
            case 3:
                start = images + r_.u16(sub + 8 + 2 * nth);
                end = images + r_.u16(sub + 10 + 2 * nth);
                break;
            case 4: {
                const uint32_t count = r_.u32(sub + 8);
                for (uint32_t k = 0; k < count; k++) {
                    const size_t pair = sub + 12 + 4 * static_cast<size_t>(k);
                    if (r_.u16(pair) != glyph) continue;
                    start = images + r_.u16(pair + 2);
                    end = images + r_.u16(pair + 6);
                    break;
                }
                break;
            }
            case 5: {
                const uint32_t size = r_.u32(sub + 8), count = r_.u32(sub + 20);
                sharedMetrics = sub + 12;
                for (uint32_t k = 0; k < count; k++) {
                    if (r_.u16(sub + 24 + 2 * static_cast<size_t>(k)) != glyph) continue;
                    start = images + static_cast<size_t>(size) * k;
                    end = start + size;
                    break;
                }
                break;
            }
            default:
                return false;
        }
        if (end <= start || !r_.has(start, end - start)) return false;  // no picture for this glyph

        // In front of the picture: how it sits, in a short form or a long one, or nothing
        // if all the glyphs of the range sit alike.
        size_t metrics = sharedMetrics, at = start;
        if (imageFormat == 17) {
            metrics = at;
            at += 5;
        } else if (imageFormat == 18) {
            metrics = at;
            at += 8;
        } else if (imageFormat != 19 || metrics == 0) {
            return false;
        }
        out.bearingX = r_.i8(metrics + 2);
        out.bearingY = r_.i8(metrics + 3);
        out.fromBottom = false;
        out.length = r_.u32(at);
        out.data = at + 4;
        return out.length > 0 && out.data + out.length <= end && r_.has(out.data, out.length);
    }
    return false;
}

bool BitmapGlyphs::has(int glyph) const {
    Found found;
    for (const Strike& strike : strikes_)
        if (find(strike, glyph, found)) return true;
    return false;
}

GlyphImage BitmapGlyphs::render(int glyph, float ppem) const {
    // The smallest picture that is at least as large as wanted, or else the largest there is.
    Found found;
    const Strike* chosen = nullptr;
    for (const Strike& strike : strikes_) {
        if (!find(strike, glyph, found)) continue;
        chosen = &strike;
        if (static_cast<float>(strike.ppem) >= ppem) break;
    }
    if (chosen == nullptr || !find(*chosen, glyph, found)) return GlyphImage();

    std::vector<uint8_t> picture;
    int width = 0, height = 0;
    std::string error;
    if (!image::decodePng(r_.data() + found.data, found.length, picture, width, height, error)) return GlyphImage();
    for (size_t i = 0; i + 3 < picture.size(); i += 4) {
        const uint32_t a = picture[i + 3];
        for (int k = 0; k < 3; k++) picture[i + static_cast<size_t>(k)] = static_cast<uint8_t>((picture[i + static_cast<size_t>(k)] * a + 127) / 255);
    }

    const float k = ppem / static_cast<float>(chosen->ppem);
    GlyphImage image;
    image.color = true;
    image.width = std::max(1, static_cast<int>(std::lround(static_cast<float>(width) * k)));
    image.height = std::max(1, static_cast<int>(std::lround(static_cast<float>(height) * k)));
    if (image.width > 4096 || image.height > 4096) return GlyphImage();
    image.left = static_cast<int>(std::lround(static_cast<float>(found.bearingX) * k));
    image.top = found.fromBottom ? static_cast<int>(std::lround(static_cast<float>(found.bearingY) * k)) + image.height
                                 : static_cast<int>(std::lround(static_cast<float>(found.bearingY) * k));
    if (image.width == width && image.height == height) image.pixels = std::move(picture);
    else scalePicture(picture, width, height, image.pixels, image.width, image.height);
    return image;
}

}  // namespace astra::text
