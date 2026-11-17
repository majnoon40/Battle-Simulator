#include "sim/tactics.hpp"

#include <algorithm>
#include <cmath>
#include <istream>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "nlohmann/json.hpp"

namespace sim {
namespace {

// Every field of data/tactics.json, with its validator. A missing field,
// a wrong type or a value out of range is an error naming the field.
void require(double value, double lo, double hi, const std::string& name) {
  if (!std::isfinite(value) || value < lo || value > hi) {
    throw std::invalid_argument("tactics: " + name + " must be a finite "
                                "number in [" + std::to_string(lo) + ", " +
                                std::to_string(hi) + "], got " +
                                std::to_string(value));
  }
}

double get_number(const nlohmann::json& parent, const std::string& name) {
  if (!parent.contains(name) || !parent.at(name).is_number()) {
    throw std::invalid_argument("tactics: missing or non-numeric field '" +
                                name + "'");
  }
  return parent.at(name).get<double>();
}

void require_weights(double a, double b, double c, const std::string& what) {
  const double sum = a + b + c;
  if (std::fabs(sum - 1.0) > 1e-9) {
    throw std::invalid_argument("tactics: " + what + " weights must sum to "
                                "1.0, got " + std::to_string(sum));
  }
}

}  // namespace

TacticsConfig parse_tactics(std::string_view json_text) {
  nlohmann::json root;
  try {
    root = nlohmann::json::parse(json_text);
  } catch (const nlohmann::json::exception& error) {
    throw std::invalid_argument(std::string("tactics: ") + error.what());
  }
  if (!root.is_object()) {
    throw std::invalid_argument("tactics: root must be an object");
  }
  TacticsConfig config;

  if (!root.contains("memory") || !root.at("memory").is_object()) {
    throw std::invalid_argument("tactics: missing 'memory' object");
  }
  const auto& memory = root.at("memory");
  {
    const double forget = get_number(memory, "forget_ticks");
    const double delay = get_number(memory, "squad_intel_delay_ticks");
    const double cap = get_number(memory, "squad_intel_max_contacts");
    if (forget < 1.0 || std::floor(forget) != forget) {
      throw std::invalid_argument("tactics: memory.forget_ticks must be a "
                                  "positive integer");
    }
    if (delay < 0.0 || std::floor(delay) != delay) {
      throw std::invalid_argument("tactics: memory.squad_intel_delay_ticks "
                                  "must be a non-negative integer");
    }
    if (cap < 1.0 || cap > 8.0 || std::floor(cap) != cap) {
      throw std::invalid_argument("tactics: memory.squad_intel_max_contacts "
                                  "must be an integer in [1, 8]");
    }
    config.memory.forget_ticks =
        static_cast<std::uint64_t>(forget);
    config.memory.squad_intel_delay_ticks =
        static_cast<std::uint64_t>(delay);
    config.memory.squad_intel_max_contacts =
        static_cast<std::uint32_t>(cap);
  }

  if (!root.contains("search") || !root.at("search").is_object()) {
    throw std::invalid_argument("tactics: missing 'search' object");
  }
  {
    const auto& search = root.at("search");
    config.search.score_base = get_number(search, "score_base");
    config.search.distance_weight = get_number(search, "distance_weight");
    config.search.age_weight = get_number(search, "age_weight");
    require(config.search.score_base, 0.0, 1.0, "search.score_base");
    require(config.search.distance_weight, 0.0, 1.0,
            "search.distance_weight");
    require(config.search.age_weight, 0.0, 1.0, "search.age_weight");
    require_weights(config.search.score_base, config.search.distance_weight,
                    config.search.age_weight, "search");
  }

  if (!root.contains("help") || !root.at("help").is_object()) {
    throw std::invalid_argument("tactics: missing 'help' object");
  }
  {
    const auto& help = root.at("help");
    config.help.radius = get_number(help, "radius");
    config.help.ally_health_below =
        get_number(help, "ally_health_below");
    config.help.strength_ratio_min =
        get_number(help, "strength_ratio_min");
    config.help.score_base = get_number(help, "score_base");
    config.help.health_weight = get_number(help, "health_weight");
    config.help.distance_weight = get_number(help, "distance_weight");
    // m13 item 2: optional cap fields (defaults in the header).
    if (help.contains("max_helpers_per_ally")) {
      const double cap = get_number(help, "max_helpers_per_ally");
      if (cap < 0.0 || cap > 1000.0) {
        throw std::invalid_argument(
            "tactics: help.max_helpers_per_ally must be in [0, 1000]");
      }
      config.help.max_helpers_per_ally = static_cast<int>(cap);
    }
    if (help.contains("routed_help_enemy_distance")) {
      const double distance =
          get_number(help, "routed_help_enemy_distance");
      if (distance < 0.0) {
        throw std::invalid_argument(
            "tactics: help.routed_help_enemy_distance must be >= 0");
      }
      config.help.routed_help_enemy_distance = distance;
    }
    if (!(config.help.radius > 0.0)) {
      throw std::invalid_argument("tactics: help.radius must be > 0");
    }
    require(config.help.ally_health_below, 0.0, 1.0,
            "help.ally_health_below");
    require(config.help.strength_ratio_min, 0.0, 1.0,
            "help.strength_ratio_min");
    require(config.help.score_base, 0.0, 1.0, "help.score_base");
    require(config.help.health_weight, 0.0, 1.0, "help.health_weight");
    require(config.help.distance_weight, 0.0, 1.0, "help.distance_weight");
    require_weights(config.help.score_base, config.help.health_weight,
                    config.help.distance_weight, "help");
  }

  if (!root.contains("flank") || !root.at("flank").is_object()) {
    throw std::invalid_argument("tactics: missing 'flank' object");
  }
  {
    const auto& flank = root.at("flank");
    const double cap = get_number(flank, "max_per_squad");
    config.flank.outnumber_ratio = get_number(flank, "outnumber_ratio");
    config.flank.offset_distance = get_number(flank, "offset_distance");
    config.flank.score_base = get_number(flank, "score_base");
    config.flank.strength_weight = get_number(flank, "strength_weight");
    if (cap < 1.0 || std::floor(cap) != cap) {
      throw std::invalid_argument("tactics: flank.max_per_squad must be a "
                                  "positive integer");
    }
    if (!(config.flank.outnumber_ratio > 1.0)) {
      throw std::invalid_argument("tactics: flank.outnumber_ratio must be "
                                  "> 1");
    }
    if (!(config.flank.offset_distance > 0.0)) {
      throw std::invalid_argument("tactics: flank.offset_distance must be "
                                  "> 0");
    }
    require(config.flank.score_base, 0.0, 1.0, "flank.score_base");
    require(config.flank.strength_weight, 0.0, 1.0, "flank.strength_weight");
    if (config.flank.score_base + config.flank.strength_weight > 1.0) {
      throw std::invalid_argument("tactics: flank weights must stay <= 1");
    }
    config.flank.max_per_squad = static_cast<std::uint32_t>(cap);
  }

  if (!root.contains("pursuit") || !root.at("pursuit").is_object()) {
    throw std::invalid_argument("tactics: missing 'pursuit' object");
  }
  {
    const auto& pursuit = root.at("pursuit");
    config.pursuit.routed_bonus = get_number(pursuit, "routed_bonus");
    config.pursuit.cavalry_bonus_scale =
        get_number(pursuit, "cavalry_bonus_scale");
    config.pursuit.cavalry_speed_threshold =
        get_number(pursuit, "cavalry_speed_threshold");
    config.pursuit.max_distance = get_number(pursuit, "max_distance");
    const double max_ticks = get_number(pursuit, "max_ticks");
    require(config.pursuit.routed_bonus, 0.0, 1.0, "pursuit.routed_bonus");
    if (!(config.pursuit.cavalry_bonus_scale >= 1.0)) {
      throw std::invalid_argument("tactics: pursuit.cavalry_bonus_scale must "
                                  "be >= 1");
    }
    if (!(config.pursuit.max_distance > 0.0)) {
      throw std::invalid_argument("tactics: pursuit.max_distance must be > "
                                  "0");
    }
    if (max_ticks < 1.0 || std::floor(max_ticks) != max_ticks) {
      throw std::invalid_argument("tactics: pursuit.max_ticks must be a "
                                  "positive integer");
    }
    config.pursuit.max_ticks = static_cast<std::uint64_t>(max_ticks);
  }

  if (!root.contains("regroup") || !root.at("regroup").is_object()) {
    throw std::invalid_argument("tactics: missing 'regroup' object");
  }
  {
    const auto& regroup = root.at("regroup");
    config.regroup.cohesion_below = get_number(regroup, "cohesion_below");
    config.regroup.bonus_max = get_number(regroup, "bonus_max");
    if (!(config.regroup.cohesion_below > 0.0 &&
          config.regroup.cohesion_below <= 1.0)) {
      throw std::invalid_argument("tactics: regroup.cohesion_below must be "
                                  "in (0, 1]");
    }
    require(config.regroup.bonus_max, 0.0, 1.0, "regroup.bonus_max");
  }

  if (!root.contains("contact_seek") ||
      !root.at("contact_seek").is_object()) {
    throw std::invalid_argument("tactics: missing 'contact_seek' object");
  }
  {
    const auto& seek = root.at("contact_seek");
    const double idle = get_number(seek, "idle_ticks");
    const double max_age = get_number(seek, "estimate_max_age");
    if (idle < 1.0 || std::floor(idle) != idle) {
      throw std::invalid_argument("tactics: contact_seek.idle_ticks must "
                                  "be a positive integer");
    }
    if (max_age < 1.0 || std::floor(max_age) != max_age) {
      throw std::invalid_argument("tactics: contact_seek.estimate_max_age "
                                  "must be a positive integer");
    }
    config.contact_seek.idle_ticks = static_cast<std::uint64_t>(idle);
    config.contact_seek.estimate_max_age =
        static_cast<std::uint64_t>(max_age);
  }

  return config;
}

TacticsConfig parse_tactics(std::istream& stream) {
  std::string text((std::istreambuf_iterator<char>(stream)),
                   std::istreambuf_iterator<char>());
  return parse_tactics(text);
}

const TacticsConfig& default_tactics_config() noexcept {
  static const TacticsConfig config = TacticsConfig{};
  return config;
}

double search_score(double contact_distance, double sight,
                    std::uint64_t contact_age, std::uint64_t forget_ticks,
                    const TacticsConfig& config) noexcept {
  const double distance_fraction =
      sight > 0.0 ? std::clamp(contact_distance / sight, 0.0, 1.0) : 1.0;
  const double age_fraction =
      forget_ticks > 0
          ? std::clamp(static_cast<double>(contact_age) /
                           static_cast<double>(forget_ticks), 0.0, 1.0)
          : 1.0;
  return std::clamp(config.search.score_base +
                        config.search.distance_weight *
                            (1.0 - distance_fraction) +
                        config.search.age_weight * (1.0 - age_fraction),
                    0.0, 1.0);
}

double help_score(double ally_health_fraction, double ally_distance,
                  double radius, const TacticsConfig& config) noexcept {
  const double distance_fraction =
      radius > 0.0 ? std::clamp(ally_distance / radius, 0.0, 1.0) : 1.0;
  return std::clamp(config.help.score_base +
                        config.help.health_weight *
                            (1.0 - std::clamp(ally_health_fraction, 0.0, 1.0)) +
                        config.help.distance_weight * (1.0 - distance_fraction),
                    0.0, 1.0);
}

double flank_score(double strength_ratio, const TacticsConfig& config) noexcept {
  return std::clamp(config.flank.score_base +
                        config.flank.strength_weight *
                            std::clamp(strength_ratio, 0.0, 1.0),
                    0.0, 1.0);
}

double pursuit_bonus(bool target_routed, double speed,
                     const TacticsConfig& config) noexcept {
  if (!target_routed) {
    return 0.0;
  }
  const double scale = speed >= config.pursuit.cavalry_speed_threshold
                           ? config.pursuit.cavalry_bonus_scale
                           : 1.0;
  return std::clamp(config.pursuit.routed_bonus * scale, 0.0, 1.0);
}

double regroup_bonus(double cohesion, bool enemy_visible,
                     const TacticsConfig& config) noexcept {
  if (enemy_visible || cohesion >= config.regroup.cohesion_below) {
    return 0.0;
  }
  return std::clamp(config.regroup.bonus_max * (1.0 - std::clamp(cohesion, 0.0, 1.0)),
                    0.0, 1.0);
}

}  // namespace sim
