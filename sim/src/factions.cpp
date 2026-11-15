#include "sim/factions.hpp"

namespace sim {

const char* to_string(Faction faction) noexcept {
  switch (faction) {
    case Faction::Red:
      return "Red";
    case Faction::Blue:
      return "Blue";
  }
  return "Unknown";
}

}  // namespace sim
