// M0 spike: see font.h.

#include "font.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace astra::spike {

namespace {

uint32_t tag(const char* s) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(s[0])) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(s[1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(s[2])) << 8) | static_cast<unsigned char>(s[3]);
}

// Accumulates exact area coverage: each edge adds, along its length, how much of every pixel
// lies to its right. A running sum over each row then gives the coverage of every pixel.
// (The method of font-rs; nothing here depends on the direction a contour winds in.)
class Accumulator {
public:
    Accumulator(int width, int height) : w_(width), h_(height), a_(static_cast<size_t>(width) * height + 4, 0.0f) {}

    void line(float x0, float y0, float x1, float y1) {
        if (y0 == y1) return;
        float dir = 1.0f;
        if (y0 > y1) {
            std::swap(x0, x1);
            std::swap(y0, y1);
            dir = -1.0f;
        }
        const float dxdy = (x1 - x0) / (y1 - y0);
        float x = x0;
        if (y0 < 0) x -= y0 * dxdy;

        const int firstRow = std::max(0, static_cast<int>(std::floor(y0)));
        const int lastRow = std::min(h_, static_cast<int>(std::ceil(y1)));
        for (int row = firstRow; row < lastRow; row++) {
            const size_t start = static_cast<size_t>(row) * w_;
            const float dy = std::min(static_cast<float>(row + 1), y1) - std::max(static_cast<float>(row), y0);
            const float xNext = x + dxdy * dy;
            const float d = dy * dir;
            const float left = std::min(x, xNext), right = std::max(x, xNext);
            const float leftFloor = std::floor(left);
            const int leftCell = static_cast<int>(leftFloor);
            const float rightCeil = std::ceil(right);
            const int rightCell = static_cast<int>(rightCeil);

            if (rightCell <= leftCell + 1) {
                const float mid = 0.5f * (x + xNext) - leftFloor;
                add(start, leftCell, d - d * mid);
                add(start, leftCell + 1, d * mid);
            } else {
                const float s = 1.0f / (right - left);
                const float leftPart = left - leftFloor;
                const float a0 = 0.5f * s * (1.0f - leftPart) * (1.0f - leftPart);
                const float rightPart = right - rightCeil + 1.0f;
                const float am = 0.5f * s * rightPart * rightPart;
                add(start, leftCell, d * a0);
                if (rightCell == leftCell + 2) {
                    add(start, leftCell + 1, d * (1.0f - a0 - am));
                } else {
                    const float a1 = s * (1.5f - leftPart);
                    add(start, leftCell + 1, d * (a1 - a0));
                    for (int cell = leftCell + 2; cell < rightCell - 1; cell++) add(start, cell, d * s);
                    const float a2 = a1 + static_cast<float>(rightCell - leftCell - 3) * s;
                    add(start, rightCell - 1, d * (1.0f - a2 - am));
                }
                add(start, rightCell, d * am);
            }
            x = xNext;
        }
    }

    void finish(std::vector<uint8_t>& out) const {
        out.resize(static_cast<size_t>(w_) * h_);
        float sum = 0.0f;
        for (size_t i = 0; i < out.size(); i++) {
            sum += a_[i];
            const float coverage = std::min(std::fabs(sum), 1.0f);
            out[i] = static_cast<uint8_t>(coverage * 255.0f + 0.5f);
        }
    }

private:
    int w_, h_;
    std::vector<float> a_;

    void add(size_t rowStart, int cell, float amount) {
        if (cell < 0) cell = 0;
        const size_t at = rowStart + static_cast<size_t>(cell);
        if (at < a_.size()) a_[at] += amount;
    }
};

}  // namespace

uint8_t Font::u8(size_t at) const { return at < data_.size() ? static_cast<uint8_t>(data_[at]) : 0; }
uint16_t Font::u16(size_t at) const { return static_cast<uint16_t>((u8(at) << 8) | u8(at + 1)); }
int16_t Font::i16(size_t at) const { return static_cast<int16_t>(u16(at)); }
uint32_t Font::u32(size_t at) const { return (static_cast<uint32_t>(u16(at)) << 16) | u16(at + 2); }

bool Font::fail(const std::string& why) {
    error_ = why;
    return false;
}

bool Font::load(const std::string& path, int faceIndex) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return fail("cannot open " + path);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    data_ = buffer.str();

    // A collection starts with 'ttcf' and a list of where each font's table directory is.
    size_t directory = 0;
    if (u32(0) == tag("ttcf")) {
        if (faceIndex < 0 || static_cast<uint32_t>(faceIndex) >= u32(8)) return fail("no such face in the collection");
        directory = u32(12 + 4 * static_cast<size_t>(faceIndex));
    }
    const uint32_t kind = u32(directory);
    if (kind != 0x00010000 && kind != tag("true")) return fail("not a TrueType-outline font (the .otf kind is not read yet)");

    size_t head = 0, hhea = 0, maxp = 0, cmap = 0, os2 = 0;
    const int tableCount = u16(directory + 4);
    for (int i = 0; i < tableCount; i++) {
        const size_t record = directory + 12 + 16 * static_cast<size_t>(i);
        const uint32_t name = u32(record);
        const size_t offset = u32(record + 8);
        if (name == tag("head")) head = offset;
        else if (name == tag("hhea")) hhea = offset;
        else if (name == tag("maxp")) maxp = offset;
        else if (name == tag("cmap")) cmap = offset;
        else if (name == tag("glyf")) glyf_ = offset;
        else if (name == tag("loca")) loca_ = offset;
        else if (name == tag("hmtx")) hmtx_ = offset;
        else if (name == tag("OS/2")) os2 = offset;
    }
    if (!head || !hhea || !maxp || !cmap || !glyf_ || !loca_ || !hmtx_) return fail("a required table is missing");

    unitsPerEm_ = u16(head + 18);
    longLoca_ = i16(head + 50) != 0;
    numGlyphs_ = u16(maxp + 4);
    numHMetrics_ = u16(hhea + 34);
    if (unitsPerEm_ == 0 || numGlyphs_ == 0 || numHMetrics_ == 0) return fail("bad header values");

    // Line metrics. Windows fonts state the ones Windows uses in OS/2 (usWinAscent/Descent).
    ascent_ = i16(hhea + 4);
    descent_ = -i16(hhea + 6);
    lineGap_ = i16(hhea + 8);
    if (os2) {
        ascent_ = u16(os2 + 74);
        descent_ = u16(os2 + 76);
        lineGap_ = 0;
        if (u16(os2) >= 2) {
            xHeight_ = i16(os2 + 86);
            capHeight_ = i16(os2 + 88);
        }
    }

    // The character map: prefer the full Unicode one (format 12), else the 16-bit one (format 4).
    const int subtables = u16(cmap + 2);
    for (int i = 0; i < subtables; i++) {
        const size_t record = cmap + 4 + 8 * static_cast<size_t>(i);
        const int platform = u16(record), encoding = u16(record + 2);
        const size_t table = cmap + u32(record + 4);
        const int format = u16(table);
        const bool unicode = platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));
        if (!unicode) continue;
        if (format == 12) cmap12_ = table;
        else if (format == 4) cmap4_ = table;
    }
    if (!cmap4_ && !cmap12_) return fail("no Unicode character map");

    // A font that does not state its x-height and capital height is measured instead.
    if (xHeight_ <= 0) xHeight_ = static_cast<int>(boundsTop(glyphIndex('x')));
    if (capHeight_ <= 0) capHeight_ = static_cast<int>(boundsTop(glyphIndex('H')));
    return true;
}

int Font::glyphIndex(uint32_t cp) const {
    if (cmap12_) {
        const uint32_t groups = u32(cmap12_ + 12);
        uint32_t lo = 0, hi = groups;
        while (lo < hi) {
            const uint32_t mid = (lo + hi) / 2;
            const size_t group = cmap12_ + 16 + 12 * static_cast<size_t>(mid);
            if (cp < u32(group)) hi = mid;
            else if (cp > u32(group + 4)) lo = mid + 1;
            else return static_cast<int>(u32(group + 8) + (cp - u32(group)));
        }
        return 0;
    }
    if (cp > 0xFFFF) return 0;
    const int segments = u16(cmap4_ + 6) / 2;
    const size_t ends = cmap4_ + 14;
    const size_t starts = ends + 2 * static_cast<size_t>(segments) + 2;
    const size_t deltas = starts + 2 * static_cast<size_t>(segments);
    const size_t offsets = deltas + 2 * static_cast<size_t>(segments);
    int lo = 0, hi = segments;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (u16(ends + 2 * static_cast<size_t>(mid)) < cp) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= segments) return 0;
    const size_t s = static_cast<size_t>(lo);
    const uint32_t start = u16(starts + 2 * s);
    if (cp < start) return 0;
    const uint16_t rangeOffset = u16(offsets + 2 * s);
    if (rangeOffset == 0) return static_cast<uint16_t>(cp + u16(deltas + 2 * s));
    const uint16_t glyph = u16(offsets + 2 * s + rangeOffset + 2 * (cp - start));
    return glyph == 0 ? 0 : static_cast<uint16_t>(glyph + u16(deltas + 2 * s));
}

int Font::advance(int glyph, float pixelSize) const {
    if (glyph < 0 || glyph >= numGlyphs_) glyph = 0;
    const size_t entry = static_cast<size_t>(std::min(glyph, numHMetrics_ - 1));
    const int units = u16(hmtx_ + 4 * entry);
    return static_cast<int>(std::lround(units * pixelSize / unitsPerEm_));
}

int Font::ascent(float pixelSize) const { return static_cast<int>(std::lround(ascent_ * pixelSize / unitsPerEm_)); }
int Font::descent(float pixelSize) const { return static_cast<int>(std::lround(descent_ * pixelSize / unitsPerEm_)); }
int Font::lineHeight(float pixelSize) const {
    return ascent(pixelSize) + descent(pixelSize) + static_cast<int>(std::lround(lineGap_ * pixelSize / unitsPerEm_));
}

bool Font::glyphRange(int glyph, size_t& start, size_t& end) const {
    if (glyph < 0 || glyph >= numGlyphs_) return false;
    const size_t g = static_cast<size_t>(glyph);
    if (longLoca_) {
        start = glyf_ + u32(loca_ + 4 * g);
        end = glyf_ + u32(loca_ + 4 * g + 4);
    } else {
        start = glyf_ + 2 * static_cast<size_t>(u16(loca_ + 2 * g));
        end = glyf_ + 2 * static_cast<size_t>(u16(loca_ + 2 * g + 2));
    }
    return end > start && end <= data_.size();  // equal means the glyph is empty (a space)
}

// The outline of a glyph in font units, y upwards. A glyph is either a set of contours or a
// list of other glyphs to place, each with its own offset and scale.
void Font::outline(int glyph, std::vector<Contour>& out, int depth) const {
    size_t at, end;
    if (depth > 8 || !glyphRange(glyph, at, end)) return;
    const int contours = i16(at);
    size_t p = at + 10;

    if (contours >= 0) {
        std::vector<int> ends(static_cast<size_t>(contours));
        for (int& e : ends) {
            e = u16(p);
            p += 2;
        }
        const int points = contours == 0 ? 0 : ends.back() + 1;
        p += 2 + u16(p);  // the hinting program is skipped: it is not run

        std::vector<uint8_t> flags(static_cast<size_t>(points));
        for (int i = 0; i < points;) {
            const uint8_t flag = u8(p++);
            flags[static_cast<size_t>(i++)] = flag;
            if (flag & 8) {
                for (int repeat = u8(p++); repeat > 0 && i < points; repeat--) flags[static_cast<size_t>(i++)] = flag;
            }
        }
        std::vector<int> xs(static_cast<size_t>(points)), ys(static_cast<size_t>(points));
        auto coordinates = [&](std::vector<int>& values, uint8_t shortBit, uint8_t sameBit) {
            int value = 0;
            for (int i = 0; i < points; i++) {
                const uint8_t flag = flags[static_cast<size_t>(i)];
                if (flag & shortBit) {
                    const int delta = u8(p++);
                    value += (flag & sameBit) ? delta : -delta;
                } else if (!(flag & sameBit)) {
                    value += i16(p);
                    p += 2;
                }
                values[static_cast<size_t>(i)] = value;
            }
        };
        coordinates(xs, 2, 16);
        coordinates(ys, 4, 32);

        int first = 0;
        for (int e : ends) {
            Contour contour;
            for (int i = first; i <= e && i < points; i++) {
                const size_t k = static_cast<size_t>(i);
                contour.push_back(Point{static_cast<float>(xs[k]), static_cast<float>(ys[k]), (flags[k] & 1) != 0});
            }
            if (contour.size() >= 2) out.push_back(std::move(contour));
            first = e + 1;
        }
        return;
    }

    for (;;) {
        const uint16_t flags = u16(p);
        const int component = u16(p + 2);
        p += 4;
        float dx, dy;
        if (flags & 1) {  // offsets are 16-bit
            dx = static_cast<float>(i16(p));
            dy = static_cast<float>(i16(p + 2));
            p += 4;
        } else {
            dx = static_cast<float>(static_cast<int8_t>(u8(p)));
            dy = static_cast<float>(static_cast<int8_t>(u8(p + 1)));
            p += 2;
        }
        float a = 1, b = 0, c = 0, d = 1;
        auto fixed = [&] {
            const float v = static_cast<float>(i16(p)) / 16384.0f;
            p += 2;
            return v;
        };
        if (flags & 8) {
            a = d = fixed();
        } else if (flags & 0x40) {
            a = fixed();
            d = fixed();
        } else if (flags & 0x80) {
            a = fixed();
            b = fixed();
            c = fixed();
            d = fixed();
        }

        std::vector<Contour> part;
        outline(component, part, depth + 1);
        for (Contour& contour : part) {
            for (Point& point : contour) {
                const float x = point.x, y = point.y;
                point.x = a * x + c * y + dx;
                point.y = b * x + d * y + dy;
            }
            out.push_back(std::move(contour));
        }
        if (!(flags & 0x20)) break;  // no more components
    }
}

float Font::boundsTop(int glyph) const {
    std::vector<Contour> contours;
    outline(glyph, contours, 0);
    float top = 0;
    for (const Contour& contour : contours)
        for (const Point& point : contour) top = std::max(top, point.y);
    return top;
}

GlyphBitmap Font::rasterize(int glyph, float pixelSize, bool hint) const {
    GlyphBitmap bitmap;
    std::vector<Contour> contours;
    outline(glyph, contours, 0);
    if (contours.empty()) return bitmap;

    const float scale = pixelSize / static_cast<float>(unitsPerEm_);

    // Hinting, by a rule of our own. The baseline sits on a pixel edge already. The x-height
    // and the capital height are moved to the nearest whole pixel and everything between is
    // stretched to fit, so the flat tops and bottoms of letters fall on pixel edges and stay
    // sharp. Nothing is moved sideways, and what hangs below the baseline is left alone.
    const float xScaled = xHeight_ * scale, capScaled = capHeight_ * scale;
    const float xSnapped = std::max(1.0f, std::round(xScaled));
    const float capSnapped = std::max(xSnapped + 1.0f, std::round(capScaled));
    auto fitY = [&](float y) {
        if (!hint || y <= 0 || xScaled <= 0 || capScaled <= xScaled) return y;
        if (y <= xScaled) return y * xSnapped / xScaled;
        if (y <= capScaled) return xSnapped + (y - xScaled) * (capSnapped - xSnapped) / (capScaled - xScaled);
        return capSnapped + (y - capScaled);
    };

    // To pixels, y still upwards, and the box that holds the glyph.
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    for (Contour& contour : contours) {
        for (Point& point : contour) {
            point.x *= scale;
            point.y = fitY(point.y * scale);
            minX = std::min(minX, point.x);
            maxX = std::max(maxX, point.x);
            minY = std::min(minY, point.y);
            maxY = std::max(maxY, point.y);
        }
    }
    bitmap.left = static_cast<int>(std::floor(minX));
    bitmap.top = static_cast<int>(std::ceil(maxY));
    bitmap.width = static_cast<int>(std::ceil(maxX)) - bitmap.left + 1;
    bitmap.height = bitmap.top - static_cast<int>(std::floor(minY)) + 1;
    if (bitmap.width <= 0 || bitmap.height <= 0 || bitmap.width > 4096 || bitmap.height > 4096) return GlyphBitmap{};

    Accumulator acc(bitmap.width, bitmap.height);
    const float originX = static_cast<float>(bitmap.left), originY = static_cast<float>(bitmap.top);
    auto toX = [&](float x) { return x - originX; };
    auto toY = [&](float y) { return originY - y; };

    auto curve = [&](float x0, float y0, float cx, float cy, float x1, float y1) {
        // A quadratic curve, cut into as many straight pieces as its bend needs.
        const float bend = std::hypot(x0 - 2 * cx + x1, y0 - 2 * cy + y1);
        const int pieces = std::clamp(static_cast<int>(std::ceil(std::sqrt(bend * 3.0f))), 1, 24);
        float px = x0, py = y0;
        for (int i = 1; i <= pieces; i++) {
            const float t = static_cast<float>(i) / static_cast<float>(pieces), u = 1.0f - t;
            const float x = u * u * x0 + 2 * u * t * cx + t * t * x1;
            const float y = u * u * y0 + 2 * u * t * cy + t * t * y1;
            acc.line(toX(px), toY(py), toX(x), toY(y));
            px = x;
            py = y;
        }
    };

    for (const Contour& contour : contours) {
        const size_t n = contour.size();
        // Start on a point of the outline; between two control points one is implied halfway.
        size_t first = 0;
        while (first < n && !contour[first].onCurve) first++;
        float startX, startY;
        if (first == n) {
            startX = (contour[0].x + contour[n - 1].x) / 2;
            startY = (contour[0].y + contour[n - 1].y) / 2;
            first = 0;
        } else {
            startX = contour[first].x;
            startY = contour[first].y;
            first++;
        }
        float px = startX, py = startY;
        bool haveControl = false;
        float cx = 0, cy = 0;
        for (size_t step = 0; step < n; step++) {
            const Point& point = contour[(first + step) % n];
            if (point.onCurve) {
                if (haveControl) curve(px, py, cx, cy, point.x, point.y);
                else acc.line(toX(px), toY(py), toX(point.x), toY(point.y));
                px = point.x;
                py = point.y;
                haveControl = false;
            } else {
                if (haveControl) {
                    const float mx = (cx + point.x) / 2, my = (cy + point.y) / 2;
                    curve(px, py, cx, cy, mx, my);
                    px = mx;
                    py = my;
                }
                cx = point.x;
                cy = point.y;
                haveControl = true;
            }
        }
        if (haveControl) curve(px, py, cx, cy, startX, startY);
        else acc.line(toX(px), toY(py), toX(startX), toY(startY));
    }

    acc.finish(bitmap.coverage);
    return bitmap;
}

}  // namespace astra::spike
