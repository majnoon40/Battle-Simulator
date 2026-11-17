#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "sim/factions.hpp"
#include "sim/squads.hpp"
#include "sim/unit.hpp"

namespace sim {

enum class TerminationReason : std::uint8_t {
  None = 0,           // battle still running
  FactionEliminated,  // a faction has no surviving units
  FactionRouted,      // M5: a faction had no active units (all dead or routed)
                      // for rout_timeout_ticks consecutive ticks
  TickLimit,          // configured tick limit reached
  RoutedThreshold,    // m13: active units stayed below max(5, 5% of the
                      // initial strength) for routed_threshold_ticks
  Stalemate,          // m13: after first contact, both factions together
                      // executed fewer than stalemate_max_engagements
                      // attacks over the last stalemate_ticks ticks
};

[[nodiscard]] const char* to_string(TerminationReason reason) noexcept;

// Final statistics for one unit type on one faction (M2). `damage_dealt`
// follows M1 combat semantics: focus-fire overkill counts, health is clamped
// only on death; `kills` is credited once, to the attack that brought the
// target from positive health to zero or below.
struct TypeReport {
  std::string type_id;
  std::uint32_t spawned = 0;
  std::uint32_t survivors = 0;
  std::uint32_t kills = 0;
  double damage_dealt = 0.0;
};

// Per-faction routing statistics (M5), tracked over the whole battle.
struct RoutingStats {
  std::uint64_t first_rout_tick = 0;  // 1-based; 0 when the faction never routed
  std::uint32_t peak_routed = 0;      // most units routed at the same time
  std::uint64_t total_routs = 0;      // cumulative break events
  std::uint64_t recoveries = 0;       // cumulative recovery events
};

// Per-faction combat-depth counters (m13 item 3, --combat v2 only).
struct CombatStats {
  std::uint64_t flank_hits = 0;
  std::uint64_t rear_hits = 0;
  std::uint64_t charge_hits = 0;
  std::uint64_t stance_hits_taken = 0;  // connected hits absorbed by stance
};

struct FactionReport {
  std::uint32_t initial = 0;    // units the faction started with
  std::uint32_t deaths = 0;     // units that died
  std::uint32_t survivors = 0;  // units still alive (routed included)
  std::uint32_t active = 0;     // M5: alive and not routed (== survivors with
                                // morale off and in legacy mode)

  // Cumulative per-action totals over the whole battle (M4), indexed by
  // Action. All zeros in legacy mode.
  std::array<std::uint64_t, kActionCount> action_counts{};

  // Routing statistics (M5); all zeros with morale off.
  RoutingStats routing{};
  CombatStats combat{};  // m13 item 3 (--combat v2; zeros under v1)

  // Squad statistics (M6); all zeros with squads off.
  SquadStats squad{};

  // Per-type breakdown (M2), filled only for battles created with a unit type
  // table, in table order, listing only types that actually spawned.
  std::vector<TypeReport> by_type;
};

struct BattleResult {
  bool finished = false;
  TerminationReason reason = TerminationReason::None;
  bool has_winner = false;         // false => draw (or still running)
  Faction winner = Faction::Red;   // valid only when has_winner is true
  std::uint64_t ticks_elapsed = 0;
  std::array<FactionReport, kFactionCount> factions{};
  // Terrain (M8), filled only for battles on a map: the longest run of
  // consecutive ticks any unit spent making less than stuck-epsilon
  // progress, how many units ever had such a run longer than 100 ticks,
  // and kills by the terrain type of the victim's cell (table order).
  std::uint32_t longest_stuck_stretch = 0;
  std::uint32_t units_stuck_over_100 = 0;
  std::vector<std::uint64_t> terrain_kills{};
  // m11a: tick of the first enemy sighting by any unit (0 = no contact
  // happened; set in every mode — the acceptance sweeps read it).
  std::uint64_t first_contact_tick = 0;
};

}  // namespace sim
