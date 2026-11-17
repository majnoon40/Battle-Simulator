#include "sim/config.hpp"

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "sim/factions.hpp"
#include "sim/spatial_hash.hpp"
#include "sim/unit_types.hpp"

namespace sim {

const char* to_string(NeighborMode mode) noexcept {
  switch (mode) {
    case NeighborMode::Brute:
      return "brute";
    case NeighborMode::Grid:
      return "grid";
  }
  return "unknown";
}

const char* to_string(AiMode mode) noexcept {
  switch (mode) {
    case AiMode::Legacy:
      return "legacy";
    case AiMode::Utility:
      return "utility";
  }
  return "unknown";
}

namespace {

// m13 audit finding 6: every floating-point statistic must be finite —
// the bare range comparisons passed +inf (inf > 0) and NaN slips were
// only caught where a two-sided comparison happened to reject them.
// This mirrors the unit-type file parser's rules (finite, plus the
// documented range per field) so the public validate_config is safe
// independently of JSON parsing.
void validate_unit_stats(const UnitStats& stats, const char* name) {
  const auto reject = [&](const char* field, const char* rule) {
    throw std::invalid_argument(std::string(name) + "." + field +
                                " must be " + rule);
  };
  if (!std::isfinite(stats.max_health) || !(stats.max_health > 0.0)) {
    reject("max_health", "a finite number > 0");
  }
  if (!std::isfinite(stats.speed) || !(stats.speed >= 0.0)) {
    reject("speed", "a finite number >= 0");
  }
  if (!std::isfinite(stats.damage) || !(stats.damage >= 0.0)) {
    reject("damage", "a finite number >= 0");
  }
  if (!std::isfinite(stats.attack_range) || !(stats.attack_range >= 0.0)) {
    reject("attack_range", "a finite number >= 0");
  }
  if (stats.attack_cooldown_ticks < 1) {
    reject("attack_cooldown_ticks", ">= 1");
  }
  if (!std::isfinite(stats.sight_radius) || !(stats.sight_radius > 0.0)) {
    reject("sight_radius", "a finite number > 0");
  }
  if (!std::isfinite(stats.leadership_radius) ||
      !(stats.leadership_radius >= 0.0)) {
    reject("leadership_radius", "a finite number >= 0");
  }
  if (!std::isfinite(stats.accuracy) || !(stats.accuracy >= 0.0) ||
      !(stats.accuracy <= 1.0)) {
    reject("accuracy", "a finite number in [0, 1]");
  }
  if (!std::isfinite(stats.damage_variance) ||
      !(stats.damage_variance >= 0.0) ||
      !(stats.damage_variance <= 1.0)) {
    reject("damage_variance", "a finite number in [0, 1]");
  }
  if (!std::isfinite(stats.cooldown_jitter) ||
      !(stats.cooldown_jitter >= 0.0) ||
      !(stats.cooldown_jitter <= 1.0)) {
    reject("cooldown_jitter", "a finite number in [0, 1]");
  }
}

void validate_composition(const SimulationConfig& config) {
  if (config.unit_types == nullptr) {
    throw std::invalid_argument(
        "a composition requires a unit type table (config.unit_types)");
  }
  for (Faction faction : kAllFactions) {
    const std::size_t index = static_cast<std::size_t>(faction);
    const std::string name = to_string(faction);
    if (config.composition[index].empty()) {
      throw std::invalid_argument(
          "when either army has a composition, both armies need one (" +
          name + " is empty)");
    }
    std::uint64_t army_total = 0;
    for (const TypeCount& entry : config.composition[index]) {
      if (entry.count == 0) {
        throw std::invalid_argument("composition entries must have count >= 1");
      }
      if (entry.type_index >= config.unit_types->size()) {
        throw std::invalid_argument(
            "composition type index " +
            std::to_string(entry.type_index) + " is out of range (table has " +
            std::to_string(config.unit_types->size()) + " types)");
      }
      army_total += entry.count;
      if (army_total > kMaxUnits) {
        throw std::invalid_argument(
            name + " army needs " + std::to_string(army_total) +
            " units, which exceeds the " + std::to_string(kMaxUnits) +
            "-unit cap");
      }
    }
  }
  std::uint64_t total = 0;
  for (const auto& army : config.composition) {
    for (const TypeCount& entry : army) {
      total += entry.count;
    }
  }
  if (total > kMaxUnits) {
    throw std::invalid_argument("composition needs " + std::to_string(total) +
                                " units in total, which exceeds the " +
                                std::to_string(kMaxUnits) + "-unit cap");
  }
}

}  // namespace

ArmySizes split_armies(std::uint32_t total_units) noexcept {
  ArmySizes sizes{};
  sizes.red = (total_units + 1) / 2;  // Red receives the extra odd unit
  sizes.blue = total_units - sizes.red;
  return sizes;
}

void validate_config(const SimulationConfig& config) {
  const bool composed = uses_composition(config);
  if (!composed) {
    if (config.total_units < 2) {
      throw std::invalid_argument(
          "total_units must be >= 2 (at least one unit per faction)");
    }
    if (config.total_units > kMaxUnits) {
      throw std::invalid_argument("total_units must not exceed " +
                                  std::to_string(kMaxUnits) + " (unit cap)");
    }
  }
  if (config.max_ticks < 1) {
    throw std::invalid_argument("max_ticks must be >= 1");
  }
  // m13 audit finding 10: battlefield dimensions must be finite and
  // strictly positive — the bare > 0 checks passed +inf (and everything
  // derived from an infinite battlefield would be non-finite too).
  if (!std::isfinite(config.battlefield_width) ||
      !(config.battlefield_width > 0.0)) {
    throw std::invalid_argument(
        "battlefield_width must be a finite number > 0");
  }
  if (!std::isfinite(config.battlefield_height) ||
      !(config.battlefield_height > 0.0)) {
    throw std::invalid_argument(
        "battlefield_height must be a finite number > 0");
  }
  if (!(config.spawn_band_fraction > 0.0) ||
      !(config.spawn_band_fraction <= 0.5)) {
    throw std::invalid_argument(
        "spawn_band_fraction must be in (0, 0.5] so the armies start apart");
  }
  if (composed) {
    validate_composition(config);
  } else {
    validate_unit_stats(config.red_stats, "red_stats");
    validate_unit_stats(config.blue_stats, "blue_stats");
    if (config.ai_mode == AiMode::Utility) {
      // A utility unit can only attack what it can see, so sight must reach
      // at least as far as the weapon (unit type files enforce the same rule
      // per type at parse time).
      const auto check_sight = [&](const UnitStats& stats, const char* name) {
        if (!(stats.sight_radius > stats.attack_range)) {
          throw std::invalid_argument(
              std::string(name) + ".sight_radius must exceed attack_range " +
              "(got sight " + std::to_string(stats.sight_radius) + ", range " +
              std::to_string(stats.attack_range) + ")");
        }
      };
      check_sight(config.red_stats, "red_stats");
      check_sight(config.blue_stats, "blue_stats");
    }
  }
  if (!(config.action_stickiness >= 0.0) ||
      !(config.action_stickiness < 1.0) ||
      !std::isfinite(config.action_stickiness)) {
    throw std::invalid_argument(
        "action_stickiness must be in [0, 1) (a small score bonus)");
  }
  for (Faction faction : kAllFactions) {
    const ObjectivePoint& objective =
        config.objectives[static_cast<std::size_t>(faction)];
    if (objective.explicit_set) {
      const std::string name = to_string(faction);
      if (!std::isfinite(objective.x) || !std::isfinite(objective.y)) {
        throw std::invalid_argument(name +
                                    " objective must be finite numbers");
      }
      if (!(objective.x >= 0.0) ||
          !(objective.x <= config.battlefield_width) ||
          !(objective.y >= 0.0) ||
          !(objective.y <= config.battlefield_height)) {
        throw std::invalid_argument(
            name + " objective must lie on the battlefield [0, width] x "
            "[0, height]");
      }
    }
  }
  // Utility-mode perception always uses the spatial grid (radius queries),
  // so the grid configuration must be valid there even in brute neighbor
  // mode.
  if (config.neighbor_mode == NeighborMode::Grid ||
      config.ai_mode == AiMode::Utility) {
    if (!(config.grid_cell_size > 0.0) ||
        !std::isfinite(config.grid_cell_size)) {
      throw std::invalid_argument(
          "grid_cell_size must be a finite number > 0");
    }
    const double cells =
        std::ceil(config.battlefield_width / config.grid_cell_size) *
        std::ceil(config.battlefield_height / config.grid_cell_size);
    if (!std::isfinite(cells) || cells > static_cast<double>(kMaxGridCells)) {
      throw std::invalid_argument(
          "grid_cell_size " + std::to_string(config.grid_cell_size) +
          " needs too many cells for this battlefield (limit " +
          std::to_string(kMaxGridCells) + ")");
    }
  }

  // Squads (M6): a utility-mode feature. Plans are validated against the
  // composition only when squads actually run; in legacy mode or with squads
  // disabled the plans are inert (legacy must keep reproducing M1-M3 and
  // --squads off must reproduce M5 exactly). A faction with a plan must
  // field exactly the matching army: `count` commander-type units (types
  // with leadership_radius > 0) and count * size soldiers. No silent
  // defaults — a mismatched scenario refuses to run.
  const bool any_plan =
      config.squad_plans[static_cast<std::size_t>(Faction::Red)].has_value() ||
      config.squad_plans[static_cast<std::size_t>(Faction::Blue)].has_value();
  if (any_plan && config.ai_mode == AiMode::Utility && config.squads_enabled) {
    if (!composed) {
      throw std::invalid_argument(
          "squad plans require a composition (a unit type table with "
          "commander types)");
    }
    for (Faction faction : kAllFactions) {
      const std::size_t index = static_cast<std::size_t>(faction);
      const auto& plan = config.squad_plans[index];
      if (!plan) {
        continue;
      }
      const std::string name = to_string(faction);
      if (plan->count < 1 || plan->size < 1) {
        throw std::invalid_argument(name +
                                    " squad plan needs count >= 1 and size >= 1");
      }
      std::uint64_t commanders = 0;
      std::uint64_t soldiers = 0;
      for (const TypeCount& entry : config.composition[index]) {
        if (config.unit_types->types[entry.type_index].stats
                .leadership_radius > 0.0) {
          commanders += entry.count;
        } else {
          soldiers += entry.count;
        }
      }
      if (commanders != plan->count) {
        throw std::invalid_argument(
            name + " squad plan wants " + std::to_string(plan->count) +
            " commanders (one per squad) but the composition has " +
            std::to_string(commanders));
      }
      const std::uint64_t expected_soldiers =
          static_cast<std::uint64_t>(plan->count) * plan->size;
      if (soldiers != expected_soldiers) {
        throw std::invalid_argument(
            name + " squad plan wants " +
            std::to_string(expected_soldiers) + " soldiers (" +
            std::to_string(plan->count) + " squads of " +
            std::to_string(plan->size) + ") but the composition has " +
            std::to_string(soldiers));
      }
    }
  }
}

}  // namespace sim
