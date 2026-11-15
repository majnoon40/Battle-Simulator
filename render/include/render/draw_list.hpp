#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "game/theme.hpp"

namespace render {

// What one draw command is.
enum class CmdKind : std::uint8_t {
  Circle,    // filled circle + optional 1px outline
  Triangle,  // filled triangle
  Line,      // line segment with a thickness in pixels
  Rect,      // axis-aligned rectangle, filled or 1px outline
  Text,      // bitmap text (the rasterizer uses a built-in 5x7 font)
};

// One draw command, in screen pixels. Geometry is baked at list-build time
// (the rasterizer and raylib only ever consume pixels); the fixed field
// layout keeps construction and comparison byte-deterministic. Field use
// by kind:
//   Circle   a=x, b=y, c=radius            (d=1 -> 1px outline in color2)
//   Triangle a..f = x1,y1,x2,y2,x3,y3
//   Line     a,b,c,d = x1,y1,x2,y2, e=thickness
//   Rect     a=x, b=y, c=w, d=h, e=1 filled else outline
//   Text     a=x, b=y, c=font size (px, 7 = 1x), text=text
struct DrawCmd {
  CmdKind kind = CmdKind::Circle;
  double a = 0.0;
  double b = 0.0;
  double c = 0.0;
  double d = 0.0;
  double e = 0.0;
  double f = 0.0;
  game::Color color{};
  game::Color color2{};  // outline (circles), ignored elsewhere
  std::string text;      // Text only

  [[nodiscard]] bool operator==(const DrawCmd&) const = default;
};

// A whole frame: the target size, the background and every command in
// back-to-front draw order. The same state, camera and options always
// build the same list, so both backends (raylib window, PNG rasterizer)
// draw identical pictures and the tests can pin exact lists.
struct DrawList {
  int width = 0;
  int height = 0;
  game::Color background{};
  std::vector<DrawCmd> cmds;

  void clear() { cmds.clear(); }
  void circle(double x, double y, double radius, game::Color fill,
              game::Color outline = {});
  void triangle(double x1, double y1, double x2, double y2, double x3,
                double y3, game::Color fill);
  void line(double x1, double y1, double x2, double y2, double thickness,
            game::Color color);
  void rect(double x, double y, double w, double h, game::Color color,
            bool filled);
  void text(double x, double y, int font_px, const std::string& content,
            game::Color color);

  [[nodiscard]] bool operator==(const DrawList&) const = default;
};

}  // namespace render
