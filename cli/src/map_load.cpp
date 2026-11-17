#include "cli/map_load.hpp"

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// stb_image is header-only; the implementation lives in exactly one
// translation unit of the CLI (the same pinned stb FetchContent that
// provides stb_image_write for render/).
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace cli {
namespace {

std::string read_file_bytes(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    throw std::runtime_error("cannot open map file '" + path + "'");
  }
  std::string text;
  char buffer[4096];
  std::size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    text.append(buffer, read);
  }
  std::fclose(file);
  return text;
}

sim::TerrainGrid load_png(const std::string& path, double cell_size,
                          const sim::TerrainTypeTable& table) {
  // m13 reliability review: pre-check the IHDR before stbi_load. A
  // compact PNG can claim huge dimensions, and stbi would allocate the
  // full decode (width * height * 3 bytes — gigabytes from a
  // kilobyte-sized file) before build_terrain_grid's 4096x4096 cap
  // ever ran. The header is read directly: 8-byte signature, then the
  // first chunk (length, "IHDR", big-endian width, height).
  {
    std::FILE* probe = std::fopen(path.c_str(), "rb");
    if (probe == nullptr) {
      throw std::runtime_error("cannot open map file '" + path + "'");
    }
    unsigned char header[24] = {};
    const std::size_t got = std::fread(header, 1, sizeof(header), probe);
    std::fclose(probe);
    const unsigned char signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n',
                                        0x1A, '\n'};
    bool oversized = false;
    if (got >= 24 && std::equal(signature, signature + 8, header) &&
        header[12] == 'I' && header[13] == 'H' && header[14] == 'D' &&
        header[15] == 'R') {
      const unsigned long png_width = (static_cast<unsigned long>(header[16]) << 24) |
                                      (static_cast<unsigned long>(header[17]) << 16) |
                                      (static_cast<unsigned long>(header[18]) << 8) |
                                      static_cast<unsigned long>(header[19]);
      const unsigned long png_height = (static_cast<unsigned long>(header[20]) << 24) |
                                       (static_cast<unsigned long>(header[21]) << 16) |
                                       (static_cast<unsigned long>(header[22]) << 8) |
                                       static_cast<unsigned long>(header[23]);
      oversized = png_width == 0 || png_height == 0 || png_width > 4096 ||
                  png_height > 4096;
    }
    if (oversized) {
      throw std::runtime_error(
          "map '" + path +
          "': PNG dimensions exceed the 4096x4096 limit (refused before "
          "decoding)");
    }
  }
  int width = 0;
  int height = 0;
  int channels = 0;
  // stbi_load fills the RGB of every pixel; the alpha channel is ignored
  // (maps are opaque by construction).
  unsigned char* pixels =
      stbi_load(path.c_str(), &width, &height, &channels, 3);
  if (pixels == nullptr) {
    throw std::runtime_error("cannot decode PNG map '" + path + "': " +
                             std::string(stbi_failure_reason()));
  }
  std::vector<std::uint16_t> cells(
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::size_t pixel =
          (static_cast<std::size_t>(y) * width + x) * 3;
      const std::uint8_t r = pixels[pixel];
      const std::uint8_t g = pixels[pixel + 1];
      const std::uint8_t b = pixels[pixel + 2];
      std::uint16_t type = sim::TerrainGrid::kNoCell;
      for (std::size_t t = 0; t < table.types.size(); ++t) {
        if (table.types[t].r == r && table.types[t].g == g &&
            table.types[t].b == b) {
          type = static_cast<std::uint16_t>(t);
          break;
        }
      }
      if (type == sim::TerrainGrid::kNoCell) {
        std::ostringstream message;
        message << "map '" << path << "': pixel (" << x << ", " << y
                << ") has color #" << std::hex << static_cast<int>(r)
                << static_cast<int>(g) << static_cast<int>(b)
                << std::dec
                << " which matches no terrain type (colors must be exact)";
        stbi_image_free(pixels);
        throw std::runtime_error(message.str());
      }
      cells[static_cast<std::size_t>(y) * width + x] = type;
    }
  }
  stbi_image_free(pixels);
  const std::string id = path.substr(path.find_last_of('/') + 1);
  try {
    return sim::build_terrain_grid(id, static_cast<std::uint32_t>(width),
                                   static_cast<std::uint32_t>(height),
                                   cell_size, cells, &table);
  } catch (const std::invalid_argument& error) {
    throw std::runtime_error(std::string("map '") + path +
                             "': " + error.what());
  }
}

}  // namespace

sim::TerrainGrid load_map(const std::string& path, double cell_size,
                          const sim::TerrainTypeTable& table) {
  if (!(cell_size > 0.0)) {
    throw std::runtime_error("map '" + path + "': cell_size must be > 0");
  }
  if (path.size() >= 5 && path.compare(path.size() - 5, 5, ".json") == 0) {
    try {
      return sim::parse_terrain_grid_json(read_file_bytes(path), cell_size,
                                          &table);
    } catch (const std::invalid_argument& error) {
      throw std::runtime_error(std::string("map '") + path +
                               "': " + error.what());
    }
  }
  return load_png(path, cell_size, table);
}

}  // namespace cli
