// Variable fonts: see variation.h.

#include "variation.h"

#include <algorithm>
#include <cmath>

#include "../section_window.h"  // last: this file's own code goes in its section (D161)

namespace astra::text {

std::unique_ptr<Variations> Variations::read(const Face& face, std::vector<Axis>& axes) {
    size_t fvar = 0, length = 0;
    if (!face.table(tag("fvar"), fvar, length)) return nullptr;
    const Reader& r = face.bytes();
    const size_t axesAt = fvar + r.u16(fvar + 4);
    const int axisCount = r.u16(fvar + 8), axisSize = r.u16(fvar + 10);
    if (axisCount == 0 || axisCount > 64 || axisSize < 20) return nullptr;

    auto out = std::unique_ptr<Variations>(new Variations());
    out->file_ = r;
    out->face_ = &face;
    for (int i = 0; i < axisCount; i++) {
        const size_t at = axesAt + static_cast<size_t>(i) * static_cast<size_t>(axisSize);
        Axis axis;
        axis.tag = r.u32(at);
        axis.minimum = r.fixed(at + 4);
        axis.defaultValue = r.fixed(at + 8);
        axis.maximum = r.fixed(at + 12);
        const char letters[5] = {static_cast<char>(axis.tag >> 24), static_cast<char>(axis.tag >> 16), static_cast<char>(axis.tag >> 8),
                                 static_cast<char>(axis.tag), 0};
        axis.name = letters;
        out->axes_.push_back(axis);
    }
    axes = out->axes_;
    face.table(tag("avar"), out->avar_, length);
    face.table(tag("HVAR"), out->hvar_, length);
    face.table(tag("MVAR"), out->mvar_, length);
    if (!face.table(tag("gvar"), out->gvar_, out->gvarLength_)) out->gvar_ = 0;
    if (out->gvar_ != 0 && r.u16(out->gvar_ + 4) != axisCount) out->gvar_ = 0;
    return out;
}

// From a value as a designer states it to a place from -1 to 1, with 0 at the default. A
// font may then bend that scale (`avar`), so that, say, the weights spread evenly.
Coords Variations::normalize(const std::vector<float>& values) const {
    Coords coords(axes_.size(), 0.0f);
    bool any = false;
    for (size_t i = 0; i < axes_.size(); i++) {
        const Axis& axis = axes_[i];
        const float value = std::clamp(i < values.size() ? values[i] : axis.defaultValue, axis.minimum, axis.maximum);
        float c = 0;
        if (value < axis.defaultValue && axis.defaultValue > axis.minimum) c = -(axis.defaultValue - value) / (axis.defaultValue - axis.minimum);
        else if (value > axis.defaultValue && axis.maximum > axis.defaultValue) c = (value - axis.defaultValue) / (axis.maximum - axis.defaultValue);
        coords[i] = c;
    }
    if (avar_ != 0 && file_.u16(avar_ + 6) == axes_.size()) {
        size_t at = avar_ + 8;
        for (size_t i = 0; i < axes_.size(); i++) {
            const int pairs = file_.u16(at);
            at += 2;
            const float c = coords[i];
            for (int k = 0; k + 1 < pairs; k++) {
                const float from0 = file_.f2dot14(at + 4 * static_cast<size_t>(k)), to0 = file_.f2dot14(at + 4 * static_cast<size_t>(k) + 2);
                const float from1 = file_.f2dot14(at + 4 * static_cast<size_t>(k) + 4), to1 = file_.f2dot14(at + 4 * static_cast<size_t>(k) + 6);
                if (c >= from0 && c <= from1) {
                    coords[i] = from1 > from0 ? to0 + (c - from0) * (to1 - to0) / (from1 - from0) : to0;
                    break;
                }
            }
            at += 4 * static_cast<size_t>(pairs);
        }
    }
    for (float c : coords) any = any || c != 0;
    // Rounded as the format stores them, so that an instance is the same wherever it is made.
    for (float& c : coords) c = std::round(c * 16384.0f) / 16384.0f;
    return any ? coords : Coords();
}

namespace {

// How much of a delta applies at `c`, for a region that rises from `start` to `peak` and
// falls to `end` along one axis.
float regionFactor(float start, float peak, float end, float c) {
    if (start > peak || peak > end) return 1;
    if (start < 0 && end > 0 && peak != 0) return 1;
    if (peak == 0) return 1;
    if (c < start || c > end) return 0;
    if (c == peak) return 1;
    return c < peak ? (c - start) / (peak - start) : (end - c) / (end - peak);
}

// The lists of numbers `gvar` packs: which points a set of deltas is for, and the deltas.
struct Packed {
    const Reader& r;
    size_t at;
    size_t end;

    // False for "all the points".
    bool points(std::vector<int>& out) {
        out.clear();
        int count = r.u8(at++);
        if (count == 0) return false;
        if (count & 0x80) count = ((count & 0x7F) << 8) | r.u8(at++);
        int point = 0;
        while (static_cast<int>(out.size()) < count && at < end) {
            const uint8_t control = r.u8(at++);
            const int run = (control & 0x7F) + 1;
            for (int i = 0; i < run && static_cast<int>(out.size()) < count; i++) {
                if (control & 0x80) {
                    point += r.u16(at);
                    at += 2;
                } else {
                    point += r.u8(at++);
                }
                out.push_back(point);
            }
        }
        return true;
    }

    void deltas(std::vector<float>& out, size_t count) {
        out.assign(count, 0.0f);
        size_t i = 0;
        while (i < count && at < end) {
            const uint8_t control = r.u8(at++);
            const size_t run = static_cast<size_t>(control & 0x3F) + 1;
            for (size_t k = 0; k < run && i < count; k++, i++) {
                if (control & 0x80) continue;  // zeros
                if (control & 0x40) {
                    out[i] = static_cast<float>(r.i16(at));
                    at += 2;
                } else {
                    out[i] = static_cast<float>(r.i8(at++));
                }
            }
        }
    }
};

// A set of deltas that names only some of a contour's points says, by that, that the others
// move along with their neighbours: each one in proportion to where it lies between the
// two named points on either side of it.
void interpolateUntouched(const std::vector<float>& original, std::vector<float>& delta, const std::vector<char>& touched, size_t first, size_t last) {
    size_t firstTouched = first;
    while (firstTouched <= last && !touched[firstTouched]) firstTouched++;
    if (firstTouched > last) return;  // none named: the contour does not move

    auto between = [&](size_t a, size_t b, size_t from, size_t to) {
        // Points from..to (wrapping within the contour) lie between the named points a and b.
        for (size_t i = from;; i = (i == last ? first : i + 1)) {
            const float lo = std::min(original[a], original[b]), hi = std::max(original[a], original[b]);
            const float dLo = original[a] <= original[b] ? delta[a] : delta[b];
            const float dHi = original[a] <= original[b] ? delta[b] : delta[a];
            const float v = original[i];
            if (lo == hi) delta[i] = delta[a] == delta[b] ? delta[a] : 0;
            else if (v <= lo) delta[i] = dLo;
            else if (v >= hi) delta[i] = dHi;
            else delta[i] = dLo + (v - lo) * (dHi - dLo) / (hi - lo);
            if (i == to) break;
        }
    };

    size_t a = firstTouched;
    for (;;) {
        // The next named point after a, going round.
        size_t b = a == last ? first : a + 1;
        while (!touched[b]) b = b == last ? first : b + 1;
        const size_t afterA = a == last ? first : a + 1;
        if (afterA != b) between(a, b, afterA, b == first ? last : b - 1);
        if (b == firstTouched) break;
        a = b;
    }
}

}  // namespace

void Variations::moveGlyphPoints(int glyph, const Coords& coords, std::vector<float>& xs, std::vector<float>& ys, const std::vector<int>& ends) const {
    if (gvar_ == 0 || coords.size() != axes_.size() || glyph < 0 || glyph >= file_.u16(gvar_ + 12)) return;
    const Reader& r = file_;
    const size_t axisCount = axes_.size();
    const size_t sharedTuples = gvar_ + r.u32(gvar_ + 8);
    const int sharedCount = r.u16(gvar_ + 6);
    const bool longOffsets = (r.u16(gvar_ + 14) & 1) != 0;
    const size_t dataArray = gvar_ + r.u32(gvar_ + 16);
    const size_t g = static_cast<size_t>(glyph);
    const size_t start = dataArray + (longOffsets ? r.u32(gvar_ + 20 + 4 * g) : 2 * static_cast<size_t>(r.u16(gvar_ + 20 + 2 * g)));
    const size_t finish = dataArray + (longOffsets ? r.u32(gvar_ + 20 + 4 * g + 4) : 2 * static_cast<size_t>(r.u16(gvar_ + 20 + 2 * g + 2)));
    if (finish <= start + 4 || finish > gvar_ + gvarLength_) return;

    const size_t pointCount = xs.size();  // with the four at the end
    const int tupleField = r.u16(start);
    const int tupleCount = tupleField & 0x0FFF;
    Packed data{r, start + r.u16(start + 2), finish};
    std::vector<int> sharedPoints;
    bool sharedAll = true;
    if (tupleField & 0x8000) sharedAll = !data.points(sharedPoints);

    const std::vector<float> originalX = xs, originalY = ys;
    std::vector<float> dx, dy, fullX, fullY;
    std::vector<int> ownPoints;
    std::vector<char> touched;
    size_t header = start + 4;
    for (int t = 0; t < tupleCount && header + 4 <= finish; t++) {
        const size_t dataSize = r.u16(header);
        const int index = r.u16(header + 2);
        header += 4;

        // Where this set of deltas applies in full, and over what stretch of each axis.
        size_t peak = 0, low = 0, high = 0;
        if (index & 0x8000) {
            peak = header;
            header += 2 * axisCount;
        } else if ((index & 0x0FFF) < sharedCount) {
            peak = sharedTuples + 2 * axisCount * static_cast<size_t>(index & 0x0FFF);
        }
        if (index & 0x4000) {
            low = header;
            high = header + 2 * axisCount;
            header += 4 * axisCount;
        }
        float scalar = peak != 0 ? 1.0f : 0.0f;
        for (size_t a = 0; a < axisCount && scalar != 0; a++) {
            const float p = r.f2dot14(peak + 2 * a), c = coords[a];
            if (p == 0) continue;
            if (c == 0) {
                scalar = 0;
            } else if (low != 0) {
                const float s = r.f2dot14(low + 2 * a), e = r.f2dot14(high + 2 * a);
                if (c < s || c > e) scalar = 0;
                else if (c != p) scalar *= c < p ? (p > s ? (c - s) / (p - s) : 1) : (e > p ? (e - c) / (e - p) : 1);
            } else if (c < std::min(0.0f, p) || c > std::max(0.0f, p)) {
                scalar = 0;
            } else {
                scalar *= c / p;
            }
        }

        Packed tuple{r, data.at, std::min(data.at + dataSize, finish)};
        data.at += dataSize;
        if (scalar == 0) continue;

        const std::vector<int>* which = &sharedPoints;
        bool all = sharedAll;
        if (index & 0x2000) {
            all = !tuple.points(ownPoints);
            which = &ownPoints;
        }
        const size_t count = all ? pointCount : which->size();
        tuple.deltas(dx, count);
        tuple.deltas(dy, count);

        if (all) {
            for (size_t i = 0; i < pointCount; i++) {
                xs[i] += dx[i] * scalar;
                ys[i] += dy[i] * scalar;
            }
            continue;
        }
        fullX.assign(pointCount, 0.0f);
        fullY.assign(pointCount, 0.0f);
        touched.assign(pointCount, 0);
        for (size_t i = 0; i < count; i++) {
            const size_t point = static_cast<size_t>((*which)[i]);
            if (point >= pointCount) continue;
            fullX[point] = dx[i];
            fullY[point] = dy[i];
            touched[point] = 1;
        }
        size_t first = 0;
        for (int e : ends) {
            const size_t last = static_cast<size_t>(e);
            if (last < first || last + 4 >= pointCount) break;  // the four at the end are not on a contour
            interpolateUntouched(originalX, fullX, touched, first, last);
            interpolateUntouched(originalY, fullY, touched, first, last);
            first = last + 1;
        }
        for (size_t i = 0; i < pointCount; i++) {
            xs[i] += fullX[i] * scalar;
            ys[i] += fullY[i] * scalar;
        }
    }
}

float Variations::storeDelta(size_t store, uint32_t outer, uint32_t inner, const Coords& coords) const {
    const Reader& r = file_;
    const size_t regions = store + r.u32(store + 2);
    if (outer >= r.u16(store + 6)) return 0;
    const size_t data = store + r.u32(store + 8 + 4 * static_cast<size_t>(outer));
    const uint32_t itemCount = r.u16(data);
    const int wordField = r.u16(data + 2);
    const bool longWords = (wordField & 0x8000) != 0;
    const size_t wordCount = static_cast<size_t>(wordField & 0x7FFF);
    const size_t regionCount = r.u16(data + 4);
    if (inner >= itemCount || wordCount > regionCount) return 0;

    const size_t wordSize = longWords ? 4 : 2, restSize = longWords ? 2 : 1;
    const size_t rowSize = wordCount * wordSize + (regionCount - wordCount) * restSize;
    const size_t row = data + 6 + 2 * regionCount + rowSize * static_cast<size_t>(inner);
    const size_t axisCount = r.u16(regions);
    const size_t regionTotal = r.u16(regions + 2);
    if (axisCount != coords.size()) return 0;

    float total = 0;
    size_t at = row;
    for (size_t k = 0; k < regionCount; k++) {
        float delta;
        if (k < wordCount) {
            delta = longWords ? static_cast<float>(r.i32(at)) : static_cast<float>(r.i16(at));
            at += wordSize;
        } else {
            delta = longWords ? static_cast<float>(r.i16(at)) : static_cast<float>(r.i8(at));
            at += restSize;
        }
        const size_t region = r.u16(data + 6 + 2 * k);
        if (delta == 0 || region >= regionTotal) continue;
        const size_t axesAt = regions + 4 + region * axisCount * 6;
        float scalar = 1;
        for (size_t a = 0; a < axisCount && scalar != 0; a++)
            scalar *= regionFactor(r.f2dot14(axesAt + 6 * a), r.f2dot14(axesAt + 6 * a + 2), r.f2dot14(axesAt + 6 * a + 4), coords[a]);
        total += delta * scalar;
    }
    return total;
}

float Variations::advanceDelta(int glyph, const Coords& coords) const {
    if (coords.size() != axes_.size() || glyph < 0) return 0;
    const Reader& r = file_;
    if (hvar_ == 0) return 0;  // the face works it out from the glyph's outline data instead
    const size_t store = hvar_ + r.u32(hvar_ + 4);
    const uint32_t mapOffset = r.u32(hvar_ + 8);
    uint32_t outer = 0, inner = static_cast<uint32_t>(glyph);
    if (mapOffset != 0) {
        const size_t map = hvar_ + mapOffset;
        const int format = r.u8(map), entryFormat = r.u8(map + 1);
        const uint32_t count = format == 0 ? r.u16(map + 2) : r.u32(map + 2);
        const size_t entries = map + (format == 0 ? 4 : 6);
        if (count == 0) return 0;
        const size_t entrySize = static_cast<size_t>((entryFormat >> 4) & 3) + 1;
        const int innerBits = (entryFormat & 0x0F) + 1;
        const size_t at = entries + entrySize * std::min<size_t>(static_cast<size_t>(glyph), count - 1);
        uint32_t entry = 0;
        for (size_t k = 0; k < entrySize; k++) entry = (entry << 8) | r.u8(at + k);
        outer = entry >> innerBits;
        inner = entry & ((uint32_t{1} << innerBits) - 1);
    }
    return storeDelta(store, outer, inner, coords);
}

float Variations::metricDelta(uint32_t which, const Coords& coords) const {
    if (mvar_ == 0 || coords.size() != axes_.size()) return 0;
    const Reader& r = file_;
    const size_t recordSize = r.u16(mvar_ + 6), count = r.u16(mvar_ + 8);
    const size_t storeOffset = r.u16(mvar_ + 10);
    if (storeOffset == 0 || recordSize < 8) return 0;
    for (size_t i = 0; i < count; i++) {
        const size_t record = mvar_ + 12 + recordSize * i;
        if (r.u32(record) == which) return storeDelta(mvar_ + storeOffset, r.u16(record + 4), r.u16(record + 6), coords);
    }
    return 0;
}

size_t regionCount(const Reader& r, size_t store, uint32_t set) {
    if (set >= r.u16(store + 6)) return 0;
    return r.u16(store + r.u32(store + 8 + 4 * static_cast<size_t>(set)) + 4);
}

std::vector<float> regionScalars(const Reader& r, size_t store, uint32_t set, const Coords& coords) {
    std::vector<float> scalars(regionCount(r, store, set), 0.0f);
    if (scalars.empty()) return scalars;
    const size_t data = store + r.u32(store + 8 + 4 * static_cast<size_t>(set));
    const size_t regions = store + r.u32(store + 2);
    const size_t axisCount = r.u16(regions), regionTotal = r.u16(regions + 2);
    // At the default place (no coords) no region counts, and the numbers stand as they are.
    if (axisCount != coords.size()) return scalars;
    for (size_t k = 0; k < scalars.size(); k++) {
        const size_t region = r.u16(data + 6 + 2 * k);
        if (region >= regionTotal) continue;
        const size_t axesAt = regions + 4 + region * axisCount * 6;
        float scalar = 1;
        for (size_t a = 0; a < axisCount && scalar != 0; a++)
            scalar *= regionFactor(r.f2dot14(axesAt + 6 * a), r.f2dot14(axesAt + 6 * a + 2), r.f2dot14(axesAt + 6 * a + 4), coords[a]);
        scalars[k] = scalar;
    }
    return scalars;
}

}  // namespace astra::text
