#include "sim/factions.hpp"

namespace sim {

const char* to_string(Faction faction) noexcept {
  switch (faction) {
    case Faction::Red:
      return "Red";
    case Faction::Blue:
      return "Blue";
    case Faction::Green:
      return "Green";
    case Faction::Yellow:
      return "Yellow";
    case Faction::Purple:
      return "Purple";
    case Faction::Orange:
      return "Orange";
    case Faction::Cyan:
      return "Cyan";
    case Faction::Magenta:
      return "Magenta";
  }
  return "Unknown";
}

}  // namespace sim
