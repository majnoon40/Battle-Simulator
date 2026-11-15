#include "render/raster.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace render {
namespace {

// ---------------------------------------------------------------------------
// Built-in 5x7 bitmap font (the classic public-domain GLCD column format:
// five bytes per glyph, each byte one column, bit 0 = top row). Covers the
// printable ASCII range 32..126. Embedded so the headless renderer never
// depends on system fonts, and so output stays byte-deterministic.
// ---------------------------------------------------------------------------
const std::uint8_t kFont[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00},  // ' '
    {0x00, 0x00, 0x5F, 0x00, 0x00},  // '!'
    {0x00, 0x07, 0x00, 0x07, 0x00},  // '"'
    {0x14, 0x7F, 0x14, 0x7F, 0x14},  // '#'
    {0x24, 0x2A, 0x7F, 0x2A, 0x12},  // '$'
    {0x23, 0x13, 0x08, 0x64, 0x62},  // '%'
    {0x36, 0x49, 0x55, 0x22, 0x50},  // '&'
    {0x00, 0x05, 0x03, 0x00, 0x00},  // '\''
    {0x00, 0x1C, 0x22, 0x41, 0x00},  // '('
    {0x00, 0x41, 0x22, 0x1C, 0x00},  // ')'
    {0x14, 0x08, 0x3E, 0x08, 0x14},  // '*'
    {0x08, 0x08, 0x3E, 0x08, 0x08},  // '+'
    {0x00, 0x50, 0x30, 0x00, 0x00},  // ','
    {0x08, 0x08, 0x08, 0x08, 0x08},  // '-'
    {0x00, 0x60, 0x60, 0x00, 0x00},  // '.'
    {0x20, 0x10, 0x08, 0x04, 0x02},  // '/'
    {0x3E, 0x51, 0x49, 0x45, 0x3E},  // '0'
    {0x00, 0x42, 0x7F, 0x40, 0x00},  // '1'
    {0x42, 0x61, 0x51, 0x49, 0x46},  // '2'
    {0x21, 0x41, 0x45, 0x4B, 0x31},  // '3'
    {0x18, 0x14, 0x12, 0x7F, 0x10},  // '4'
    {0x27, 0x45, 0x45, 0x45, 0x39},  // '5'
    {0x3C, 0x4A, 0x49, 0x49, 0x30},  // '6'
    {0x01, 0x71, 0x09, 0x05, 0x03},  // '7'
    {0x36, 0x49, 0x49, 0x49, 0x36},  // '8'
    {0x06, 0x49, 0x49, 0x29, 0x1E},  // '9'
    {0x00, 0x36, 0x36, 0x00, 0x00},  // ':'
    {0x00, 0x56, 0x36, 0x00, 0x00},  // ';'
    {0x08, 0x14, 0x22, 0x41, 0x00},  // '<'
    {0x14, 0x14, 0x14, 0x14, 0x14},  // '='
    {0x00, 0x41, 0x22, 0x14, 0x08},  // '>'
    {0x02, 0x01, 0x51, 0x09, 0x06},  // '?'
    {0x32, 0x49, 0x79, 0x41, 0x3E},  // '@'
    {0x7E, 0x11, 0x11, 0x11, 0x7E},  // 'A'
    {0x7F, 0x49, 0x49, 0x49, 0x36},  // 'B'
    {0x3E, 0x41, 0x41, 0x41, 0x22},  // 'C'
    {0x7F, 0x41, 0x41, 0x22, 0x1C},  // 'D'
    {0x7F, 0x49, 0x49, 0x49, 0x41},  // 'E'
    {0x7F, 0x09, 0x09, 0x09, 0x01},  // 'F'
    {0x3E, 0x41, 0x49, 0x49, 0x3A},  // 'G'
    {0x7F, 0x08, 0x08, 0x08, 0x7F},  // 'H'
    {0x00, 0x41, 0x7F, 0x41, 0x00},  // 'I'
    {0x20, 0x40, 0x41, 0x3F, 0x01},  // 'J'
    {0x7F, 0x08, 0x14, 0x22, 0x41},  // 'K'
    {0x7F, 0x40, 0x40, 0x40, 0x40},  // 'L'
    {0x7F, 0x02, 0x0C, 0x02, 0x7F},  // 'M'
    {0x7F, 0x04, 0x08, 0x10, 0x7F},  // 'N'
    {0x3E, 0x41, 0x41, 0x41, 0x3E},  // 'O'
    {0x7F, 0x09, 0x09, 0x09, 0x06},  // 'P'
    {0x3E, 0x41, 0x51, 0x21, 0x5E},  // 'Q'
    {0x7F, 0x09, 0x19, 0x29, 0x46},  // 'R'
    {0x46, 0x49, 0x49, 0x49, 0x31},  // 'S'
    {0x01, 0x01, 0x7F, 0x01, 0x01},  // 'T'
    {0x3F, 0x40, 0x40, 0x40, 0x3F},  // 'U'
    {0x1F, 0x20, 0x40, 0x20, 0x1F},  // 'V'
    {0x7F, 0x20, 0x18, 0x20, 0x7F},  // 'W'
    {0x63, 0x14, 0x08, 0x14, 0x63},  // 'X'
    {0x03, 0x04, 0x78, 0x04, 0x03},  // 'Y'
    {0x61, 0x51, 0x49, 0x45, 0x43},  // 'Z'
    {0x00, 0x7F, 0x41, 0x41, 0x00},  // '['
    {0x02, 0x04, 0x08, 0x10, 0x20},  // '\\'
    {0x00, 0x41, 0x41, 0x7F, 0x00},  // ']'
    {0x04, 0x02, 0x01, 0x02, 0x04},  // '^'
    {0x40, 0x40, 0x40, 0x40, 0x40},  // '_'
    {0x00, 0x01, 0x02, 0x04, 0x00},  // '`'
    {0x20, 0x54, 0x54, 0x54, 0x78},  // 'a'
    {0x7F, 0x48, 0x44, 0x44, 0x38},  // 'b'
    {0x38, 0x44, 0x44, 0x44, 0x20},  // 'c'
    {0x38, 0x44, 0x44, 0x48, 0x7F},  // 'd'
    {0x38, 0x54, 0x54, 0x54, 0x18},  // 'e'
    {0x08, 0x7E, 0x09, 0x01, 0x02},  // 'f'
    {0x0C, 0x52, 0x52, 0x52, 0x3E},  // 'g'
    {0x7F, 0x08, 0x04, 0x04, 0x78},  // 'h'
    {0x00, 0x44, 0x7D, 0x40, 0x00},  // 'i'
    {0x20, 0x40, 0x44, 0x3D, 0x00},  // 'j'
    {0x7F, 0x10, 0x28, 0x44, 0x00},  // 'k'
    {0x00, 0x41, 0x7F, 0x40, 0x00},  // 'l'
    {0x7C, 0x04, 0x18, 0x04, 0x78},  // 'm'
    {0x7C, 0x08, 0x04, 0x04, 0x78},  // 'n'
    {0x38, 0x44, 0x44, 0x44, 0x38},  // 'o'
    {0x7C, 0x14, 0x14, 0x14, 0x08},  // 'p'
    {0x08, 0x14, 0x14, 0x18, 0x7C},  // 'q'
    {0x7C, 0x08, 0x04, 0x04, 0x08},  // 'r'
    {0x48, 0x54, 0x54, 0x54, 0x20},  // 's'
    {0x04, 0x3F, 0x44, 0x40, 0x20},  // 't'
    {0x3C, 0x40, 0x40, 0x20, 0x7C},  // 'u'
    {0x1C, 0x20, 0x40, 0x20, 0x1C},  // 'v'
    {0x3C, 0x40, 0x30, 0x40, 0x3C},  // 'w'
    {0x44, 0x28, 0x10, 0x28, 0x44},  // 'x'
    {0x0C, 0x50, 0x50, 0x50, 0x3C},  // 'y'
    {0x44, 0x64, 0x54, 0x4C, 0x44},  // 'z'
    {0x00, 0x08, 0x36, 0x41, 0x00},  // '{'
    {0x00, 0x00, 0x7F, 0x00, 0x00},  // '|'
    {0x00, 0x41, 0x36, 0x08, 0x00},  // '}'
    {0x08, 0x08, 0x2A, 0x1C, 0x08},  // '~'
};

struct Canvas {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> pixels;  // RGB, row-major

  void fill(const game::Color& color) {
    const std::size_t size =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3;
    pixels.resize(size);
    for (std::size_t i = 0; i < size; i += 3) {
      pixels[i] = color.r;
      pixels[i + 1] = color.g;
      pixels[i + 2] = color.b;
    }
  }

  // Source-over blend of one pixel (no-op off canvas).
  void blend(int x, int y, const game::Color& color) {
    if (x < 0 || y < 0 || x >= width || y >= height) {
      return;
    }
    const std::size_t index =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
         static_cast<std::size_t>(x)) *
        3;
    if (color.a == 255) {
      pixels[index] = color.r;
      pixels[index + 1] = color.g;
      pixels[index + 2] = color.b;
      return;
    }
    if (color.a == 0) {
      return;
    }
    const int alpha = color.a;
    const auto mix = [alpha](std::uint8_t dst, std::uint8_t src) {
      return static_cast<std::uint8_t>(
          (static_cast<int>(src) * alpha +
           static_cast<int>(dst) * (255 - alpha) + 127) /
          255);
    };
    pixels[index] = mix(pixels[index], color.r);
    pixels[index + 1] = mix(pixels[index + 1], color.g);
    pixels[index + 2] = mix(pixels[index + 2], color.b);
  }

  // Filled circle (radius may be fractional; radius < 0.5 is one pixel).
  void fill_circle(double cx, double cy, double radius,
                   const game::Color& color) {
    if (radius < 0.5) {
      blend(static_cast<int>(cx), static_cast<int>(cy), color);
      return;
    }
    const int r = static_cast<int>(radius + 0.5);
    const int r_sq = static_cast<int>((radius + 0.5) * (radius + 0.5));
    for (int dy = -r; dy <= r; ++dy) {
      for (int dx = -r; dx <= r; ++dx) {
        if (dx * dx + dy * dy <= r_sq) {
          blend(static_cast<int>(cx) + dx, static_cast<int>(cy) + dy, color);
        }
      }
    }
  }

  // 1px circle outline.
  void outline_circle(double cx, double cy, double radius,
                      const game::Color& color) {
    const double r = std::max(0.5, radius);
    const int lo = static_cast<int>(std::floor(cx - r)) - 1;
    const int hi = static_cast<int>(std::ceil(cx + r)) + 1;
    const int vlo = static_cast<int>(std::floor(cy - r)) - 1;
    const int vhi = static_cast<int>(std::ceil(cy + r)) + 1;
    for (int y = vlo; y <= vhi; ++y) {
      for (int x = lo; x <= hi; ++x) {
        const double dx = static_cast<double>(x) - cx;
        const double dy = static_cast<double>(y) - cy;
        if (std::abs(std::sqrt(dx * dx + dy * dy) - r) <= 0.71) {
          blend(x, y, color);
        }
      }
    }
  }

  // Filled triangle by scanlines (edges intersected per row).
  void fill_triangle(double x1, double y1, double x2, double y2, double x3,
                     double y3, const game::Color& color) {
    const double min_y = std::min({y1, y2, y3});
    const double max_y = std::max({y1, y2, y3});
    int y_start = static_cast<int>(std::ceil(min_y));
    int y_end = static_cast<int>(std::floor(max_y));
    for (int y = y_start; y <= y_end; ++y) {
      // Intersect the scanline with all three edges.
      double crossings[3];
      int count = 0;
      const double py = static_cast<double>(y) + 0.5;
      const auto edge = [&](double ax, double ay, double bx, double by) {
        if ((ay <= py && by > py) || (by <= py && ay > py)) {
          crossings[count++] = ax + (py - ay) * (bx - ax) / (by - ay);
        }
      };
      edge(x1, y1, x2, y2);
      edge(x2, y2, x3, y3);
      edge(x3, y3, x1, y1);
      if (count < 2) {
        continue;
      }
      const double left = std::min(crossings[0], crossings[1]);
      const double right = std::max(crossings[0], crossings[1]);
      for (int x = static_cast<int>(std::ceil(left - 0.5));
           x <= static_cast<int>(std::floor(right - 0.5)); ++x) {
        blend(x, y, color);
      }
    }
  }

  // Line with a thickness: a round brush walked along the segment. Long
  // off-screen lines are cropped by blend() itself, and the step count is
  // bounded by the segment length, so this stays fast for the grid and the
  // squad links.
  void thick_line(double x1, double y1, double x2, double y2,
                  double thickness, const game::Color& color) {
    const double brush = std::max(0.5, thickness / 2.0);
    const double dx = x2 - x1;
    const double dy = y2 - y1;
    const double length = std::sqrt(dx * dx + dy * dy);
    const double step = std::max(0.25, brush * 0.5);
    for (double t = 0.0; t <= length; t += step) {
      const double px = x1 + (length > 0.0 ? dx * t / length : 0.0);
      const double py = y1 + (length > 0.0 ? dy * t / length : 0.0);
      // Only draw when the brush can touch the canvas.
      if (px < -brush - 1.0 || py < -brush - 1.0 ||
          px > width + brush + 1.0 || py > height + brush + 1.0) {
        continue;
      }
      fill_circle(px, py, brush, color);
    }
  }

  void fill_rect(int x, int y, int w, int h, const game::Color& color) {
    for (int row = 0; row < h; ++row) {
      for (int column = 0; column < w; ++column) {
        blend(x + column, y + row, color);
      }
    }
  }

  void draw_text(double x, double y, int font_px, const std::string& content,
                 const game::Color& color) {
    if (font_px < 7) {
      font_px = 7;
    }
    const int scale = std::max(1, (font_px + 3) / 7);
    int cursor = static_cast<int>(x);
    const int top = static_cast<int>(y);
    for (char character : content) {
      const unsigned char code = static_cast<unsigned char>(character);
      if (code < 32 || code > 126) {
        cursor += 6 * scale;  // unsupported: blank advance
        continue;
      }
      const std::uint8_t* glyph = kFont[code - 32];
      for (int column = 0; column < 5; ++column) {
        const std::uint8_t bits = glyph[column];
        for (int row = 0; row < 7; ++row) {
          if (bits & (1u << row)) {
            fill_rect(cursor + column * scale, top + row * scale, scale,
                      scale, color);
          }
        }
      }
      cursor += 6 * scale;
    }
  }
};

}  // namespace

Image rasterize(const DrawList& list) {
  Canvas canvas;
  canvas.width = list.width;
  canvas.height = list.height;
  if (list.width <= 0 || list.height <= 0) {
    return Image{list.width, list.height, {}};
  }
  canvas.fill(list.background);
  for (const DrawCmd& cmd : list.cmds) {
    switch (cmd.kind) {
      case CmdKind::Circle: {
        const bool outline_only = cmd.e > 0.5;
        if (!outline_only) {
          canvas.fill_circle(cmd.a, cmd.b, cmd.c, cmd.color);
        }
        if (cmd.d > 0.5 || outline_only) {
          canvas.outline_circle(cmd.a, cmd.b, cmd.c,
                                outline_only ? cmd.color2 : cmd.color2);
        }
        break;
      }
      case CmdKind::Triangle:
        canvas.fill_triangle(cmd.a, cmd.b, cmd.c, cmd.d, cmd.e, cmd.f,
                             cmd.color);
        break;
      case CmdKind::Line:
        canvas.thick_line(cmd.a, cmd.b, cmd.c, cmd.d, cmd.e, cmd.color);
        break;
      case CmdKind::Rect: {
        const int x = static_cast<int>(cmd.a);
        const int y = static_cast<int>(cmd.b);
        const int w = static_cast<int>(cmd.c);
        const int h = static_cast<int>(cmd.d);
        if (cmd.e > 0.5) {
          canvas.fill_rect(x, y, w, h, cmd.color);
        } else {
          canvas.fill_rect(x, y, w, 1, cmd.color);                    // top
          canvas.fill_rect(x, y + h - 1, w, 1, cmd.color);            // bottom
          canvas.fill_rect(x, y, 1, h, cmd.color);                    // left
          canvas.fill_rect(x + w - 1, y, 1, h, cmd.color);            // right
        }
        break;
      }
      case CmdKind::Text:
        canvas.draw_text(cmd.a, cmd.b, static_cast<int>(cmd.c), cmd.text,
                         cmd.color);
        break;
    }
  }
  Image image;
  image.width = canvas.width;
  image.height = canvas.height;
  image.pixels = std::move(canvas.pixels);
  return image;
}

}  // namespace render
