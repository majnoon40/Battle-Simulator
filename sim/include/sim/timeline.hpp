#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "sim/factions.hpp"

namespace sim {

// One timeline sample (M10 Part 2a): the state of both factions at one
// tick. Sampled every timeline_interval ticks (default 10 when the CSV
// export is requested) plus once at the battle's end. Sampling only
// reads state — enabling it never changes a battle.
struct FactionTimeline {
  std::uint32_t alive = 0;
  std::uint32_t routed = 0;  // alive but routing
  std::uint32_t dead = 0;    // cumulative deaths so far
  std::uint32_t engagements = 0;  // alive units attacking this tick
  double morale_sum = 0.0;   // over alive units (morale off: 50 each)
  std::uint32_t morale_n = 0;
  std::vector<std::uint32_t> alive_by_type;  // type_index -> alive count
};

struct TimelineRow {
  std::uint64_t tick = 0;  // 1-based
  std::array<FactionTimeline, kFactionCount> factions{};
};

}  // namespace sim
