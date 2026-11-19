#pragma once

#include "sim/terrain.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "sim/factions.hpp"
#include "sim/morale.hpp"
#include "sim/personalities.hpp"
#include "sim/scenario.hpp"
#include "sim/squads.hpp"
#include "sim/unit.hpp"
#include "sim/unit_types.hpp"

namespace sim {

class EventRecorder;  // events.hpp (M10 Part 2a)
struct CombatConfig;  // combat.hpp (m13 item 3)

// Hard cap on the number of units in one battle (raised by M3 to 20,000;
// the spatial hash keeps the neighbor search from dominating at scale).
// Enforced for both the legacy total-units split and scenario compositions.
inline constexpr std::uint32_t kMaxUnits = 20000;

// How decide_targets finds the nearest enemy. Grid is the default (M3);
// Brute keeps the M1/M2 O(n) scan as a test oracle and CLI baseline.
enum class NeighborMode : std::uint8_t { Brute, Grid };

[[nodiscard]] const char* to_string(NeighborMode mode) noexcept;

// Which AI drives decisions (M4). Legacy reproduces every M1-M3 reference
// result exactly (unlimited sight, everyone attacks when in range). Utility
// is the new perception-limited, personality-weighted action scoring.
enum class AiMode : std::uint8_t { Legacy, Utility };

[[nodiscard]] const char* to_string(AiMode mode) noexcept;

// A per-faction objective point (M4): the only global information utility
// units have — where to Advance when no enemy is visible. Defaults to the
// center of the opposing faction's spawn band; scenarios may override.
struct ObjectivePoint {
  double x = 0.0;
  double y = 0.0;
  bool explicit_set = false;  // false -> the simulation computes the default
};

// A per-faction squad plan in the simulation configuration (M6): `count`
// squads, each of `size` soldiers plus one commander. Mirrors the scenario's
// ScenarioSquads; the CLI maps one to the other.
struct SquadPlan {
  std::uint32_t count = 0;
  std::uint32_t size = 25;
};

// Data-driven simulation configuration. The CLI fills this from its arguments
// plus the loaded unit type table and scenario; gameplay code never hardcodes
// statistics.
//
// Two mutually exclusive army modes:
//  - Legacy (M1, used when `composition` is empty): `total_units` is split
//    between the factions (Red gets the extra unit when odd) and every unit
//    receives its faction's `red_stats` / `blue_stats`.
//  - Composition (M2): both `composition` arrays are non-empty and name, per
//    faction, an ordered list of (type table index, count). Units spawn per
//    type with stats from `unit_types`; `total_units` and `*_stats` are
//    ignored (but `total_units` may be set to the real total for reporting).
struct SimulationConfig {
  std::uint64_t seed = 42;
  std::uint32_t total_units = 500;  // legacy mode: split between factions; Red gets the extra unit when odd
  std::uint64_t max_ticks = 3000;   // tick limit; the battle also ends on faction elimination
  double battlefield_width = 1000.0;
  double battlefield_height = 1000.0;
  double spawn_band_fraction = 0.15;  // each faction spawns inside a band of width * this fraction
  UnitStats red_stats{};   // legacy mode only
  UnitStats blue_stats{};  // legacy mode only

  // Composition mode (M2). `unit_types` is not owned and must outlive the
  // simulation; it also drives per-type reports and the "type" field in JSON
  // exports.
  const UnitTypeTable* unit_types = nullptr;
  std::array<std::vector<TypeCount>, kFactionCount> composition{};

  // Neighbor search (M3). Grid mode builds a SpatialHashGrid over the
  // battlefield and maintains it after movement and deaths; results are
  // identical to Brute mode by construction and by test. Utility-mode
  // perception always uses the grid (radius queries), so in utility mode the
  // neighbor mode has no effect on results.
  NeighborMode neighbor_mode = NeighborMode::Grid;
  double grid_cell_size = 16.0;  // world units per grid cell (grid mode only; 16 measured best)

  // Utility AI (M4). `personalities` is not owned and must outlive the
  // simulation; nullptr selects the built-in default table (identical to
  // data/personalities.json). Personalities are derived from a hash of
  // (seed, unit id), never from the spawn RNG stream. Objectives default to
  // the center of the opposing spawn band unless explicitly set.
  AiMode ai_mode = AiMode::Utility;
  const PersonalityTable* personalities = nullptr;
  double action_stickiness = 0.05;  // score bonus for the current action
  std::array<ObjectivePoint, kFactionCount> objectives{};

  // Morale and routing (M5). Applies to utility mode only — legacy mode
  // ignores morale regardless of this flag so it keeps reproducing the
  // M1-M3 references. `morale` is not owned and must outlive the simulation;
  // nullptr selects the built-in default config (identical to
  // data/morale.json). With morale off, utility battles are bit-identical
  // to their M4 runs.
  bool morale_enabled = true;
  const MoraleConfig* morale = nullptr;

  // Squads, formations and commanders (M6). Utility mode only — legacy mode
  // ignores squads entirely (it must keep reproducing M1-M3), and a faction
  // whose squad plan is unset fields no squads even when enabled. With
  // squads off, utility battles are bit-identical to their M5 runs. The
  // configs are not owned and must outlive the simulation; nullptr selects
  // the built-in defaults (identical to data/formations.json and
  // data/commanders.json).
  bool squads_enabled = true;
  const FormationConfig* formations = nullptr;
  const CommanderConfig* commanders = nullptr;
  std::array<std::optional<SquadPlan>, kFactionCount> squad_plans{};

  // Terrain (M8): a grid of terrain cells, or nullptr for the classic
  // open field (no terrain rule fires at all then, so every M1-M7
  // reference reproduces byte-for-byte). The battlefield size must equal
  // the grid size times its cell size; the loader enforces that.
  const TerrainGrid* terrain = nullptr;
  // Cap on how much a cell's sight_modifier may enlarge an observer's
  // sight radius, so scaled radius queries stay valid for the grid.
  double terrain_sight_max_scale = 2.0;

  // Tactical depth (M10): combat variance, decision intervals, memory and
  // the Search/Help/Flank actions — everything in data/tactics.json plus
  // the per-type accuracy / damage_variance / cooldown_jitter stats.
  // Utility mode only; legacy mode ignores it. With tactics off there is
  // no battle-time randomness at all and the M1-M9 action set is scored,
  // so every M1-M9 reference reproduces byte-for-byte. `tactics` is not
  // owned and must outlive the simulation; nullptr selects the built-in
  // default config (identical to data/tactics.json).
  bool tactics_enabled = true;
  const TacticsConfig* tactics = nullptr;
  // Contact seeking (M10 continuation, --contact-seek): parked units
  // retarget Advance to the faction's last known enemy estimate. Utility
  // + tactics mode only; off reproduces Part 1 byte-for-byte.
  bool contact_seek_enabled = true;

  // Battle events (M10 Part 2a, --events): when set, the simulation
  // records deaths (with killer), routs, recoveries, commander deaths,
  // successions, order changes, flank/help/pursuit/seek starts and
  // squad collapses into this recorder (and streams JSONL if the
  // recorder has an attached stream). Null (default) records nothing;
  // a recorder changes no behavior — battles are byte-identical.
  EventRecorder* events = nullptr;

  // Timeline sampling (M10 Part 2a, --export-csv): sample every N ticks
  // (and once at the end). 0 = off (default). Sampling only reads state.
  std::uint32_t timeline_interval = 0;

  // M10 spawn patterns (per faction; default Band = the M1-M9 uniform
  // random spawn from the RNG stream). The other patterns place units
  // deterministically inside the band (counter RNG channel 5); scenarios
  // opt in per faction.
  std::array<SpawnPattern, kFactionCount> spawn_patterns{};
  // m13 item 5c: optional spawn anchors (drag-to-place; see
  // sim/scenario.hpp). Unset = the historical full-height side band.
  std::array<std::optional<SpawnAnchor>, kFactionCount> spawn_anchors{};

  // Side swap (M10, --swap-sides): Red spawns in the eastern band and
  // Blue in the western one (the band contents, default objectives and
  // legacy stats swap with them; explicit objectives are the caller's to
  // swap). Purely positional — unit factions are unchanged.
  bool swap_sides = false;

  // Navigation v2 (m11a, --nav v2|v1; default v2). v2 fixes the m11a
  // diagnosis findings: formation members far from their slot march
  // along the faction flow field instead of beelining to the projected
  // slot (which jams whole squads against rivers and valley walls in
  // terrain corridors), and their march moves are goal-seeking so the
  // stuck accounting sees them. v1 is the exact M10 navigation — with
  // nav_v2 false every map battle (and every --swap-sides setup; the
  // CLI's objective-swap fix is gated on this flag too) reproduces the
  // M10 binary byte-for-byte. The flag only matters with a terrain map
  // (or --swap-sides on a scenario with explicit objectives); no-map
  // references are identical either way.
  bool nav_v2 = true;

  // Availability and termination v2 (m13 item 2, --ai-avail / --end
  // v2|v1; both default v2). ai_avail_v2: unavailable actions stay
  // excluded from selection — the M10 regroup term no longer clamps an
  // unavailable Advance (enemy visible) up to a pseudo-available 0.0
  // (the m12 audit's root cause), a delivered MoveTo keeps the march
  // available explicitly instead, Help is capped per ally and never
  // targets routed allies far from combat, and stale faction contact
  // estimates decay when a seeker reaches them and sees nothing.
  // end_v2: a faction also ends the battle (ROUTED_THRESHOLD) when its
  // active units stay below max(termination.routed_threshold_min_units,
  // termination.routed_threshold_fraction of its initial strength) for
  // termination.routed_threshold_ticks consecutive ticks (data/morale.json).
  // v1 for both flags reproduces milestone-12 byte-for-byte.
  bool ai_avail_v2 = true;
  bool end_v2 = true;

  // Combat depth (m13 item 3, --combat v2|v1; default v2). v2 adds
  // facing (flank/rear damage multipliers, rear morale loss), cavalry
  // charges, fatigue, experience, the commander aura and the defensive
  // stance of high-cohesion Hold squads — every value in
  // data/combat.json (config.combat; nullptr = the built-in defaults).
  // v1 is the exact milestone-12 combat, byte-for-byte.
  bool combat_v2 = true;
  const CombatConfig* combat = nullptr;

  // Player orders (m11b, --orders on|off; default on). issue_order is
  // inert when false. With no orders issued the battle is byte-identical
  // either way (the delivery machinery only runs when an order exists).
  bool orders_enabled = true;

  // Deterministic multithreading (m13 item 4, --threads N; default 1).
  // 0 and 1 both run every phase inline on the simulation thread —
  // byte-identical to the pre-threads binary. N >= 2 creates a
  // persistent worker pool that runs the read-only perception and
  // decision computations in fixed-size chunks (the chunk size is
  // independent of N); every worker writes only its own preallocated
  // slots and all ordered state (event recording, the per-ally Help
  // cap, the faction estimate decay, commits) stays on the simulation
  // thread. Output is byte-identical for every thread count (verified
  // across 1/2/3/4/8 by tests; docs/determinism.md "Threading").
  std::uint32_t thread_count = 1;
};

// Per-faction unit counts for a total: Red receives the extra unit when the
// total is odd.
struct ArmySizes {
  std::uint32_t red;
  std::uint32_t blue;
};

[[nodiscard]] ArmySizes split_armies(std::uint32_t total_units) noexcept;

// True when this configuration uses the M2 composition mode (and therefore
// requires a unit type table).
[[nodiscard]] inline bool uses_composition(const SimulationConfig& config) {
  return !config.composition[static_cast<std::size_t>(Faction::Red)].empty() ||
         !config.composition[static_cast<std::size_t>(Faction::Blue)].empty();
}

// Throws std::invalid_argument with a descriptive message for every invalid
// field. The simulation refuses to start from an invalid configuration.
void validate_config(const SimulationConfig& config);

}  // namespace sim
