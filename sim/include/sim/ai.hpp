#pragma once

#include <array>
#include <cstdint>

#include "sim/tactics.hpp"
#include "sim/unit.hpp"

namespace sim {

// What one unit sensed on one tick (M4), computed from the tick-start
// snapshot only. `nearest_visible_enemy` is the nearest enemy (by squared
// distance, ties to the lower unit id — the M1 rule) within sight_radius,
// inclusive; the centroid fields are valid exactly when visible_enemies > 0.
struct Perception {
  int nearest_visible_enemy = -1;  // index into the world units vector
  double nearest_visible_enemy_distance = 0.0;
  std::uint32_t visible_enemies = 0;
  std::uint32_t visible_allies = 0;  // excludes the unit itself
  std::uint32_t routed_allies = 0;   // M5: routed among the visible allies
  double enemy_centroid_x = 0.0;
  double enemy_centroid_y = 0.0;
};

// The five action scores, indexed by Action. Available actions are scored in
// [0, 1]; unavailable actions carry -1.0.
using ActionScores = std::array<double, kActionCount>;

// The inputs every score is computed from (see docs/ai.md for the formulas
// and their rationale). All derived values are clamped to [0, 1] by the
// caller so the scores can never leave [0, 1] or become non-finite.
struct ScoreInputs {
  double health_fraction = 1.0;    // health / max_health
  double strength_ratio = 1.0;     // (visible allies + 1) / (allies + enemies + 1)
  double distance_fraction = 0.0;  // distance to nearest visible enemy / sight radius
  double cooldown_readiness = 1.0; // 1 - cooldown_remaining / attack_cooldown_ticks
  double bravery = 0.5;
  double caution = 0.5;
  double aggression = 0.5;
  bool has_visible_enemy = false;
  bool enemy_in_attack_range = false;
  // M10 (tactics on only; see docs/ai.md for the formulas):
  bool has_fresh_contact = false;  // memory or squad intel newer than forget_ticks
  double contact_distance = 0.0;    // world units to the freshest contact
  double contact_age = 0.0;         // ticks since that contact was seen
  bool ally_in_need = false;        // a visible ally is attacked or badly hurt
  double ally_health_fraction = 1.0;
  double ally_distance = 0.0;       // to that ally
  bool squad_engaged = false;       // some squad member sees an enemy
  bool member_may_flank = false;    // under the per-squad flanker cap
  // (visible allies + 1) / (visible enemies + 1), capped at 99; the raw
  // local outnumbering ratio Flank requires (unlike strength_ratio, which
  // is a normalized share and never exceeds 1).
  double local_enemy_ratio = 1.0;
  bool target_routed = false;       // the nearest visible enemy is fleeing
  double unit_speed = 2.0;          // for the cavalry pursuit scale
  bool pursuit_within_bounds = false;  // under pursuit.max_ticks / max_distance
  bool has_squad = false;
  double squad_cohesion = 1.0;
  double sight_radius = 40.0;  // normalizes the search contact distance
};

// Scores the actions. Availability rules (documented in docs/ai.md):
//   Attack   — a visible enemy is within attack range
//   Approach — at least one visible enemy
//   Advance  — no visible enemy
//   Retreat  — at least one visible enemy
//   Hold     — always
//   Search   — no visible enemy, a contact fresher than memory.forget_ticks
//   Help     — a visible ally in need and the local strength ratio allows
//   Flank    — the squad is frontally engaged, the local ratio
//              (visible allies + 1) / (visible enemies + 1) is at least
//              flank.outnumber_ratio and this member is under the cap
// Every score is a fixed linear blend of the inputs; weights within one
// action sum to exactly 1.0, so scores stay in [0, 1] whenever the inputs
// do. `tactics` selects the M10 extensions: null scores only the five
// M4-M9 actions (Search/Help/Flank unavailable, no pursuit or regroup
// terms) — exactly the pre-M10 behavior. With tactics, Approach gains the
// bounded pursuit bonus against routed enemies and Advance the regroup
// bonus at low squad cohesion (formulas in docs/ai.md).
[[nodiscard]] ActionScores score_actions(
    const ScoreInputs& inputs, const TacticsConfig* tactics = nullptr);

// Picks the action with the highest score. `current` (the unit's previous
// action, or None) receives a `stickiness` bonus added to its score so units
// do not flip-flop between near-equal actions. Exact ties break by the fixed
// action order Attack > Approach > Advance > Retreat > Hold (the enum order).
[[nodiscard]] Action choose_action(const ActionScores& scores, Action current,
                                   double stickiness) noexcept;

// Minimum commit time (M4): a freshly chosen action is held for this many
// ticks before scores are consulted again, as long as it stays available.
// This complements the stickiness bonus and damps score cycles (for example
// the cooldown-readiness dip right after an attack).
inline constexpr std::uint32_t kActionCommitTicks = 3;

}  // namespace sim
