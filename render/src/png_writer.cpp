#include "render/png_writer.hpp"

#include <cstdio>
#include <vector>

// stb_image_write is header-only; the implementation lives in exactly one
// translation unit of the render library.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace render {
namespace {

// stb callback: append the encoded bytes to a std::vector.
void append_bytes(void* context, void* data, int size) {
  auto* out = static_cast<std::vector<std::uint8_t>*>(context);
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  out->insert(out->end(), bytes, bytes + size);
}

}  // namespace

std::vector<std::uint8_t> encode_png(const Image& image) {
  std::vector<std::uint8_t> bytes;
  if (image.width <= 0 || image.height <= 0 ||
      image.pixels.size() !=
          static_cast<std::size_t>(image.width) *
              static_cast<std::size_t>(image.height) * 3) {
    return bytes;
  }
  const int stride = image.width * 3;
  const int written = stbi_write_png_to_func(
      append_bytes, &bytes, image.width, image.height, 3,
      image.pixels.data(), stride);
  if (written == 0) {
    bytes.clear();
  }
  return bytes;
}

bool write_png(const Image& image, const std::string& path) {
  const std::vector<std::uint8_t> bytes = encode_png(image);
  if (bytes.empty()) {
    return false;
  }
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) ==
                  bytes.size();
  std::fclose(file);
  return ok;
}

}  // namespace render
