#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "render/raster.hpp"

namespace render {

// Encodes an Image as PNG bytes (via stb_image_write, deterministic for a
// given input: the same image always encodes to the same bytes).
[[nodiscard]] std::vector<std::uint8_t> encode_png(const Image& image);

// Encodes and writes to `path` (creating/truncating it). Returns false when
// the file cannot be opened or written.
[[nodiscard]] bool write_png(const Image& image, const std::string& path);

}  // namespace render
