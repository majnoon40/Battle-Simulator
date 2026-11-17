#pragma once

// m13 item 3 — combat depth (--combat v2, the default since 13.1; v1 is
// the milestone-12 combat, byte-for-byte). One validated data file,
// data/combat.json, drives every value:
//
//   facing    — units have a heading (their last movement direction;
//               spawned facing their objective). Attacks into the
//               target's flank arc deal x flank_multiplier, into the rear
//               arc x rear_multiplier, and rear hits cost the target
//               extra morale.
//   charge    — a fast unit (type speed >= min_speed) that covered at
//               least min_distance toward its target over the last
//               window_ticks ticks deals x multiplier on its first hit,
//               then its run resets. A target on Hold or in a dense line
//               takes the bonus x held_reduction.
//   fatigue   — [0, 1]; grows on full-speed moves and attacks, recovers
//               when idle. At exhausted_level the unit fights and moves
//               worse.
//   experience— [0, 1]; grows with kills and engaged ticks, grants
//               accuracy and morale-loss resistance.
//   aura      — a commander (a unit with leadership_radius) projects its
//               aura over same-faction units inside that radius: more
//               damage, less morale loss.
//   stance    — a unit whose squad holds the Hold order (or that
//               chooses Hold) with cohesion above cohesion_min takes a
//               defensive stance: incoming damage x damage_factor,
//               morale-loss resistance, and the charge reduction above.
//
// All hooks are pure functions of unit state; no RNG is added, so v2
// stays exactly as deterministic as v1 (same seed, same bytes; --threads
// and --replay keep working unchanged).

#include <cstdint>
#include <string_view>

namespace sim {

struct CombatConfig {
  struct Facing {
    double flank_degrees = 60.0;   // front | flank boundary (from front)
    double rear_degrees = 120.0;   // flank | rear boundary
    double flank_multiplier = 1.25;
    double rear_multiplier = 1.5;
    double rear_morale_loss = 2.0;  // extra morale points per rear hit
  } facing;
  struct Charge {
    double min_speed = 4.0;  // "cavalry": unit type speed threshold
    std::uint32_t window_ticks = 10;
    double min_distance = 20.0;  // run distance toward the target
    double multiplier = 1.5;
    double held_reduction = 0.5;  // target on Hold / dense line
    double dense_line_radius = 3.0;
    std::uint32_t dense_line_allies = 3;
  } charge;
  struct Fatigue {
    double move_gain_per_tick = 1.0 / 500.0;
    double fight_gain_per_attack = 1.0 / 40.0;
    double recover_per_tick = 1.0 / 300.0;
    double exhausted_level = 1.0;  // the level where the factors bite
    double speed_factor = 0.7;
    double damage_factor = 0.85;
    double accuracy_factor = 0.85;
  } fatigue;
  struct Experience {
    double gain_per_kill = 0.05;
    double gain_per_engaged_tick = 1.0 / 2000.0;
    double accuracy_bonus_max = 0.10;
    double morale_resist_max = 0.10;
  } experience;
  struct Aura {
    double damage_bonus = 0.05;
    double morale_reduction = 0.25;
  } aura;
  struct Stance {
    double cohesion_min = 0.6;
    double damage_factor = 0.8;  // incoming damage multiplier
    double morale_resist = 0.2;
  } stance;
};

// The built-in defaults (used when SimulationConfig::combat is null).
[[nodiscard]] inline CombatConfig default_combat_config() noexcept {
  return CombatConfig{};
}

// Parses and validates a combat configuration (see data/combat.json).
// Throws std::invalid_argument with a field-naming message on any error.
[[nodiscard]] CombatConfig parse_combat(std::string_view json_text);

// The attack arc at the TARGET: the angle between the target's facing
// and the direction from the target to the attacker (the attacker's
// offset dx/dy from the target). At most flank_degrees from front: a
// frontal hit (x1). Up to rear_degrees: a flank hit. Beyond: a rear
// hit. A coincident attacker is frontal.
struct AttackArc {
  double multiplier = 1.0;
  bool flank = false;
  bool rear = false;
};
[[nodiscard]] AttackArc attack_arc(double target_facing_radians,
                                   double attacker_dx, double attacker_dy,
                                   const CombatConfig& config) noexcept;

}  // namespace sim
