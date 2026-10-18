// Colour glyphs: see color.h.

#include "color.h"

#include <algorithm>
#include <cmath>

#include "../section_window.h"  // last: this file's own code goes in its section (D161)

namespace astra::text {

namespace {

const float kPi = 3.14159265358979323846f;

// x' = a*x + c*y + e,  y' = b*x + d*y + f
struct Affine {
    float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;

    // This transform applied after `first`.
    Affine after(const Affine& first) const {
        return Affine{a * first.a + c * first.b, b * first.a + d * first.b, a * first.c + c * first.d, b * first.c + d * first.d,
                      a * first.e + c * first.f + e, b * first.e + d * first.f + f};
    }
    bool invert(Affine& out) const {
        const float det = a * d - b * c;
        if (std::fabs(det) < 1e-12f) return false;
        out.a = d / det;
        out.b = -b / det;
        out.c = -c / det;
        out.d = a / det;
        out.e = -(out.a * e + out.c * f);
        out.f = -(out.b * e + out.d * f);
        return true;
    }
    void apply(float x, float y, float& ox, float& oy) const {
        ox = a * x + c * y + e;
        oy = b * x + d * y + f;
    }
    static Affine translate(float dx, float dy) { return Affine{1, 0, 0, 1, dx, dy}; }
    Affine around(float cx, float cy) const { return translate(cx, cy).after(*this).after(translate(-cx, -cy)); }
};

// A picture being painted: red, green, blue and alpha for each pixel, as fractions, the
// colours already multiplied by the alpha.
struct Layer {
    int width = 0, height = 0;
    std::vector<float> rgba;
    Layer(int w, int h) : width(w), height(h), rgba(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0.0f) {}
};

struct Stop {
    float offset;
    float color[4];  // premultiplied
};

float blendChannel(int mode, float s, float d) {
    // The "separable" ways of mixing, on colours not multiplied by alpha.
    switch (mode) {
        case 13: return s + d - s * d;                                                    // screen
        case 14: return d <= 0.5f ? 2 * s * d : 1 - 2 * (1 - s) * (1 - d);                // overlay
        case 15: return std::min(s, d);                                                   // darken
        case 16: return std::max(s, d);                                                   // lighten
        case 17: return d == 0 ? 0 : s >= 1 ? 1 : std::min(1.0f, d / (1 - s));            // colour dodge
        case 18: return d >= 1 ? 1 : s <= 0 ? 0 : 1 - std::min(1.0f, (1 - d) / s);        // colour burn
        case 19: return s <= 0.5f ? 2 * s * d : 1 - 2 * (1 - s) * (1 - d);                // hard light
        case 20: {                                                                        // soft light
            if (s <= 0.5f) return d - (1 - 2 * s) * d * (1 - d);
            const float g = d <= 0.25f ? ((16 * d - 12) * d + 4) * d : std::sqrt(d);
            return d + (2 * s - 1) * (g - d);
        }
        case 21: return std::fabs(s - d);                                                 // difference
        case 22: return s + d - 2 * s * d;                                                // exclusion
        case 23: return s * d;                                                            // multiply
        default: return s;
    }
}

// Lays `source` onto `backdrop`, pixel for pixel, in one of the format's ways of combining.
void composite(Layer& backdrop, const Layer& source, int mode) {
    const size_t pixels = static_cast<size_t>(backdrop.width) * static_cast<size_t>(backdrop.height);
    for (size_t i = 0; i < pixels; i++) {
        float* d = &backdrop.rgba[i * 4];
        const float* s = &source.rgba[i * 4];
        const float sa = s[3], da = d[3];
        float fs = 1, fd = 1 - sa;  // source over, the usual one
        switch (mode) {
            case 0: fs = 0; fd = 0; break;            // clear
            case 1: fs = 1; fd = 0; break;            // source
            case 2: fs = 0; fd = 1; break;            // destination
            case 3: break;                            // source over
            case 4: fs = 1 - da; fd = 1; break;       // destination over
            case 5: fs = da; fd = 0; break;           // source in
            case 6: fs = 0; fd = sa; break;           // destination in
            case 7: fs = 1 - da; fd = 0; break;       // source out
            case 8: fs = 0; fd = 1 - sa; break;       // destination out
            case 9: fs = da; fd = 1 - sa; break;      // source atop
            case 10: fs = 1 - da; fd = sa; break;     // destination atop
            case 11: fs = 1 - da; fd = 1 - sa; break; // xor
            case 12: fs = 1; fd = 1; break;           // plus
            default: {
                if (mode >= 13 && mode <= 23) {
                    // Mixed where the two overlap, and each as it is where only one is.
                    for (int k = 0; k < 3; k++) {
                        const float sc = sa > 0 ? s[k] / sa : 0, dc = da > 0 ? d[k] / da : 0;
                        d[k] = (1 - da) * s[k] + (1 - sa) * d[k] + sa * da * blendChannel(mode, sc, dc);
                    }
                    d[3] = sa + da - sa * da;
                    continue;
                }
                break;  // the modes that work on hue and saturation are painted as source over
            }
        }
        for (int k = 0; k < 4; k++) d[k] = std::min(1.0f, s[k] * fs + d[k] * fd);
    }
}

}  // namespace

std::unique_ptr<ColorTables> ColorTables::read(const Face& face) {
    size_t colr = 0, cpal = 0, length = 0;
    if (!face.table(tag("COLR"), colr, length) || !face.table(tag("CPAL"), cpal, length)) return nullptr;
    auto out = std::unique_ptr<ColorTables>(new ColorTables());
    const Reader& r = face.bytes();
    out->r_ = r;
    out->colr_ = colr;
    out->cpal_ = cpal;
    out->version_ = r.u16(colr);
    out->baseGlyphCount_ = r.u16(colr + 2);
    out->baseGlyphs_ = colr + r.u32(colr + 4);
    out->layers_ = colr + r.u32(colr + 8);
    out->layerCount_ = r.u16(colr + 12);
    if (out->version_ >= 1) {
        if (const uint32_t at = r.u32(colr + 14)) out->paintGlyphs_ = colr + at;
        if (const uint32_t at = r.u32(colr + 18)) out->paintLayers_ = colr + at;
        if (const uint32_t at = r.u32(colr + 22)) out->clips_ = colr + at;
    }
    // The first palette: the one for a light background, which is every font's default.
    out->colorCount_ = r.u16(cpal + 2);
    out->colors_ = cpal + r.u32(cpal + 8) + 4 * static_cast<size_t>(r.u16(cpal + 12));
    return out;
}

bool ColorTables::layersOf(int glyph, int& first, int& count) const {
    int lo = 0, hi = baseGlyphCount_;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        const size_t record = baseGlyphs_ + 6 * static_cast<size_t>(mid);
        const int id = r_.u16(record);
        if (id < glyph) lo = mid + 1;
        else if (id > glyph) hi = mid;
        else {
            first = r_.u16(record + 2);
            count = r_.u16(record + 4);
            return count > 0 && first + count <= layerCount_;
        }
    }
    return false;
}

size_t ColorTables::paintOf(int glyph) const {
    if (paintGlyphs_ == 0) return 0;
    uint32_t lo = 0, hi = r_.u32(paintGlyphs_);
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        const size_t record = paintGlyphs_ + 4 + 6 * static_cast<size_t>(mid);
        const int id = r_.u16(record);
        if (id < glyph) lo = mid + 1;
        else if (id > glyph) hi = mid;
        else return paintGlyphs_ + r_.u32(record + 2);
    }
    return 0;
}

bool ColorTables::clipBox(int glyph, float& minX, float& minY, float& maxX, float& maxY) const {
    if (clips_ == 0) return false;
    const uint32_t count = r_.u32(clips_ + 1);
    for (uint32_t i = 0; i < count; i++) {
        const size_t record = clips_ + 5 + 7 * static_cast<size_t>(i);
        if (glyph < r_.u16(record) || glyph > r_.u16(record + 2)) continue;
        const size_t box = clips_ + r_.u24(record + 4);
        minX = r_.i16(box + 1);
        minY = r_.i16(box + 3);
        maxX = r_.i16(box + 5);
        maxY = r_.i16(box + 7);
        return maxX > minX && maxY > minY;
    }
    return false;
}

bool ColorTables::has(int glyph) const {
    int first, count;
    return paintOf(glyph) != 0 || layersOf(glyph, first, count);
}

// Paints one glyph's tree of paints into a box of pixels.
struct ColorTables::Painter {
    const ColorTables& t;
    const Face& face;
    const Coords& coords;
    uint32_t foreground;
    int left, top, width, height;  // the box, placed as a GlyphImage is
    int budget = 4000;             // how many paints may be visited: a damaged font can loop

    const Reader& r() const { return t.r_; }

    void colorOf(int paletteIndex, float alpha, float out[4]) const {
        float red, green, blue, a;
        if (paletteIndex == 0xFFFF) {
            red = static_cast<float>((foreground >> 24) & 0xFF) / 255.0f;
            green = static_cast<float>((foreground >> 16) & 0xFF) / 255.0f;
            blue = static_cast<float>((foreground >> 8) & 0xFF) / 255.0f;
            a = static_cast<float>(foreground & 0xFF) / 255.0f;
        } else if (paletteIndex < t.colorCount_) {
            const size_t at = t.colors_ + 4 * static_cast<size_t>(paletteIndex);
            blue = static_cast<float>(r().u8(at)) / 255.0f;
            green = static_cast<float>(r().u8(at + 1)) / 255.0f;
            red = static_cast<float>(r().u8(at + 2)) / 255.0f;
            a = static_cast<float>(r().u8(at + 3)) / 255.0f;
        } else {
            red = green = blue = 0;
            a = 1;
        }
        a *= std::clamp(alpha, 0.0f, 1.0f);
        out[0] = red * a;
        out[1] = green * a;
        out[2] = blue * a;
        out[3] = a;
    }

    // How much of each pixel of the box an outline covers, once `transform` has put it in pixels.
    bool coverageOf(int glyph, const Affine& transform, std::vector<uint8_t>& coverage) const {
        Path path;
        if (!face.outline(glyph, coords, path)) return false;
        path.transform(transform.a, transform.b, transform.c, transform.d, transform.e, transform.f);
        rasterizeInto(path, left, top, width, height, coverage);
        return true;
    }

    void readStops(size_t line, bool variable, int& extend, std::vector<Stop>& stops) const {
        extend = r().u8(line);
        const int count = r().u16(line + 1);
        const size_t size = variable ? 10 : 6;
        for (int i = 0; i < count; i++) {
            const size_t at = line + 3 + size * static_cast<size_t>(i);
            Stop stop;
            stop.offset = r().f2dot14(at);
            colorOf(r().u16(at + 2), r().f2dot14(at + 4), stop.color);
            stops.push_back(stop);
        }
        std::stable_sort(stops.begin(), stops.end(), [](const Stop& a, const Stop& b) { return a.offset < b.offset; });
    }

    static void colorAt(const std::vector<Stop>& stops, int extend, float tValue, float out[4]) {
        if (stops.empty()) {
            out[0] = out[1] = out[2] = out[3] = 0;
            return;
        }
        const float first = stops.front().offset, last = stops.back().offset;
        float v = tValue;
        if (last > first) {
            const float span = last - first;
            if (extend == 1) {  // repeat
                v = first + std::fmod(std::fmod(v - first, span) + span, span);
            } else if (extend == 2) {  // reflect
                float m = std::fmod(std::fmod(v - first, 2 * span) + 2 * span, 2 * span);
                if (m > span) m = 2 * span - m;
                v = first + m;
            }
        }
        if (v <= first) {
            std::copy(stops.front().color, stops.front().color + 4, out);
            return;
        }
        if (v >= last) {
            std::copy(stops.back().color, stops.back().color + 4, out);
            return;
        }
        for (size_t i = 0; i + 1 < stops.size(); i++) {
            const Stop& a = stops[i];
            const Stop& b = stops[i + 1];
            if (v > b.offset) continue;
            const float f = b.offset > a.offset ? (v - a.offset) / (b.offset - a.offset) : 1;
            for (int k = 0; k < 4; k++) out[k] = a.color[k] + (b.color[k] - a.color[k]) * f;
            return;
        }
        std::copy(stops.back().color, stops.back().color + 4, out);
    }

    // Fills the layer with what a paint is at each pixel, where `mask` (if given) lets it through.
    // Only for the paints that are a colour at every point: plain colours and gradients.
    bool fill(size_t paint, const Affine& transform, const std::vector<uint8_t>* mask, Layer& target) {
        const int format = r().u8(paint);
        const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
        auto put = [&](size_t i, const float color[4]) {
            const float m = mask != nullptr ? static_cast<float>((*mask)[i]) / 255.0f : 1.0f;
            if (m <= 0) return;
            float* d = &target.rgba[i * 4];
            const float sa = color[3] * m;
            for (int k = 0; k < 4; k++) d[k] = color[k] * m + d[k] * (1 - sa);
        };

        if (format == 2 || format == 3) {  // one colour
            float color[4];
            colorOf(r().u16(paint + 1), r().f2dot14(paint + 3), color);
            for (size_t i = 0; i < pixels; i++) put(i, color);
            return true;
        }
        if (format < 4 || format > 9) return false;

        Affine inverse;
        if (!transform.invert(inverse)) return true;  // squashed flat: nothing to see
        int extend = 0;
        std::vector<Stop> stops;
        readStops(paint + r().u24(paint + 1), (format & 1) != 0, extend, stops);

        // What a point of the font's space is on the gradient's scale.
        float x0 = r().i16(paint + 4), y0 = r().i16(paint + 6);
        float lineX = 0, lineY = 0, lineLength2 = 0;  // linear
        float x1 = 0, y1 = 0, r0 = 0, r1 = 0;         // radial
        float startAngle = 0, endAngle = 0;           // sweep
        if (format <= 5) {
            // The colours run from the first point towards the second, across lines that
            // lie the way the third point says.
            const float px1 = r().i16(paint + 8), py1 = r().i16(paint + 10), px2 = r().i16(paint + 12), py2 = r().i16(paint + 14);
            float perpX = -(py2 - y0), perpY = px2 - x0;
            const float perpLength2 = perpX * perpX + perpY * perpY;
            if (perpLength2 > 0) {
                const float k = ((px1 - x0) * perpX + (py1 - y0) * perpY) / perpLength2;
                lineX = perpX * k;
                lineY = perpY * k;
            } else {
                lineX = px1 - x0;
                lineY = py1 - y0;
            }
            lineLength2 = lineX * lineX + lineY * lineY;
            if (lineLength2 <= 0) return true;
        } else if (format <= 7) {
            r0 = r().u16(paint + 8);
            x1 = r().i16(paint + 10);
            y1 = r().i16(paint + 12);
            r1 = r().u16(paint + 14);
        } else {
            startAngle = r().f2dot14(paint + 8);
            endAngle = r().f2dot14(paint + 10);
        }

        for (int row = 0; row < height; row++) {
            for (int col = 0; col < width; col++) {
                const size_t i = static_cast<size_t>(row) * static_cast<size_t>(width) + static_cast<size_t>(col);
                if (mask != nullptr && (*mask)[i] == 0) continue;
                float fx, fy;
                inverse.apply(static_cast<float>(left + col) + 0.5f, static_cast<float>(top - row) - 0.5f, fx, fy);
                float tValue;
                if (format <= 5) {
                    tValue = ((fx - x0) * lineX + (fy - y0) * lineY) / lineLength2;
                } else if (format <= 7) {
                    // The point lies on the circle that is t of the way from the first to the second.
                    const float cdx = x1 - x0, cdy = y1 - y0, pdx = fx - x0, pdy = fy - y0, dr = r1 - r0;
                    const float qa = cdx * cdx + cdy * cdy - dr * dr;
                    const float qb = pdx * cdx + pdy * cdy + r0 * dr;
                    const float qc = pdx * pdx + pdy * pdy - r0 * r0;
                    if (std::fabs(qa) < 1e-6f) {
                        if (std::fabs(qb) < 1e-6f) continue;
                        tValue = qc / (2 * qb);
                    } else {
                        const float disc = qb * qb - qa * qc;
                        if (disc < 0) continue;
                        const float root = std::sqrt(disc);
                        tValue = (qb + root) / qa;
                        if (r0 + tValue * dr < 0) tValue = (qb - root) / qa;
                    }
                    if (r0 + tValue * dr < 0) continue;
                } else {
                    float angle = std::atan2(fy - y0, fx - x0) / kPi;  // in half turns, anticlockwise from the right
                    if (angle < 0) angle += 2;
                    if (endAngle == startAngle) continue;
                    tValue = (angle - startAngle) / (endAngle - startAngle);
                }
                float color[4];
                colorAt(stops, extend, tValue, color);
                put(i, color);
            }
        }
        return true;
    }

    // Paints the tree under `paint` onto the layer.
    void paint(size_t at, const Affine& transform, Layer& target) {
        if (at == 0 || --budget < 0) return;
        const int format = r().u8(at);
        auto child = [&](size_t field) { return at + r().u24(at + field); };

        switch (format) {
            case 1: {  // layers, bottom first
                const int count = r().u8(at + 1);
                const uint32_t first = r().u32(at + 2);
                if (t.paintLayers_ == 0) return;
                const uint32_t total = r().u32(t.paintLayers_);
                for (int i = 0; i < count && first + static_cast<uint32_t>(i) < total; i++)
                    paint(t.paintLayers_ + r().u32(t.paintLayers_ + 4 + 4 * (static_cast<size_t>(first) + static_cast<size_t>(i))), transform, target);
                return;
            }
            case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 9:
                fill(at, transform, nullptr, target);
                return;
            case 10: {  // an outline, filled with the paint below it
                std::vector<uint8_t> mask;
                if (!coverageOf(r().u16(at + 4), transform, mask)) return;
                const size_t inner = child(1);
                if (fill(inner, transform, &mask, target)) return;
                // Something more than a colour inside the outline: painted apart, then cut to it.
                Layer scratch(width, height);
                paint(inner, transform, scratch);
                for (size_t i = 0; i < mask.size(); i++) {
                    const float m = static_cast<float>(mask[i]) / 255.0f;
                    for (int k = 0; k < 4; k++) scratch.rgba[i * 4 + static_cast<size_t>(k)] *= m;
                }
                composite(target, scratch, 3);
                return;
            }
            case 11:  // another colour glyph, as it is
                paint(t.paintOf(r().u16(at + 1)), transform, target);
                return;
            case 12: case 13: {  // any transform
                const size_t m = at + r().u24(at + 4);
                const Affine local{r().fixed(m), r().fixed(m + 4), r().fixed(m + 8), r().fixed(m + 12), r().fixed(m + 16), r().fixed(m + 20)};
                paint(child(1), transform.after(local), target);
                return;
            }
            case 14: case 15:  // moved
                paint(child(1), transform.after(Affine::translate(r().i16(at + 4), r().i16(at + 6))), target);
                return;
            case 16: case 17: case 18: case 19: case 20: case 21: case 22: case 23: {  // scaled
                const bool uniform = format >= 20, centred = format == 18 || format == 19 || format >= 22;
                const float sx = r().f2dot14(at + 4), sy = uniform ? sx : r().f2dot14(at + 6);
                Affine local{sx, 0, 0, sy, 0, 0};
                if (centred) {
                    const size_t c = at + (uniform ? 6 : 8);
                    local = local.around(r().i16(c), r().i16(c + 2));
                }
                paint(child(1), transform.after(local), target);
                return;
            }
            case 24: case 25: case 26: case 27: {  // turned
                const float angle = r().f2dot14(at + 4) * kPi;
                Affine local{std::cos(angle), std::sin(angle), -std::sin(angle), std::cos(angle), 0, 0};
                if (format >= 26) local = local.around(r().i16(at + 6), r().i16(at + 8));
                paint(child(1), transform.after(local), target);
                return;
            }
            case 28: case 29: case 30: case 31: {  // slanted
                const float xAngle = r().f2dot14(at + 4) * kPi, yAngle = r().f2dot14(at + 6) * kPi;
                Affine local{1, std::tan(yAngle), -std::tan(xAngle), 1, 0, 0};
                if (format >= 30) local = local.around(r().i16(at + 8), r().i16(at + 10));
                paint(child(1), transform.after(local), target);
                return;
            }
            case 32: {  // one paint laid on another, in a way the font chooses
                Layer backdrop(width, height), source(width, height);
                paint(at + r().u24(at + 5), transform, backdrop);
                paint(child(1), transform, source);
                composite(backdrop, source, r().u8(at + 4));
                composite(target, backdrop, 3);
                return;
            }
            default:
                return;
        }
    }

    // The box a tree of paints needs, in the font's units, for a font that does not say.
    void boundsOf(size_t at, const Affine& transform, bool& any, float& minX, float& minY, float& maxX, float& maxY) {
        if (at == 0 || --budget < 0) return;
        const int format = r().u8(at);
        auto child = [&](size_t field) { return at + r().u24(at + field); };
        switch (format) {
            case 1: {
                const int count = r().u8(at + 1);
                const uint32_t first = r().u32(at + 2);
                if (t.paintLayers_ == 0) return;
                const uint32_t total = r().u32(t.paintLayers_);
                for (int i = 0; i < count && first + static_cast<uint32_t>(i) < total; i++)
                    boundsOf(t.paintLayers_ + r().u32(t.paintLayers_ + 4 + 4 * (static_cast<size_t>(first) + static_cast<size_t>(i))), transform, any, minX, minY, maxX, maxY);
                return;
            }
            case 10: {
                Path path;
                float x0, y0, x1, y1;
                if (!face.outline(r().u16(at + 4), coords, path)) return;
                path.transform(transform.a, transform.b, transform.c, transform.d, transform.e, transform.f);
                if (!path.bounds(x0, y0, x1, y1)) return;
                minX = any ? std::min(minX, x0) : x0;
                minY = any ? std::min(minY, y0) : y0;
                maxX = any ? std::max(maxX, x1) : x1;
                maxY = any ? std::max(maxY, y1) : y1;
                any = true;
                return;
            }
            case 11: boundsOf(t.paintOf(r().u16(at + 1)), transform, any, minX, minY, maxX, maxY); return;
            case 12: case 13: {
                const size_t m = at + r().u24(at + 4);
                const Affine local{r().fixed(m), r().fixed(m + 4), r().fixed(m + 8), r().fixed(m + 12), r().fixed(m + 16), r().fixed(m + 20)};
                boundsOf(child(1), transform.after(local), any, minX, minY, maxX, maxY);
                return;
            }
            case 14: case 15:
                boundsOf(child(1), transform.after(Affine::translate(r().i16(at + 4), r().i16(at + 6))), any, minX, minY, maxX, maxY);
                return;
            case 32:
                boundsOf(at + r().u24(at + 5), transform, any, minX, minY, maxX, maxY);
                boundsOf(child(1), transform, any, minX, minY, maxX, maxY);
                return;
            default:
                // Scaled, turned or slanted: the box of what is underneath, generously.
                if (format >= 16 && format <= 31) boundsOf(child(1), transform, any, minX, minY, maxX, maxY);
                return;
        }
    }
};

GlyphImage ColorTables::render(const Face& face, int glyph, const Coords& coords, float scale, uint32_t foreground) const {
    GlyphImage image;
    const Affine toPixels{scale, 0, 0, scale, 0, 0};
    const size_t root = paintOf(glyph);
    int firstLayer = 0, layerCount = 0;
    const bool flat = root == 0 && layersOf(glyph, firstLayer, layerCount);
    if (root == 0 && !flat) return image;

    // The box, in pixels: what the font states for the glyph, or what its outlines take up.
    float minX = 0, minY = 0, maxX = 0, maxY = 0;
    bool any = false;
    Painter painter{*this, face, coords, foreground, 0, 0, 0, 0};
    if (root != 0 && clipBox(glyph, minX, minY, maxX, maxY)) {
        minX *= scale;
        minY *= scale;
        maxX *= scale;
        maxY *= scale;
        any = true;
    } else if (root != 0) {
        painter.boundsOf(root, toPixels, any, minX, minY, maxX, maxY);
        painter.budget = 4000;
        // A paint that is scaled or turned may reach past the outlines measured: some room.
        const float margin = (maxX - minX + maxY - minY) * 0.1f;
        minX -= margin;
        minY -= margin;
        maxX += margin;
        maxY += margin;
    } else {
        for (int i = 0; i < layerCount; i++) {
            Path path;
            float x0, y0, x1, y1;
            if (!face.outline(r_.u16(layers_ + 4 * static_cast<size_t>(firstLayer + i)), coords, path) || !path.bounds(x0, y0, x1, y1)) continue;
            minX = any ? std::min(minX, x0 * scale) : x0 * scale;
            minY = any ? std::min(minY, y0 * scale) : y0 * scale;
            maxX = any ? std::max(maxX, x1 * scale) : x1 * scale;
            maxY = any ? std::max(maxY, y1 * scale) : y1 * scale;
            any = true;
        }
    }
    if (!any) return image;

    image.left = static_cast<int>(std::floor(minX));
    image.top = static_cast<int>(std::ceil(maxY));
    image.width = static_cast<int>(std::ceil(maxX)) - image.left + 1;
    image.height = image.top - static_cast<int>(std::floor(minY)) + 1;
    if (image.width <= 0 || image.height <= 0 || image.width > 4096 || image.height > 4096) return GlyphImage{};
    painter.left = image.left;
    painter.top = image.top;
    painter.width = image.width;
    painter.height = image.height;

    Layer layer(image.width, image.height);
    if (flat) {
        // Version 0: each layer is an outline in one colour, the first at the bottom.
        std::vector<uint8_t> mask;
        for (int i = 0; i < layerCount; i++) {
            const size_t record = layers_ + 4 * static_cast<size_t>(firstLayer + i);
            if (!painter.coverageOf(r_.u16(record), toPixels, mask)) continue;
            float color[4];
            painter.colorOf(r_.u16(record + 2), 1.0f, color);
            for (size_t p = 0; p < mask.size(); p++) {
                const float m = static_cast<float>(mask[p]) / 255.0f;
                if (m <= 0) continue;
                float* d = &layer.rgba[p * 4];
                const float sa = color[3] * m;
                for (int k = 0; k < 4; k++) d[k] = color[k] * m + d[k] * (1 - sa);
            }
        }
    } else {
        painter.paint(root, toPixels, layer);
    }

    image.color = true;
    image.pixels.resize(layer.rgba.size());
    for (size_t i = 0; i < layer.rgba.size(); i++) image.pixels[i] = static_cast<uint8_t>(std::clamp(layer.rgba[i], 0.0f, 1.0f) * 255.0f + 0.5f);
    return image;
}

}  // namespace astra::text
