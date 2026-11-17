// m13 item 3 — combat depth: the config parser and the arc helper.
// See combat.hpp. The parser follows the house style (unknown fields and
// out-of-range values are rejected with a message naming the field).
#include "sim/combat.hpp"

#include <cmath>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace sim {
namespace {

constexpr double kPi = 3.14159265358979323846;

[[noreturn]] void fail(const std::string& message) {
  throw std::invalid_argument("combat: " + message);
}

const nlohmann::json& section_of(const nlohmann::json& root,
                                 const char* name) {
  if (!root.contains(name) || !root.at(name).is_object()) {
    fail(std::string("missing '") + name + "' object");
  }
  return root.at(name);
}

double get_number(const nlohmann::json& object, const char* section,
                  const char* field) {
  if (!object.contains(field) || !object.at(field).is_number()) {
    fail(std::string(section) + "." + field + " must be a number");
  }
  return object.at(field).get<double>();
}

std::uint32_t get_uint(const nlohmann::json& object, const char* section,
                       const char* field) {
  const double value = get_number(object, section, field);
  if (value < 0.0 || value > 1e9 || std::floor(value) != value) {
    fail(std::string(section) + "." + field + " must be a whole number");
  }
  return static_cast<std::uint32_t>(value);
}

void reject_unknown(const nlohmann::json& object, const char* section,
                    std::initializer_list<const char*> allowed) {
  for (const auto& [key, value] : object.items()) {
    bool known = false;
    for (const char* field : allowed) {
      if (key == field) {
        known = true;
        break;
      }
    }
    if (!known) {
      fail(std::string("unknown field '") + key + "' in '" + section + "'");
    }
  }
}

}  // namespace

CombatConfig parse_combat(std::string_view json_text) {
  nlohmann::json root;
  try {
    root = nlohmann::json::parse(json_text);
  } catch (const nlohmann::json::exception& error) {
    fail(std::string("invalid JSON (") + error.what() + ")");
  }
  if (!root.is_object()) {
    fail("config must be a JSON object");
  }
  reject_unknown(root, "combat config",
                 {"facing", "charge", "fatigue", "experience", "aura",
                  "stance"});

  CombatConfig config;
  {
    const nlohmann::json& facing = section_of(root, "facing");
    reject_unknown(facing, "facing",
                   {"flank_degrees", "rear_degrees", "flank_multiplier",
                    "rear_multiplier", "rear_morale_loss"});
    config.facing.flank_degrees =
        get_number(facing, "facing", "flank_degrees");
    config.facing.rear_degrees = get_number(facing, "facing", "rear_degrees");
    if (!(config.facing.flank_degrees > 0.0) ||
        config.facing.rear_degrees <= config.facing.flank_degrees ||
        config.facing.rear_degrees >= 180.0) {
      fail("facing needs 0 < flank_degrees < rear_degrees < 180");
    }
    config.facing.flank_multiplier =
        get_number(facing, "facing", "flank_multiplier");
    config.facing.rear_multiplier =
        get_number(facing, "facing", "rear_multiplier");
    if (config.facing.flank_multiplier <= 0.0 ||
        config.facing.rear_multiplier <= 0.0) {
      fail("facing multipliers must be > 0");
    }
    config.facing.rear_morale_loss =
        get_number(facing, "facing", "rear_morale_loss");
    if (config.facing.rear_morale_loss < 0.0) {
      fail("facing.rear_morale_loss must be >= 0");
    }
  }
  {
    const nlohmann::json& charge = section_of(root, "charge");
    reject_unknown(charge, "charge",
                   {"min_speed", "window_ticks", "min_distance",
                    "multiplier", "held_reduction", "dense_line_radius",
                    "dense_line_allies"});
    config.charge.min_speed = get_number(charge, "charge", "min_speed");
    config.charge.window_ticks = get_uint(charge, "charge", "window_ticks");
    if (config.charge.window_ticks == 0 ||
        config.charge.window_ticks > 1000) {
      fail("charge.window_ticks must be in [1, 1000]");
    }
    config.charge.min_distance =
        get_number(charge, "charge", "min_distance");
    config.charge.multiplier = get_number(charge, "charge", "multiplier");
    config.charge.held_reduction =
        get_number(charge, "charge", "held_reduction");
    config.charge.dense_line_radius =
        get_number(charge, "charge", "dense_line_radius");
    config.charge.dense_line_allies =
        get_uint(charge, "charge", "dense_line_allies");
    if (config.charge.min_speed < 0.0 || config.charge.min_distance < 0.0 ||
        config.charge.multiplier <= 0.0 ||
        config.charge.held_reduction < 0.0 || config.charge.held_reduction > 1.0 ||
        config.charge.dense_line_radius < 0.0) {
      fail("charge values out of range (multiplier > 0, held_reduction "
           "in [0, 1], distances >= 0)");
    }
  }
  {
    const nlohmann::json& fatigue = section_of(root, "fatigue");
    reject_unknown(fatigue, "fatigue",
                   {"move_gain_per_tick", "fight_gain_per_attack",
                    "recover_per_tick", "exhausted_level", "speed_factor",
                    "damage_factor", "accuracy_factor"});
    config.fatigue.move_gain_per_tick =
        get_number(fatigue, "fatigue", "move_gain_per_tick");
    config.fatigue.fight_gain_per_attack =
        get_number(fatigue, "fatigue", "fight_gain_per_attack");
    config.fatigue.recover_per_tick =
        get_number(fatigue, "fatigue", "recover_per_tick");
    config.fatigue.exhausted_level =
        get_number(fatigue, "fatigue", "exhausted_level");
    config.fatigue.speed_factor =
        get_number(fatigue, "fatigue", "speed_factor");
    config.fatigue.damage_factor =
        get_number(fatigue, "fatigue", "damage_factor");
    config.fatigue.accuracy_factor =
        get_number(fatigue, "fatigue", "accuracy_factor");
    if (config.fatigue.move_gain_per_tick < 0.0 ||
        config.fatigue.fight_gain_per_attack < 0.0 ||
        config.fatigue.recover_per_tick < 0.0 ||
        config.fatigue.exhausted_level <= 0.0 ||
        config.fatigue.speed_factor <= 0.0 ||
        config.fatigue.speed_factor > 1.0 ||
        config.fatigue.damage_factor <= 0.0 ||
        config.fatigue.damage_factor > 1.0 ||
        config.fatigue.accuracy_factor <= 0.0 ||
        config.fatigue.accuracy_factor > 1.0) {
      fail("fatigue values out of range (gains >= 0, exhausted_level > 0, "
           "factors in (0, 1])");
    }
  }
  {
    const nlohmann::json& experience = section_of(root, "experience");
    reject_unknown(experience, "experience",
                   {"gain_per_kill", "gain_per_engaged_tick",
                    "accuracy_bonus_max", "morale_resist_max"});
    config.experience.gain_per_kill =
        get_number(experience, "experience", "gain_per_kill");
    config.experience.gain_per_engaged_tick =
        get_number(experience, "experience", "gain_per_engaged_tick");
    config.experience.accuracy_bonus_max =
        get_number(experience, "experience", "accuracy_bonus_max");
    config.experience.morale_resist_max =
        get_number(experience, "experience", "morale_resist_max");
    if (config.experience.gain_per_kill < 0.0 ||
        config.experience.gain_per_engaged_tick < 0.0 ||
        config.experience.accuracy_bonus_max < 0.0 ||
        config.experience.morale_resist_max < 0.0 ||
        config.experience.accuracy_bonus_max > 1.0 ||
        config.experience.morale_resist_max > 1.0) {
      fail("experience values out of range (gains >= 0, caps in [0, 1])");
    }
  }
  {
    const nlohmann::json& aura = section_of(root, "aura");
    reject_unknown(aura, "aura", {"damage_bonus", "morale_reduction"});
    config.aura.damage_bonus = get_number(aura, "aura", "damage_bonus");
    config.aura.morale_reduction =
        get_number(aura, "aura", "morale_reduction");
    if (config.aura.damage_bonus < 0.0 || config.aura.damage_bonus > 1.0 ||
        config.aura.morale_reduction < 0.0 ||
        config.aura.morale_reduction > 1.0) {
      fail("aura values must be in [0, 1]");
    }
  }
  {
    const nlohmann::json& stance = section_of(root, "stance");
    reject_unknown(stance, "stance",
                   {"cohesion_min", "damage_factor", "morale_resist"});
    config.stance.cohesion_min = get_number(stance, "stance", "cohesion_min");
    config.stance.damage_factor =
        get_number(stance, "stance", "damage_factor");
    config.stance.morale_resist =
        get_number(stance, "stance", "morale_resist");
    if (config.stance.cohesion_min < 0.0 || config.stance.cohesion_min > 1.0 ||
        config.stance.damage_factor <= 0.0 ||
        config.stance.damage_factor > 1.0 ||
        config.stance.morale_resist < 0.0 ||
        config.stance.morale_resist > 1.0) {
      fail("stance values out of range (cohesion_min in [0, 1], "
           "damage_factor in (0, 1], morale_resist in [0, 1])");
    }
  }
  return config;
}

AttackArc attack_arc(double target_facing_radians, double attacker_dx,
                     double attacker_dy,
                     const CombatConfig& config) noexcept {
  AttackArc arc;
  const double length = std::sqrt(attacker_dx * attacker_dx +
                                  attacker_dy * attacker_dy);
  if (length <= 0.0) {
    return arc;  // coincident: treat as frontal
  }
  const double to_attacker = std::atan2(attacker_dy, attacker_dx);
  double delta = std::abs(to_attacker - target_facing_radians);
  while (delta > kPi) {
    delta = 2.0 * kPi - delta;  // wrap to [0, pi]
  }
  const double flank =
      config.facing.flank_degrees * kPi / 180.0;
  const double rear = config.facing.rear_degrees * kPi / 180.0;
  if (delta <= flank) {
    return arc;  // frontal
  }
  if (delta <= rear) {
    arc.flank = true;
    arc.multiplier = config.facing.flank_multiplier;
    return arc;
  }
  arc.rear = true;
  arc.multiplier = config.facing.rear_multiplier;
  return arc;
}

}  // namespace sim
