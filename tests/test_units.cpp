// Unit-level behavior tests: initialization, nearest-enemy selection,
// movement, attack range, cooldown, damage, death, dead-unit rules, stable
// identifiers and faction restrictions. Built on a Simulation with an
// explicit initial state so every scenario is fully controlled.
#include "sim/neighbors.hpp"
#include "sim/simulation.hpp"

#include <cmath>
#include <cstdint>
#include <set>
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

// Two stationary units that never reach each other (no movement, no range).
sim::Simulation idle_pair() {
  const sim::UnitStats stats = make_stats(100.0, 0.0, 10.0, 0.0, 5);
  return sim::Simulation(
      small_config(),
      {make_unit(1, sim::Faction::Red, 10.0, 10.0, 100.0, stats),
       make_unit(2, sim::Faction::Blue, 90.0, 90.0, 100.0, stats)});
}

TEST(UnitInitializationTest, SpawnedArmiesHaveExpectedShape) {
  sim::SimulationConfig config;
  config.ai_mode = sim::AiMode::Legacy;  // M4: old tests pin legacy
  config.seed = 42;
  config.total_units = 500;
  config.max_ticks = 1;
  sim::Simulation simulation(config);

  ASSERT_EQ(simulation.world().units.size(), 500u);

  std::set<std::uint32_t> ids;
  const sim::ArmySizes sizes = sim::split_armies(500);
  ASSERT_EQ(sizes.red, 250u);
  ASSERT_EQ(sizes.blue, 250u);

  std::uint32_t red = 0;
  std::uint32_t blue = 0;
  const double band = config.battlefield_width * config.spawn_band_fraction;
  for (const sim::Unit& unit : simulation.world().units) {
    EXPECT_TRUE(unit.alive);
    EXPECT_GT(unit.id, 0u);
    ids.insert(unit.id);
    EXPECT_DOUBLE_EQ(unit.health, unit.stats.max_health);
    EXPECT_EQ(unit.cooldown_remaining, 0u);
    EXPECT_GE(unit.x, 0.0);
    EXPECT_LT(unit.x, config.battlefield_width);
    EXPECT_GE(unit.y, 0.0);
    EXPECT_LT(unit.y, config.battlefield_height);
    if (unit.faction == sim::Faction::Red) {
      ++red;
      EXPECT_LT(unit.x, band);  // Red spawns in the left band
    } else {
      ++blue;
      EXPECT_GE(unit.x, config.battlefield_width - band);  // right band
    }
  }
  EXPECT_EQ(ids.size(), 500u);  // identifiers are unique
  EXPECT_EQ(red, 250u);
  EXPECT_EQ(blue, 250u);
}

TEST(UnitInitializationTest, OddUnitCountGivesRedTheExtraUnit) {
  const sim::ArmySizes sizes = sim::split_armies(5);
  EXPECT_EQ(sizes.red, 3u);
  EXPECT_EQ(sizes.blue, 2u);
}

TEST(NearestEnemyTest, PicksTheClosestEnemy) {
  const sim::UnitStats stats = make_stats(100.0, 0.0, 0.0, 0.0, 5);
  std::vector<sim::Unit> units;
  units.push_back(make_unit(1, sim::Faction::Red, 0.0, 0.0, 100.0, stats));
  units.push_back(make_unit(2, sim::Faction::Blue, 10.0, 0.0, 100.0, stats));
  units.push_back(make_unit(3, sim::Faction::Blue, 3.0, 0.0, 100.0, stats));
  EXPECT_EQ(sim::nearest_enemy_index(units, units[0]), 2);  // id 3 at (3,0)
}

TEST(NearestEnemyTest, DistanceTieBreaksToLowerId) {
  const sim::UnitStats stats = make_stats(100.0, 0.0, 0.0, 0.0, 5);
  std::vector<sim::Unit> units;
  units.push_back(make_unit(1, sim::Faction::Red, 0.0, 0.0, 100.0, stats));
  units.push_back(make_unit(2, sim::Faction::Blue, 5.0, 0.0, 100.0, stats));
  units.push_back(make_unit(3, sim::Faction::Blue, 0.0, 5.0, 100.0, stats));
  EXPECT_EQ(sim::nearest_enemy_index(units, units[0]), 1);  // id 2 wins the tie
}

TEST(NearestEnemyTest, IgnoresOwnFaction) {
  // A red ally sits closer than the blue enemy; the red unit must still
  // target the blue enemy (faction restriction).
  const sim::UnitStats stats = make_stats(100.0, 0.0, 0.0, 0.0, 5);
  std::vector<sim::Unit> units;
  units.push_back(make_unit(1, sim::Faction::Red, 0.0, 0.0, 100.0, stats));
  units.push_back(make_unit(2, sim::Faction::Red, 1.0, 0.0, 100.0, stats));
  units.push_back(make_unit(3, sim::Faction::Blue, 40.0, 0.0, 100.0, stats));
  EXPECT_EQ(sim::nearest_enemy_index(units, units[0]), 2);
}

TEST(NearestEnemyTest, IgnoresDeadEnemies) {
  const sim::UnitStats stats = make_stats(100.0, 0.0, 0.0, 0.0, 5);
  std::vector<sim::Unit> units;
  units.push_back(make_unit(1, sim::Faction::Red, 0.0, 0.0, 100.0, stats));
  units.push_back(make_unit(2, sim::Faction::Blue, 1.0, 0.0, 100.0, stats));
  units.push_back(make_unit(3, sim::Faction::Blue, 8.0, 0.0, 100.0, stats));
  units[1].alive = false;  // the closest blue is dead
  EXPECT_EQ(sim::nearest_enemy_index(units, units[0]), 2);  // id 3 instead
}

TEST(NearestEnemyTest, ReturnsMinusOneWithoutLivingEnemies) {
  const sim::UnitStats stats = make_stats(100.0, 0.0, 0.0, 0.0, 5);
  std::vector<sim::Unit> units;
  units.push_back(make_unit(1, sim::Faction::Red, 0.0, 0.0, 100.0, stats));
  units.push_back(make_unit(2, sim::Faction::Blue, 1.0, 0.0, 100.0, stats));
  units[1].alive = false;
  EXPECT_EQ(sim::nearest_enemy_index(units, units[0]), -1);
}

TEST(MovementTest, UnitMovesTowardNearestEnemy) {
  // Red speed 2 chasing a stationary blue; both units act simultaneously.
  sim::SimulationConfig config = small_config();
  const sim::UnitStats red_stats = make_stats(100.0, 2.0, 0.0, 0.0, 5);
  const sim::UnitStats blue_stats = make_stats(100.0, 0.0, 0.0, 0.0, 5);
  sim::Simulation simulation(
      config, {make_unit(1, sim::Faction::Red, 0.0, 0.0, 100.0, red_stats),
               make_unit(2, sim::Faction::Blue, 10.0, 0.0, 100.0, blue_stats)});
  simulation.tick();
  const sim::Unit& red = *simulation.world().find_by_id(1);
  EXPECT_NEAR(red.x, 2.0, 1e-9);
  EXPECT_NEAR(red.y, 0.0, 1e-9);
}

TEST(MovementTest, MoveIsClampedToTheTargetPosition) {
  // Distance 1.5 < speed 2: the unit lands on the target, never overshoots.
  sim::SimulationConfig config = small_config();
  const sim::UnitStats red_stats = make_stats(100.0, 2.0, 0.0, 0.0, 5);
  const sim::UnitStats blue_stats = make_stats(100.0, 0.0, 0.0, 0.0, 5);
  sim::Simulation simulation(
      config, {make_unit(1, sim::Faction::Red, 0.0, 0.0, 100.0, red_stats),
               make_unit(2, sim::Faction::Blue, 1.5, 0.0, 100.0, blue_stats)});
  simulation.tick();
  const sim::Unit& red = *simulation.world().find_by_id(1);
  EXPECT_NEAR(red.x, 1.5, 1e-9);
}

TEST(MovementTest, BothUnitsMoveInTheSameTick) {
  sim::SimulationConfig config = small_config();
  const sim::UnitStats red_stats = make_stats(100.0, 1.0, 0.0, 0.0, 5);
  const sim::UnitStats blue_stats = make_stats(100.0, 2.0, 0.0, 0.0, 5);
  sim::Simulation simulation(
      config, {make_unit(1, sim::Faction::Red, 0.0, 0.0, 100.0, red_stats),
               make_unit(2, sim::Faction::Blue, 10.0, 0.0, 100.0, blue_stats)});
  simulation.tick();
  const sim::Unit& red = *simulation.world().find_by_id(1);
  const sim::Unit& blue = *simulation.world().find_by_id(2);
  EXPECT_NEAR(red.x, 1.0, 1e-9);   // red moved right
  EXPECT_NEAR(blue.x, 8.0, 1e-9);  // blue moved left, from the same snapshot
}

TEST(MovementTest, DeadUnitsDoNotMove) {
  sim::SimulationConfig config = small_config();
  const sim::UnitStats stats = make_stats(100.0, 5.0, 0.0, 0.0, 5);
  sim::Simulation simulation(
      config, {make_unit(1, sim::Faction::Red, 0.0, 0.0, 100.0, stats),
               make_unit(2, sim::Faction::Blue, 10.0, 0.0, 100.0, stats)});
  sim::World& world = const_cast<sim::World&>(simulation.world());
  world.units[0].alive = false;  // kill red before the tick
  world.units[0].x = 3.0;
  world.units[0].y = 4.0;
  simulation.tick();
  const sim::Unit& red = *simulation.world().find_by_id(1);
  EXPECT_NEAR(red.x, 3.0, 1e-12);
  EXPECT_NEAR(red.y, 4.0, 1e-12);
}

TEST(AttackRangeTest, AttacksAtExactlyTheRangeBoundary) {
  sim::SimulationConfig config = small_config();
  const sim::UnitStats red_stats = make_stats(100.0, 0.0, 10.0, 2.0, 5);
  const sim::UnitStats blue_stats = make_stats(1000.0, 0.0, 0.0, 0.0, 5);
  sim::Simulation simulation(
      config,
      {make_unit(1, sim::Faction::Red, 0.0, 0.0, 100.0, red_stats),
       make_unit(2, sim::Faction::Blue, 2.0, 0.0, 1000.0, blue_stats)});  // distance == range
  simulation.tick();
  const sim::Unit& blue = *simulation.world().find_by_id(2);
  EXPECT_DOUBLE_EQ(blue.health, 990.0);  // boundary is inclusive
}

TEST(AttackRangeTest, DoesNotAttackBeyondTheRange) {
  sim::SimulationConfig config = small_config();
  const sim::UnitStats red_stats = make_stats(100.0, 0.0, 10.0, 2.0, 5);
  const sim::UnitStats blue_stats = make_stats(1000.0, 0.0, 0.0, 0.0, 5);
  sim::Simulation simulation(
      config,
      {make_unit(1, sim::Faction::Red, 0.0, 0.0, 100.0, red_stats),
       make_unit(2, sim::Faction::Blue, 2.5, 0.0, 1000.0, blue_stats)});  // distance > range
  simulation.tick();
  const sim::Unit& blue = *simulation.world().find_by_id(2);
  EXPECT_DOUBLE_EQ(blue.health, 1000.0);
}

TEST(AttackCooldownTest, AttacksOncePerCooldownInterval) {
  sim::SimulationConfig config = small_config();
  const sim::UnitStats red_stats = make_stats(100.0, 0.0, 10.0, 10.0, 5);
  const sim::UnitStats blue_stats = make_stats(10000.0, 0.0, 0.0, 0.0, 5);
  sim::Simulation simulation(
      config, {make_unit(1, sim::Faction::Red, 0.0, 0.0, 100.0, red_stats),
               make_unit(2, sim::Faction::Blue, 1.0, 0.0, 10000.0, blue_stats)});

  simulation.tick();  // tick 1: first attack
  EXPECT_DOUBLE_EQ(simulation.world().find_by_id(2)->health, 9990.0);

  for (int tick = 2; tick <= 5; ++tick) {  // ticks 2..5: cooling down
    simulation.tick();
    ASSERT_DOUBLE_EQ(simulation.world().find_by_id(2)->health, 9990.0)
        << "unexpected attack during cooldown at tick " << tick;
  }

  simulation.tick();  // tick 6: cooldown of 5 ticks has elapsed
  EXPECT_DOUBLE_EQ(simulation.world().find_by_id(2)->health, 9980.0);
}

TEST(DamageTest, DamageIsSubtractedFromTargetHealth) {
  sim::SimulationConfig config = small_config();
  const sim::UnitStats red_stats = make_stats(100.0, 0.0, 17.0, 10.0, 5);
  const sim::UnitStats blue_stats = make_stats(100.0, 0.0, 0.0, 0.0, 5);
  sim::Simulation simulation(
      config, {make_unit(1, sim::Faction::Red, 0.0, 0.0, 100.0, red_stats),
               make_unit(2, sim::Faction::Blue, 1.0, 0.0, 100.0, blue_stats)});
  simulation.tick();
  EXPECT_DOUBLE_EQ(simulation.world().find_by_id(2)->health, 83.0);
}

TEST(DamageTest, OverkillIsClampedToZeroHealth) {
  sim::SimulationConfig config = small_config();
  const sim::UnitStats red_stats = make_stats(100.0, 0.0, 30.0, 10.0, 5);
  const sim::UnitStats blue_stats = make_stats(10.0, 0.0, 0.0, 0.0, 5);
  sim::Simulation simulation(
      config, {make_unit(1, sim::Faction::Red, 0.0, 0.0, 100.0, red_stats),
               make_unit(2, sim::Faction::Blue, 1.0, 0.0, 10.0, blue_stats)});
  simulation.tick();
  const sim::Unit& blue = *simulation.world().find_by_id(2);
  EXPECT_DOUBLE_EQ(blue.health, 0.0);
  EXPECT_FALSE(blue.alive);
}

TEST(DeathTest, UnitDiesWhenHealthReachesExactlyZero) {
  sim::SimulationConfig config = small_config();
  const sim::UnitStats red_stats = make_stats(100.0, 0.0, 10.0, 10.0, 5);
  const sim::UnitStats blue_stats = make_stats(10.0, 0.0, 0.0, 0.0, 5);
  sim::Simulation simulation(
      config, {make_unit(1, sim::Faction::Red, 0.0, 0.0, 100.0, red_stats),
               make_unit(2, sim::Faction::Blue, 1.0, 0.0, 10.0, blue_stats)});
  simulation.tick();
  const sim::Unit& blue = *simulation.world().find_by_id(2);
  EXPECT_DOUBLE_EQ(blue.health, 0.0);
  EXPECT_FALSE(blue.alive);
}

TEST(DeathTest, DeadUnitCannotAttackOnLaterTicks) {
  // Red dies on tick 1 (blue hits for 100). Red's own tick-1 attack still
  // lands because attacks resolve simultaneously, but from tick 2 on, red is
  // dead and must deal no further damage.
  sim::SimulationConfig config = small_config();
  const sim::UnitStats red_stats = make_stats(10.0, 0.0, 50.0, 10.0, 5);
  const sim::UnitStats blue_stats = make_stats(1000.0, 0.0, 100.0, 10.0, 5);
  sim::Simulation simulation(
      config, {make_unit(1, sim::Faction::Red, 0.0, 0.0, 10.0, red_stats),
               make_unit(2, sim::Faction::Blue, 1.0, 0.0, 1000.0, blue_stats)});

  simulation.tick();
  EXPECT_FALSE(simulation.world().find_by_id(1)->alive);
  EXPECT_DOUBLE_EQ(simulation.world().find_by_id(2)->health, 950.0);  // mutual volley

  for (int tick = 2; tick <= 6; ++tick) {
    simulation.tick();
    ASSERT_DOUBLE_EQ(simulation.world().find_by_id(2)->health, 950.0)
        << "dead unit attacked at tick " << tick;
  }
}

TEST(DeathTest, DeadUnitsAreNotTargeted) {
  // Red dead; blue must not select red as a target (no crash, stays put).
  sim::SimulationConfig config = small_config();
  const sim::UnitStats stats = make_stats(100.0, 3.0, 0.0, 0.0, 5);
  sim::Simulation simulation(
      config, {make_unit(1, sim::Faction::Red, 0.0, 0.0, 100.0, stats),
               make_unit(2, sim::Faction::Blue, 10.0, 0.0, 100.0, stats)});
  sim::World& world = const_cast<sim::World&>(simulation.world());
  world.units[0].alive = false;
  simulation.tick();
  const sim::Unit& blue = *simulation.world().find_by_id(2);
  EXPECT_NEAR(blue.x, 10.0, 1e-12);  // no living target: no movement
}

TEST(StableIdTest, IdsStayUniqueAndNeverChange) {
  sim::SimulationConfig config;
  config.ai_mode = sim::AiMode::Legacy;  // M4: old tests pin legacy
  config.seed = 42;
  config.total_units = 60;
  config.max_ticks = 400;
  sim::Simulation simulation(config);

  std::set<std::uint32_t> ids_before;
  for (const sim::Unit& unit : simulation.world().units) {
    ids_before.insert(unit.id);
  }
  ASSERT_EQ(ids_before.size(), 60u);

  static_cast<void>(simulation.run());

  for (const sim::Unit& unit : simulation.world().units) {
    EXPECT_TRUE(ids_before.count(unit.id) > 0);  // no new ids appeared
  }
  EXPECT_EQ(simulation.world().units.size(), 60u);  // units are never removed
}

TEST(StableIdTest, DeadUnitsStayDead) {
  sim::SimulationConfig config;
  config.ai_mode = sim::AiMode::Legacy;  // M4: old tests pin legacy
  config.seed = 42;
  config.total_units = 60;
  config.max_ticks = 400;
  sim::Simulation simulation(config);
  static_cast<void>(simulation.run());

  for (const sim::Unit& unit : simulation.world().units) {
    if (!unit.alive) {
      EXPECT_DOUBLE_EQ(unit.health, 0.0);
    } else {
      EXPECT_GT(unit.health, 0.0);
    }
  }
}

TEST(InitialStateTest, DuplicateIdsAreRejected) {
  sim::SimulationConfig config = small_config();
  const sim::UnitStats stats = make_stats(100.0, 0.0, 0.0, 0.0, 5);
  std::vector<sim::Unit> units;
  units.push_back(make_unit(1, sim::Faction::Red, 0.0, 0.0, 100.0, stats));
  units.push_back(make_unit(1, sim::Faction::Blue, 10.0, 0.0, 100.0, stats));
  EXPECT_THROW(sim::Simulation(config, units), std::invalid_argument);
}

TEST(InitialStateTest, ZeroIdIsRejected) {
  sim::SimulationConfig config = small_config();
  const sim::UnitStats stats = make_stats(100.0, 0.0, 0.0, 0.0, 5);
  std::vector<sim::Unit> units;
  units.push_back(make_unit(0, sim::Faction::Red, 0.0, 0.0, 100.0, stats));
  EXPECT_THROW(sim::Simulation(config, units), std::invalid_argument);
}

TEST(IdlePairTest, IdleEnemiesDoNotDamageEachOther) {
  sim::Simulation simulation = idle_pair();
  static_cast<void>(simulation.run());  // runs to the tick limit with no contact
  const sim::BattleResult result = simulation.result();
  EXPECT_TRUE(result.finished);
  EXPECT_EQ(result.reason, sim::TerminationReason::TickLimit);
  EXPECT_DOUBLE_EQ(simulation.world().find_by_id(1)->health, 100.0);
  EXPECT_DOUBLE_EQ(simulation.world().find_by_id(2)->health, 100.0);
}

}  // namespace
