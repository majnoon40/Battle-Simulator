#include "sim/world.hpp"

namespace sim {

std::uint32_t World::alive_count(Faction faction) const noexcept {
  std::uint32_t count = 0;
  for (const Unit& unit : units) {
    if (unit.alive && unit.faction == faction) {
      ++count;
    }
  }
  return count;
}

const Unit* World::find_by_id(std::uint32_t id) const noexcept {
  for (const Unit& unit : units) {
    if (unit.id == id) {
      return &unit;
    }
  }
  return nullptr;
}

}  // namespace sim
