#pragma once

#include <cstdint>
#include <iosfwd>
#include <string_view>

namespace sim {

// M10 Part 1 — tactical depth configuration (data/tactics.json).
//
// Combat variance numbers (accuracy, damage_variance, cooldown jitter) are
// per unit type and live in data/units.json; this file holds everything
// else: memory lifetimes, squad intel, and the search / help / flank /
// pursuit / regroup parameters. Every field is required and validated;
// there are no silent defaults when parsing a file.
struct TacticsConfig {
  // Personal memory and squad intel. A unit remembers the position of the
  // nearest enemy it last saw for memory.forget_ticks ticks; after that the
  // memory is gone and the unit falls back to Advance. Squad members
  // inside the acting commander's leadership radius additionally share
  // their recent enemy contacts: up to squad_intel_max_contacts entries,
  // each readable by other members only squad_intel_delay_ticks ticks
  // after it was seen (orders and intel travel at the same kind of delay).
  struct Memory {
    std::uint64_t forget_ticks = 1200;
    std::uint32_t squad_intel_max_contacts = 8;   // 1..8
    std::uint64_t squad_intel_delay_ticks = 12;
  } memory;

  // Search / Pursue (M10): move to the last known or squad-reported
  // contact when no enemy is visible.
  //   score = score_base
  //         + distance_weight * (1 - clamp(contact_distance / sight, 0, 1))
  //         + age_weight      * (1 - clamp(contact_age / forget_ticks, 0, 1))
  // Available exactly when no enemy is visible and a contact (own memory
  // or squad intel) is fresher than forget_ticks. Weights sum to 1.0.
  struct Search {
    double score_base = 0.55;
    double distance_weight = 0.25;
    double age_weight = 0.20;
  } search;

  // Help ally (M10): an ally inside help.radius is "in need" when its
  // health fraction is below ally_health_below or it took damage on the
  // previous tick; helping is available when the helper's local strength
  // ratio (visible allies + 1) / (allies + enemies + 1) is at least
  // strength_ratio_min. The helper moves toward the neediest visible ally.
  //   score = score_base
  //         + health_weight   * (1 - ally_health_fraction)
  //         + distance_weight * (1 - clamp(ally_distance / radius, 0, 1))
  // Weights sum to 1.0.
  struct Help {
    double radius = 40.0;
    double ally_health_below = 0.5;
    double strength_ratio_min = 0.8;
    double score_base = 0.50;
    double health_weight = 0.30;
    double distance_weight = 0.20;
    // m13 item 2 (--ai-avail v2): at most max_helpers_per_ally units may
    // choose Help toward the same ally in one tick (decisions run in unit
    // index order, so the cap is deterministic), and a routed ally farther
    // than routed_help_enemy_distance from any living enemy is never a
    // Help target (it is out of the fight, not in danger). Optional in
    // data files; these are the defaults.
    int max_helpers_per_ally = 2;
    double routed_help_enemy_distance = 30.0;
  } help;

  // Flank (M10): when the unit's squad has a frontally engaged enemy group
  // (at least one member sees an enemy) and the unit's local strength
  // ratio is at least outnumber_ratio, a member may flank: it first moves
  // to a point offset_distance world units perpendicular to the
  // squad-centroid -> enemy-centroid line (the side is fixed per member:
  // odd member index takes the left of the advance axis, even the right),
  // then approaches. At most max_per_squad soldiers per squad may ever
  // flank — the first max_per_squad soldiers in member order, so the cap
  // is deterministic.
  //   score = score_base + strength_weight * clamp(strength_ratio, 0, 1)
  // available when the local ratio (visible allies + 1) / (visible
  // enemies + 1) is at least outnumber_ratio.
  struct Flank {
    std::uint32_t max_per_squad = 4;   // 1..squad size
    double outnumber_ratio = 1.25;     // > 1
    double offset_distance = 25.0;     // > 0
    double score_base = 0.65;
    double strength_weight = 0.20;
  } flank;

  // Pursuit of routed enemies (M10): a bounded bonus on Approach when the
  // nearest visible enemy is routed (fleeing):
  //   bonus = routed_bonus * (speed >= cavalry_speed_threshold
  //                             ? cavalry_bonus_scale : 1.0)
  // bounded by both pursuit.max_distance (world units from the point the
  // pursuit started) and pursuit.max_ticks (consecutive pursuit ticks);
  // past either bound the bonus is zero and normal Advance resumes.
  struct Pursuit {
    double routed_bonus = 0.25;          // [0, 1]
    double cavalry_bonus_scale = 2.0;    // >= 1
    double cavalry_speed_threshold = 4.0;
    double max_distance = 300.0;         // > 0
    std::uint64_t max_ticks = 240;
  } pursuit;

  // Regroup score term (M10): a bonus on the Advance score (whose movement
  // already steers members back to their formation slots) when the squad's
  // cohesion is below cohesion_below and no enemy is visible:
  //   advance += bonus_max * (1 - cohesion)   (then clamped to [0, 1])
  struct Regroup {
    double cohesion_below = 0.35;  // (0, 1]
    double bonus_max = 0.20;       // [0, 1]
  } regroup;

  // Contact seeking (M10 continuation): a faction-level, delayed estimate
  // of the enemy centroid — the mean position of the nearest enemies its
  // members saw, updated whenever any member sees an enemy. A unit that
  // has seen no enemy for idle_ticks consecutive ticks escalates its
  // freshest contact to that estimate (while it is no older than
  // estimate_max_age ticks): Search (or Advance, with no fresh contact)
  // then heads for the estimate instead of a stale personal memory.
  // Because both factions' estimates point into the contact zone, armies
  // that passed each other converge back on it instead of standing on
  // ghost memories or sitting at swapped objectives forever.
  // --contact-seek off disables it (Part 1 behavior).
  struct ContactSeek {
    std::uint64_t idle_ticks = 100;     // >= 1
    std::uint64_t estimate_max_age = 2400;  // >= 1
  } contact_seek;
};

// Parses a tactics configuration from JSON text (see data/tactics.json for
// the shape). Throws std::invalid_argument with a descriptive message for
// every invalid input — malformed JSON, wrong types, missing or unknown
// fields, non-positive lifetimes, weights outside [0, 1] or not summing
// to 1.0 (within 1e-9), radii/distances <= 0, outnumber_ratio <= 1, or a
// contact cap outside 1..8. Never silently defaults. Performs no file I/O.
[[nodiscard]] TacticsConfig parse_tactics(std::string_view json_text);

// Same parsing and validation, reading from any std::istream.
[[nodiscard]] TacticsConfig parse_tactics(std::istream& stream);

// The built-in default tactics configuration, identical to the shipped
// data/tactics.json. Static storage; do not modify.
[[nodiscard]] const TacticsConfig& default_tactics_config() noexcept;

// ---------------------------------------------------------------------------
// Counter-based battle randomness (M10).
//
// The M1-M9 simulation has no battle-time randomness at all (the spawn RNG
// is the only stream), which is why --variance off reproduces every
// earlier reference byte-for-byte. With variance on, every roll comes from
// this pure function instead: keyed by (seed, unit id, tick, channel) and
// nothing else — there is no shared sequence, so no result can depend on
// iteration order and shuffling the unit vector cannot change any roll.
//
// Channels (documented in docs/ai.md):
//   0  combat hit roll        (compared against the type's accuracy)
//   1  combat damage roll     (damage variance spread)
//   2  cooldown jitter roll   (post-attack cooldown spread)
//   3  decision interval draw (spawn-time, 1..5 ticks)
//   4  composition range draw (scenario army sizing)
//   5  spawn pattern jitter   (cluster pattern placement)
[[nodiscard]] inline std::uint64_t tactics_mix64(std::uint64_t z) noexcept {
  z += 0x9E3779B97F4A7C15ULL;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

// One uniform double in [0, 1) for (seed, unit id, tick, channel).
[[nodiscard]] inline double tactics_roll(std::uint64_t seed,
                                         std::uint32_t unit_id,
                                         std::uint64_t tick,
                                         std::uint32_t channel) noexcept {
  std::uint64_t z = tactics_mix64(seed ^ (0xD1B54A32D192ED03ULL * channel));
  z = tactics_mix64(z + unit_id);
  z = tactics_mix64(z + tick);
  return static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
}

// The unit's decision interval (M10): 1..5 ticks, drawn once at spawn from
// channel 3 with tick 0 (a spawn-time property, like the personality).
[[nodiscard]] inline std::uint32_t decision_interval_of(
    std::uint64_t seed, std::uint32_t unit_id) noexcept {
  const double roll = tactics_roll(seed, unit_id, 0, 3);
  return 1 + static_cast<std::uint32_t>(roll * 5.0) % 5;
}

// ---------------------------------------------------------------------------
// Pure action-score helpers (M10). All inputs are clamped by the caller;
// all outputs are finite and in [0, 1].

// Search score (see TacticsConfig::Search for the formula).
[[nodiscard]] double search_score(double contact_distance, double sight,
                                  std::uint64_t contact_age,
                                  std::uint64_t forget_ticks,
                                  const TacticsConfig& config) noexcept;

// Help score (see TacticsConfig::Help).
[[nodiscard]] double help_score(double ally_health_fraction,
                                double ally_distance, double radius,
                                const TacticsConfig& config) noexcept;

// Flank score (see TacticsConfig::Flank).
[[nodiscard]] double flank_score(double strength_ratio,
                                 const TacticsConfig& config) noexcept;

// Pursuit bonus (see TacticsConfig::Pursuit): routed_bonus scaled for
// cavalry-speed types, 0 when the target is not routed.
[[nodiscard]] double pursuit_bonus(bool target_routed, double speed,
                                   const TacticsConfig& config) noexcept;

// The regroup bonus on Advance (see TacticsConfig::Regroup): 0 unless the
// cohesion is below the threshold and no enemy is visible.
[[nodiscard]] double regroup_bonus(double cohesion, bool enemy_visible,
                                   const TacticsConfig& config) noexcept;

}  // namespace sim
