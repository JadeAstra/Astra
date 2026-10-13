// Reading a font file: see face.h.

#include "face.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

#include "bitmaps.h"
#include "cff.h"
#include "color.h"
#include "variation.h"

namespace astra::text {

// ---- Paths ----

void Path::transform(float a, float b, float c, float d, float e, float f) {
    for (size_t i = 0; i + 1 < points.size(); i += 2) {
        const float x = points[i], y = points[i + 1];
        points[i] = a * x + c * y + e;
        points[i + 1] = b * x + d * y + f;
    }
}

bool Path::bounds(float& minX, float& minY, float& maxX, float& maxY) const {
    if (points.size() < 2) return false;
    minX = maxX = points[0];
    minY = maxY = points[1];
    // Control points are included: a curve lies inside the hull of its points, so the box is
    // a little generous and never too small.
    for (size_t i = 0; i + 1 < points.size(); i += 2) {
        minX = std::min(minX, points[i]);
        maxX = std::max(maxX, points[i]);
        minY = std::min(minY, points[i + 1]);
        maxY = std::max(maxY, points[i + 1]);
    }
    return true;
}

// ---- Loading ----

namespace {

struct OwnedBytes final : FontData {
    std::vector<uint8_t> bytes;
    explicit OwnedBytes(std::vector<uint8_t> b) : bytes(std::move(b)) {
        data = bytes.data();
        size = bytes.size();
    }
};

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

}  // namespace

int Face::count(const std::string& path) {
    std::ifstream file(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
    uint8_t head[12] = {};
    if (!file.read(reinterpret_cast<char*>(head), sizeof head)) return 0;
    const Reader r(head, sizeof head);
    const uint32_t kind = r.u32(0);
    if (kind == tag("ttcf")) return static_cast<int>(std::min<uint32_t>(r.u32(8), 1024));
    return kind == 0x00010000 || kind == tag("true") || kind == tag("OTTO") ? 1 : 0;
}

std::shared_ptr<Face> Face::load(const std::string& path, int index, std::string& error) {
    auto data = openFontFile(path);
    if (data == nullptr) {
        error = "cannot read " + path;
        return nullptr;
    }
    return fromData(std::move(data), index, path, error);
}

std::shared_ptr<Face> Face::fromBytes(std::vector<uint8_t> bytes, int index, const std::string& path, std::string& error) {
    return fromData(std::make_shared<OwnedBytes>(std::move(bytes)), index, path, error);
}

std::shared_ptr<Face> Face::fromData(std::shared_ptr<const FontData> data, int index, const std::string& path, std::string& error) {
    std::shared_ptr<Face> face(new Face());
    face->owned_ = std::move(data);
    face->file_ = Reader(face->owned_->data, face->owned_->size);
    face->path_ = path;
    face->index_ = index;
    if (!face->read(error)) return nullptr;
    return face;
}

Face::~Face() = default;

bool Face::table(uint32_t name, size_t& offset, size_t& length) const {
    const int tables = file_.u16(directory_ + 4);
    for (int i = 0; i < tables; i++) {
        const size_t record = directory_ + 12 + 16 * static_cast<size_t>(i);
        if (file_.u32(record) != name) continue;
        offset = file_.u32(record + 8);
        length = file_.u32(record + 12);
        return file_.has(offset, length) && length > 0;
    }
    return false;
}

bool Face::read(std::string& error) {
    auto fail = [&](const char* why) {
        error = path_ + ": " + why;
        return false;
    };

    // A collection starts with 'ttcf' and a list of where each font's table directory is.
    directory_ = 0;
    if (file_.u32(0) == tag("ttcf")) {
        if (index_ < 0 || static_cast<uint32_t>(index_) >= file_.u32(8)) return fail("no such font in the collection");
        directory_ = file_.u32(12 + 4 * static_cast<size_t>(index_));
    } else if (index_ != 0) {
        return fail("the file holds one font");
    }
    const uint32_t kind = file_.u32(directory_);
    if (kind != 0x00010000 && kind != tag("true") && kind != tag("OTTO")) return fail("not a TrueType or OpenType font");

    size_t head = 0, hhea = 0, maxp = 0, cmap = 0, os2 = 0, name = 0, post = 0, cff = 0, cffLength = 0, length = 0;
    if (!table(tag("head"), head, length) || !table(tag("hhea"), hhea, length) || !table(tag("maxp"), maxp, length) ||
        !table(tag("cmap"), cmap, length) || !table(tag("hmtx"), hmtx_, hmtxLength_))
        return fail("a required table is missing");
    table(tag("OS/2"), os2, length);
    table(tag("name"), name, length);
    table(tag("post"), post, length);
    const bool hasGlyf = table(tag("glyf"), glyf_, glyfLength_) && table(tag("loca"), loca_, length);
    // Cubic outlines: `CFF `, or `CFF2` in a variable font.
    const bool hasCff = table(tag("CFF "), cff, cffLength) || table(tag("CFF2"), cff, cffLength);
    size_t unused = 0;
    // A font may have no outlines at all and be all pictures (an emoji font).
    const bool hasPictures = (table(tag("CBDT"), unused, length) && table(tag("CBLC"), unused, length)) || table(tag("sbix"), unused, length);
    if (!hasGlyf && !hasCff && !hasPictures) return fail("the font has no outlines");

    unitsPerEm_ = file_.u16(head + 18);
    longLoca_ = file_.i16(head + 50) != 0;
    glyphCount_ = file_.u16(maxp + 4);
    hMetricCount_ = file_.u16(hhea + 34);
    if (unitsPerEm_ < 16 || unitsPerEm_ > 16384 || glyphCount_ == 0 || hMetricCount_ == 0) return fail("bad header values");
    if (!hasGlyf) glyf_ = loca_ = 0;

    // Line measurements. A font says which pair it means to be used: the "typographic" ones
    // if it sets the flag for them, and otherwise the ones Windows has always used.
    ascent_ = file_.i16(hhea + 4);
    descent_ = -file_.i16(hhea + 6);
    lineGap_ = file_.i16(hhea + 8);
    weight_ = 400;
    stretch_ = 5;
    if (os2 != 0) {
        const int version = file_.u16(os2);
        weight_ = std::clamp<int>(file_.u16(os2 + 4), 1, 1000);
        stretch_ = std::clamp<int>(file_.u16(os2 + 6), 1, 9);
        const uint16_t selection = file_.u16(os2 + 62);
        italic_ = (selection & 0x01) != 0 || (selection & 0x200) != 0;
        if ((selection & 0x80) != 0) {
            ascent_ = file_.i16(os2 + 68);
            descent_ = -file_.i16(os2 + 70);
            lineGap_ = file_.i16(os2 + 72);
        } else if (file_.u16(os2 + 74) != 0 || file_.u16(os2 + 76) != 0) {
            ascent_ = file_.u16(os2 + 74);
            descent_ = file_.u16(os2 + 76);
            lineGap_ = 0;
        }
        if (version >= 2) {
            xHeight_ = file_.i16(os2 + 86);
            capHeight_ = file_.i16(os2 + 88);
        }
        monospace_ = file_.u8(os2 + 35) == 9;  // PANOSE: proportion "monospaced"
    } else {
        italic_ = (file_.u16(head + 44) & 0x02) != 0;
        if ((file_.u16(head + 44) & 0x01) != 0) weight_ = 700;
    }
    if (ascent_ <= 0 && descent_ <= 0) {
        ascent_ = unitsPerEm_ * 4 / 5;
        descent_ = unitsPerEm_ / 5;
    }
    if (post != 0 && file_.u32(post + 12) != 0) monospace_ = true;

    readCharacterMap(cmap);
    if (cmap4_ == 0 && cmap12_ == 0 && cmap13_ == 0 && cmap6_ == 0 && cmap0_ == 0) return fail("no character map that can be read");
    if (name != 0) readNames(name);
    if (family_.empty()) family_ = std::filesystem::path(path_).stem().string();
    if (style_.empty()) style_ = "Regular";
    if (fullName_.empty()) fullName_ = family_ + (style_ == "Regular" ? "" : " " + style_);

    if (hasCff && !hasGlyf) {
        cff_ = std::make_unique<CffFont>();
        if (!cff_->load(file_, cff, cffLength, error)) {
            error = path_ + ": " + error;
            return false;
        }
    }
    variations_ = Variations::read(*this, axes_);
    color_ = ColorTables::read(*this);
    if (hasPictures) bitmaps_ = BitmapGlyphs::read(*this);

    // A font that does not state its x-height and capital height is measured instead.
    auto topOf = [&](uint32_t cp) {
        Path path;
        float minX, minY, maxX, maxY = 0;
        const int glyph = glyphIndex(cp);
        if (glyph == 0 || !outline(glyph, {}, path) || !path.bounds(minX, minY, maxX, maxY)) return 0;
        return static_cast<int>(maxY);
    };
    if (xHeight_ <= 0) xHeight_ = topOf('x');
    if (capHeight_ <= 0) capHeight_ = topOf('H');
    if (xHeight_ <= 0) xHeight_ = ascent_ / 2;
    if (capHeight_ <= 0) capHeight_ = ascent_ * 7 / 10;
    return true;
}

// The names a font carries: what its family is called and what style it is. A font has
// several of each, for different systems and languages; English for Windows is preferred.
void Face::readNames(size_t nameTable) {
    const int count = file_.u16(nameTable + 2);
    const size_t strings = nameTable + file_.u16(nameTable + 4);
    // By name id: [1] family, [2] style, [4] full name, [16] and [17] the family and style a
    // font with more than four styles gives (so that "Segoe UI Semibold" has family "Segoe UI").
    std::string best[18];
    int rank[18] = {};
    for (int i = 0; i < count; i++) {
        const size_t record = nameTable + 6 + 12 * static_cast<size_t>(i);
        const int platform = file_.u16(record), encoding = file_.u16(record + 2), language = file_.u16(record + 4);
        const int id = file_.u16(record + 6);
        if (id > 17) continue;
        const size_t length = file_.u16(record + 8), at = strings + file_.u16(record + 10);
        if (!file_.has(at, length)) continue;

        int score = 0;
        std::string text;
        if (platform == 3 || platform == 0) {
            // UTF-16, high byte first.
            score = platform == 3 ? (language == 0x409 ? 4 : 2) : 3;
            if (platform == 3 && encoding != 1 && encoding != 10 && encoding != 0) continue;
            for (size_t k = 0; k + 1 < length; k += 2) {
                uint32_t cp = file_.u16(at + k);
                if (cp >= 0xD800 && cp < 0xDC00 && k + 3 < length) {
                    const uint32_t low = file_.u16(at + k + 2);
                    if (low >= 0xDC00 && low < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        k += 2;
                    }
                }
                appendUtf8(text, cp);
            }
        } else if (platform == 1 && encoding == 0) {
            score = 1;
            for (size_t k = 0; k < length; k++) {
                const uint8_t c = file_.u8(at + k);
                text += c < 0x80 ? static_cast<char>(c) : '?';
            }
        } else {
            continue;
        }
        if (score > rank[id] && !text.empty()) {
            rank[id] = score;
            best[id] = std::move(text);
        }
    }
    family_ = !best[16].empty() ? best[16] : best[1];
    style_ = !best[17].empty() ? best[17] : best[2];
    fullName_ = best[4];
    legacyFamily_ = best[1];
}

// ---- The character map ----

void Face::readCharacterMap(size_t cmap) {
    const int subtables = file_.u16(cmap + 2);
    int best12 = 0, best4 = 0;
    for (int i = 0; i < subtables; i++) {
        const size_t record = cmap + 4 + 8 * static_cast<size_t>(i);
        const int platform = file_.u16(record), encoding = file_.u16(record + 2);
        const size_t at = cmap + file_.u32(record + 4);
        const int format = file_.u16(at);
        const bool unicode = platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));
        const bool symbols = platform == 3 && encoding == 0;
        if (!unicode && !symbols) {
            if (platform == 1 && encoding == 0 && format == 0 && cmap0_ == 0) cmap0_ = at;  // the last resort
            continue;
        }
        // Among several, the Windows ones win, as they are what fonts are tested with.
        const int score = platform == 3 ? 2 : 1;
        if (format == 12 && score > best12) {
            cmap12_ = at;
            best12 = score;
        } else if (format == 4 && score > best4) {
            cmap4_ = at;
            best4 = score;
            if (symbols) symbol_ = true;
        } else if (format == 13 && cmap13_ == 0) {
            cmap13_ = at;
        } else if (format == 6 && cmap6_ == 0) {
            cmap6_ = at;
        }
    }
    // A symbol font is one only if it has no ordinary Unicode map as well.
    if (best4 == 2 && cmap12_ != 0) symbol_ = false;
}

int Face::glyphIndex(uint32_t cp) const {
    auto inRange = [&](uint32_t glyph) { return glyph < static_cast<uint32_t>(glyphCount_) ? static_cast<int>(glyph) : 0; };

    if (cmap12_ != 0 || cmap13_ != 0) {
        const size_t at = cmap12_ != 0 ? cmap12_ : cmap13_;
        uint32_t lo = 0, hi = file_.u32(at + 12);
        while (lo < hi) {
            const uint32_t mid = lo + (hi - lo) / 2;
            const size_t group = at + 16 + 12 * static_cast<size_t>(mid);
            if (cp < file_.u32(group)) hi = mid;
            else if (cp > file_.u32(group + 4)) lo = mid + 1;
            else return inRange(cmap12_ != 0 ? file_.u32(group + 8) + (cp - file_.u32(group)) : file_.u32(group + 8));
        }
        if (cmap12_ != 0) return 0;
    }
    if (cmap4_ != 0) {
        // A symbol font keeps its characters at U+F000 and up; a plain character means the
        // one there.
        if (symbol_ && cp < 0x100) cp += 0xF000;
        if (cp > 0xFFFF) return 0;
        const int segments = file_.u16(cmap4_ + 6) / 2;
        const size_t ends = cmap4_ + 14;
        const size_t starts = ends + 2 * static_cast<size_t>(segments) + 2;
        const size_t deltas = starts + 2 * static_cast<size_t>(segments);
        const size_t offsets = deltas + 2 * static_cast<size_t>(segments);
        int lo = 0, hi = segments;
        while (lo < hi) {
            const int mid = (lo + hi) / 2;
            if (file_.u16(ends + 2 * static_cast<size_t>(mid)) < cp) lo = mid + 1;
            else hi = mid;
        }
        if (lo >= segments) return 0;
        const size_t s = static_cast<size_t>(lo);
        const uint32_t start = file_.u16(starts + 2 * s);
        if (cp < start) return 0;
        const uint16_t rangeOffset = file_.u16(offsets + 2 * s);
        if (rangeOffset == 0) return inRange(static_cast<uint16_t>(cp + file_.u16(deltas + 2 * s)));
        const uint16_t glyph = file_.u16(offsets + 2 * s + rangeOffset + 2 * (cp - start));
        return glyph == 0 ? 0 : inRange(static_cast<uint16_t>(glyph + file_.u16(deltas + 2 * s)));
    }
    if (cmap6_ != 0) {
        const uint32_t first = file_.u16(cmap6_ + 6), count = file_.u16(cmap6_ + 8);
        return cp >= first && cp - first < count ? inRange(file_.u16(cmap6_ + 10 + 2 * static_cast<size_t>(cp - first))) : 0;
    }
    if (cmap0_ != 0 && cp < 0x80) return inRange(file_.u8(cmap0_ + 6 + cp));
    return 0;
}

// ---- Measurements ----

float Face::metric(uint32_t which, float base, const Coords& coords) const {
    if (coords.empty() || variations_ == nullptr) return base;
    return base + variations_->metricDelta(which, coords);
}

float Face::ascent(const Coords& coords) const { return metric(tag("hcla"), static_cast<float>(ascent_), coords); }
float Face::descent(const Coords& coords) const { return metric(tag("hcld"), static_cast<float>(descent_), coords); }
float Face::lineGap(const Coords& coords) const { return metric(tag("hlgp"), static_cast<float>(lineGap_), coords); }
float Face::xHeight(const Coords& coords) const { return metric(tag("xhgt"), static_cast<float>(xHeight_), coords); }
float Face::capHeight(const Coords& coords) const { return metric(tag("cpht"), static_cast<float>(capHeight_), coords); }

float Face::advance(int glyph, const Coords& coords) const {
    if (glyph < 0 || glyph >= glyphCount_) glyph = 0;
    const size_t entry = static_cast<size_t>(std::min(glyph, hMetricCount_ - 1));
    float units = static_cast<float>(file_.u16(hmtx_ + 4 * entry));
    // A width cannot be negative, but some fonts write one for a mark that sits on another
    // letter (65530 for -6). It takes no room.
    if (units >= 32768.0f) units = 0;
    if (!coords.empty() && variations_ != nullptr) {
        if (variations_->hasAdvanceTable()) {
            units += variations_->advanceDelta(glyph, coords);
        } else if (variations_->hasGlyphData()) {
            // A font without the table of widths says how a glyph's width changes with its outline.
            Path unused;
            float change = 0;
            glyfOutline(glyph, coords, unused, 0, &change);
            units += change;
        }
    }
    return std::max(units, 0.0f);
}

Coords Face::normalize(const std::vector<float>& values) const {
    return variations_ != nullptr ? variations_->normalize(values) : Coords();
}

Coords Face::instance(float weight, float widthPercent, float opticalSize) const {
    if (axes_.empty()) return Coords();
    std::vector<float> values;
    for (const Axis& axis : axes_) {
        float value = axis.defaultValue;
        if (axis.tag == tag("wght")) value = weight;
        else if (axis.tag == tag("wdth")) value = widthPercent;
        else if (axis.tag == tag("opsz") && opticalSize > 0) value = opticalSize;
        values.push_back(std::clamp(value, axis.minimum, axis.maximum));
    }
    return normalize(values);
}

// ---- Outlines ----

bool Face::outline(int glyph, const Coords& coords, Path& path) const {
    if (glyph < 0 || glyph >= glyphCount_) return false;
    const size_t before = path.verbs.size();
    if (cff_ != nullptr) cff_->outline(glyph, coords, path);
    else glyfOutline(glyph, coords, path, 0);
    return path.verbs.size() > before;
}

bool Face::glyfRange(int glyph, size_t& start, size_t& end) const {
    if (glyph < 0 || glyph >= glyphCount_ || glyf_ == 0) return false;
    const size_t g = static_cast<size_t>(glyph);
    if (longLoca_) {
        start = glyf_ + file_.u32(loca_ + 4 * g);
        end = glyf_ + file_.u32(loca_ + 4 * g + 4);
    } else {
        start = glyf_ + 2 * static_cast<size_t>(file_.u16(loca_ + 2 * g));
        end = glyf_ + 2 * static_cast<size_t>(file_.u16(loca_ + 2 * g + 2));
    }
    return end > start && end <= glyf_ + glyfLength_;  // equal means the glyph is empty (a space)
}

namespace {

struct GlyfPoint {
    float x, y;
    bool onCurve;
};

// One closed contour of quadratic curves, as TrueType stores it: points on the outline and
// control points between them, with a point on the outline implied halfway between two
// control points in a row.
void appendContour(Path& path, const GlyfPoint* points, size_t n) {
    if (n < 2) return;
    size_t first = 0;
    while (first < n && !points[first].onCurve) first++;
    float startX, startY;
    if (first == n) {
        startX = (points[0].x + points[n - 1].x) / 2;
        startY = (points[0].y + points[n - 1].y) / 2;
        first = 0;
    } else {
        startX = points[first].x;
        startY = points[first].y;
        first++;
    }
    path.moveTo(startX, startY);
    bool haveControl = false;
    float cx = 0, cy = 0;
    for (size_t step = 0; step < n; step++) {
        const GlyfPoint& point = points[(first + step) % n];
        if (point.onCurve) {
            if (haveControl) path.quadTo(cx, cy, point.x, point.y);
            else if (step + 1 < n || point.x != startX || point.y != startY) path.lineTo(point.x, point.y);
            haveControl = false;
        } else {
            if (haveControl) path.quadTo(cx, cy, (cx + point.x) / 2, (cy + point.y) / 2);
            cx = point.x;
            cy = point.y;
            haveControl = true;
        }
    }
    if (haveControl) path.quadTo(cx, cy, startX, startY);
    path.close();
}

}  // namespace

// A glyph is either a set of contours, or a list of other glyphs to place, each with an
// offset and perhaps a scale. In a variable font the points of the first kind, and the
// offsets of the second, are moved for the instance asked for.
bool Face::glyfOutline(int glyph, const Coords& coords, Path& path, int depth, float* widthChange) const {
    size_t at, end;
    const bool vary = !coords.empty() && variations_ != nullptr && variations_->hasGlyphData();
    if (depth > 8) return false;
    if (!glyfRange(glyph, at, end)) {
        // No outline (a space): its width may change all the same.
        if (vary && widthChange != nullptr) {
            std::vector<float> xs(4, 0.0f), ys(4, 0.0f);
            variations_->moveGlyphPoints(glyph, coords, xs, ys, {});
            *widthChange = xs[1] - xs[0];
        }
        return false;
    }
    const Reader& r = file_;
    const int contours = r.i16(at);
    size_t p = at + 10;

    if (contours >= 0) {
        if (contours == 0) return false;
        std::vector<int> ends(static_cast<size_t>(contours));
        for (int& e : ends) {
            e = r.u16(p);
            p += 2;
        }
        const int count = ends.back() + 1;
        if (count <= 0 || count > 20000) return false;
        p += 2 + r.u16(p);  // the hinting program is skipped: it is not run

        const size_t n = static_cast<size_t>(count);
        std::vector<uint8_t> flags(n);
        for (size_t i = 0; i < n && p < end;) {
            const uint8_t flag = r.u8(p++);
            flags[i++] = flag;
            if (flag & 8) {
                for (int repeat = r.u8(p++); repeat > 0 && i < n; repeat--) flags[i++] = flag;
            }
        }
        std::vector<float> xs(n + 4, 0.0f), ys(n + 4, 0.0f);
        auto coordinates = [&](std::vector<float>& values, uint8_t shortBit, uint8_t sameBit) {
            int value = 0;
            for (size_t i = 0; i < n; i++) {
                const uint8_t flag = flags[i];
                if (flag & shortBit) {
                    const int delta = r.u8(p++);
                    value += (flag & sameBit) ? delta : -delta;
                } else if (!(flag & sameBit)) {
                    value += r.i16(p);
                    p += 2;
                }
                values[i] = static_cast<float>(value);
            }
        };
        coordinates(xs, 2, 16);
        coordinates(ys, 4, 32);
        if (vary) {
            variations_->moveGlyphPoints(glyph, coords, xs, ys, ends);
            if (widthChange != nullptr) *widthChange = xs[n + 1] - xs[n];
        }

        std::vector<GlyfPoint> points(n);
        for (size_t i = 0; i < n; i++) points[i] = GlyfPoint{xs[i], ys[i], (flags[i] & 1) != 0};
        size_t first = 0;
        for (int e : ends) {
            const size_t last = static_cast<size_t>(e);
            if (last >= first && last < n) appendContour(path, points.data() + first, last - first + 1);
            first = last + 1;
        }
        return true;
    }

    // A composite: read every part first, so that their offsets can be moved together.
    struct Part {
        int glyph;
        float dx, dy;
        float a, b, c, d;
    };
    std::vector<Part> parts;
    for (;;) {
        if (p + 4 > end || parts.size() > 256) break;
        const uint16_t flags = r.u16(p);
        Part part{r.u16(p + 2), 0, 0, 1, 0, 0, 1};
        p += 4;
        if (flags & 1) {
            part.dx = static_cast<float>(r.i16(p));
            part.dy = static_cast<float>(r.i16(p + 2));
            p += 4;
        } else {
            part.dx = static_cast<float>(r.i8(p));
            part.dy = static_cast<float>(r.i8(p + 1));
            p += 2;
        }
        if (!(flags & 2)) part.dx = part.dy = 0;  // placed by matching points, which nothing in use does
        if (flags & 8) {
            part.a = part.d = r.f2dot14(p);
            p += 2;
        } else if (flags & 0x40) {
            part.a = r.f2dot14(p);
            part.d = r.f2dot14(p + 2);
            p += 4;
        } else if (flags & 0x80) {
            part.a = r.f2dot14(p);
            part.b = r.f2dot14(p + 2);
            part.c = r.f2dot14(p + 4);
            part.d = r.f2dot14(p + 6);
            p += 8;
        }
        parts.push_back(part);
        if (!(flags & 0x20)) break;
    }
    if (vary && !parts.empty()) {
        std::vector<float> xs(parts.size() + 4, 0.0f), ys(parts.size() + 4, 0.0f);
        for (size_t i = 0; i < parts.size(); i++) {
            xs[i] = parts[i].dx;
            ys[i] = parts[i].dy;
        }
        variations_->moveGlyphPoints(glyph, coords, xs, ys, {});
        if (widthChange != nullptr) *widthChange = xs[parts.size() + 1] - xs[parts.size()];
        for (size_t i = 0; i < parts.size(); i++) {
            parts[i].dx = xs[i];
            parts[i].dy = ys[i];
        }
    }
    bool any = false;
    for (const Part& part : parts) {
        Path piece;
        if (!glyfOutline(part.glyph, coords, piece, depth + 1)) continue;
        piece.transform(part.a, part.b, part.c, part.d, part.dx, part.dy);
        path.verbs.insert(path.verbs.end(), piece.verbs.begin(), piece.verbs.end());
        path.points.insert(path.points.end(), piece.points.begin(), piece.points.end());
        any = true;
    }
    return any;
}

}  // namespace astra::text
