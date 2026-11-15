#include "sim/results.hpp"

namespace sim {

const char* to_string(TerminationReason reason) noexcept {
  switch (reason) {
    case TerminationReason::None:
      return "none";
    case TerminationReason::FactionEliminated:
      return "faction_eliminated";
    case TerminationReason::FactionRouted:
      return "faction_routed";
    case TerminationReason::TickLimit:
      return "tick_limit";
  }
  return "unknown";
}

}  // namespace sim
