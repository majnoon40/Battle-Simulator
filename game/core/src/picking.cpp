#include "game/picking.hpp"

#include <cmath>

namespace game {

std::int32_t pick_unit(const sim::Simulation& simulation,
                       const Camera& camera, int screen_width,
                       int screen_height, double screen_x, double screen_y,
                       double pick_radius_px) {
  const double world_x = camera.world_x(screen_x, screen_width);
  const double world_y = camera.world_y(screen_y, screen_height);
  const double radius = pick_radius_px / camera.zoom;
  const double radius_sq = radius * radius;
  std::int32_t best = -1;
  double best_sq = 0.0;
  for (std::size_t i = 0; i < simulation.world().units.size(); ++i) {
    const sim::Unit& unit = simulation.world().units[i];
    if (!unit.alive) {
      continue;  // dead units are not drawn and not pickable
    }
    const double dx = unit.x - world_x;
    const double dy = unit.y - world_y;
    const double distance_sq = dx * dx + dy * dy;
    if (distance_sq > radius_sq) {
      continue;
    }
    if (best < 0 || distance_sq < best_sq) {
      best = static_cast<std::int32_t>(i);
      best_sq = distance_sq;
    }
  }
  return best;
}

std::int32_t pick_squad(const sim::Simulation& simulation,
                        const Camera& camera, int screen_width,
                        int screen_height, double screen_x, double screen_y,
                        double pick_radius_px) {
  if (!simulation.squads_active()) {
    return -1;
  }
  const double world_x = camera.world_x(screen_x, screen_width);
  const double world_y = camera.world_y(screen_y, screen_height);
  const double radius = pick_radius_px / camera.zoom;
  const double radius_sq = radius * radius;
  std::int32_t best = -1;
  double best_sq = 0.0;
  for (const sim::Squad& squad : simulation.squads()) {
    const double dx = squad.centroid_x - world_x;
    const double dy = squad.centroid_y - world_y;
    const double distance_sq = dx * dx + dy * dy;
    if (distance_sq > radius_sq) {
      continue;
    }
    if (best < 0 || distance_sq < best_sq) {
      best = static_cast<std::int32_t>(squad.id);
      best_sq = distance_sq;
    }
  }
  return best;
}

}  // namespace game
