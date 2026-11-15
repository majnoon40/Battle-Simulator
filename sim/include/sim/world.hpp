#pragma once

#include <cstdint>
#include <vector>

#include "sim/unit.hpp"

namespace sim {

// Basic world state: every unit ever spawned plus the battlefield dimensions
// and the number of completed ticks. Units are appended in spawn order and
// never removed, so `units` ordering (and therefore every iteration over it)
// is stable for the whole battle.
struct World {
  std::vector<Unit> units;
  double width = 1000.0;
  double height = 1000.0;
  std::uint64_t tick_count = 0;  // completed ticks

  [[nodiscard]] std::uint32_t alive_count(Faction faction) const noexcept;
  [[nodiscard]] const Unit* find_by_id(std::uint32_t id) const noexcept;
};

}  // namespace sim
