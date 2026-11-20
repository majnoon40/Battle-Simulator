#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sim/squads.hpp"

namespace sim {

class Simulation;

// One invariant violation found by Simulation::check_invariants (M15 S1).
// `unit_index` is kNoUnit when the violation is not about a specific unit
// (a global count, an order queue, the grid); `unit_id` is 0 then.
struct InvariantViolation {
  static constexpr std::size_t kNoUnit = static_cast<std::size_t>(-1);

  std::uint64_t tick = 0;        // completed battle ticks at detection
  std::uint32_t unit_id = 0;     // stable id of the offending unit, 0 = none
  std::size_t unit_index = kNoUnit;  // index into the world units vector
  const char* invariant = "";    // short name, e.g. "health-range"
  std::string detail;            // human-readable values, e.g. "health -3.5
                                 // outside [0, 100]"
};

// Test seam for the invariant checker (M15 S1). The checker validates the
// private state the simulation maintains, so the tests that prove it fires
// need to deliberately corrupt exactly that private state. Every function
// takes the simulation to corrupt plus the target index / value, performs
// the one write it names, and nothing else. Never used by the simulation,
// the CLI or any production code path.
struct InvariantCorruption {
  static void unit_id(Simulation& s, std::size_t index, std::uint32_t id);
  static void position(Simulation& s, std::size_t index, double x, double y);
  static void health(Simulation& s, std::size_t index, double health);
  static void max_health(Simulation& s, std::size_t index, double max_health);
  static void morale(Simulation& s, std::size_t index, double morale);
  static void fatigue(Simulation& s, std::size_t index, double fatigue);
  static void experience(Simulation& s, std::size_t index, double experience);
  static void facing(Simulation& s, std::size_t index, double facing);
  static void faction(Simulation& s, std::size_t index, Faction faction);
  static void target_of(Simulation& s, std::size_t index, int target);
  static void revive(Simulation& s, std::size_t index);  // alive = true
  static void unit_squad(Simulation& s, std::size_t index,
                         std::int32_t squad_id);
  static void squad_member_index(Simulation& s, std::size_t index,
                                 std::uint32_t member_index);
  static void squad_commander(Simulation& s, std::size_t squad,
                              std::uint32_t commander_index,
                              bool command_active);
  static void squad_cohesion(Simulation& s, std::size_t squad,
                             double cohesion);
  static void push_pending_order(Simulation& s,
                                 const struct PlayerOrder& order);
};

}  // namespace sim
