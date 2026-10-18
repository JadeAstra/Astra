// PNG pictures: see png.h.

#include "png.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

#include "../section_window.h"  // last: this file's own code goes in its section (D161)

namespace astra::image {

uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t n = 0; n < 256; n++) {
            uint32_t c = n;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (size_t i = 0; i < size; i++) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

namespace {

uint32_t adler32(const uint8_t* data, size_t size) {
    uint32_t a = 1, b = 0;
    while (size > 0) {
        const size_t run = std::min<size_t>(size, 5552);
        for (size_t i = 0; i < run; i++) {
            a += data[i];
            b += a;
        }
        a %= 65521;
        b %= 65521;
        data += run;
        size -= run;
    }
    return (b << 16) | a;
}

// ---- Compressing ----

// The lengths and distances of the format: each is a code, then some extra bits.
const int lengthBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const int lengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const int distBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                          193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const int distExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

struct BitWriter {
    std::vector<uint8_t>& out;
    uint32_t bits = 0;
    int count = 0;

    void put(uint32_t value, int length) {
        bits |= value << count;
        count += length;
        while (count >= 8) {
            out.push_back(static_cast<uint8_t>(bits));
            bits >>= 8;
            count -= 8;
        }
    }
    // A Huffman code goes in with its first bit first.
    void code(uint32_t value, int length) {
        uint32_t reversed = 0;
        for (int i = 0; i < length; i++) reversed |= ((value >> i) & 1) << (length - 1 - i);
        put(reversed, length);
    }
    void flush() {
        if (count > 0) out.push_back(static_cast<uint8_t>(bits));
        bits = 0;
        count = 0;
    }
};

// The format's ready-made table of codes, which costs nothing to send.
void fixedLiteral(BitWriter& w, int symbol) {
    if (symbol < 144) w.code(0x30 + static_cast<uint32_t>(symbol), 8);
    else if (symbol < 256) w.code(0x190 + static_cast<uint32_t>(symbol - 144), 9);
    else if (symbol < 280) w.code(static_cast<uint32_t>(symbol - 256), 7);
    else w.code(0xC0 + static_cast<uint32_t>(symbol - 280), 8);
}

}  // namespace

std::vector<uint8_t> deflate(const uint8_t* data, size_t size) {
    std::vector<uint8_t> out;
    out.reserve(size / 2 + 64);
    out.push_back(0x78);
    out.push_back(0x9C);
    BitWriter w{out};
    w.put(1, 1);  // the last block
    w.put(1, 2);  // with the ready-made codes

    // Repeats are found through a table of where each run of three bytes was last seen,
    // and a chain from each place to the one before with the same three.
    const int hashBits = 15, window = 32768, maxChain = 48;
    std::vector<int32_t> head(size_t{1} << hashBits, -1);
    std::vector<int32_t> previous(size, -1);
    auto hashAt = [&](size_t i) { return ((static_cast<uint32_t>(data[i]) << 16 | static_cast<uint32_t>(data[i + 1]) << 8 | data[i + 2]) * 2654435761u) >> (32 - hashBits); };
    auto insert = [&](size_t i) {
        if (i + 2 >= size) return;
        const uint32_t h = hashAt(i);
        previous[i] = head[h];
        head[h] = static_cast<int32_t>(i);
    };

    size_t i = 0;
    while (i < size) {
        int bestLength = 0, bestDistance = 0;
        if (i + 2 < size) {
            const int limit = static_cast<int>(std::min<size_t>(258, size - i));
            int chain = maxChain;
            for (int32_t at = head[hashAt(i)]; at >= 0 && chain-- > 0; at = previous[static_cast<size_t>(at)]) {
                const int distance = static_cast<int>(i - static_cast<size_t>(at));
                if (distance > window) break;
                if (data[static_cast<size_t>(at) + static_cast<size_t>(bestLength)] != data[i + static_cast<size_t>(bestLength)]) continue;
                int length = 0;
                while (length < limit && data[static_cast<size_t>(at) + static_cast<size_t>(length)] == data[i + static_cast<size_t>(length)]) length++;
                if (length > bestLength) {
                    bestLength = length;
                    bestDistance = distance;
                    if (length == limit) break;
                }
            }
        }
        if (bestLength >= 3) {
            int code = 28;
            while (lengthBase[code] > bestLength) code--;
            fixedLiteral(w, 257 + code);
            w.put(static_cast<uint32_t>(bestLength - lengthBase[code]), lengthExtra[code]);
            int dcode = 29;
            while (distBase[dcode] > bestDistance) dcode--;
            w.code(static_cast<uint32_t>(dcode), 5);
            w.put(static_cast<uint32_t>(bestDistance - distBase[dcode]), distExtra[dcode]);
            for (int k = 0; k < bestLength; k++) insert(i + static_cast<size_t>(k));
            i += static_cast<size_t>(bestLength);
        } else {
            fixedLiteral(w, data[i]);
            insert(i);
            i++;
        }
    }
    fixedLiteral(w, 256);
    w.flush();
    const uint32_t check = adler32(data, size);
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(check >> shift));
    return out;
}

// ---- Expanding ----

namespace {

struct BitReader {
    const uint8_t* data;
    size_t size, at = 0;
    uint32_t bits = 0;
    int count = 0;
    bool overrun = false;

    uint32_t take(int length) {
        while (count < length) {
            if (at < size) bits |= static_cast<uint32_t>(data[at++]) << count;
            else overrun = true;
            count += 8;
        }
        const uint32_t value = bits & ((1u << length) - 1);
        bits >>= length;
        count -= length;
        return length == 0 ? 0 : value;
    }
    void toByte() {
        bits = 0;
        count = 0;
    }
};

// A table of codes, by the lengths of the codes: how many codes there are of each length,
// and the symbols in the order of their codes.
struct Huffman {
    uint16_t counts[16] = {};
    uint16_t symbols[320] = {};

    bool build(const uint8_t* lengths, int n) {
        std::memset(counts, 0, sizeof counts);
        for (int i = 0; i < n; i++) counts[lengths[i]]++;
        counts[0] = 0;
        int left = 1;
        for (int length = 1; length < 16; length++) {
            left = left * 2 - counts[length];
            if (left < 0) return false;  // more codes than the lengths allow
        }
        uint16_t offsets[16] = {};
        for (int length = 1; length < 15; length++) offsets[length + 1] = static_cast<uint16_t>(offsets[length] + counts[length]);
        for (int i = 0; i < n; i++)
            if (lengths[i] != 0) symbols[offsets[lengths[i]]++] = static_cast<uint16_t>(i);
        return true;
    }

    int decode(BitReader& r) const {
        int code = 0, first = 0, index = 0;
        for (int length = 1; length < 16; length++) {
            code |= static_cast<int>(r.take(1));
            const int count = counts[length];
            if (code - count < first) return symbols[index + (code - first)];
            index += count;
            first = (first + count) * 2;
            code *= 2;
            if (r.overrun) return -1;
        }
        return -1;
    }
};

}  // namespace

bool inflate(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t limit, std::string& error) {
    auto fail = [&](const char* why) {
        error = why;
        return false;
    };
    if (size < 2 || (data[0] & 0x0F) != 8 || ((static_cast<unsigned>(data[0]) << 8) | data[1]) % 31 != 0 || (data[1] & 0x20) != 0)
        return fail("not compressed data");
    BitReader r{data + 2, size - 2};

    Huffman fixedLengths, fixedDistances;
    {
        uint8_t lengths[288];
        for (int i = 0; i < 288; i++) lengths[i] = static_cast<uint8_t>(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
        fixedLengths.build(lengths, 288);
        uint8_t distances[30];
        std::memset(distances, 5, sizeof distances);
        fixedDistances.build(distances, 30);
    }

    for (bool last = false; !last;) {
        last = r.take(1) != 0;
        const uint32_t kind = r.take(2);
        if (r.overrun) return fail("compressed data ends early");
        if (kind == 0) {
            r.toByte();
            if (r.at + 4 > r.size) return fail("compressed data ends early");
            const size_t length = r.data[r.at] | (static_cast<size_t>(r.data[r.at + 1]) << 8);
            const size_t check = r.data[r.at + 2] | (static_cast<size_t>(r.data[r.at + 3]) << 8);
            r.at += 4;
            if ((length ^ check) != 0xFFFF) return fail("damaged compressed data");
            if (r.at + length > r.size) return fail("compressed data ends early");
            if (out.size() + length > limit) return fail("compressed data is larger than it should be");
            out.insert(out.end(), r.data + r.at, r.data + r.at + length);
            r.at += length;
            continue;
        }
        if (kind == 3) return fail("damaged compressed data");

        Huffman ownLengths, ownDistances;
        const Huffman* lengthCodes = &fixedLengths;
        const Huffman* distanceCodes = &fixedDistances;
        if (kind == 2) {
            // The block brings its own tables, themselves written with a third, small one.
            const int literals = static_cast<int>(r.take(5)) + 257, distances = static_cast<int>(r.take(5)) + 1, codes = static_cast<int>(r.take(4)) + 4;
            if (literals > 286 || distances > 30) return fail("damaged compressed data");
            static const int order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t lengths[320] = {};
            for (int i = 0; i < codes; i++) lengths[order[i]] = static_cast<uint8_t>(r.take(3));
            Huffman codeLengths;
            if (!codeLengths.build(lengths, 19)) return fail("damaged compressed data");
            std::memset(lengths, 0, sizeof lengths);
            for (int i = 0; i < literals + distances;) {
                const int symbol = codeLengths.decode(r);
                if (symbol < 0) return fail("damaged compressed data");
                if (symbol < 16) {
                    lengths[i++] = static_cast<uint8_t>(symbol);
                    continue;
                }
                uint8_t repeated = 0;
                int times;
                if (symbol == 16) {
                    if (i == 0) return fail("damaged compressed data");
                    repeated = lengths[i - 1];
                    times = 3 + static_cast<int>(r.take(2));
                } else if (symbol == 17) {
                    times = 3 + static_cast<int>(r.take(3));
                } else {
                    times = 11 + static_cast<int>(r.take(7));
                }
                if (i + times > literals + distances) return fail("damaged compressed data");
                while (times-- > 0) lengths[i++] = repeated;
            }
            if (!ownLengths.build(lengths, literals) || !ownDistances.build(lengths + literals, distances)) return fail("damaged compressed data");
            lengthCodes = &ownLengths;
            distanceCodes = &ownDistances;
        }

        for (;;) {
            const int symbol = lengthCodes->decode(r);
            if (symbol < 0 || r.overrun) return fail("damaged compressed data");
            if (symbol < 256) {
                if (out.size() >= limit) return fail("compressed data is larger than it should be");
                out.push_back(static_cast<uint8_t>(symbol));
                continue;
            }
            if (symbol == 256) break;
            if (symbol > 285) return fail("damaged compressed data");
            const int length = lengthBase[symbol - 257] + static_cast<int>(r.take(lengthExtra[symbol - 257]));
            const int dcode = distanceCodes->decode(r);
            if (dcode < 0 || dcode > 29) return fail("damaged compressed data");
            const size_t distance = static_cast<size_t>(distBase[dcode]) + r.take(distExtra[dcode]);
            if (distance > out.size()) return fail("damaged compressed data");
            if (out.size() + static_cast<size_t>(length) > limit) return fail("compressed data is larger than it should be");
            size_t from = out.size() - distance;
            for (int k = 0; k < length; k++) out.push_back(out[from++]);
        }
    }
    return true;
}

// ---- PNG ----

namespace {

const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

void putU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(v >> shift));
}

void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& body) {
    putU32(out, static_cast<uint32_t>(body.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), body.begin(), body.end());
    putU32(out, crc32(out.data() + start, out.size() - start));
}

uint32_t getU32(const uint8_t* p) { return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3]; }

int paeth(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

}  // namespace

std::vector<uint8_t> encodePng(const uint8_t* rgba, int width, int height) {
    std::vector<uint8_t> out(signature, signature + 8);
    std::vector<uint8_t> header;
    putU32(header, static_cast<uint32_t>(width));
    putU32(header, static_cast<uint32_t>(height));
    // A picture with nothing see-through is written without its alpha.
    const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    bool opaque = true;
    for (size_t i = 0; i < pixels && opaque; i++) opaque = rgba[i * 4 + 3] == 255;
    const size_t bpp = opaque ? 3 : 4;
    header.insert(header.end(), {8, static_cast<uint8_t>(opaque ? 2 : 6), 0, 0, 0});
    chunk(out, "IHDR", header);

    // Each row is written as its difference from something near it, whichever of the
    // format's five ways leaves the smallest numbers: that is what makes it compress.
    const size_t rowBytes = static_cast<size_t>(width) * bpp;
    std::vector<uint8_t> raw;
    raw.reserve((rowBytes + 1) * static_cast<size_t>(height));
    std::vector<uint8_t> current(rowBytes), above(rowBytes, 0), tried(rowBytes), best(rowBytes);
    for (int y = 0; y < height; y++) {
        const uint8_t* row = rgba + static_cast<size_t>(y) * static_cast<size_t>(width) * 4;
        for (size_t x = 0; x < static_cast<size_t>(width); x++)
            for (size_t k = 0; k < bpp; k++) current[x * bpp + k] = row[x * 4 + k];
        int bestFilter = 0;
        long bestSum = -1;
        for (int filter = 0; filter < 5; filter++) {
            long sum = 0;
            for (size_t i = 0; i < rowBytes; i++) {
                const int left = i >= bpp ? current[i - bpp] : 0, up = above[i], upLeft = i >= bpp ? above[i - bpp] : 0;
                int predicted = 0;
                if (filter == 1) predicted = left;
                else if (filter == 2) predicted = up;
                else if (filter == 3) predicted = (left + up) / 2;
                else if (filter == 4) predicted = paeth(left, up, upLeft);
                tried[i] = static_cast<uint8_t>(current[i] - predicted);
                sum += std::abs(static_cast<int8_t>(tried[i]));
            }
            if (bestSum < 0 || sum < bestSum) {
                bestSum = sum;
                bestFilter = filter;
                best.swap(tried);
            }
        }
        raw.push_back(static_cast<uint8_t>(bestFilter));
        raw.insert(raw.end(), best.begin(), best.end());
        above = current;
    }
    chunk(out, "IDAT", deflate(raw.data(), raw.size()));
    chunk(out, "IEND", {});
    return out;
}

bool decodePng(const uint8_t* data, size_t size, std::vector<uint8_t>& rgba, int& width, int& height, std::string& error) {
    auto fail = [&](const char* why) {
        error = why;
        return false;
    };
    if (size < 8 || std::memcmp(data, signature, 8) != 0) return fail("not a PNG picture");

    uint32_t w = 0, h = 0;
    int depth = 0, colorType = -1, interlace = 0;
    std::vector<uint8_t> palette, paletteAlpha, compressed;
    int clearGrey = -1, clearRed = -1, clearGreen = -1, clearBlue = -1;  // the one colour that is see-through, if any
    for (size_t at = 8; at + 12 <= size;) {
        const uint32_t length = getU32(data + at);
        if (length > size - at - 12) return fail("damaged PNG picture");
        const uint8_t* body = data + at + 8;
        const std::string type(reinterpret_cast<const char*>(data + at + 4), 4);
        if (type == "IHDR" && length >= 13) {
            w = getU32(body);
            h = getU32(body + 4);
            depth = body[8];
            colorType = body[9];
            interlace = body[12];
            if (body[10] != 0 || body[11] != 0 || interlace > 1) return fail("a PNG picture of a kind that is not read");
        } else if (type == "PLTE") {
            palette.assign(body, body + length);
        } else if (type == "tRNS") {
            if (colorType == 3) paletteAlpha.assign(body, body + length);
            else if (colorType == 0 && length >= 2) clearGrey = (body[0] << 8) | body[1];
            else if (colorType == 2 && length >= 6) {
                clearRed = (body[0] << 8) | body[1];
                clearGreen = (body[2] << 8) | body[3];
                clearBlue = (body[4] << 8) | body[5];
            }
        } else if (type == "IDAT") {
            compressed.insert(compressed.end(), body, body + length);
        } else if (type == "IEND") {
            break;
        }
        at += static_cast<size_t>(length) + 12;
    }

    int channels = 0;
    switch (colorType) {
        case 0: channels = 1; break;
        case 2: channels = 3; break;
        case 3: channels = 1; break;
        case 4: channels = 2; break;
        case 6: channels = 4; break;
        default: return fail("damaged PNG picture");
    }
    const bool depthFits = depth == 8 || (depth == 16 && colorType != 3) || ((depth == 1 || depth == 2 || depth == 4) && (colorType == 0 || colorType == 3));
    if (w == 0 || h == 0 || w > 32768 || h > 32768 || !depthFits) return fail("damaged PNG picture");
    if (static_cast<uint64_t>(w) * h > (uint64_t{1} << 28)) return fail("the PNG picture is too large");
    if (colorType == 3 && palette.size() < 3) return fail("damaged PNG picture");

    const size_t bitsPerPixel = static_cast<size_t>(channels) * static_cast<size_t>(depth);
    const size_t bpp = std::max<size_t>(1, bitsPerPixel / 8);  // what "the pixel to the left" means for the filters
    auto rowBytesOf = [&](uint32_t pixels) { return (static_cast<size_t>(pixels) * bitsPerPixel + 7) / 8; };

    // An interlaced picture comes as seven smaller pictures, coarse to fine.
    static const int startX[7] = {0, 4, 0, 2, 0, 1, 0}, startY[7] = {0, 0, 4, 0, 2, 0, 1}, stepX[7] = {8, 8, 4, 4, 2, 2, 1}, stepY[7] = {8, 8, 8, 4, 4, 2, 2};
    struct Pass {
        uint32_t w, h;
        int x0, y0, dx, dy;
    };
    std::vector<Pass> passes;
    size_t expected = 0;
    if (interlace == 0) {
        passes.push_back({w, h, 0, 0, 1, 1});
    } else {
        for (int p = 0; p < 7; p++) {
            const uint32_t pw = (w + static_cast<uint32_t>(stepX[p] - startX[p] - 1)) / static_cast<uint32_t>(stepX[p]);
            const uint32_t ph = (h + static_cast<uint32_t>(stepY[p] - startY[p] - 1)) / static_cast<uint32_t>(stepY[p]);
            if (pw == 0 || ph == 0) continue;
            passes.push_back({pw, ph, startX[p], startY[p], stepX[p], stepY[p]});
        }
    }
    for (const Pass& pass : passes) expected += (rowBytesOf(pass.w) + 1) * pass.h;

    std::vector<uint8_t> raw;
    raw.reserve(expected);
    if (!inflate(compressed.data(), compressed.size(), raw, expected, error)) return false;
    if (raw.size() < expected) return fail("the PNG picture ends early");

    width = static_cast<int>(w);
    height = static_cast<int>(h);
    rgba.assign(static_cast<size_t>(w) * h * 4, 0);

    size_t at = 0;
    std::vector<uint8_t> above;
    for (const Pass& pass : passes) {
        const size_t rowBytes = rowBytesOf(pass.w);
        above.assign(rowBytes, 0);
        for (uint32_t row = 0; row < pass.h; row++) {
            const int filter = raw[at++];
            uint8_t* line = &raw[at];
            at += rowBytes;
            for (size_t i = 0; i < rowBytes; i++) {
                const int left = i >= bpp ? line[i - bpp] : 0, up = above[i], upLeft = i >= bpp ? above[i - bpp] : 0;
                int predicted = 0;
                switch (filter) {
                    case 0: break;
                    case 1: predicted = left; break;
                    case 2: predicted = up; break;
                    case 3: predicted = (left + up) / 2; break;
                    case 4: predicted = paeth(left, up, upLeft); break;
                    default: return fail("damaged PNG picture");
                }
                line[i] = static_cast<uint8_t>(line[i] + predicted);
            }
            above.assign(line, line + rowBytes);

            // One value of a pixel, as it is stored: `depth` bits.
            auto sample = [&](size_t index) -> int {
                if (depth == 8) return line[index];
                if (depth == 16) return (line[index * 2] << 8) | line[index * 2 + 1];
                const size_t bit = index * static_cast<size_t>(depth);
                return (line[bit / 8] >> (8 - depth - static_cast<int>(bit % 8))) & ((1 << depth) - 1);
            };
            // The same as a byte: 0 to 255 whatever the depth.
            auto byteOf = [&](int value) -> uint8_t {
                if (depth == 8) return static_cast<uint8_t>(value);
                if (depth == 16) return static_cast<uint8_t>((static_cast<uint32_t>(value) * 255 + 32767) / 65535);
                return static_cast<uint8_t>(value * 255 / ((1 << depth) - 1));
            };
            const size_t y = static_cast<size_t>(pass.y0) + static_cast<size_t>(row) * static_cast<size_t>(pass.dy);
            for (uint32_t col = 0; col < pass.w; col++) {
                const size_t x = static_cast<size_t>(pass.x0) + static_cast<size_t>(col) * static_cast<size_t>(pass.dx);
                uint8_t* out = &rgba[(y * w + x) * 4];
                const size_t first = static_cast<size_t>(col) * static_cast<size_t>(channels);
                switch (colorType) {
                    case 0: {
                        const int grey = sample(first);
                        out[0] = out[1] = out[2] = byteOf(grey);
                        out[3] = grey == clearGrey ? 0 : 255;
                        break;
                    }
                    case 2: {
                        const int red = sample(first), green = sample(first + 1), blue = sample(first + 2);
                        out[0] = byteOf(red);
                        out[1] = byteOf(green);
                        out[2] = byteOf(blue);
                        out[3] = red == clearRed && green == clearGreen && blue == clearBlue ? 0 : 255;
                        break;
                    }
                    case 3: {
                        const size_t entry = static_cast<size_t>(sample(first));
                        if (entry * 3 + 2 < palette.size()) {
                            out[0] = palette[entry * 3];
                            out[1] = palette[entry * 3 + 1];
                            out[2] = palette[entry * 3 + 2];
                        }
                        out[3] = entry < paletteAlpha.size() ? paletteAlpha[entry] : 255;
                        break;
                    }
                    case 4:
                        out[0] = out[1] = out[2] = byteOf(sample(first));
                        out[3] = byteOf(sample(first + 1));
                        break;
                    default:
                        out[0] = byteOf(sample(first));
                        out[1] = byteOf(sample(first + 1));
                        out[2] = byteOf(sample(first + 2));
                        out[3] = byteOf(sample(first + 3));
                        break;
                }
            }
        }
    }
    return true;
}

}  // namespace astra::image
