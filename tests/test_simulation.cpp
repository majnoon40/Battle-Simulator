// Battle-level tests: termination, winner rules, statistics, per-tick
// invariants and full-run determinism at the API level.
#include "sim/simulation.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace {

sim::UnitStats make_stats(double max_health, double speed, double damage,
                          double range, std::uint32_t cooldown) {
  sim::UnitStats stats;
  stats.max_health = max_health;
  stats.speed = speed;
  stats.damage = damage;
  stats.attack_range = range;
  stats.attack_cooldown_ticks = cooldown;
  return stats;
}

sim::Unit make_unit(std::uint32_t id, sim::Faction faction, double x, double y,
                    double health, sim::UnitStats stats) {
  sim::Unit unit;
  unit.id = id;
  unit.faction = faction;
  unit.x = x;
  unit.y = y;
  unit.health = health;
  unit.stats = stats;
  unit.alive = true;
  return unit;
}

sim::SimulationConfig small_config() {
  sim::SimulationConfig config;
  config.ai_mode = sim::AiMode::Legacy;  // M4: old tests pin legacy
  config.seed = 1;
  config.max_ticks = 100;
  config.battlefield_width = 100.0;
  config.battlefield_height = 100.0;
  return config;
}

TEST(TerminationTest, BattleEndsWhenAFactionIsEliminated) {
  // One strong red unit against one weak blue unit that starts in range.
  sim::SimulationConfig config = small_config();
  const sim::UnitStats red_stats = make_stats(100.0, 0.0, 50.0, 10.0, 5);
  const sim::UnitStats blue_stats = make_stats(40.0, 0.0, 1.0, 10.0, 5);
  sim::Simulation simulation(
      config, {make_unit(1, sim::Faction::Red, 0.0, 0.0, 100.0, red_stats),
               make_unit(2, sim::Faction::Blue, 1.0, 0.0, 40.0, blue_stats)});

  const sim::BattleResult result = simulation.run();

  EXPECT_TRUE(result.finished);
  EXPECT_EQ(result.reason, sim::TerminationReason::FactionEliminated);
  EXPECT_TRUE(result.has_winner);
  EXPECT_EQ(result.winner, sim::Faction::Red);
  EXPECT_EQ(result.factions[static_cast<std::size_t>(sim::Faction::Red)].deaths, 0u);
  EXPECT_EQ(result.factions[static_cast<std::size_t>(sim::Faction::Blue)].deaths, 1u);
  EXPECT_EQ(result.factions[static_cast<std::size_t>(sim::Faction::Blue)].survivors, 0u);
  EXPECT_LT(result.ticks_elapsed, config.max_ticks);
}

TEST(TerminationTest, MutualAnnihilationIsADraw) {
  sim::SimulationConfig config = small_config();
  const sim::UnitStats stats = make_stats(50.0, 0.0, 100.0, 10.0, 5);
  sim::Simulation simulation(
      config, {make_unit(1, sim::Faction::Red, 0.0, 0.0, 50.0, stats),
               make_unit(2, sim::Faction::Blue, 1.0, 0.0, 50.0, stats)});

  const sim::BattleResult result = simulation.run();

  EXPECT_TRUE(result.finished);
  EXPECT_EQ(result.reason, sim::TerminationReason::FactionEliminated);
  EXPECT_FALSE(result.has_winner);  // both died in the same tick
  EXPECT_EQ(result.ticks_elapsed, 1u);
}

TEST(TerminationTest, BattleEndsAtTickLimit) {
  // Two immobile units that never reach each other: the tick limit fires.
  sim::SimulationConfig config = small_config();
  config.max_ticks = 10;
  const sim::UnitStats stats = make_stats(100.0, 0.0, 10.0, 0.0, 5);
  sim::Simulation simulation(
      config, {make_unit(1, sim::Faction::Red, 0.0, 0.0, 100.0, stats),
               make_unit(2, sim::Faction::Blue, 50.0, 0.0, 100.0, stats)});

  const sim::BattleResult result = simulation.run();

  EXPECT_TRUE(result.finished);
  EXPECT_EQ(result.reason, sim::TerminationReason::TickLimit);
  EXPECT_EQ(result.ticks_elapsed, 10u);
  EXPECT_FALSE(result.has_winner);  // 1 survivor on each side: draw
}

TEST(TerminationTest, TickLimitWinnerHasMoreSurvivors) {
  // 2 reds vs 1 blue, none can reach each other: red wins on survivors.
  sim::SimulationConfig config = small_config();
  config.max_ticks = 5;
  const sim::UnitStats stats = make_stats(100.0, 0.0, 10.0, 0.0, 5);
  sim::Simulation simulation(
      config, {make_unit(1, sim::Faction::Red, 0.0, 0.0, 100.0, stats),
               make_unit(2, sim::Faction::Red, 0.0, 10.0, 100.0, stats),
               make_unit(3, sim::Faction::Blue, 50.0, 0.0, 100.0, stats)});

  const sim::BattleResult result = simulation.run();

  EXPECT_EQ(result.reason, sim::TerminationReason::TickLimit);
  EXPECT_TRUE(result.has_winner);
  EXPECT_EQ(result.winner, sim::Faction::Red);
  EXPECT_EQ(result.factions[static_cast<std::size_t>(sim::Faction::Red)].survivors, 2u);
  EXPECT_EQ(result.factions[static_cast<std::size_t>(sim::Faction::Blue)].survivors, 1u);
}

TEST(TerminationTest, FreshSimulationIsNotFinished) {
  sim::Simulation simulation(small_config());
  EXPECT_FALSE(simulation.finished());
  EXPECT_EQ(simulation.tick_index(), 0u);
}

TEST(TerminationTest, TickAfterFinishIsANoOp) {
  sim::SimulationConfig config = small_config();
  config.max_ticks = 3;
  const sim::UnitStats stats = make_stats(100.0, 0.0, 10.0, 0.0, 5);
  sim::Simulation simulation(
      config, {make_unit(1, sim::Faction::Red, 0.0, 0.0, 100.0, stats),
               make_unit(2, sim::Faction::Blue, 50.0, 0.0, 100.0, stats)});
  static_cast<void>(simulation.run());
  ASSERT_TRUE(simulation.finished());

  const double x = simulation.world().find_by_id(1)->x;
  simulation.tick();
  simulation.tick();
  EXPECT_EQ(simulation.tick_index(), 3u);
  EXPECT_DOUBLE_EQ(simulation.world().find_by_id(1)->x, x);
}

TEST(StatisticsTest, DeathsPlusSurvivorsMatchInitialCounts) {
  sim::SimulationConfig config;
  config.ai_mode = sim::AiMode::Legacy;  // M4: old tests pin legacy
  config.seed = 7;
  config.total_units = 40;
  config.max_ticks = 500;
  sim::Simulation simulation(config);
  const sim::BattleResult result = simulation.run();

  for (std::size_t i = 0; i < sim::kFactionCount; ++i) {
    EXPECT_EQ(result.factions[i].deaths + result.factions[i].survivors,
              result.factions[i].initial);
  }
  EXPECT_EQ(result.factions[0].initial + result.factions[1].initial, 40u);
  EXPECT_LE(result.ticks_elapsed, config.max_ticks);
}

TEST(StatisticsTest, SmallBattleProducesAKnownOutcome) {
  // 20 units per faction with M1 defaults resolve into a real fight: some
  // units die, the battle terminates before the limit.
  sim::SimulationConfig config;
  config.ai_mode = sim::AiMode::Legacy;  // M4: old tests pin legacy
  config.seed = 42;
  config.total_units = 40;
  config.max_ticks = 2000;
  sim::Simulation simulation(config);
  const sim::BattleResult result = simulation.run();

  EXPECT_TRUE(result.finished);
  EXPECT_GT(result.factions[0].deaths + result.factions[1].deaths, 0u);
  EXPECT_LT(result.ticks_elapsed, config.max_ticks);
}

TEST(InvariantsTest, HealthBoundariesHoldEveryTick) {
  sim::SimulationConfig config;
  config.ai_mode = sim::AiMode::Legacy;  // M4: old tests pin legacy
  config.seed = 42;
  config.total_units = 60;
  config.max_ticks = 600;
  sim::Simulation simulation(config);

  for (int tick = 0; tick < 600 && !simulation.finished(); ++tick) {
    simulation.tick();
    for (const sim::Unit& unit : simulation.world().units) {
      EXPECT_TRUE(std::isfinite(unit.x));
      EXPECT_TRUE(std::isfinite(unit.y));
      EXPECT_TRUE(std::isfinite(unit.health));
      if (unit.alive) {
        EXPECT_GT(unit.health, 0.0);
        EXPECT_LE(unit.health, unit.stats.max_health);
      } else {
        EXPECT_DOUBLE_EQ(unit.health, 0.0);
      }
    }
  }
}

TEST(InvariantsTest, DeadUnitsNeverComeBack) {
  sim::SimulationConfig config;
  config.ai_mode = sim::AiMode::Legacy;  // M4: old tests pin legacy
  config.seed = 123;
  config.total_units = 60;
  config.max_ticks = 600;
  sim::Simulation simulation(config);

  std::vector<bool> was_alive;
  for (const sim::Unit& unit : simulation.world().units) {
    was_alive.push_back(unit.alive);
  }

  for (int tick = 0; tick < 600 && !simulation.finished(); ++tick) {
    simulation.tick();
    for (std::size_t i = 0; i < simulation.world().units.size(); ++i) {
      if (!was_alive[i]) {
        EXPECT_FALSE(simulation.world().units[i].alive)
            << "unit " << simulation.world().units[i].id << " resurrected";
      }
      was_alive[i] = simulation.world().units[i].alive;
    }
  }
}

TEST(DeterminismTest, IdenticalConfigurationsProduceIdenticalRuns) {
  sim::SimulationConfig config;
  config.ai_mode = sim::AiMode::Legacy;  // M4: old tests pin legacy
  config.seed = 42;
  config.total_units = 100;
  config.max_ticks = 300;

  sim::Simulation a(config);
  const sim::BattleResult result_a = a.run();
  sim::Simulation b(config);
  const sim::BattleResult result_b = b.run();

  ASSERT_EQ(a.world().units.size(), b.world().units.size());
  for (std::size_t i = 0; i < a.world().units.size(); ++i) {
    const sim::Unit& ua = a.world().units[i];
    const sim::Unit& ub = b.world().units[i];
    EXPECT_EQ(ua.id, ub.id);
    EXPECT_EQ(ua.faction, ub.faction);
    EXPECT_DOUBLE_EQ(ua.x, ub.x);
    EXPECT_DOUBLE_EQ(ua.y, ub.y);
    EXPECT_DOUBLE_EQ(ua.health, ub.health);
    EXPECT_EQ(ua.alive, ub.alive);
  }
  EXPECT_EQ(result_a.ticks_elapsed, result_b.ticks_elapsed);
  EXPECT_EQ(result_a.reason, result_b.reason);
  EXPECT_EQ(result_a.has_winner, result_b.has_winner);
  EXPECT_EQ(result_a.winner, result_b.winner);
}

TEST(DeterminismTest, DifferentSeedsProduceDifferentSpawns) {
  sim::SimulationConfig config_a;
  config_a.ai_mode = sim::AiMode::Legacy;  // M4: old tests pin legacy
  config_a.seed = 1;
  config_a.total_units = 100;
  config_a.max_ticks = 1;
  sim::SimulationConfig config_b = config_a;
  config_b.seed = 2;

  sim::Simulation a(config_a);
  sim::Simulation b(config_b);

  bool differ = false;
  for (std::size_t i = 0; i < a.world().units.size(); ++i) {
    if (a.world().units[i].x != b.world().units[i].x ||
        a.world().units[i].y != b.world().units[i].y) {
      differ = true;
      break;
    }
  }
  EXPECT_TRUE(differ);
}

TEST(ConfigValidationTest, InvalidConfigurationsAreRejected) {
  sim::SimulationConfig base;
  base.total_units = 10;
  base.max_ticks = 100;

  sim::SimulationConfig too_few = base;
  too_few.total_units = 1;
  EXPECT_THROW(sim::Simulation{too_few}, std::invalid_argument);

  sim::SimulationConfig zero_ticks = base;
  zero_ticks.max_ticks = 0;
  EXPECT_THROW(sim::Simulation{zero_ticks}, std::invalid_argument);

  sim::SimulationConfig bad_width = base;
  bad_width.battlefield_width = 0.0;
  EXPECT_THROW(sim::Simulation{bad_width}, std::invalid_argument);

  sim::SimulationConfig bad_band = base;
  bad_band.spawn_band_fraction = 0.6;
  EXPECT_THROW(sim::Simulation{bad_band}, std::invalid_argument);

  sim::SimulationConfig bad_health = base;
  bad_health.red_stats.max_health = 0.0;
  EXPECT_THROW(sim::Simulation{bad_health}, std::invalid_argument);

  sim::SimulationConfig bad_cooldown = base;
  bad_cooldown.blue_stats.attack_cooldown_ticks = 0;
  EXPECT_THROW(sim::Simulation{bad_cooldown}, std::invalid_argument);
}

}  // namespace
