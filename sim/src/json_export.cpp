#include "sim/json_export.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

#include <nlohmann/json.hpp>

#include "sim/factions.hpp"

namespace sim {
namespace {

// Locale-independent, fixed-precision float preparation for export:
// 1. rejects NaN and infinity (they must never appear in exported states),
// 2. quantizes to 6 decimal places,
// 3. normalizes negative zero to +0.0.
// nlohmann::json then serializes doubles with its own deterministic,
// locale-independent shortest round-trip algorithm, and object keys in
// stable (alphabetical) order.
double stable(double value) {
  if (!std::isfinite(value)) {
    throw std::runtime_error(
        "non-finite value in simulation state; refusing to export");
  }
  double quantized = std::round(value * 1e6) / 1e6;
  if (quantized == 0.0) {
    quantized = 0.0;  // normalizes -0.0 and values that round to zero
  }
  return quantized;
}

nlohmann::json unit_stats_json(const UnitStats& stats) {
  nlohmann::json json;
  json["attack_cooldown_ticks"] = stats.attack_cooldown_ticks;
  json["attack_range"] = stable(stats.attack_range);
  json["damage"] = stable(stats.damage);
  json["max_health"] = stable(stats.max_health);
  json["sight_radius"] = stable(stats.sight_radius);
  json["speed"] = stable(stats.speed);
  return json;
}

// M2 unit type entry, using the data-file field names (health, damage,
// speed, range, cooldown, sight_radius) so the exported table mirrors
// units.json. Format 5 adds leadership_radius (M6 commanders).
nlohmann::json unit_type_json(const UnitType& type, bool squads) {
  nlohmann::json json;
  json["cooldown"] = type.stats.attack_cooldown_ticks;
  json["damage"] = stable(type.stats.damage);
  json["health"] = stable(type.stats.max_health);
  json["id"] = type.id;
  if (squads) {
    json["leadership_radius"] = stable(type.stats.leadership_radius);
  }
  json["range"] = stable(type.stats.attack_range);
  json["sight_radius"] = stable(type.stats.sight_radius);
  json["speed"] = stable(type.stats.speed);
  return json;
}

// M4 personality: the traits plus the archetype name. The archetype index
// refers to the table the battle was created with; out-of-range indices
// (explicitly injected personalities) export as "custom". Format 5 adds the
// discipline trait (M6); formats 3-4 keep the exact M4/M5 shape.
nlohmann::json personality_json(const Personality& personality,
                                const PersonalityTable& table, bool squads) {
  nlohmann::json json;
  if (personality.archetype != kCustomArchetype &&
      static_cast<std::size_t>(personality.archetype) < table.size()) {
    json["archetype"] =
        table.archetypes[personality.archetype].id;
  } else {
    json["archetype"] = "custom";
  }
  json["aggression"] = stable(personality.aggression);
  json["bravery"] = stable(personality.bravery);
  json["caution"] = stable(personality.caution);
  if (squads) {
    json["discipline"] = stable(personality.discipline);
  }
  return json;
}

nlohmann::json unit_json(const Unit& unit, const UnitTypeTable* types,
                         const PersonalityTable& personalities, bool morale,
                         const UnitSquadInfo* squad) {
  nlohmann::json json;
  json["alive"] = unit.alive;
  json["faction"] = to_string(unit.faction);
  json["health"] = stable(unit.health);
  json["id"] = unit.id;
  json["last_action"] = to_string(static_cast<Action>(unit.last_action));
  json["max_health"] = stable(unit.stats.max_health);
  if (morale) {
    // M5 fields, present exactly when morale is enabled.
    json["morale"] = stable(unit.morale);
    json["routed"] = unit.routed;
    json["state"] = to_string(static_cast<MoraleState>(unit.morale_state));
  }
  json["personality"] =
      personality_json(unit.personality, personalities, squad != nullptr);
  if (squad != nullptr) {
    // M6 fields (format 5): the member's squad view.
    json["cohesion"] = stable(squad->cohesion);
    json["formation"] =
        squad->squad_id >= 0 ? Simulation::formation_name(squad->formation)
                             : "none";
    json["order"] = to_string(squad->order);
    json["squad"] = squad->squad_id;
  }
  if (types != nullptr) {
    json["type"] = types->types[unit.type_index].id;
  }
  json["x"] = stable(unit.x);
  json["y"] = stable(unit.y);
  return json;
}

nlohmann::json type_report_json(const TypeReport& report) {
  nlohmann::json json;
  json["damage_dealt"] = stable(report.damage_dealt);
  json["kills"] = report.kills;
  json["spawned"] = report.spawned;
  json["survivors"] = report.survivors;
  json["type"] = report.type_id;
  return json;
}

}  // namespace

std::string export_state_json(const Simulation& simulation) {
  const SimulationConfig& config = simulation.config();
  const World& world = simulation.world();
  const BattleResult result = simulation.result();
  const UnitTypeTable* const types = config.unit_types;
  const bool composed =
      types != nullptr && !config.composition[0].empty() &&
      !config.composition[static_cast<std::size_t>(Faction::Blue)].empty();

  // Format 4 (M5, morale enabled) adds per-unit "morale", "state" and
  // "routed" and per-faction "routing" statistics. Format 5 (M6, squads
  // active) additionally adds the per-unit squad fields (squad, formation,
  // order, cohesion) and the personality discipline trait, leadership_radius
  // in the unit type table and per-faction squad statistics. Morale-off
  // battles and legacy battles serialize in the exact format-3 shape and
  // squads-off battles in the exact format-4/3 shape, so --squads off
  // exports are byte-identical to the M5 runs of the same battle. No paths,
  // timestamps or mode flags are exported, so identical battles serialize
  // identically.
  const bool morale = simulation.morale_enabled();
  const bool squads = simulation.squads_active();
  // M10: tactics on widens action_counts to the eight actions.
  const bool tactics = simulation.tactics_active();
  // Format 6 (M8, terrain): adds config.map (id and size) and
  // statistics.terrain_kills (kills by the terrain type of the victim's
  // cell). Map-less battles serialize in the exact format-3/4/5 shape of
  // their M1-M7 runs, byte-for-byte.
  const sim::TerrainGrid* const terrain = simulation.terrain();
  nlohmann::json root;
  // Format 7 (M10, tactics on): action_counts gains search/help/flank.
  root["format_version"] =
      tactics ? 7
              : (terrain != nullptr ? 6 : (squads ? 5 : (morale ? 4 : 3)));
  root["seed"] = config.seed;
  root["ticks_elapsed"] = result.ticks_elapsed;

  nlohmann::json config_json;
  config_json["battlefield_height"] = stable(config.battlefield_height);
  config_json["battlefield_width"] = stable(config.battlefield_width);
  config_json["max_ticks"] = config.max_ticks;
  config_json["spawn_band_fraction"] = stable(config.spawn_band_fraction);
  if (terrain != nullptr) {
    nlohmann::json map_json;
    map_json["id"] = terrain->map_id;
    map_json["width"] = terrain->width;
    map_json["height"] = terrain->height;
    map_json["cell_size"] = stable(terrain->cell_size);
    config_json["map"] = std::move(map_json);
  }
  config_json["total_units"] = world.units.size();
  if (types != nullptr) {
    nlohmann::json unit_types_json = nlohmann::json::array();
    for (const UnitType& type : types->types) {
      unit_types_json.push_back(unit_type_json(type, squads));
    }
    config_json["unit_types"] = std::move(unit_types_json);
  }
  if (composed) {
    nlohmann::json armies_json;
    for (Faction faction : kAllFactions) {
      nlohmann::json entries = nlohmann::json::array();
      for (const TypeCount& entry :
           config.composition[static_cast<std::size_t>(faction)]) {
        nlohmann::json item;
        item["count"] = entry.count;
        item["type"] = types->types[entry.type_index].id;
        entries.push_back(std::move(item));
      }
      armies_json[to_string(faction)] = std::move(entries);
    }
    config_json["armies"] = std::move(armies_json);
  } else {
    config_json["unit_stats"]["Blue"] = unit_stats_json(config.blue_stats);
    config_json["unit_stats"]["Red"] = unit_stats_json(config.red_stats);
  }
  root["config"] = std::move(config_json);

  nlohmann::json factions_json = nlohmann::json::array();
  for (Faction faction : kAllFactions) {
    factions_json.push_back(to_string(faction));
  }
  root["factions"] = std::move(factions_json);

  // Units are exported in ascending id order regardless of world layout.
  std::vector<std::size_t> ordered_indices;
  ordered_indices.reserve(world.units.size());
  for (std::size_t i = 0; i < world.units.size(); ++i) {
    ordered_indices.push_back(i);
  }
  std::sort(ordered_indices.begin(), ordered_indices.end(),
            [&world](std::size_t a, std::size_t b) {
              return world.units[a].id < world.units[b].id;
            });
  nlohmann::json units_json = nlohmann::json::array();
  for (std::size_t index : ordered_indices) {
    const UnitSquadInfo squad_info = simulation.unit_squad_info(index);
    units_json.push_back(unit_json(world.units[index], types,
                                   simulation.personality_table(), morale,
                                   squads ? &squad_info : nullptr));
  }
  root["units"] = std::move(units_json);

  nlohmann::json statistics_json;
  std::uint32_t total_deaths = 0;
  for (Faction faction : kAllFactions) {
    const FactionReport& report =
        result.factions[static_cast<std::size_t>(faction)];
    nlohmann::json faction_json;
    faction_json["deaths"] = report.deaths;
    faction_json["initial"] = report.initial;
    faction_json["survivors"] = report.survivors;
    nlohmann::json action_counts;
    // M10: with tactics on, Search/Help/Flank join the action counts;
    // with tactics off exactly the five M4-M9 keys are written, so every
    // pinned M1-M9 reference stays byte-identical.
    const std::size_t action_keys = tactics ? kActionCount : kLegacyActionCount;
    for (std::size_t action = 0; action < action_keys; ++action) {
      action_counts[to_string(static_cast<Action>(action))] =
          report.action_counts[action];
    }
    faction_json["action_counts"] = std::move(action_counts);
    if (morale) {
      faction_json["active"] = report.active;
      nlohmann::json routing;
      routing["first_rout_tick"] = report.routing.first_rout_tick;
      routing["peak_routed"] = report.routing.peak_routed;
      routing["recoveries"] = report.routing.recoveries;
      routing["total_routs"] = report.routing.total_routs;
      faction_json["routing"] = std::move(routing);
    }
    if (squads) {
      nlohmann::json squad;
      squad["commander_deaths"] = report.squad.commander_deaths;
      squad["drops_from_formation"] = report.squad.drops_from_formation;
      nlohmann::json order_counts;
      for (std::size_t order = 0; order < kOrderCount; ++order) {
        order_counts[to_string(static_cast<Order>(order))] =
            report.squad.order_counts[order];
      }
      squad["order_counts"] = std::move(order_counts);
      squad["squads"] = report.squad.squads;
      squad["successions"] = report.squad.successions;
      faction_json["squads"] = std::move(squad);
    }
    if (!report.by_type.empty()) {
      nlohmann::json by_type = nlohmann::json::array();
      for (const TypeReport& type : report.by_type) {
        by_type.push_back(type_report_json(type));
      }
      faction_json["by_type"] = std::move(by_type);
    }
    statistics_json[to_string(faction)] = std::move(faction_json);
    total_deaths += report.deaths;
  }
  statistics_json["total_deaths"] = total_deaths;
  if (terrain != nullptr && !result.terrain_kills.empty()) {
    nlohmann::json terrain_kills = nlohmann::json::array();
    for (std::size_t t = 0; t < result.terrain_kills.size(); ++t) {
      if (result.terrain_kills[t] == 0) {
        continue;  // types where nobody died carry no entry
      }
      nlohmann::json entry;
      entry["terrain"] = terrain->table->types[t].id;
      entry["kills"] = result.terrain_kills[t];
      terrain_kills.push_back(std::move(entry));
    }
    statistics_json["terrain_kills"] = std::move(terrain_kills);
    statistics_json["longest_stuck_stretch"] = result.longest_stuck_stretch;
    statistics_json["units_stuck_over_100"] = result.units_stuck_over_100;
  }
  root["statistics"] = std::move(statistics_json);

  nlohmann::json termination_json;
  termination_json["reason"] = to_string(result.reason);
  termination_json["winner"] =
      result.has_winner ? to_string(result.winner) : "draw";
  root["termination"] = std::move(termination_json);

  return root.dump(2, ' ', false, nlohmann::json::error_handler_t::strict);
}

}  // namespace sim
