#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>

#include "sim/ai.hpp"
#include "sim/unit.hpp"

namespace sim {

// The five morale states (M5), derived from the morale value via the
// thresholds in data/morale.json. A Broken unit is routed: it flees and
// cannot fight until it recovers (docs/morale.md).
enum class MoraleState : std::uint8_t {
  Confident = 0,
  Steady = 1,
  Worried = 2,
  Shaken = 3,
  Broken = 4,
};

[[nodiscard]] const char* to_string(MoraleState state) noexcept;

// One morale configuration, loaded from data/morale.json. Every field is
// required; validation rejects missing fields, non-finite or negative rates,
// unordered state thresholds, and a recovery threshold that does not lie
// strictly above the Broken threshold (the hysteresis requirement).
struct MoraleConfig {
  // Starting morale: base + bravery_scale * bravery, per unit.
  struct Start {
    double base = 40.0;
    double bravery_scale = 40.0;
  } start;

  // State thresholds on the morale value [0, 100]: Confident >= confident_min
  // > Steady >= steady_min > Worried >= worried_min > Shaken >= shaken_min
  // > Broken. shaken_min is the breaking value.
  struct States {
    double confident_min = 75.0;
    double steady_min = 50.0;
    double worried_min = 30.0;
    double shaken_min = 12.0;
  } states;

  // Loss sources. All losses are scaled by the personality factor
  // clamp(1 + caution_amplifier*caution - bravery_shield*bravery,
  // min_scale, 2): bravery shields, caution amplifies. `ally_rout` and
  // `routed_ally_presence_per_tick` are the rout-contagion channels: seeing a
  // nearby ally break drains morale once (one-tick delay), and a routed
  // ally streaming past keeps draining it every tick — that is what lets a
  // local collapse spread army-wide instead of staying local.
  struct Loss {
    double ally_death = 10.0;              // per ally death within sight
    double ally_rout = 3.0;                // per ally rout within sight
    double routed_ally_presence_per_tick = 0.3;  // per routed ally in sight
    double outnumbered_per_tick = 0.4;     // strength ratio below threshold
    double damage_per_health_fraction = 25.0;  // per fraction of max health lost
    double isolation_per_tick = 0.6;       // enemies visible, no allies
    double caution_amplifier = 0.5;
    double bravery_shield = 0.5;
    double min_scale = 0.2;
  } loss;

  // Gain sources (not personality-scaled).
  struct Gain {
    double kill = 4.0;                     // per kill this tick
    double superiority_per_tick = 0.15;    // strength ratio above threshold
    double allies_present_per_tick = 0.05; // allies visible while threatened
    double safe_per_tick = 0.35;           // no enemy in sight
  } gain;

  // Strength-ratio thresholds for the outnumbered loss and the superiority
  // gain; the ratio is (visible allies + 1) / (allies + enemies + 1).
  struct Thresholds {
    double outnumbered_below = 0.4;
    double superiority_above = 0.75;
  } thresholds;

  // Recovery from routing: a routed unit rejoins (as Shaken or better) only
  // after at least safe_ticks consecutive ticks with no enemy in sight AND
  // morale strictly above morale_threshold, which must exceed shaken_min —
  // that gap is the hysteresis that prevents break/recover flip-flopping.
  struct Recovery {
    double morale_threshold = 35.0;
    std::uint64_t safe_ticks = 120;
  } recovery;

  // Utility score modifiers by state: Confident adds confident_bonus to
  // attack/approach (and subtracts it from retreat); Worried and Shaken
  // subtract their penalty from attack/approach and add it to retreat.
  // Steady modifies nothing; Broken never scores (it flees).
  struct Ai {
    double confident_bonus = 0.05;
    double worried_penalty = 0.10;
    double shaken_penalty = 0.25;
  } ai;

  // Per-tick bounds on the total morale change (losses may exceed the cap;
  // the excess then takes further ticks to bite, making morale "heavy").
  double max_loss_per_tick = 10.0;
  double max_gain_per_tick = 2.5;

  // A faction ends (FACTION_ROUTED) after this many consecutive ticks with
  // no active units (all dead or routed).
  std::uint64_t rout_timeout_ticks = 30;
};

// Parses a morale configuration from JSON text (see data/morale.json for the
// shape). Throws std::invalid_argument with a descriptive message for every
// invalid input. Never silently defaults. Performs no file I/O.
[[nodiscard]] MoraleConfig parse_morale(std::string_view json_text);

// Same parsing and validation, reading from any std::istream.
[[nodiscard]] MoraleConfig parse_morale(std::istream& stream);

// The built-in default configuration, identical to the shipped
// data/morale.json. Static storage; do not modify.
[[nodiscard]] const MoraleConfig& default_morale_config() noexcept;

// The state a morale value maps to (thresholds from the config).
[[nodiscard]] MoraleState state_from_morale(
    double morale, const MoraleConfig::States& states) noexcept;

// Starting morale for one unit from its bravery (clamped to [0, 100]).
[[nodiscard]] double starting_morale(
    double bravery, const MoraleConfig::Start& start) noexcept;

// Everything one unit's morale change is computed from, all taken from the
// tick-start snapshot plus this tick's events (docs/morale.md).
struct MoraleInputs {
  bool enemies_visible = false;
  bool allies_visible = false;
  double strength_ratio = 1.0;  // (visible allies + 1) / (allies + enemies + 1)
  double damage_fraction = 0.0; // damage taken this tick / max health
  std::uint32_t kills = 0;      // kills credited this tick
  std::uint32_t ally_deaths = 0;  // ally deaths within sight this tick
  std::uint32_t ally_routs = 0;   // ally routs within sight (previous tick)
  std::uint32_t routed_allies = 0;  // routed allies within sight right now
  double bravery = 0.5;
  double caution = 0.5;
};

// The bounded morale change for one tick. Deterministic pure function of the
// inputs; the result is finite and lies in [-max_loss_per_tick,
// +max_gain_per_tick] by construction.
[[nodiscard]] double morale_delta(const MoraleInputs& inputs,
                                  const MoraleConfig& config) noexcept;

// Applies the morale-state score modifiers to already computed action scores
// (in place). Unavailable actions (-1) are left untouched; modified scores
// stay in [0, 1].
void apply_morale_modifiers(ActionScores& scores, MoraleState state,
                            const MoraleConfig::Ai& ai) noexcept;

}  // namespace sim
