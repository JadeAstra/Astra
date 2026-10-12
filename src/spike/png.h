// M0 spike: writes a picture as a PNG file, with no library. The data is stored, not compressed.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace astra::spike {

// `rgba` holds width * height pixels, four bytes each, top row first.
bool writePng(const std::string& path, int width, int height, const std::vector<uint8_t>& rgba);

}  // namespace astra::spike
