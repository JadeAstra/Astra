// PNG pictures, read and written by code of our own.
//
// A picture here is `width` by `height` pixels of four bytes each: red, green, blue and
// alpha, top row first, the colours not multiplied by the alpha. That is what a PNG file
// holds, so nothing is lost going either way.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace astra::image {

// Writes a picture as the bytes of a PNG file, compressed.
std::vector<uint8_t> encodePng(const uint8_t* rgba, int width, int height);

// Reads the bytes of a PNG file: every kind the format has (grey, palette, colour, with or
// without alpha, 1 to 16 bits, interlaced or not). False, with `error`, if it is not one or
// is damaged.
bool decodePng(const uint8_t* data, size_t size, std::vector<uint8_t>& rgba, int& width, int& height, std::string& error);

// The compression PNG uses (zlib's "deflate"), for whatever else needs it.
std::vector<uint8_t> deflate(const uint8_t* data, size_t size);
bool inflate(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t limit, std::string& error);

uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc = 0);

}  // namespace astra::image
