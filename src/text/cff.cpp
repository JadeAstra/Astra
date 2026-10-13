// Cubic outlines: see cff.h.

#include "cff.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "variation.h"

namespace astra::text {

namespace {

// A "dict": operands followed by the operator they belong to, over and over. This walks one
// and hands out each operator with its operands.
struct Dict {
    const Reader& r;
    size_t at, end;
    double operands[48];
    int count = 0;

    // The next operator (two-byte ones as 0x0C00 + the second byte), or -1 at the end.
    int next() {
        count = 0;
        while (at < end) {
            const uint8_t b = r.u8(at);
            if (b <= 24) {  // 22 to 24 are operators in the variable kind of table only
                at++;
                if (b == 12) return 0x0C00 | r.u8(at++);
                return b;
            }
            double value = 0;
            if (b == 28) {
                value = r.i16(at + 1);
                at += 3;
            } else if (b == 29) {
                value = r.i32(at + 1);
                at += 5;
            } else if (b == 30) {
                // A real number, two decimal digits or signs to the byte.
                at++;
                std::string text;
                for (bool done = false; !done && at < end;) {
                    const uint8_t pair = r.u8(at++);
                    for (int half = 0; half < 2 && !done; half++) {
                        const int nibble = half == 0 ? pair >> 4 : pair & 0x0F;
                        if (nibble <= 9) text += static_cast<char>('0' + nibble);
                        else if (nibble == 0xA) text += '.';
                        else if (nibble == 0xB) text += 'E';
                        else if (nibble == 0xC) text += "E-";
                        else if (nibble == 0xE) text += '-';
                        else if (nibble == 0xF) done = true;
                    }
                }
                value = std::atof(text.c_str());
            } else if (b >= 32 && b <= 246) {
                value = static_cast<int>(b) - 139;
                at++;
            } else if (b >= 247 && b <= 250) {
                value = (static_cast<int>(b) - 247) * 256 + r.u8(at + 1) + 108;
                at += 2;
            } else if (b >= 251 && b <= 254) {
                value = -(static_cast<int>(b) - 251) * 256 - r.u8(at + 1) - 108;
                at += 2;
            } else {
                at++;  // reserved: passed over
                continue;
            }
            if (count < 48) operands[count++] = value;
        }
        return -1;
    }
};

}  // namespace

bool CffFont::readIndex(size_t at, Index& out) const {
    out = Index();
    // How many items there are is two bytes, or four in the variable kind of table.
    const size_t countSize = cff2_ ? 4 : 2;
    if (at + countSize > limit_) return false;
    out.count = cff2_ ? r_.u32(at) : r_.u16(at);
    if (out.count == 0) {
        out.end = at + countSize;
        return true;
    }
    if (out.count > (limit_ - at)) return false;
    out.offSize = r_.u8(at + countSize);
    if (out.offSize < 1 || out.offSize > 4) return false;
    out.offsets = at + countSize + 1;
    out.data = out.offsets + (out.count + 1) * static_cast<size_t>(out.offSize) - 1;
    size_t start, finish;
    if (!item(out, out.count - 1, start, finish)) return false;
    out.end = finish;
    return true;
}

bool CffFont::item(const Index& index, size_t i, size_t& start, size_t& end) const {
    if (i >= index.count) return false;
    auto offset = [&](size_t k) {
        size_t value = 0;
        const size_t at = index.offsets + k * static_cast<size_t>(index.offSize);
        for (int b = 0; b < index.offSize; b++) value = (value << 8) | r_.u8(at + static_cast<size_t>(b));
        return value;
    };
    start = index.data + offset(i);
    end = index.data + offset(i + 1);
    return start <= end && end <= limit_;
}

bool CffFont::readPrivate(size_t dictStart, size_t dictEnd, Index& subrs, int& vsindex) const {
    subrs = Index();
    vsindex = 0;
    bool fine = true;
    Dict dict{r_, dictStart, dictEnd, {}, 0};
    for (int op = dict.next(); op >= 0; op = dict.next()) {
        // Subrs: where this dict's subroutines are, counted from the dict itself.
        if (op == 19 && dict.count >= 1) fine = readIndex(dictStart + static_cast<size_t>(dict.operands[0]), subrs);
        // vsindex: which set of regions this dict's glyphs blend by, unless they say otherwise.
        else if (op == 22 && cff2_ && dict.count >= 1) vsindex = static_cast<int>(dict.operands[0]);
    }
    return fine;
}

bool CffFont::load(const Reader& file, size_t offset, size_t length, std::string& error) {
    r_ = file;
    base_ = offset;
    limit_ = offset + length;
    auto fail = [&](const char* why) {
        error = why;
        return false;
    };
    const int version = r_.u8(base_);
    if (version != 1 && version != 2) return fail("an outline table of a version that is not read");
    cff2_ = version == 2;

    size_t topStart, topEnd;
    if (cff2_) {
        // The variable kind: no names and no strings, the one top dict straight after the
        // header, and the shared subroutines after that.
        topStart = base_ + r_.u8(base_ + 2);
        topEnd = topStart + r_.u16(base_ + 3);
        if (topEnd > limit_ || !readIndex(topEnd, globalSubrs_)) return fail("the outline table is damaged");
    } else {
        Index names, topDicts, strings;
        if (!readIndex(base_ + r_.u8(base_ + 2), names) || !readIndex(names.end, topDicts) || !readIndex(topDicts.end, strings) ||
            !readIndex(strings.end, globalSubrs_))
            return fail("the outline table is damaged");
        if (!item(topDicts, 0, topStart, topEnd)) return fail("the outline table has no font in it");
    }
    size_t charStrings = 0, privateOffset = 0, privateSize = 0, fdArray = 0;
    Dict top{r_, topStart, topEnd, {}, 0};
    for (int op = top.next(); op >= 0; op = top.next()) {
        if (op == 17 && top.count >= 1) charStrings = static_cast<size_t>(top.operands[0]);
        else if (op == 18 && top.count >= 2) {
            privateSize = static_cast<size_t>(top.operands[0]);
            privateOffset = static_cast<size_t>(top.operands[1]);
        } else if (op == 0x0C24 && top.count >= 1) fdArray = static_cast<size_t>(top.operands[0]);
        else if (op == 0x0C25 && top.count >= 1) fdSelect_ = base_ + static_cast<size_t>(top.operands[0]);
        // vstore: the regions of the axes that glyph programs blend by. Two bytes of length come first.
        else if (op == 24 && cff2_ && top.count >= 1) variationStore_ = base_ + static_cast<size_t>(top.operands[0]) + 2;
        else if (op == 0x0C06 && top.count >= 1 && top.operands[0] != 2) return fail("outlines of a kind that is not read (Type 1 programs)");
    }
    if (charStrings == 0 || !readIndex(base_ + charStrings, charStrings_)) return fail("the outline table has no glyph programs");

    if (fdArray != 0) {
        // A font for many scripts: several font dicts, and a table saying which a glyph uses.
        Index dicts;
        if (!readIndex(base_ + fdArray, dicts)) return fail("the outline table is damaged");
        for (size_t i = 0; i < dicts.count; i++) {
            size_t start, end;
            Index subrs;
            int vsindex = 0;
            if (item(dicts, i, start, end)) {
                Dict fontDict{r_, start, end, {}, 0};
                for (int op = fontDict.next(); op >= 0; op = fontDict.next()) {
                    if (op == 18 && fontDict.count >= 2) {
                        const size_t at = base_ + static_cast<size_t>(fontDict.operands[1]);
                        readPrivate(at, at + static_cast<size_t>(fontDict.operands[0]), subrs, vsindex);
                    }
                }
            }
            localSubrs_.push_back(subrs);
            vsindexes_.push_back(vsindex);
        }
        if (localSubrs_.empty()) return fail("the outline table is damaged");
        fdSelectFormat_ = fdSelect_ != 0 ? r_.u8(fdSelect_) : -1;
    } else {
        Index subrs;
        int vsindex = 0;
        if (privateSize != 0) readPrivate(base_ + privateOffset, base_ + privateOffset + privateSize, subrs, vsindex);
        localSubrs_.push_back(subrs);
        vsindexes_.push_back(vsindex);
    }
    return true;
}

int CffFont::fontDictOf(int glyph) const {
    if (fdSelectFormat_ == 0) return r_.u8(fdSelect_ + 1 + static_cast<size_t>(glyph));
    if (fdSelectFormat_ == 3) {
        const int ranges = r_.u16(fdSelect_ + 1);
        int lo = 0, hi = ranges;
        while (lo + 1 < hi) {
            const int mid = (lo + hi) / 2;
            if (r_.u16(fdSelect_ + 3 + 3 * static_cast<size_t>(mid)) <= glyph) lo = mid;
            else hi = mid;
        }
        return r_.u8(fdSelect_ + 3 + 3 * static_cast<size_t>(lo) + 2);
    }
    if (fdSelectFormat_ == 4) {  // the same with larger numbers, for a font of more than 65,535 glyphs
        const uint32_t ranges = r_.u32(fdSelect_ + 1);
        uint32_t lo = 0, hi = ranges;
        while (lo + 1 < hi) {
            const uint32_t mid = lo + (hi - lo) / 2;
            if (r_.u32(fdSelect_ + 5 + 6 * static_cast<size_t>(mid)) <= static_cast<uint32_t>(glyph)) lo = mid;
            else hi = mid;
        }
        return r_.u16(fdSelect_ + 5 + 6 * static_cast<size_t>(lo) + 4);
    }
    return 0;
}

// The state of one glyph's program as it runs.
struct CffFont::Run {
    Path* path;
    const Index* local;
    // The variable kind: where on the axes the outline is wanted, the set of regions in
    // use, and how much each of them counts there (worked out when first needed).
    const Coords* coords = nullptr;
    int vsindex = 0;
    int scalarsFor = -1;
    std::vector<float> scalars;
    float stack[513];
    int size = 0;
    float x = 0, y = 0;
    int stems = 0;
    bool open = false;       // a contour has been started and not yet closed
    bool widthSeen = false;  // the optional width in front of the first operator is past
    bool done = false;
    int steps = 0;

    void push(float value) {
        if (size < 513) stack[size++] = value;
    }
    void moveTo(float dx, float dy) {
        if (open) path->close();
        x += dx;
        y += dy;
        path->moveTo(x, y);
        open = true;
    }
    void lineTo(float dx, float dy) {
        x += dx;
        y += dy;
        path->lineTo(x, y);
    }
    void curveTo(float dx1, float dy1, float dx2, float dy2, float dx3, float dy3) {
        const float c1x = x + dx1, c1y = y + dy1;
        const float c2x = c1x + dx2, c2y = c1y + dy2;
        x = c2x + dx3;
        y = c2y + dy3;
        path->cubicTo(c1x, c1y, c2x, c2y, x, y);
    }
    // The first operator that empties the stack may have one number more than it needs in
    // front: the glyph's width, which is taken from elsewhere. `wanted` is how many it needs
    // (-1: an even number).
    void dropWidth(int wanted) {
        if (widthSeen) return;
        widthSeen = true;
        const bool extra = wanted < 0 ? (size % 2) == 1 : size > wanted;
        if (!extra) return;
        for (int i = 1; i < size; i++) stack[i - 1] = stack[i];
        size--;
    }
};

bool CffFont::run(Run& s, size_t at, size_t end, int depth) const {
    if (depth > 10) return false;
    auto bias = [](const Index& index) { return index.count < 1240 ? 107 : index.count < 33900 ? 1131 : 32768; };

    while (at < end && !s.done) {
        if (++s.steps > 200000) return false;  // a program that never ends
        const uint8_t b = r_.u8(at++);

        // Numbers.
        if (b == 28) {
            s.push(static_cast<float>(r_.i16(at)));
            at += 2;
            continue;
        }
        if (b >= 32) {
            if (b <= 246) {
                s.push(static_cast<float>(static_cast<int>(b) - 139));
            } else if (b <= 250) {
                s.push(static_cast<float>((static_cast<int>(b) - 247) * 256 + r_.u8(at++) + 108));
            } else if (b <= 254) {
                s.push(static_cast<float>(-(static_cast<int>(b) - 251) * 256 - r_.u8(at++) - 108));
            } else {
                s.push(static_cast<float>(r_.i32(at)) / 65536.0f);
                at += 4;
            }
            continue;
        }

        float* a = s.stack;
        const int n = s.size;
        switch (b) {
            case 1:   // hstem
            case 3:   // vstem
            case 18:  // hstemhm
            case 23:  // vstemhm
                s.dropWidth(-1);
                s.stems += s.size / 2;
                s.size = 0;
                break;
            case 19:  // hintmask
            case 20:  // cntrmask
                s.dropWidth(-1);
                s.stems += s.size / 2;
                s.size = 0;
                at += static_cast<size_t>((s.stems + 7) / 8);  // one bit for each hint: not used
                break;

            case 21:  // rmoveto
                s.dropWidth(2);
                if (s.size >= 2) s.moveTo(s.stack[0], s.stack[1]);
                s.size = 0;
                break;
            case 22:  // hmoveto
                s.dropWidth(1);
                if (s.size >= 1) s.moveTo(s.stack[0], 0);
                s.size = 0;
                break;
            case 4:  // vmoveto
                s.dropWidth(1);
                if (s.size >= 1) s.moveTo(0, s.stack[0]);
                s.size = 0;
                break;

            case 5:  // rlineto
                for (int i = 0; i + 1 < n; i += 2) s.lineTo(a[i], a[i + 1]);
                s.size = 0;
                break;
            case 6:  // hlineto: lines by turns across and up
            case 7:  // vlineto: by turns up and across
                for (int i = 0; i < n; i++) {
                    const bool across = (b == 6) == (i % 2 == 0);
                    s.lineTo(across ? a[i] : 0, across ? 0 : a[i]);
                }
                s.size = 0;
                break;

            case 8:  // rrcurveto
                for (int i = 0; i + 5 < n; i += 6) s.curveTo(a[i], a[i + 1], a[i + 2], a[i + 3], a[i + 4], a[i + 5]);
                s.size = 0;
                break;
            case 24: {  // rcurveline: curves, then a line
                int i = 0;
                for (; i + 7 < n; i += 6) s.curveTo(a[i], a[i + 1], a[i + 2], a[i + 3], a[i + 4], a[i + 5]);
                if (i + 1 < n) s.lineTo(a[i], a[i + 1]);
                s.size = 0;
                break;
            }
            case 25: {  // rlinecurve: lines, then a curve
                int i = 0;
                for (; i + 7 < n; i += 2) s.lineTo(a[i], a[i + 1]);
                if (i + 5 < n) s.curveTo(a[i], a[i + 1], a[i + 2], a[i + 3], a[i + 4], a[i + 5]);
                s.size = 0;
                break;
            }
            case 26: {  // vvcurveto: curves that start and end going up
                int i = 0;
                float dx1 = 0;
                if (n % 4 == 1) dx1 = a[i++];
                for (; i + 3 < n; i += 4) {
                    s.curveTo(dx1, a[i], a[i + 1], a[i + 2], 0, a[i + 3]);
                    dx1 = 0;
                }
                s.size = 0;
                break;
            }
            case 27: {  // hhcurveto: curves that start and end going across
                int i = 0;
                float dy1 = 0;
                if (n % 4 == 1) dy1 = a[i++];
                for (; i + 3 < n; i += 4) {
                    s.curveTo(a[i], dy1, a[i + 1], a[i + 2], a[i + 3], 0);
                    dy1 = 0;
                }
                s.size = 0;
                break;
            }
            case 30:    // vhcurveto: curves that start up and end across, then the reverse, in turn
            case 31: {  // hvcurveto: the same starting across
                bool across = b == 31;
                int i = 0;
                while (i + 3 < n) {
                    const bool last = n - i == 5;  // the final curve may carry one number more
                    if (across) s.curveTo(a[i], 0, a[i + 1], a[i + 2], last ? a[i + 4] : 0, a[i + 3]);
                    else s.curveTo(0, a[i], a[i + 1], a[i + 2], a[i + 3], last ? a[i + 4] : 0);
                    across = !across;
                    i += last ? 5 : 4;
                }
                s.size = 0;
                break;
            }

            case 10:    // callsubr
            case 29: {  // callgsubr
                if (s.size < 1) return false;
                const Index& subrs = b == 10 ? *s.local : globalSubrs_;
                const int number = static_cast<int>(s.stack[--s.size]) + bias(subrs);
                size_t start, finish;
                if (number < 0 || !item(subrs, static_cast<size_t>(number), start, finish)) return false;
                if (!run(s, start, finish, depth + 1)) return false;
                break;
            }
            case 11:  // return
                return true;

            case 15:  // vsindex: which set of regions the blends that follow go by
                if (cff2_ && s.size >= 1) s.vsindex = static_cast<int>(s.stack[s.size - 1]);
                s.size = 0;
                break;
            case 16: {  // blend: numbers, each with how much it changes towards every region
                if (!cff2_ || s.size < 1) {
                    s.size = 0;
                    break;
                }
                if (s.scalarsFor != s.vsindex) {
                    s.scalars = variationStore_ != 0 && s.coords != nullptr ? regionScalars(r_, variationStore_, static_cast<uint32_t>(s.vsindex), *s.coords)
                                                                           : std::vector<float>();
                    s.scalarsFor = s.vsindex;
                }
                const int values = static_cast<int>(s.stack[--s.size]);
                const int regions = static_cast<int>(variationStore_ != 0 ? regionCount(r_, variationStore_, static_cast<uint32_t>(s.vsindex)) : 0);
                const int first = s.size - values * (regions + 1);
                if (values < 0 || first < 0) return false;
                for (int i = 0; i < values; i++) {
                    float value = s.stack[first + i];
                    const float* deltas = &s.stack[first + values + i * regions];
                    for (int k = 0; k < regions && static_cast<size_t>(k) < s.scalars.size(); k++) value += deltas[k] * s.scalars[static_cast<size_t>(k)];
                    s.stack[first + i] = value;
                }
                s.size = first + values;
                break;
            }
            case 14:  // endchar
                s.dropWidth(0);
                s.done = true;
                break;

            case 12: {
                const uint8_t op = r_.u8(at++);
                switch (op) {
                    case 34:  // hflex
                        if (n >= 7) {
                            s.curveTo(a[0], 0, a[1], a[2], a[3], 0);
                            s.curveTo(a[4], 0, a[5], -a[2], a[6], 0);
                        }
                        s.size = 0;
                        break;
                    case 35:  // flex
                        if (n >= 12) {
                            s.curveTo(a[0], a[1], a[2], a[3], a[4], a[5]);
                            s.curveTo(a[6], a[7], a[8], a[9], a[10], a[11]);
                        }
                        s.size = 0;
                        break;
                    case 36:  // hflex1
                        if (n >= 9) {
                            s.curveTo(a[0], a[1], a[2], a[3], a[4], 0);
                            s.curveTo(a[5], 0, a[6], a[7], a[8], -(a[1] + a[3] + a[7]));
                        }
                        s.size = 0;
                        break;
                    case 37:  // flex1
                        if (n >= 11) {
                            const float dx = a[0] + a[2] + a[4] + a[6] + a[8], dy = a[1] + a[3] + a[5] + a[7] + a[9];
                            s.curveTo(a[0], a[1], a[2], a[3], a[4], a[5]);
                            if (std::fabs(dx) > std::fabs(dy)) s.curveTo(a[6], a[7], a[8], a[9], a[10], -dy);
                            else s.curveTo(a[6], a[7], a[8], a[9], -dx, a[10]);
                        }
                        s.size = 0;
                        break;

                    // Arithmetic, which a few fonts' programs use.
                    case 9:   if (n >= 1) a[n - 1] = std::fabs(a[n - 1]); break;                       // abs
                    case 10:  if (n >= 2) { a[n - 2] += a[n - 1]; s.size--; } break;                    // add
                    case 11:  if (n >= 2) { a[n - 2] -= a[n - 1]; s.size--; } break;                    // sub
                    case 12:  if (n >= 2) { a[n - 2] = a[n - 1] != 0 ? a[n - 2] / a[n - 1] : 0; s.size--; } break;  // div
                    case 14:  if (n >= 1) a[n - 1] = -a[n - 1]; break;                                  // neg
                    case 18:  if (n >= 1) s.size--; break;                                              // drop
                    case 24:  if (n >= 2) { a[n - 2] *= a[n - 1]; s.size--; } break;                    // mul
                    case 26:  if (n >= 1) a[n - 1] = std::sqrt(std::max(a[n - 1], 0.0f)); break;        // sqrt
                    case 27:  if (n >= 1) s.push(a[n - 1]); break;                                      // dup
                    case 28:  if (n >= 2) std::swap(a[n - 1], a[n - 2]); break;                         // exch
                    case 29: {                                                                         // index
                        if (n >= 1) {
                            const int i = std::max(0, static_cast<int>(a[n - 1]));
                            a[n - 1] = n - 2 - i >= 0 ? a[n - 2 - i] : 0;
                        }
                        break;
                    }
                    default:
                        s.size = 0;  // an operator this does not know: what it was given is dropped
                        break;
                }
                break;
            }

            default:
                s.size = 0;
                break;
        }
    }
    return true;
}

bool CffFont::outline(int glyph, const Coords& coords, Path& path) const {
    size_t start, end;
    if (glyph < 0 || !item(charStrings_, static_cast<size_t>(glyph), start, end)) return false;
    size_t dict = static_cast<size_t>(fontDictOf(glyph));
    if (dict >= localSubrs_.size()) dict = 0;
    Run state;
    state.path = &path;
    state.local = &localSubrs_[dict];
    if (cff2_) {
        state.coords = &coords;
        state.vsindex = vsindexes_[dict];
        state.widthSeen = true;  // these programs carry no width
    }
    const size_t before = path.verbs.size();
    if (!run(state, start, end, 0)) {
        // A program that cannot be run draws nothing rather than half a glyph.
        path.verbs.resize(before);
        size_t points = 0;
        for (size_t i = 0; i < before; i++) points += path.verbs[i] == Path::Move || path.verbs[i] == Path::Line ? 2 : path.verbs[i] == Path::Quad ? 4 : path.verbs[i] == Path::Cubic ? 6 : 0;
        path.points.resize(points);
        return false;
    }
    if (state.open) path.close();
    return path.verbs.size() > before;
}

}  // namespace astra::text
