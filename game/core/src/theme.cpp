#include "game/theme.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "sim/morale.hpp"
#include "sim/squads.hpp"

namespace game {
namespace {

// Saturation reduction for routed units: mix the color toward its own
// luminance (65% gray), keeping the hue recognizable but washed out.
Color desaturate(const Color& color) {
  const double luminance =
      0.299 * color.r + 0.587 * color.g + 0.114 * color.b;
  const auto mix = [luminance](std::uint8_t channel) {
    const double value =
        0.35 * channel + 0.65 * luminance;  // keep 35% of the channel
    return static_cast<std::uint8_t>(std::clamp(value, 0.0, 255.0));
  };
  return Color{mix(color.r), mix(color.g), mix(color.b), color.a};
}

std::string fixed(double value, int decimals) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
  return buffer;
}

std::string to_string_lower(const std::string& text) {
  std::string lowered = text;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return lowered;
}

}  // namespace

double unit_size(const std::string& type_id) {
  if (type_id == "cavalry") {
    return 2.6;  // heavy unit, drawn larger
  }
  if (type_id == "commander") {
    return 3.0;
  }
  return 2.0;  // infantry, archer and the fallback
}

UnitShape shape_of(const std::string& type_id) {
  if (type_id == "archer") {
    return UnitShape::Triangle;
  }
  if (type_id == "cavalry" || type_id == "commander") {
    return UnitShape::Rect;
  }
  return UnitShape::Circle;  // infantry and the fallback
}

Color faction_color(sim::Faction faction) {
  if (faction == sim::Faction::Red) {
    return Color{217, 60, 62, 255};
  }
  return Color{62, 120, 216, 255};
}

Color routed_color(sim::Faction faction) {
  return desaturate(faction_color(faction));
}

bool is_commander_type(const sim::Unit& unit) {
  return unit.stats.leadership_radius > 0.0;
}

std::vector<std::string> hud_lines(const sim::Simulation& simulation,
                                   double fps, double sim_tps, int speed,
                                   bool limited,
                                   const std::string& scenario_name) {
  std::vector<std::string> lines;
  char buffer[160];

  // Count alive/routed/dead per faction in one pass.
  std::uint32_t alive[2] = {0, 0};
  std::uint32_t routed[2] = {0, 0};
  for (const sim::Unit& unit : simulation.world().units) {
    const std::size_t index = unit.faction == sim::Faction::Red ? 0 : 1;
    if (!unit.alive) {
      continue;
    }
    ++alive[index];
    if (unit.routed) {
      ++routed[index];
    }
  }
  const auto initial_red =
      simulation.result().factions[static_cast<std::size_t>(sim::Faction::Red)]
          .initial;
  const auto initial_blue =
      simulation.result()
          .factions[static_cast<std::size_t>(sim::Faction::Blue)]
          .initial;
  const std::uint32_t dead_red = initial_red - alive[0];
  const std::uint32_t dead_blue = initial_blue - alive[1];

  std::snprintf(buffer, sizeof(buffer), "tick %llu",
                static_cast<unsigned long long>(simulation.tick_index()));
  std::string line = buffer;
  if (fps > 0.0) {
    std::snprintf(buffer, sizeof(buffer), "  fps %.0f", fps);
    line += buffer;
  }
  if (sim_tps > 0.0) {
    std::snprintf(buffer, sizeof(buffer), "  sim %.1f t/s", sim_tps);
    line += buffer;
  }
  lines.push_back(line);
  line = "speed " + std::to_string(speed) + "x";
  if (limited) {
    line += "  SIM LIMITED";
  }
  lines.push_back(line);
  lines.push_back("seed " + std::to_string(simulation.config().seed) +
                  "  " + scenario_name);
  std::snprintf(buffer, sizeof(buffer),
                "Red  %u alive  %u routed  %u dead", alive[0], routed[0],
                dead_red);
  lines.push_back(buffer);
  std::snprintf(buffer, sizeof(buffer),
                "Blue %u alive  %u routed  %u dead", alive[1], routed[1],
                dead_blue);
  lines.push_back(buffer);
  return lines;
}

std::vector<std::string> inspector_lines(const sim::Simulation& simulation,
                                         std::size_t unit_index) {
  std::vector<std::string> lines;
  if (unit_index >= simulation.world().units.size()) {
    return lines;
  }
  const sim::Unit& unit = simulation.world().units[unit_index];
  char buffer[160];
  std::string type = "unit";
  if (simulation.config().unit_types != nullptr) {
    type = simulation.config().unit_types->types[unit.type_index].id;
  }
  std::snprintf(buffer, sizeof(buffer), "unit %u  %s  %s%s", unit.id,
                type.c_str(), sim::to_string(unit.faction),
                unit.alive ? "" : "  DEAD");
  lines.push_back(buffer);
  std::snprintf(buffer, sizeof(buffer), "hp %.0f/%.0f  morale %.0f %s",
                unit.health, unit.stats.max_health, unit.morale,
                sim::to_string(static_cast<sim::MoraleState>(
                    unit.morale_state)));
  lines.push_back(buffer);
  std::snprintf(buffer, sizeof(buffer), "action %s", sim::to_string(
                    static_cast<sim::Action>(unit.last_action)));
  lines.push_back(buffer);
  const sim::PersonalityTable& table = simulation.personality_table();
  std::string archetype = "custom";
  if (unit.personality.archetype != sim::kCustomArchetype &&
      static_cast<std::size_t>(unit.personality.archetype) < table.size()) {
    archetype = table.archetypes[unit.personality.archetype].id;
  }
  lines.push_back("personality " + archetype + "  br " +
                  fixed(unit.personality.bravery, 2) + "  ca " +
                  fixed(unit.personality.caution, 2) + "  ag " +
                  fixed(unit.personality.aggression, 2) + "  di " +
                  fixed(unit.personality.discipline, 2));
  const sim::UnitSquadInfo squad = simulation.unit_squad_info(unit_index);
  if (squad.squad_id >= 0) {
    lines.push_back("squad " + std::to_string(squad.squad_id) + "  " +
                    sim::Simulation::formation_name(squad.formation) +
                    "  order " + to_string_lower(sim::to_string(squad.order)) +
                    "  cohesion " + fixed(squad.cohesion, 2));
  } else {
    lines.push_back("squad none");
  }
  // Utility scores when the debug capture is armed on exactly this unit.
  const sim::AiDebugCapture& debug = simulation.ai_debug();
  if (debug.captured && debug.unit_id == unit.id) {
    lines.push_back(std::string("scores  attack ") +
                    fixed(debug.scores[static_cast<std::size_t>(
                              sim::Action::Attack)], 2) +
                    "  approach " +
                    fixed(debug.scores[static_cast<std::size_t>(
                              sim::Action::Approach)], 2) +
                    "  advance " +
                    fixed(debug.scores[static_cast<std::size_t>(
                              sim::Action::Advance)], 2));
    lines.push_back(std::string("         retreat ") +
                    fixed(debug.scores[static_cast<std::size_t>(
                              sim::Action::Retreat)], 2) +
                    "  hold " +
                    fixed(debug.scores[static_cast<std::size_t>(
                              sim::Action::Hold)], 2) +
                    "  order " + to_string_lower(sim::to_string(
                                    static_cast<sim::Order>(debug.order))) +
                    "  obedi " + fixed(debug.obedience, 2));
  }
  return lines;
}

std::vector<std::string> squad_panel_lines(const sim::Simulation& simulation,
                                           std::int32_t squad_id) {
  std::vector<std::string> lines;
  if (!simulation.squads_active() || squad_id < 0 ||
      static_cast<std::size_t>(squad_id) >= simulation.squads().size()) {
    return lines;
  }
  const sim::Squad& squad =
      simulation.squads()[static_cast<std::size_t>(squad_id)];
  char buffer[160];
  std::uint32_t members_alive = 0;
  std::uint32_t members_routed = 0;
  for (const sim::SquadMemberState& member : squad.members) {
    const sim::Unit& unit = simulation.world().units[member.unit_index];
    if (!unit.alive) {
      continue;
    }
    ++members_alive;
    if (unit.routed) {
      ++members_routed;
    }
  }
  std::snprintf(buffer, sizeof(buffer), "squad %u  %s  %s", squad.id,
                sim::to_string(squad.faction),
                sim::Simulation::formation_name(squad.formation).c_str());
  lines.push_back(buffer);
  std::snprintf(buffer, sizeof(buffer), "members %u/%u alive  %u routed",
                members_alive, static_cast<unsigned>(squad.members.size()),
                members_routed);
  lines.push_back(buffer);
  if (squad.command_active) {
    std::snprintf(buffer, sizeof(buffer), "commander unit %u",
                  simulation.world().units[squad.commander_index].id);
  } else {
    std::snprintf(buffer, sizeof(buffer), "commander none (lost tick %llu)",
                  static_cast<unsigned long long>(squad.command_lost_tick));
  }
  lines.push_back(buffer);
  lines.push_back("order " +
                  to_string_lower(sim::to_string(
                      squad.command_active ? squad.order : sim::Order::None)) +
                  "  cohesion " + fixed(squad.cohesion, 2));
  const sim::SquadStats& stats =
      simulation.result()
          .factions[static_cast<std::size_t>(squad.faction)]
          .squad;
  std::snprintf(buffer, sizeof(buffer),
                "cmd deaths %llu  successions %llu  drops %llu",
                static_cast<unsigned long long>(stats.commander_deaths),
                static_cast<unsigned long long>(stats.successions),
                static_cast<unsigned long long>(stats.drops_from_formation));
  lines.push_back(buffer);
  return lines;
}

}  // namespace game
