#pragma once

#include <cstdint>
#include <vector>

#include "render/draw_list.hpp"

namespace render {

// One rasterized frame: tightly packed RGB, 3 bytes per pixel, row-major,
// top-left origin.
struct Image {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> pixels;

  [[nodiscard]] bool operator==(const Image&) const = default;
};

// Software rasterizer (M7): paints a DrawList into an Image with src-over
// alpha blending, using a built-in 5x7 bitmap font for text. Pure and
// deterministic — the same list always yields the same bytes. Text is
// rendered with the built-in font (ASCII 32..126); out-of-range characters
// draw as a filled box. No GPU, no raylib, no file I/O.
[[nodiscard]] Image rasterize(const DrawList& list);

}  // namespace render
