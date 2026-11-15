#pragma once

#include <cstdint>

#include "game/camera.hpp"
#include "sim/simulation.hpp"

namespace game {

// Picks the alive unit nearest to a screen point within `pick_radius_px`
// (converted to a world radius via the camera). Ties break to the lower
// unit index. Returns the unit index, or -1 when nothing is in range (or
// the world is empty).
[[nodiscard]] std::int32_t pick_unit(const sim::Simulation& simulation,
                                     const Camera& camera, int screen_width,
                                     int screen_height, double screen_x,
                                     double screen_y,
                                     double pick_radius_px);

// Picks the squad whose centroid is nearest to a screen point within
// `pick_radius_px` (the badges are drawn at the centroids). Returns the
// global squad id, or -1 when squads are off or nothing is in range.
[[nodiscard]] std::int32_t pick_squad(const sim::Simulation& simulation,
                                      const Camera& camera, int screen_width,
                                      int screen_height, double screen_x,
                                      double screen_y,
                                      double pick_radius_px);

}  // namespace game
