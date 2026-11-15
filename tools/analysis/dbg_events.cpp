// Debug: why does the 6v6 melee in test_events.cpp produce no deaths?
#include <cstdio>
#include <vector>

#include <nlohmann/json.hpp>
#include "sim/events.hpp"
#include "sim/simulation.hpp"
#include "sim/unit_types.hpp"

namespace {

sim::UnitTypeTable infantry_types() {
  return sim::parse_unit_types(nlohmann::json::array({
      nlohmann::json{{"id", "infantry"},
                     {"health", 100.0},
                     {"damage", 12.0},
                     {"speed", 2.0},
                     {"range", 2.0},
                     {"cooldown", 1},
                     {"sight_radius", 40.0}},
  }).dump());
}

sim::Unit make_unit(std::uint32_t id, sim::Faction faction, double x,
                    double y) {
  sim::Unit unit;
  unit.id = id;
  unit.faction = faction;
  unit.type_index = 0;
  unit.x = x;
  unit.y = y;
  unit.health = 100.0;
  unit.stats.max_health = 100.0;
  unit.stats.damage = 100.0;
  unit.stats.speed = 2.0;
  unit.stats.attack_range = 2.0;
  unit.stats.attack_cooldown_ticks = 1;
  unit.stats.sight_radius = 40.0;
  unit.stats.accuracy = 1.0;
  unit.alive = true;
  return unit;
}

}  // namespace

int main() {
  const sim::UnitTypeTable types = infantry_types();
  std::vector<sim::Unit> units;
  for (int i = 0; i < 6; ++i) {
    units.push_back(make_unit(i + 1, sim::Faction::Red, 10.0 + i * 2.0, 500.0));
    units.push_back(
        make_unit(i + 101, sim::Faction::Blue, 30.0 + i * 2.0, 500.0));
  }
  sim::SimulationConfig config;
  config.seed = 7;
  config.max_ticks = 400;
  config.battlefield_width = 1000.0;
  config.battlefield_height = 1000.0;
  config.spawn_band_fraction = 0.15;
  config.unit_types = &types;
  config.ai_mode = sim::AiMode::Utility;
  config.morale_enabled = false;
  config.tactics_enabled = true;
  sim::EventRecorder recorder;
  config.events = &recorder;
  sim::Simulation simulation(config, units);
  const sim::BattleResult result = simulation.run();
  std::printf("end %llu reason %s winner %d\n",
              static_cast<unsigned long long>(result.ticks_elapsed),
              sim::to_string(result.reason),
              result.has_winner ? static_cast<int>(result.winner) : -1);
  std::printf("deaths %zu + %zu, events %zu\n",
              static_cast<std::size_t>(result.factions[0].deaths),
              static_cast<std::size_t>(result.factions[1].deaths),
              recorder.events().size());
  for (const sim::Unit& unit : simulation.world().units) {
    std::printf("unit %u %s x %.1f h %.0f alive %d last %d\n", unit.id,
                unit.faction == sim::Faction::Red ? "R" : "B", unit.x,
                unit.health, unit.alive ? 1 : 0,
                static_cast<int>(unit.last_action));
  }
  return 0;
}
