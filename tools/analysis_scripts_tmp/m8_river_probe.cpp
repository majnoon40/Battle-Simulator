// M8 acceptance probe: river crossing times on data/maps/demo-river.png.
// A lone strong Red unit (with a harmless Blue bait far east so the battle
// does not end by elimination) advances toward the objective; we count
// the ticks until it stands east of the river (x > 520), from three
// start points: the bridge row, the ford row, and midway (forced detour).
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "cli/map_load.hpp"
#include "sim/config.hpp"
#include "sim/simulation.hpp"
#include "sim/terrain.hpp"
#include "sim/unit_types.hpp"

namespace {
sim::Unit make_unit(std::uint32_t id, sim::Faction faction, double x,
                    double y, double health) {
  sim::Unit unit;
  unit.id = id;
  unit.faction = faction;
  unit.x = x;
  unit.y = y;
  unit.health = health;
  unit.stats.max_health = health;
  unit.stats.damage = 40.0;
  unit.stats.speed = 2.0;
  unit.stats.attack_range = 2.0;
  unit.stats.attack_cooldown_ticks = 5;
  unit.stats.sight_radius = 40.0;
  unit.alive = true;
  unit.morale = 100.0;
  return unit;
}
}  // namespace

int main() {
  std::ifstream terrain_file("data/terrain.json");
  std::stringstream terrain_buffer;
  terrain_buffer << terrain_file.rdbuf();
  const sim::TerrainTypeTable table =
      sim::parse_terrain_types(terrain_buffer.str());
  const sim::TerrainGrid grid =
      cli::load_map("data/maps/demo-river.png", 5.0, table);

  const double starts[4][2] = {
      {250, 500}, {250, 750}, {250, 625}, {400, 750}};
  const char* names[4] = {"bridge row (y=500)", "ford row (y=750)",
                          "midway (y=625, detour)",
                          "close to the ford (x=400, y=750)"};
  for (int run = 0; run < 4; ++run) {
    sim::SimulationConfig config;
    config.seed = 42;
    config.max_ticks = 1200;
    config.ai_mode = sim::AiMode::Utility;
    config.morale_enabled = false;
    config.terrain = &grid;
    config.battlefield_width = grid.width * grid.cell_size;
    config.battlefield_height = grid.height * grid.cell_size;
    config.spawn_band_fraction = 0.15;
    sim::Unit red =
        make_unit(1, sim::Faction::Red, starts[run][0], starts[run][1], 200.0);
    sim::Unit blue = make_unit(2, sim::Faction::Blue, 950.0, starts[run][1],
                               10.0);
    sim::Simulation battle(config, {red, blue});
    if (run == 3) {
      sim::AiDebugCapture capture;
      capture.unit_id = 1;
      capture.tick = 150;
      capture.explicit_tick = true;
      battle.set_ai_debug(capture);
    }
    std::uint64_t ticks = 0;
    while (!battle.finished() &&
           battle.world().units[0].x <= 520.0) {
      battle.tick();
      ++ticks;
      if (run == 3 && ticks == 150) {
        const sim::AiDebugCapture& debug = battle.ai_debug();
        std::printf(
            "  debug tick 150: captured=%d enemies=%u allies=%u target=%d "
            "dist=%.1f chosen=%d hp=%.0f\n",
            static_cast<int>(debug.captured), debug.visible_enemies,
            debug.visible_allies, debug.target, debug.target_distance,
            static_cast<int>(debug.chosen), debug.health);
      }
      if (run == 3 && ticks % 100 == 0) {
        const sim::Unit& u = battle.world().units[0];
        const char* cell =
            grid.passable_at(u.x, u.y) ? "passable" : "IMPASSABLE";
        std::printf("  tick %llu: (%.1f, %.1f) cell(%u,%u) %s action %d\n",
                    static_cast<unsigned long long>(ticks), u.x, u.y,
                    static_cast<unsigned>(u.x / 5),
                    static_cast<unsigned>(u.y / 5), cell,
                    static_cast<int>(u.last_action));
      }
    }
    const sim::Unit& unit = battle.world().units[0];
    std::printf("%-28s start (%.0f, %.0f) -> crossed at tick %llu, now at "
                "(%.1f, %.1f)%s\n",
                names[run], starts[run][0], starts[run][1],
                static_cast<unsigned long long>(ticks), unit.x, unit.y,
                unit.x > 520.0 ? "" : " (never - stopped)");
  }
  return 0;
}
