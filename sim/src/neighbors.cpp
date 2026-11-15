#include "sim/neighbors.hpp"

namespace sim {

int nearest_enemy_index(const std::vector<Unit>& units, const Unit& unit) {
  int best = -1;
  double best_d2 = 0.0;
  for (std::size_t i = 0; i < units.size(); ++i) {
    const Unit& other = units[i];
    if (!other.alive || other.faction == unit.faction) {
      continue;
    }
    const double dx = other.x - unit.x;
    const double dy = other.y - unit.y;
    const double d2 = dx * dx + dy * dy;
    // Strictly closer wins; exact ties go to the lower unit id, so the
    // result is independent of the order of `units`.
    if (best < 0 || d2 < best_d2 ||
        (d2 == best_d2 && other.id < units[static_cast<std::size_t>(best)].id)) {
      best = static_cast<int>(i);
      best_d2 = d2;
    }
  }
  return best;
}

}  // namespace sim
