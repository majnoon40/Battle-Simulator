// m11 Task 1 — Nile swapped standoff probe (analysis only, not in the
// product build). Runs earth-nile-delta-v2 with --swap-sides semantics
// (exactly the battle_cli config path) and samples every 100 ticks:
// per-window kills per faction, the action histogram of alive units,
// the share of units with a visible enemy that barely moved over the
// window, the minimum distance between the nearest Red/Blue pair and
// whether impassable terrain separates them (BFS), and the count of
// alive units that see an enemy but have no passable path to any enemy
// (multi-source BFS distance fields). Output: one JSON line per
// sample.
//
// Build (from the repository root, after the normal build):
//   g++ -std=c++20 -O2 -Isim/include -Icli/include \
//       -Ibuild/_deps/json-src/single_include tools/analysis/standoff_probe.cpp \
//       build/sim/libsim.a build/cli/libcli_args.a -o /tmp/standoff_probe
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "cli/cli_args.hpp"
#include "cli/map_load.hpp"
#include "sim/events.hpp"
#include "sim/personalities.hpp"
#include "sim/scenario.hpp"
#include "sim/simulation.hpp"
#include "sim/squads.hpp"
#include "sim/terrain.hpp"

namespace {

std::string read_file(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

const char* action_name(sim::Action action) {
  switch (action) {
    case sim::Action::Attack: return "attack";
    case sim::Action::Approach: return "approach";
    case sim::Action::Advance: return "advance";
    case sim::Action::Retreat: return "retreat";
    case sim::Action::Hold: return "hold";
    case sim::Action::Flee: return "flee";
    case sim::Action::Search: return "search";
    case sim::Action::Help: return "help";
    case sim::Action::Flank: return "flank";
  }
  return "?";
}

// Multi-source BFS distance over passable cells (in cell steps, from
// any source cell); unreachable cells stay kInf.
std::vector<std::uint32_t> distance_field(
    const sim::TerrainGrid& grid, const std::vector<std::uint32_t>& sources,
    std::uint32_t& reachable_sources) {
  constexpr std::uint32_t kInf = std::numeric_limits<std::uint32_t>::max();
  std::vector<std::uint32_t> dist(grid.cell_count(), kInf);
  std::deque<std::uint32_t> queue;
  for (std::uint32_t cell : sources) {
    if (dist[cell] == kInf) {
      dist[cell] = 0;
      queue.push_back(cell);
    }
  }
  reachable_sources = static_cast<std::uint32_t>(queue.size());
  while (!queue.empty()) {
    const std::uint32_t cell = queue.front();
    queue.pop_front();
    const std::uint32_t x = cell % grid.width;
    const std::uint32_t y = cell / grid.width;
    const std::uint32_t d = dist[cell] + 1;
    const auto visit = [&](std::uint32_t nx, std::uint32_t ny) {
      if (nx >= grid.width || ny >= grid.height) {
        return;
      }
      const std::uint32_t next = ny * grid.width + nx;
      if (dist[next] != kInf ||
          !grid.table->types[grid.cells[next]].passable) {
        return;
      }
      dist[next] = d;
      queue.push_back(next);
    };
    visit(x + 1, y);
    visit(x - 1, y);
    visit(x, y + 1);
    visit(x, y - 1);
  }
  return dist;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario_path =
      argc > 1 ? argv[1] : "data/scenarios/earth-nile-delta-v2.json";
  const int seeds = argc > 2 ? std::atoi(argv[2]) : 10;
  const std::uint64_t max_ticks = argc > 3
                                       ? static_cast<std::uint64_t>(
                                             std::atoll(argv[3]))
                                       : 3000;
  const std::uint64_t interval = 100;
  const double epsilon = 4.0;  // world units moved per window = "stationary"

  const sim::Scenario scenario =
      sim::parse_scenario(read_file(scenario_path));
  const sim::UnitTypeTable types =
      sim::parse_unit_types(read_file("data/units.json"));
  const sim::TerrainTypeTable terrain_types =
      sim::parse_terrain_types(read_file("data/terrain.json"));
  const sim::TerrainGrid grid = cli::load_map(
      "data/maps/" + scenario.map->file, scenario.map->cell_size,
      terrain_types);
  const sim::PersonalityTable personalities =
      sim::parse_personalities(read_file("data/personalities.json"));
  const sim::MoraleConfig morale =
      sim::parse_morale(read_file("data/morale.json"));
  const sim::FormationConfig formations =
      sim::parse_formations(read_file("data/formations.json"));
  const sim::CommanderConfig commanders =
      sim::parse_commanders(read_file("data/commanders.json"));
  const sim::TacticsConfig tactics =
      sim::parse_tactics(read_file("data/tactics.json"));

  for (int seed = 1; seed <= seeds; ++seed) {
    sim::EventRecorder recorder;
    sim::SimulationConfig config;
    config.seed = static_cast<std::uint64_t>(seed);
    config.max_ticks = max_ticks;
    config.battlefield_width = grid.width * grid.cell_size;
    config.battlefield_height = grid.height * grid.cell_size;
    config.terrain = &grid;
    config.unit_types = &types;
    config.personalities = &personalities;
    config.morale_enabled = true;
    config.morale = &morale;
    config.tactics_enabled = true;
    config.contact_seek_enabled = true;
    config.tactics = &tactics;
    config.swap_sides = true;
    config.nav_v2 = true;
    config.events = &recorder;
    config.composition = sim::build_composition(scenario, types, seed);
    // The same objective/composition setup battle_cli does, then the
    // m11a side swap (explicit objectives included).
    for (sim::Faction faction : sim::kAllFactions) {
      const std::size_t index = static_cast<std::size_t>(faction);
      if (const auto& objective = scenario.objectives[index]) {
        config.objectives[index] =
            sim::ObjectivePoint{objective->x, objective->y, true};
      }
      if (const auto& plan = scenario.squads[index]) {
        config.squad_plans[index] = sim::SquadPlan{plan->count, plan->size};
      }
    }
    std::uint32_t total_units = 0;
    for (const auto& army : config.composition) {
      for (const sim::TypeCount& entry : army) {
        total_units += entry.count;
      }
    }
    config.total_units = total_units;
    config.neighbor_mode = sim::NeighborMode::Grid;
    config.ai_mode = sim::AiMode::Utility;
    config.squads_enabled = true;
    config.formations = &formations;
    config.commanders = &commanders;
    cli::apply_side_swap(config);
    sim::Simulation battle(config);
    const std::size_t unit_count = battle.world().units.size();

    std::vector<double> prev_x(unit_count, 0.0);
    std::vector<double> prev_y(unit_count, 0.0);
    for (std::size_t i = 0; i < unit_count; ++i) {
      prev_x[i] = battle.world().units[i].x;
      prev_y[i] = battle.world().units[i].y;
    }
    std::size_t death_cursor = 0;

    for (std::uint64_t tick = 1; tick <= max_ticks && !battle.finished();
         ++tick) {
      battle.tick();
      if (tick % interval != 0 && !battle.finished()) {
        continue;
      }
      // Kills this window, per victim faction.
      std::uint32_t red_killed = 0;
      std::uint32_t blue_killed = 0;
      for (; death_cursor < recorder.events().size(); ++death_cursor) {
        const sim::Event& event = recorder.events()[death_cursor];
        if (event.type != sim::EventType::Death) {
          continue;
        }
        for (const sim::Unit& unit : battle.world().units) {
          if (unit.id == event.a) {
            if (unit.faction == sim::Faction::Red) {
              ++red_killed;
            } else {
              ++blue_killed;
            }
            break;
          }
        }
      }
      // Action histogram + routed count over alive units.
      std::uint32_t alive_red = 0;
      std::uint32_t alive_blue = 0;
      std::uint32_t routed = 0;
      std::vector<std::pair<const char*, std::uint32_t>> actions;
      auto bump = [&actions](const char* name) {
        for (auto& entry : actions) {
          if (std::string_view(entry.first) == name) {
            entry.second += 1;
            return;
          }
        }
        actions.emplace_back(name, 1u);
      };
      double min_pair = std::numeric_limits<double>::max();
      std::size_t nearest_red = 0;
      std::size_t nearest_blue = 0;
      std::uint32_t visible_red = 0;
      std::uint32_t visible_blue = 0;
      std::uint32_t visible_stationary = 0;
      for (std::size_t i = 0; i < unit_count; ++i) {
        const sim::Unit& unit = battle.world().units[i];
        if (!unit.alive) {
          continue;
        }
        if (unit.faction == sim::Faction::Red) {
          ++alive_red;
        } else {
          ++alive_blue;
        }
        if (unit.routed) {
          ++routed;
        }
        bump(action_name(static_cast<sim::Action>(unit.last_action)));
        const sim::Perception& perception = battle.perception(i);
        const bool sees =
            battle.morale_enabled() && !unit.routed &&
            perception.visible_enemies > 0;
        if (sees) {
          if (unit.faction == sim::Faction::Red) {
            ++visible_red;
          } else {
            ++visible_blue;
          }
          const double dx = unit.x - prev_x[i];
          const double dy = unit.y - prev_y[i];
          if (dx * dx + dy * dy < epsilon * epsilon) {
            ++visible_stationary;
          }
        }
        prev_x[i] = unit.x;
        prev_y[i] = unit.y;
      }
      // Nearest enemy pair (all alive, and non-routed only).
      double min_active_pair = std::numeric_limits<double>::max();
      for (std::size_t i = 0; i < unit_count; ++i) {
        const sim::Unit& red = battle.world().units[i];
        if (!red.alive || red.faction != sim::Faction::Red || red.routed) {
          continue;
        }
        for (std::size_t j = 0; j < unit_count; ++j) {
          const sim::Unit& blue = battle.world().units[j];
          if (!blue.alive || blue.faction != sim::Faction::Blue ||
              blue.routed) {
            continue;
          }
          const double dx = red.x - blue.x;
          const double dy = red.y - blue.y;
          const double distance = dx * dx + dy * dy;
          if (distance < min_active_pair) {
            min_active_pair = distance;
          }
        }
      }
      min_active_pair =
          min_active_pair == std::numeric_limits<double>::max()
              ? -1.0
              : std::sqrt(min_active_pair);
      for (std::size_t i = 0; i < unit_count; ++i) {
        const sim::Unit& red = battle.world().units[i];
        if (!red.alive || red.faction != sim::Faction::Red) {
          continue;
        }
        for (std::size_t j = 0; j < unit_count; ++j) {
          const sim::Unit& blue = battle.world().units[j];
          if (!blue.alive || blue.faction != sim::Faction::Blue) {
            continue;
          }
          const double dx = red.x - blue.x;
          const double dy = red.y - blue.y;
          const double distance = dx * dx + dy * dy;
          if (distance < min_pair) {
            min_pair = distance;
            nearest_red = i;
            nearest_blue = j;
          }
        }
      }
      min_pair = min_pair == std::numeric_limits<double>::max()
                     ? -1.0
                     : std::sqrt(min_pair);
      // Terrain separation of the nearest pair: BFS from the red cell.
      const sim::Unit& red_unit = battle.world().units[nearest_red];
      const sim::Unit& blue_unit = battle.world().units[nearest_blue];
      const auto cell_of = [&](const sim::Unit& unit) {
        const std::uint32_t cx = static_cast<std::uint32_t>(
            unit.x / grid.cell_size);
        const std::uint32_t cy = static_cast<std::uint32_t>(
            unit.y / grid.cell_size);
        return std::min(cy, grid.height - 1) * grid.width +
               std::min(cx, grid.width - 1);
      };
      std::vector<std::uint32_t> red_source{cell_of(red_unit)};
      std::uint32_t sources = 0;
      const std::vector<std::uint32_t> red_field =
          distance_field(grid, red_source, sources);
      const std::uint32_t blue_cell = cell_of(blue_unit);
      const std::uint32_t pair_path =
          red_field[blue_cell];
      const bool pair_separated =
          pair_path == std::numeric_limits<std::uint32_t>::max();
      // Visible-but-unreachable: distance field from every alive enemy
      // cell; a seeing unit with no path to ANY enemy counts.
      std::vector<std::uint32_t> blue_cells;
      std::vector<std::uint32_t> red_cells;
      for (const sim::Unit& unit : battle.world().units) {
        if (!unit.alive) {
          continue;
        }
        (unit.faction == sim::Faction::Red ? red_cells : blue_cells)
            .push_back(cell_of(unit));
      }
      const std::vector<std::uint32_t> to_blue =
          distance_field(grid, blue_cells, sources);
      const std::vector<std::uint32_t> to_red =
          distance_field(grid, red_cells, sources);
      std::uint32_t visible_unreachable = 0;
      if (battle.morale_enabled()) {
        for (std::size_t i = 0; i < unit_count; ++i) {
          const sim::Unit& unit = battle.world().units[i];
          if (!unit.alive || unit.routed ||
              battle.perception(i).visible_enemies == 0) {
            continue;
          }
          const std::uint32_t cell = cell_of(unit);
          const std::vector<std::uint32_t>& field =
              unit.faction == sim::Faction::Red ? to_blue : to_red;
          if (field[cell] == std::numeric_limits<std::uint32_t>::max()) {
            ++visible_unreachable;
          }
        }
      }

      std::cout << "{\"seed\":" << seed << ",\"tick\":" << tick
                << ",\"alive_red\":" << alive_red
                << ",\"alive_blue\":" << alive_blue
                << ",\"red_killed\":" << red_killed
                << ",\"blue_killed\":" << blue_killed
                << ",\"routed\":" << routed
                << ",\"visible_red\":" << visible_red
                << ",\"visible_blue\":" << visible_blue
                << ",\"visible_stationary\":" << visible_stationary
                << ",\"min_pair_dist\":" << min_pair
                << ",\"pair_path_cells\":"
                << (pair_separated ? -1 : static_cast<int>(pair_path))
                << ",\"pair_separated\":"
                << (pair_separated ? "true" : "false")
                << ",\"visible_unreachable\":" << visible_unreachable
                << ",\"min_active_pair\":" << min_active_pair
                << ",\"actions\":{";
      for (std::size_t i = 0; i < actions.size(); ++i) {
        if (i > 0) {
          std::cout << ",";
        }
        std::cout << "\"" << actions[i].first << "\":"
                  << actions[i].second;
      }
      std::cout << "}}\n";
    }
    // Final outcome.
    const sim::BattleResult result = battle.result();
    std::uint32_t final_red = 0;
    std::uint32_t final_blue = 0;
    for (const sim::Unit& unit : battle.world().units) {
      if (unit.alive) {
        if (unit.faction == sim::Faction::Red) {
          ++final_red;
        } else {
          ++final_blue;
        }
      }
    }
    std::cout << "{\"seed\":" << seed << ",\"tick\":"
              << result.ticks_elapsed << ",\"final\":true"
              << ",\"winner\":\""
              << (result.has_winner
                      ? (result.winner == sim::Faction::Red ? "red"
                                                            : "blue")
                      : "draw")
              << "\",\"alive_red\":" << final_red
              << ",\"alive_blue\":" << final_blue << "}\n";
  }
  return 0;
}
