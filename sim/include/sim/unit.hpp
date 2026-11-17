#pragma once

#include <cstddef>
#include <cstdint>

#include "sim/factions.hpp"

namespace sim {

// Combat statistics for one unit archetype. Values are copied into each unit
// at spawn time so that units are self-contained; they originate from
// SimulationConfig and are never hardcoded inside decision or combat code.
struct UnitStats {
  double max_health = 100.0;  // health at spawn
  double speed = 2.0;         // world units moved per tick
  double damage = 12.0;       // health removed per successful attack
  double attack_range = 2.0;  // maximum attack distance (inclusive)
  std::uint32_t attack_cooldown_ticks = 5;  // ticks between attacks (>= 1)
  // Perception radius (M4, inclusive): a utility-mode unit senses enemies and
  // allies only within this distance. Ignored by the legacy AI. Unit type
  // files must keep this strictly above the type's attack range.
  double sight_radius = 40.0;
  // Command radius (M6): a unit type with leadership_radius > 0 is a
  // commander type — one such unit leads each squad, its orders reach squad
  // members inside this radius (with a distance-based delay) and its death
  // shocks them. 0 (the default when the field is absent) means the type
  // cannot command.
  double leadership_radius = 0.0;
  // Combat variance (M10, --variance on only; defaults keep the M1-M9
  // deterministic combat exactly): accuracy is the per-attack hit chance in
  // [0, 1]; damage_variance spreads each hit's damage by
  // damage * (1 + damage_variance * (2*roll - 1)); cooldown_jitter spreads
  // the post-attack cooldown by the same formula (then clamped to >= 1).
  // All rolls come from the counter-based tactics_roll (docs/ai.md).
  double accuracy = 1.0;
  double damage_variance = 0.0;  // [0, 1], 0 = exact damage
  double cooldown_jitter = 0.0;  // [0, 1], 0 = exact cooldown
};

// The utility-AI actions (M4), in fixed tie-break order: when two action
// scores are exactly equal, the earlier action wins. M10 adds Search,
// Help and Flank after the original five; None and Flee move with them.
enum class Action : std::uint8_t {
  Attack = 0,    // a visible enemy is within attack range: stop and attack
  Approach = 1,  // move toward the nearest visible enemy
  Advance = 2,   // no enemy visible: move toward the faction objective
  Retreat = 3,   // move away from the visible enemy centroid, toward own side
  Hold = 4,      // stay in place
  Search = 5,    // M10: move to a remembered or squad-reported enemy contact
  Help = 6,      // M10: move to an ally that is attacked or badly hurt
  Flank = 7,     // M10: move to a point offset off the enemy front, then in
  None = 8,      // no action chosen yet (before the first tick, legacy mode)
  Flee = 9,      // M5: forced by routing — not a scored action, never counted
};

// All scored actions (the first five are the M4-M9 set; Search, Help and
// Flank exist only with tactics on, M10).
inline constexpr std::size_t kActionCount = 8;
// The M4-M9 export set: JSON action_counts objects carry exactly these
// five keys when tactics are off, so every M1-M9 reference stays
// byte-identical.
inline constexpr std::size_t kLegacyActionCount = 5;

// M10 spawn patterns (scenario opt-in; Band is the M1-M9 default and the
// only pattern that uses the spawn RNG stream). The others place units
// deterministically inside the faction's spawn band from the counter RNG
// (channel 5), so the same seed always yields the same formation:
//   Band    — uniform random in the band (M1-M9 behavior)
//   Line    — one rank across the band's height, centered in its width
//   Columns — four vertical columns spaced across the band
//   Echelon — diagonal stair-steps, leading edge toward the enemy
//   Cluster — five tight groups around deterministic band centers
enum class SpawnPattern : std::uint8_t {
  Band = 0,
  Line = 1,
  Columns = 2,
  Echelon = 3,
  Cluster = 4,
};

[[nodiscard]] const char* to_string(SpawnPattern pattern) noexcept;

[[nodiscard]] const char* to_string(Action action) noexcept;

// A unit's personality (M4): traits in [0, 1] plus the archetype the traits
// were drawn from (an index into the personality table in use, or
// kCustomArchetype for explicitly injected personalities). Derived
// deterministically from a hash of (seed, unit id) — never from the spawn
// RNG stream, so spawn positions are identical in every AI mode. Discipline
// (M6) is drawn fourth from the same private stream, after bravery, caution
// and aggression, so the M4/M5 traits are bit-identical to earlier
// milestones; it scales how strongly a unit obeys its commander's order.
inline constexpr std::uint8_t kCustomArchetype = 0xFF;

struct Personality {
  double bravery = 0.5;
  double caution = 0.5;
  double aggression = 0.5;
  double discipline = 0.5;  // M6: order obedience weight
  std::uint8_t archetype = kCustomArchetype;
};

// A single combat unit. `id` is assigned once at spawn, is never reused, and
// units are never removed from the world vector, so index positions stay
// stable for the whole battle. `type_index` names the unit's archetype in the
// type table the battle was created with (meaningful only then; 0 otherwise).
struct Unit {
  std::uint32_t id = 0;  // stable identifier, 1-based; 0 is reserved
  Faction faction = Faction::Red;
  std::uint32_t type_index = 0;  // index into the UnitTypeTable (M2)
  double x = 0.0;
  double y = 0.0;
  double health = 0.0;
  UnitStats stats{};
  std::uint32_t cooldown_remaining = 0;  // ticks until the next attack is allowed
  Personality personality{};             // M4: hash-derived, mode-independent
  std::uint8_t last_action = static_cast<std::uint8_t>(Action::None);
  // Last committed move vector (M6): (x, y) displacement of the most recent
  // tick in which this unit moved. Squad alignment steering averages these;
  // (0, 0) for units that never moved. Internal state only — never exported.
  double last_move_x = 0.0;
  double last_move_y = 0.0;

  // m13 item 3 (--combat v2; all zero/unused under v1):
  double facing = 0.0;      // heading in radians (last movement dir)
  double fatigue = 0.0;     // [0, 1]: 1 = exhausted (slower, weaker)
  double experience = 0.0;  // [0, 1]: kills + engagement time
  // Morale (M5, utility mode with morale enabled; zero/Steady otherwise).
  double morale = 0.0;  // [0, 100]
  std::uint8_t morale_state = 1;  // MoraleState; 1 == Steady (no modifiers)
  bool routed = false;  // Broken and fleeing: alive, but not active
  bool alive = false;
  // Tactical memory (M10, tactics on only): the position of the nearest
  // enemy this unit last saw and the 1-based tick it was seen on (0 = the
  // unit has never seen an enemy). Expired memories are not consulted.
  double memory_x = 0.0;
  double memory_y = 0.0;
  std::uint64_t memory_tick = 0;
  // Pursuit bookkeeping (M10): which routed enemy (unit id) is being
  // pursued, for how many consecutive ticks, and where the pursuit
  // started. Past pursuit.max_ticks or max_distance the pursuer
  // disengages: the routed enemy stops driving Approach (unless it is
  // inside attack range) and the unit's memory of it is suppressed, so
  // winners never chase forever. Switching to a different target resets
  // the counter.
  std::uint32_t pursuit_ticks = 0;
  std::uint32_t pursuit_target = 0;
  double pursuit_anchor_x = 0.0;
  double pursuit_anchor_y = 0.0;
  // Decision interval (M10, tactics on only): this unit re-scores its
  // actions every `interval` ticks (1..5, drawn from the counter RNG at
  // spawn); between decisions the committed action persists like the M4
  // commit time. With tactics off every unit re-scores every tick, as in
  // M4-M9 (the commit minimum still applies).
  std::uint8_t decision_interval = 1;
};

}  // namespace sim
