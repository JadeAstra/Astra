// M0 spike: see png.h.

#include "png.h"

#include <algorithm>
#include <fstream>

namespace astra::spike {

namespace {

uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t n = 0; n < 256; n++) {
            uint32_t c = n;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < size; i++) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void put32(std::vector<uint8_t>& out, uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(v >> shift));
}

void chunk(std::ofstream& file, const char* type, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> out;
    put32(out, static_cast<uint32_t>(body.size()));
    const size_t typeAt = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), body.begin(), body.end());
    put32(out, crc32(out.data() + typeAt, out.size() - typeAt, 0));
    file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
}

}  // namespace

bool writePng(const std::string& path, int width, int height, const std::vector<uint8_t>& rgba) {
    if (width <= 0 || height <= 0 || rgba.size() < static_cast<size_t>(width) * height * 4) return false;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write("\x89PNG\r\n\x1a\n", 8);

    std::vector<uint8_t> header;
    put32(header, static_cast<uint32_t>(width));
    put32(header, static_cast<uint32_t>(height));
    header.insert(header.end(), {8, 6, 0, 0, 0});  // 8 bits a channel, RGBA, no interlace
    chunk(file, "IHDR", header);

    // Each row starts with a filter byte (0: none).
    std::vector<uint8_t> raw;
    raw.reserve((static_cast<size_t>(width) * 4 + 1) * height);
    for (int y = 0; y < height; y++) {
        raw.push_back(0);
        const uint8_t* row = rgba.data() + static_cast<size_t>(y) * width * 4;
        raw.insert(raw.end(), row, row + static_cast<size_t>(width) * 4);
    }

    // A zlib stream made of stored blocks of at most 65535 bytes, then its Adler-32 checksum.
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (size_t at = 0; at < raw.size();) {
        const size_t n = std::min<size_t>(65535, raw.size() - at);
        z.push_back(at + n == raw.size() ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n & 0xFF));
        z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n & 0xFF));
        z.push_back(static_cast<uint8_t>((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at + n));
        for (size_t i = at; i < at + n; i++) {
            a = (a + raw[i]) % 65521;
            b = (b + a) % 65521;
        }
        at += n;
    }
    put32(z, (b << 16) | a);
    chunk(file, "IDAT", z);
    chunk(file, "IEND", {});
    return static_cast<bool>(file);
}

}  // namespace astra::spike
