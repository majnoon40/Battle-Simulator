#pragma once

#include <algorithm>

namespace game {

// Absolute zoom limits (pixels per world unit), shared by every backend so
// the window, the PNG renderer and the tests agree on the reachable views.
inline constexpr double kMinZoom = 0.05;
inline constexpr double kMaxZoom = 32.0;

// A 2D camera: the world point at the screen center plus a zoom of screen
// pixels per world unit. Pure math only — no raylib, no state beyond the
// three numbers, so the same camera drives the window, the PNG rasterizer
// and the tests. Screen coordinates are pixels with (0, 0) at the top-left
// and y growing downward (the same convention raylib and the rasterizer
// use); world coordinates are the simulation's, projected without flipping
// (the battlefield is y-down agnostic).
struct Camera {
  double center_x = 0.0;
  double center_y = 0.0;
  double zoom = 1.0;  // screen pixels per world unit

  // A camera that fits the whole battlefield into the screen, centered,
  // with the largest zoom that still shows all of it.
  [[nodiscard]] static Camera fit(double world_width, double world_height,
                                  int screen_width, int screen_height) {
    Camera camera;
    camera.center_x = world_width / 2.0;
    camera.center_y = world_height / 2.0;
    camera.zoom = std::min(
        static_cast<double>(screen_width) / world_width,
        static_cast<double>(screen_height) / world_height);
    camera.clamp_zoom();
    return camera;
  }

  // Keeps zoom inside [kMinZoom, kMaxZoom].
  void clamp_zoom() { zoom = std::clamp(zoom, kMinZoom, kMaxZoom); }

  // World -> screen. Conversion is affine, so it round-trips exactly (up to
  // floating point) through screen_to_world.
  [[nodiscard]] double screen_x(double world_x, int screen_width) const {
    return (world_x - center_x) * zoom + static_cast<double>(screen_width) / 2.0;
  }
  [[nodiscard]] double screen_y(double world_y, int screen_height) const {
    return (world_y - center_y) * zoom + static_cast<double>(screen_height) / 2.0;
  }
  // Screen -> world.
  [[nodiscard]] double world_x(double sx, int screen_width) const {
    return (sx - static_cast<double>(screen_width) / 2.0) / zoom + center_x;
  }
  [[nodiscard]] double world_y(double sy, int screen_height) const {
    return (sy - static_cast<double>(screen_height) / 2.0) / zoom + center_y;
  }

  // Pans by a screen-space delta (drag distance in pixels).
  void pan(double screen_dx, double screen_dy) {
    center_x -= screen_dx / zoom;
    center_y -= screen_dy / zoom;
  }

  // Zooms by `factor` about a screen point: the world point under that
  // pixel stays under it. Zoom stays clamped; when the clamp bites, the
  // world point is kept for the resulting zoom.
  void zoom_at(double sx, double sy, double factor, int screen_width,
               int screen_height) {
    const double before_x = world_x(sx, screen_width);
    const double before_y = world_y(sy, screen_height);
    zoom *= factor;
    clamp_zoom();
    const double after_x = world_x(sx, screen_width);
    const double after_y = world_y(sy, screen_height);
    center_x += before_x - after_x;
    center_y += before_y - after_y;
  }
};

}  // namespace game
