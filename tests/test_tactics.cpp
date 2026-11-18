// M10 Part 1 — tactical depth: counter-RNG order independence, combat
// variance, decision intervals, memory decay, the Search/Help/Flank
// actions, pursuit bounds, regroup, spawn patterns, composition ranges
// and side swapping.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "sim/ai.hpp"
#include "sim/json_export.hpp"
#include "sim/scenario.hpp"
#include "sim/simulation.hpp"
#include "sim/tactics.hpp"
#include "sim/unit_types.hpp"

namespace {

constexpr std::size_t kRed = static_cast<std::size_t>(sim::Faction::Red);
constexpr std::size_t kBlue = static_cast<std::size_t>(sim::Faction::Blue);

sim::UnitTypeTable infantry_types(double health = 100.0, double damage = 12.0,
                                  double speed = 2.0, double range = 2.0,
                                  double sight = 40.0, double accuracy = 1.0,
                                  double variance = 0.0) {
  return sim::parse_unit_types(nlohmann::json::array({
      nlohmann::json{{"id", "infantry"},
                     {"health", health},
                     {"damage", damage},
                     {"speed", speed},
                     {"range", range},
                     {"cooldown", 1},
                     {"sight_radius", sight},
                     {"accuracy", accuracy},
                     {"damage_variance", variance}},
  }).dump());
}

sim::Unit make_unit(std::uint32_t id, sim::Faction faction, double x,
                    double y, double health = 100.0) {
  sim::Unit unit;
  unit.id = id;
  unit.faction = faction;
  unit.type_index = 0;
  unit.x = x;
  unit.y = y;
  unit.health = health;
  unit.stats.max_health = 100.0;
  unit.stats.damage = 12.0;
  unit.stats.speed = 2.0;
  unit.stats.attack_range = 2.0;
  unit.stats.attack_cooldown_ticks = 1;
  unit.stats.sight_radius = 40.0;
  unit.stats.accuracy = 1.0;
  unit.alive = true;
  return unit;
}

sim::SimulationConfig tactics_config(const sim::UnitTypeTable& types,
                                     std::uint64_t max_ticks = 60) {
  sim::SimulationConfig config;
  config.seed = 42;
  config.max_ticks = max_ticks;
  config.battlefield_width = 1000.0;
  config.battlefield_height = 1000.0;
  config.ai_mode = sim::AiMode::Utility;
  config.unit_types = &types;
  config.morale_enabled = false;  // isolate the tactics layer
  config.squads_enabled = false;
  config.composition[kRed] = {{0, 1}};
  config.composition[kBlue] = {{0, 1}};
  return config;
}

sim::TacticsConfig quick_tactics() { return sim::TacticsConfig{}; }

}  // namespace

// ---------------------------------------------------------------------------
// Counter-based RNG.

TEST(TacticsRollTest, InRangeAndDeterministic) {
  for (std::uint32_t channel = 0; channel < 6; ++channel) {
    for (std::uint64_t tick = 0; tick < 50; ++tick) {
      const double roll = sim::tactics_roll(42, 7, tick, channel);
      EXPECT_GE(roll, 0.0);
      EXPECT_LT(roll, 1.0);
      EXPECT_EQ(roll, sim::tactics_roll(42, 7, tick, channel));
    }
  }
  // Different channels produce different streams.
  EXPECT_NE(sim::tactics_roll(42, 7, 1, 0), sim::tactics_roll(42, 7, 1, 1));
  // Order independence by construction: the roll depends on nothing else.
  std::vector<double> forward;
  for (std::uint32_t id = 1; id <= 100; ++id) {
    forward.push_back(sim::tactics_roll(42, id, 3, 0));
  }
  std::vector<double> backward;
  for (std::uint32_t i = 100; i >= 1; --i) {
    backward.push_back(sim::tactics_roll(42, i, 3, 0));
  }
  std::reverse(backward.begin(), backward.end());
  EXPECT_EQ(forward, backward);
}

TEST(TacticsRollTest, RoughlyUniform) {
  double sum = 0.0;
  std::uint64_t below = 0;
  const std::uint64_t draws = 100000;
  for (std::uint64_t i = 0; i < draws; ++i) {
    const double roll = sim::tactics_roll(42, static_cast<std::uint32_t>(i), 5, 0);
    sum += roll;
    below += roll < 0.5 ? 1 : 0;
  }
  EXPECT_NEAR(sum / draws, 0.5, 0.01);
  EXPECT_NEAR(static_cast<double>(below) / draws, 0.5, 0.02);
}

TEST(DecisionIntervalTest, OneToFiveTicksAndSpread) {
  std::array<int, 6> seen{};
  for (std::uint32_t id = 1; id <= 1000; ++id) {
    const std::uint32_t interval = sim::decision_interval_of(42, id);
    EXPECT_GE(interval, 1u);
    EXPECT_LE(interval, 5u);
    seen[interval] += 1;
  }
  for (int i = 1; i <= 5; ++i) {
    EXPECT_GT(seen[i], 0) << "interval " << i << " never drawn";
  }
}

// ---------------------------------------------------------------------------
// Tactics config parsing.

TEST(ParseTacticsTest, DefaultFileShapeParses) {
  const sim::TacticsConfig config = sim::default_tactics_config();
  EXPECT_EQ(config.memory.forget_ticks, 1200u);
  EXPECT_EQ(config.flank.max_per_squad, 4u);
  EXPECT_DOUBLE_EQ(config.pursuit.routed_bonus, 0.25);
  // The shipped file equals the built-in defaults.
  // TEST_DATA_DIR, not a relative path: ctest --test-dir runs the test
  // processes from the build directory, where data/ does not exist.
  std::ifstream file(std::filesystem::path(TEST_DATA_DIR) /
                      "tactics.json");
  ASSERT_TRUE(file.good());
  const sim::TacticsConfig parsed = sim::parse_tactics(file);
  EXPECT_EQ(parsed.memory.forget_ticks, config.memory.forget_ticks);
  EXPECT_DOUBLE_EQ(parsed.flank.score_base, config.flank.score_base);
  EXPECT_DOUBLE_EQ(parsed.help.radius, config.help.radius);
  EXPECT_DOUBLE_EQ(parsed.regroup.bonus_max, config.regroup.bonus_max);
  EXPECT_EQ(parsed.contact_seek.idle_ticks, config.contact_seek.idle_ticks);
}

// ---------------------------------------------------------------------------
// Contact seeking (M10 continuation).

TEST(ContactSeekTest, ParkedUnitAdvancesTowardFactionEstimate) {
  const sim::UnitTypeTable types = infantry_types();
  sim::TacticsConfig tactics = quick_tactics();
  tactics.contact_seek.idle_ticks = 3;
  // Seer (Blue) sits far east; Red observer parks AT its objective
  // (default objectives: Red -> east band center x=925, y=500) with no
  // enemy in sight. A second Red unit (the spotter) stands near the
  // enemy and reports the estimate.
  std::vector<sim::Unit> units{
      make_unit(1, sim::Faction::Red, 925.0, 500.0),          // parked
      make_unit(2, sim::Faction::Red, 700.0, 500.0, 1000000.0),  // spotter
      make_unit(3, sim::Faction::Blue, 720.0, 500.0, 1000000.0), // enemy
  };
  sim::SimulationConfig config = tactics_config(types, 30);
  config.tactics = &tactics;
  sim::Simulation simulation(config, units);
  // Tick 1: the spotter sees the enemy -> the faction estimate is set.
  simulation.tick();
  EXPECT_GT(simulation.faction_estimate_tick(sim::Faction::Red), 0u);
  // Let the observer's idle counter mature, then check that it leaves
  // the objective toward the estimate (west), not toward the objective.
  for (int i = 0; i < 20; ++i) {
    simulation.tick();
  }
  const sim::Unit& observer = simulation.world().units[0];
  EXPECT_LT(observer.x, 925.0) << "observer never left the objective";
  EXPECT_LT(observer.x, 895.0) << "observer is not seeking the estimate";
}

TEST(ContactSeekTest, DoesNotFireFreshOrFarFromObjectiveOrStale) {
  const sim::UnitTypeTable types = infantry_types();
  sim::TacticsConfig tactics = quick_tactics();
  tactics.contact_seek.idle_ticks = 3;
  tactics.contact_seek.estimate_max_age = 5;
  // Far from the objective (mid-field): no parking, no seek.
  {
    std::vector<sim::Unit> units{
        make_unit(1, sim::Faction::Red, 500.0, 500.0),
        make_unit(2, sim::Faction::Blue, 720.0, 500.0),
    };
    sim::SimulationConfig config = tactics_config(types, 10);
    config.tactics = &tactics;
    sim::Simulation simulation(config, units);
    for (int i = 0; i < 8; ++i) {
      simulation.tick();
    }
    // The unit still advances on the objective line: it moved EAST.
    EXPECT_GT(simulation.world().units[0].x, 500.0);
  }
  // Parked but the estimate is stale: the unit keeps holding the
  // objective instead of chasing an ancient sighting.
  {
    std::vector<sim::Unit> units{
        make_unit(1, sim::Faction::Red, 925.0, 500.0),
    };
    sim::SimulationConfig config = tactics_config(types, 20);
    config.tactics = &tactics;
    sim::Simulation simulation(config, units);
    for (int i = 0; i < 16; ++i) {
      simulation.tick();
    }
    // No enemy ever seen -> no estimate -> no seek: still at objective.
    EXPECT_NEAR(simulation.world().units[0].x, 925.0, 30.0);
    EXPECT_EQ(simulation.faction_estimate_tick(sim::Faction::Red), 0u);
  }
}

TEST(ContactSeekTest, OffReproducesPart1ByteForByte) {
  const sim::UnitTypeTable types = infantry_types(1000000.0, 1.0, 2.0,
                                                  2.0, 40.0, 0.85, 0.2);
  sim::TacticsConfig tactics = quick_tactics();
  tactics.contact_seek.idle_ticks = 3;  // fires within the battle
  const auto run = [&types, &tactics](bool seek) {
    std::vector<sim::Unit> units{
        make_unit(1, sim::Faction::Red, 925.0, 500.0, 1000000.0),
        make_unit(2, sim::Faction::Red, 300.0, 500.0, 1000000.0),
        make_unit(3, sim::Faction::Blue, 320.0, 500.0, 1000000.0),
    };
    sim::SimulationConfig config = tactics_config(types, 60);
    config.tactics = &tactics;
    config.contact_seek_enabled = seek;
    sim::Simulation simulation(config, units);
    static_cast<void>(simulation.run());
    return sim::export_state_json(simulation);
  };
  EXPECT_EQ(run(false), run(false));   // deterministic both ways
  EXPECT_NE(run(true), run(false));    // the feature changes the battle
}

TEST(ParseTacticsTest, RejectsInvalidConfigs) {
  const std::string base = R"json({
    "memory": {"forget_ticks": 240, "squad_intel_max_contacts": 8,
               "squad_intel_delay_ticks": 12},
    "search": {"score_base": 0.55, "distance_weight": 0.25, "age_weight": 0.20},
    "help": {"radius": 40.0, "ally_health_below": 0.5,
             "strength_ratio_min": 0.8, "score_base": 0.50,
             "health_weight": 0.30, "distance_weight": 0.20},
    "flank": {"max_per_squad": 4, "outnumber_ratio": 1.25,
              "offset_distance": 25.0, "score_base": 0.65,
              "strength_weight": 0.20},
    "pursuit": {"routed_bonus": 0.25, "cavalry_bonus_scale": 2.0,
                "cavalry_speed_threshold": 4.0, "max_distance": 300.0,
                "max_ticks": 240},
    "regroup": {"cohesion_below": 0.35, "bonus_max": 0.20},
    "contact_seek": {"idle_ticks": 100, "estimate_max_age": 2400}
  })json";
  EXPECT_NO_THROW(sim::parse_tactics(base));
  // Weights that do not sum to 1.
  EXPECT_THROW(sim::parse_tactics(std::string(base)
                  .replace(base.find("0.55, \"distance_weight\""), 4, "0.70")),
               std::invalid_argument);
  // Missing field.
  EXPECT_THROW(sim::parse_tactics(std::string(base).replace(
                   base.find("\"forget_ticks\""), 15, "not_a_field")),
               std::invalid_argument);
  // outnumber_ratio <= 1.
  EXPECT_THROW(sim::parse_tactics(std::string(base).replace(
                   base.find("1.25"), 4, "1.0")),
               std::invalid_argument);
  // Contact cap out of range.
  EXPECT_THROW(sim::parse_tactics(std::string(base).replace(
                   base.find("\"squad_intel_max_contacts\": 8"), 30,
                   "\"squad_intel_max_contacts\": 9")),
               std::invalid_argument);
  // Negative distance.
  EXPECT_THROW(sim::parse_tactics(std::string(base).replace(
                   base.find("\"radius\": 40.0"), 16, "\"radius\": -1.0")),
               std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Score bounds over random inputs.

TEST(ScoreBoundsTest, RandomInputsStayFiniteAndClamped) {
  std::mt19937 rng(7);
  std::uniform_real_distribution<double> unit(0.0, 1.0);
  const sim::TacticsConfig tactics;
  for (int i = 0; i < 10000; ++i) {
    sim::ScoreInputs inputs;
    inputs.health_fraction = unit(rng);
    inputs.strength_ratio = unit(rng);
    inputs.distance_fraction = unit(rng);
    inputs.cooldown_readiness = unit(rng);
    inputs.bravery = unit(rng);
    inputs.caution = unit(rng);
    inputs.aggression = unit(rng);
    inputs.has_visible_enemy = unit(rng) < 0.5;
    inputs.enemy_in_attack_range = unit(rng) < 0.3;
    inputs.has_fresh_contact = unit(rng) < 0.5;
    inputs.contact_distance = unit(rng) * 200.0;
    inputs.contact_age = unit(rng) * 500.0;
    inputs.sight_radius = 40.0;
    inputs.ally_in_need = unit(rng) < 0.4;
    inputs.ally_health_fraction = unit(rng);
    inputs.ally_distance = unit(rng) * 80.0;
    inputs.squad_engaged = unit(rng) < 0.5;
    inputs.member_may_flank = unit(rng) < 0.5;
    inputs.local_enemy_ratio = unit(rng) * 20.0;
    inputs.target_routed = unit(rng) < 0.3;
    inputs.unit_speed = unit(rng) * 8.0;
    inputs.pursuit_within_bounds = unit(rng) < 0.5;
    inputs.has_squad = unit(rng) < 0.5;
    inputs.squad_cohesion = unit(rng);
    const sim::ActionScores scores = sim::score_actions(inputs, &tactics);
    for (double score : scores) {
      EXPECT_TRUE(std::isfinite(score));
      EXPECT_TRUE(score == -1.0 || (score >= 0.0 && score <= 1.0));
    }
  }
}

// ---------------------------------------------------------------------------
// Order independence and determinism with variance ON.

TEST(OrderIndependenceTest, ShuffledUnitsSamePerIdOutcomes) {
  const sim::UnitTypeTable types = infantry_types();
  const auto run = [&types](bool reversed) {
    std::vector<sim::Unit> units;
    // Red 1..10 west, Blue 11..20 east, all inside one sight radius so the
    // battle is a dense melee with many variance rolls.
    for (std::uint32_t i = 1; i <= 10; ++i) {
      units.push_back(make_unit(i, sim::Faction::Red, 480.0 + i, 500.0));
    }
    for (std::uint32_t i = 11; i <= 20; ++i) {
      units.push_back(make_unit(i, sim::Faction::Blue, 520.0 + i - 10, 500.0));
    }
    if (reversed) {
      std::reverse(units.begin(), units.end());
    }
    sim::SimulationConfig config = tactics_config(types, 80);
    sim::Simulation simulation(config, units);
    static_cast<void>(simulation.run());
    std::map<std::uint32_t, std::pair<bool, double>> outcome;
    for (const sim::Unit& unit : simulation.world().units) {
      outcome[unit.id] = {unit.alive, unit.alive ? unit.health : 0.0};
    }
    return outcome;
  };
  EXPECT_EQ(run(false), run(true));
}

TEST(DeterminismTest, VarianceOnDoubleRunsAreByteIdentical) {
  const sim::UnitTypeTable types =
      infantry_types(100.0, 12.0, 2.0, 2.0, 40.0, 0.85, 0.2);
  const auto run = [&types] {
    std::vector<sim::Unit> units;
    for (std::uint32_t i = 1; i <= 6; ++i) {
      units.push_back(make_unit(i, sim::Faction::Red, 490.0 + i, 500.0));
    }
    for (std::uint32_t i = 7; i <= 12; ++i) {
      units.push_back(make_unit(i, sim::Faction::Blue, 510.0 + i - 6, 500.0));
    }
    sim::SimulationConfig config = tactics_config(types, 60);
    sim::Simulation simulation(config, units);
    static_cast<void>(simulation.run());
    return sim::export_state_json(simulation);
  };
  EXPECT_EQ(run(), run());
}

TEST(DeterminismTest, VarianceOffMatchesVarianceOffPins) {
  // The M8 byte pins (test_terrain.cpp) already prove variance off
  // reproduces M1-M9 exactly; here the same binary run twice must agree
  // with itself, and variance on must be a different battle than off
  // (otherwise the variance path is dead code).
  const sim::UnitTypeTable types =
      infantry_types(100.0, 12.0, 2.0, 2.0, 40.0, 0.7, 0.3);
  const auto run = [&types](bool tactics) {
    std::vector<sim::Unit> units{make_unit(1, sim::Faction::Red, 490.0, 500.0),
                                 make_unit(2, sim::Faction::Blue, 510.0, 500.0)};
    sim::SimulationConfig config = tactics_config(types, 60);
    config.tactics_enabled = tactics;
    sim::Simulation simulation(config, units);
    static_cast<void>(simulation.run());
    return sim::export_state_json(simulation);
  };
  const std::string off_a = run(false);
  const std::string off_b = run(false);
  EXPECT_EQ(off_a, off_b);
  EXPECT_NE(run(true), off_a);
}

// ---------------------------------------------------------------------------
// Combat variance statistics.

TEST(VarianceTest, AccuracyAndDamageStayWithinBounds) {
  // One-shot damage, adjacency, cooldown 1: every tick each attacker
  // attacks; with accuracy 0.5 roughly half the attacks connect.
  const sim::UnitTypeTable types =
      infantry_types(1000000.0, 1000.0, 2.0, 2.0, 40.0, 0.5, 0.0);
  std::vector<sim::Unit> units{make_unit(1, sim::Faction::Red, 499.0, 500.0, 1000000.0),
                               make_unit(2, sim::Faction::Blue, 501.0, 500.0, 1000000.0)};
  for (sim::Unit& unit : units) {
    unit.stats = types.types[0].stats;  // the type's damage/cooldown/accuracy
  }
  sim::SimulationConfig config = tactics_config(types, 400);
  config.combat_v2 = false;  // m13: combat multipliers are out of scope here
  sim::Simulation simulation(config, units);
  static_cast<void>(simulation.run());
  // 400 ticks, 2 attackers, one attack per tick each: ~800 attacks, half
  // hit. Every hit removes 1000 of a million: nothing dies, health sums
  // show the hits.
  const double red_lost = 1000000.0 - simulation.world().units[0].health;
  const double blue_lost = 1000000.0 - simulation.world().units[1].health;
  const double hits = (red_lost + blue_lost) / 1000.0;
  EXPECT_GE(hits, 0.40 * 800);
  EXPECT_LE(hits, 0.60 * 800);
}

TEST(VarianceTest, DamageSpreadWithinConfiguredFraction) {
  // accuracy 1, variance 0.3: every hit's damage is within [0.7, 1.3] of
  // the base and the mean is close to the base.
  const sim::UnitTypeTable types =
      infantry_types(1000000.0, 100.0, 2.0, 2.0, 40.0, 1.0, 0.3);
  std::vector<sim::Unit> units{make_unit(1, sim::Faction::Red, 499.0, 500.0, 1000000.0),
                               make_unit(2, sim::Faction::Blue, 501.0, 500.0, 1000000.0)};
  for (sim::Unit& unit : units) {
    unit.stats = types.types[0].stats;
  }
  sim::SimulationConfig config = tactics_config(types, 300);
  config.combat_v2 = false;  // m13: combat multipliers are out of scope here
  sim::Simulation simulation(config, units);
  static_cast<void>(simulation.run());
  const double red_lost = 1000000.0 - simulation.world().units[0].health;
  const double blue_lost = 1000000.0 - simulation.world().units[1].health;
  const double mean = (red_lost + blue_lost) / 600.0;  // 300 ticks x 2
  EXPECT_GE(mean, 70.0);
  EXPECT_LE(mean, 130.0);
  EXPECT_NEAR(mean, 100.0, 8.0);
}

// ---------------------------------------------------------------------------
// Memory decay at the exact tick.

TEST(MemoryTest, ForgetsAtTheConfiguredTick) {
  const sim::UnitTypeTable types = infantry_types();
  sim::TacticsConfig tactics = quick_tactics();
  tactics.memory.forget_ticks = 10;
  // Red sees far (100), Blue sees little (10). Blue advances east, away
  // from Red's sight, so Red's memory ages without a visible enemy.
  sim::UnitTypeTable asymmetric = sim::parse_unit_types(nlohmann::json::array({
      nlohmann::json{{"id", "red_type"}, {"health", 100.0}, {"damage", 12.0},
                     {"speed", 2.0}, {"range", 2.0}, {"cooldown", 5},
                     {"sight_radius", 100.0}},
      nlohmann::json{{"id", "blue_type"}, {"health", 100.0}, {"damage", 12.0},
                     {"speed", 5.0}, {"range", 2.0}, {"cooldown", 5},
                     {"sight_radius", 10.0}},
  }).dump());
  // Red at 700 with sight 100; Blue at 630 with sight 10. Blue's default
  // objective is the western band, so it advances away from Red at speed
  // 5: Red sees it at tick 1, loses it after ~6 ticks, then searches the
  // memory until it is 10 ticks old.
  std::vector<sim::Unit> units{make_unit(1, sim::Faction::Red, 700.0, 500.0),
                               make_unit(2, sim::Faction::Blue, 630.0, 500.0)};
  units[0].type_index = 0;
  units[1].type_index = 1;
  for (auto& unit : units) {
    if (unit.faction == sim::Faction::Blue) {
      unit.stats.speed = 5.0;
      unit.stats.sight_radius = 10.0;
    } else {
      unit.stats.sight_radius = 100.0;
    }
  }
  sim::SimulationConfig config = tactics_config(asymmetric, 40);
  config.tactics = &tactics;
  sim::Simulation simulation(config, units);
  // Tick 1: Red sees Blue (70 < 100) and remembers. Blue flees east at
  // speed 5; after ~6 ticks it is beyond 100 and Red keeps searching
  // toward the memory until it is 10 ticks old.
  std::string actions;
  for (int tick = 0; tick < 40; ++tick) {
    simulation.tick();
    actions += std::to_string(static_cast<int>(
                   static_cast<sim::Action>(
                       simulation.world().units[0].last_action))) +
               ",";
  }
  // Search (5) appears at some point while the memory is fresh...
  EXPECT_NE(actions.find("5,"), std::string::npos)
      << "Search never chosen; actions: " << actions;
  // ...and after forgetting, Red falls back to Advance (2).
  const std::string tail = actions.substr(actions.size() - 6);
  EXPECT_NE(tail.find("2,"), std::string::npos)
      << "no Advance after forgetting; actions: " << actions;
}

TEST(MemoryTest, UnitWithNoMemoryNeverSearches) {
  const sim::UnitTypeTable types = infantry_types();
  // A lone Red unit with no enemy anywhere: no memory, no squad intel.
  std::vector<sim::Unit> units{make_unit(1, sim::Faction::Red, 100.0, 100.0)};
  sim::SimulationConfig config = tactics_config(types, 10);
  sim::Simulation simulation(config, units);
  for (int i = 0; i < 10; ++i) {
    simulation.tick();
    EXPECT_EQ(static_cast<sim::Action>(
                  simulation.world().units[0].last_action),
              sim::Action::Advance);
  }
}

// ---------------------------------------------------------------------------
// Help ally.

TEST(HelpTest, FiresForWoundedAllyAndNotForHealthyOnes) {
  const sim::UnitTypeTable types = infantry_types();
  sim::TacticsConfig tactics = quick_tactics();
  // Cautious helper with low aggression so Advance cannot outscore Help.
  std::vector<sim::Personality> personalities = {
      {0.9, 0.9, 0.1, 0.5, sim::kCustomArchetype},
      {0.5, 0.5, 0.5, 0.5, sim::kCustomArchetype},
  };
  std::vector<sim::Unit> units{make_unit(1, sim::Faction::Red, 500.0, 500.0),
                               make_unit(2, sim::Faction::Red, 505.0, 500.0, 5.0)};
  sim::SimulationConfig config = tactics_config(types, 3);
  config.tactics = &tactics;
  sim::Simulation simulation(config, units, personalities);
  simulation.tick();
  // The healthy unit heads for its wounded neighbour (Help = 6).
  EXPECT_EQ(static_cast<sim::Action>(simulation.world().units[0].last_action),
            sim::Action::Help);

  // Same setup with a healthy neighbour: no Help available at all.
  std::vector<sim::Unit> healthy{make_unit(1, sim::Faction::Red, 500.0, 500.0),
                                 make_unit(2, sim::Faction::Red, 505.0, 500.0, 100.0)};
  sim::Simulation control(tactics_config(types, 3), healthy, personalities);
  control.tick();
  EXPECT_NE(static_cast<sim::Action>(control.world().units[0].last_action),
            sim::Action::Help);
}

// ---------------------------------------------------------------------------
// Flank.

TEST(FlankTest, FiresUnderCapAndNeverAboveIt) {
  // A squad of one commander plus five soldiers against two visible
  // enemies: local ratio (5+1)/(2+1) = 2.0 >= 1.25, squad engaged.
  sim::UnitTypeTable squad_types = sim::parse_unit_types(nlohmann::json::array({
      nlohmann::json{{"id", "soldier"}, {"health", 100.0}, {"damage", 1.0},
                     {"speed", 2.0}, {"range", 2.0}, {"cooldown", 9},
                     {"sight_radius", 60.0}},
      nlohmann::json{{"id", "leader"}, {"health", 150.0}, {"damage", 1.0},
                     {"speed", 2.0}, {"range", 2.0}, {"cooldown", 9},
                     {"sight_radius", 60.0}, {"leadership_radius", 120.0}},
  }).dump());
  sim::TacticsConfig tactics = quick_tactics();
  std::vector<sim::Unit> units;
  std::uint32_t next_id = 1;
  for (int i = 0; i < 5; ++i) {
    sim::Unit u = make_unit(next_id++, sim::Faction::Red, 480.0 + 3.0 * i, 500.0);
    u.type_index = 0;
    u.stats = squad_types.types[0].stats;  // sight 60 sees the enemy group
    units.push_back(u);
  }
  {
    sim::Unit u = make_unit(next_id++, sim::Faction::Red, 480.0, 490.0, 150.0);
    u.type_index = 1;
    u.stats = squad_types.types[1].stats;
    units.push_back(u);
  }
  for (int i = 0; i < 2; ++i) {
    sim::Unit u = make_unit(next_id++, sim::Faction::Blue, 540.0 + 3.0 * i, 500.0);
    u.type_index = 0;
    u.stats = squad_types.types[0].stats;
    units.push_back(u);
  }
  sim::SimulationConfig config = tactics_config(squad_types, 6);
  config.squads_enabled = true;
  config.squad_plans[kRed] = sim::SquadPlan{1, 5};
  config.composition[kRed] = {{0, 5}, {1, 1}};
  config.composition[kBlue] = {{0, 2}};
  config.tactics = &tactics;
  // Low aggression across the board so Flank outranks Approach for the
  // members under the cap (member order is commander then soldiers by id).
  std::vector<sim::Personality> personalities;
  for (std::size_t i = 0; i < units.size(); ++i) {
    personalities.push_back({0.9, 0.2, 0.05, 0.9, sim::kCustomArchetype});
  }
  sim::Simulation simulation(config, units, personalities);
  bool any_flank = false;
  int flankers = 0;
  for (int tick = 0; tick < 6; ++tick) {
    simulation.tick();
  }
  const nlohmann::json state =
      nlohmann::json::parse(sim::export_state_json(simulation));
  for (const auto& unit : state["units"]) {
    if (unit["faction"] == "Red" && unit["last_action"] == "flank") {
      any_flank = true;
      flankers += 1;
    }
  }
  EXPECT_TRUE(any_flank) << "Flank never chosen in the outnumbered squad";
  EXPECT_LE(flankers, static_cast<int>(tactics.flank.max_per_squad));
}

TEST(FlankTest, PureBonusHelpersStayBounded) {
  const sim::TacticsConfig tactics;
  EXPECT_DOUBLE_EQ(sim::pursuit_bonus(false, 2.0, tactics), 0.0);
  EXPECT_DOUBLE_EQ(sim::pursuit_bonus(true, 2.0, tactics), 0.25);
  EXPECT_DOUBLE_EQ(sim::pursuit_bonus(true, 5.0, tactics), 0.5);
  EXPECT_DOUBLE_EQ(sim::regroup_bonus(0.5, false, tactics), 0.0);
  EXPECT_NEAR(sim::regroup_bonus(0.1, false, tactics), 0.18, 1e-12);
  EXPECT_DOUBLE_EQ(sim::regroup_bonus(0.1, true, tactics), 0.0);
  EXPECT_DOUBLE_EQ(sim::regroup_bonus(0.5, true, tactics), 0.0);
  for (double distance = 0; distance <= 200; distance += 25) {
    const double score = sim::search_score(distance, 40.0, 0, 240, tactics);
    EXPECT_GE(score, 0.0);
    EXPECT_LE(score, 1.0);
  }
}

// ---------------------------------------------------------------------------
// Pursuit bounds.

TEST(PursuitTest, CounterIncrementsAndResets) {
  const sim::UnitTypeTable types = infantry_types();
  sim::TacticsConfig tactics = quick_tactics();
  tactics.pursuit.max_ticks = 3;
  // A routed Blue unit flees; healthy Red pursue it.
  std::vector<sim::Unit> units{make_unit(1, sim::Faction::Red, 490.0, 500.0),
                               make_unit(2, sim::Faction::Blue, 510.0, 500.0)};
  units[0].morale = 80.0;      // healthy pursuer (explicit states keep it)
  units[0].morale_state = 2;   // Confident
  units[1].routed = true;
  units[1].morale = 1.0;
  units[1].morale_state = 0;   // Broken
  sim::SimulationConfig config = tactics_config(types, 10);
  config.morale_enabled = true;
  config.tactics = &tactics;
  sim::Simulation simulation(config, units);
  simulation.tick();
  EXPECT_EQ(simulation.world().units[0].pursuit_ticks, 1u);
  simulation.tick();
  simulation.tick();
  simulation.tick();
  // Capped at max_ticks = 3: the fourth consecutive pursuit tick keeps the
  // counter at the bound (no unbounded chase).
  EXPECT_LE(simulation.world().units[0].pursuit_ticks, 3u);
  EXPECT_GE(simulation.world().units[0].pursuit_ticks, 3u);
}

// ---------------------------------------------------------------------------
// Metamorphic: a unit outside sight and memory changes nothing.

TEST(MetamorphicTest, FarUnitDoesNotChangeScores) {
  const sim::UnitTypeTable types = infantry_types(100.0, 12.0, 2.0, 2.0, 40.0,
                                                  0.8, 0.2);
  const auto run = [&types](bool with_far_unit) {
    std::vector<sim::Unit> units{make_unit(1, sim::Faction::Red, 480.0, 500.0),
                                 make_unit(2, sim::Faction::Blue, 510.0, 500.0)};
    if (with_far_unit) {
      // 400 away: outside both units' sight and neither has memory of it.
      units.push_back(make_unit(99, sim::Faction::Blue, 10.0, 10.0));
    }
    sim::SimulationConfig config = tactics_config(types, 2);
    sim::AiDebugCapture debug;
    debug.unit_id = 1;
    debug.explicit_tick = true;
    debug.tick = 1;
    sim::Simulation simulation(config, units);
    simulation.set_ai_debug(debug);
    simulation.tick();
    return simulation.ai_debug();
  };
  const sim::AiDebugCapture without = run(false);
  const sim::AiDebugCapture with_far = run(true);
  ASSERT_TRUE(without.captured);
  ASSERT_TRUE(with_far.captured);
  EXPECT_EQ(without.scores, with_far.scores);
  EXPECT_EQ(without.chosen, with_far.chosen);
  EXPECT_EQ(without.visible_enemies, with_far.visible_enemies);
}

// ---------------------------------------------------------------------------
// Spawn patterns and composition ranges (scenario level).

std::string scenario_json(const std::string& pattern,
                          const std::string& red_army = "\"infantry\": 200",
                          const std::string& blue_army = "\"infantry\": 200") {
  std::string text = R"json({
    "battlefield": {"width": 1000.0, "height": 1000.0},
    "spawn_band_fraction": 0.15,
    "armies": {"red": {)json";
  text += red_army + R"json(}, "blue": {)json" + blue_army + "}}";
  if (!pattern.empty()) {
    text += ", \"spawn_pattern\": \"" + pattern + "\"";
  }
  text += "}";
  return text;
}

TEST(SpawnPatternTest, PatternsStayInsideTheBandAndDiffer) {
  const sim::UnitTypeTable types = infantry_types();
  const auto positions = [&types](const std::string& pattern) {
    const sim::Scenario scenario = sim::parse_scenario(
        scenario_json(pattern.empty() ? "band" : pattern));
    sim::SimulationConfig config;
    config.seed = 42;
    config.max_ticks = 1;
    config.battlefield_width = scenario.battlefield_width;
    config.battlefield_height = scenario.battlefield_height;
    config.spawn_band_fraction = scenario.spawn_band_fraction;
    config.unit_types = &types;
    config.ai_mode = sim::AiMode::Utility;
    config.morale_enabled = false;
    config.squads_enabled = false;
    config.composition =
        sim::build_composition(scenario, types, config.seed);
    for (sim::Faction faction : sim::kAllFactions) {
      config.spawn_patterns[static_cast<std::size_t>(faction)] =
          scenario.spawn_patterns[static_cast<std::size_t>(faction)];
    }
    sim::Simulation simulation(config);
    std::vector<double> xs;
    for (const sim::Unit& unit : simulation.world().units) {
      xs.push_back(unit.x);
    }
    return xs;
  };
  const std::vector<double> band = positions("band");
  const std::vector<double> line = positions("line");
  const std::vector<double> columns = positions("columns");
  const std::vector<double> echelon = positions("echelon");
  const std::vector<double> cluster = positions("cluster");
  EXPECT_EQ(band.size(), 400u);
  EXPECT_NE(band, line);
  EXPECT_NE(line, columns);
  EXPECT_NE(columns, echelon);
  EXPECT_NE(echelon, cluster);
  // Every unit of every pattern stays inside its faction's band (the
  // default fraction 0.15 -> Red [0, 150], Blue [850, 1000]).
  for (const std::vector<double>& xs : {line, columns, echelon, cluster}) {
    for (std::size_t i = 0; i < xs.size(); ++i) {
      if (i < xs.size() / 2) {
        EXPECT_LE(xs[i], 150.0);
      } else {
        EXPECT_GE(xs[i], 850.0);
      }
    }
  }
}

TEST(CompositionRangeTest, DrawsStayInRangeAndReproducePerSeed) {
  const sim::UnitTypeTable types = infantry_types();
  const sim::Scenario scenario = sim::parse_scenario(
      scenario_json("", "\"infantry\": [150, 200]"));
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    const auto composition = sim::build_composition(scenario, types, seed);
    const std::uint32_t red = composition[kRed][0].count;
    const std::uint32_t blue = composition[kBlue][0].count;
    EXPECT_GE(red, 150u);
    EXPECT_LE(red, 200u);
    EXPECT_GE(blue, 150u);
    EXPECT_LE(blue, 200u);
    EXPECT_EQ(red, sim::build_composition(scenario, types, seed)[kRed][0].count);
    EXPECT_EQ(blue,
              sim::build_composition(scenario, types, seed)[kBlue][0].count);
  }
  // At least two different seeds draw different armies somewhere.
  bool varies = false;
  for (std::uint64_t seed = 2; seed <= 20 && !varies; ++seed) {
    varies = sim::build_composition(scenario, types, seed)[kRed][0].count !=
             sim::build_composition(scenario, types, 1)[kRed][0].count;
  }
  EXPECT_TRUE(varies);
}

TEST(ScenarioParseTest, RejectsUnknownPatternsAndBadRanges) {
  EXPECT_THROW(sim::parse_scenario(scenario_json("wedge")),
               std::invalid_argument);
  EXPECT_THROW(sim::parse_scenario(scenario_json("", "\"infantry\": [200, 100]")),
               std::invalid_argument);
  EXPECT_THROW(sim::parse_scenario(scenario_json("", "\"infantry\": [200]")),
               std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Squad intel sharing.

TEST(SquadIntelTest, ContactsAreSharedWithDelay) {
  // Two-member squad: the seer spots an enemy far to the east; the
  // searcher (out of sight of the enemy) picks Search only once the shared
  // contact is old enough to read.
  sim::UnitTypeTable two_types = sim::parse_unit_types(nlohmann::json::array({
      nlohmann::json{{"id", "soldier"}, {"health", 100.0}, {"damage", 1.0},
                     {"speed", 2.0}, {"range", 2.0}, {"cooldown", 9},
                     {"sight_radius", 30.0}},
      nlohmann::json{{"id", "leader"}, {"health", 150.0}, {"damage", 1.0},
                     {"speed", 2.0}, {"range", 2.0}, {"cooldown", 9},
                     {"sight_radius", 30.0}, {"leadership_radius", 200.0}},
  }).dump());
  sim::TacticsConfig tactics = quick_tactics();
  tactics.memory.forget_ticks = 500;
  tactics.memory.squad_intel_delay_ticks = 5;
  std::vector<sim::Unit> units;
  // Seer (east, sees the enemy 20 away), commander (center), searcher
  // (west, 70 from the enemy: outside its own sight).
  sim::Unit seer = make_unit(1, sim::Faction::Red, 560.0, 500.0);
  seer.type_index = 0;
  seer.stats.sight_radius = 30.0;
  sim::Unit commander = make_unit(2, sim::Faction::Red, 520.0, 500.0, 150.0);
  commander.type_index = 1;
  commander.stats.sight_radius = 30.0;
  commander.stats.leadership_radius = 200.0;
  sim::Unit searcher = make_unit(3, sim::Faction::Red, 480.0, 500.0);
  searcher.type_index = 0;
  searcher.stats.sight_radius = 30.0;
  sim::Unit enemy = make_unit(4, sim::Faction::Blue, 580.0, 500.0);
  enemy.type_index = 0;
  enemy.stats.sight_radius = 30.0;
  units = {seer, commander, searcher, enemy};
  sim::SimulationConfig config = tactics_config(two_types, 30);
  config.squads_enabled = true;
  config.squad_plans[kRed] = sim::SquadPlan{1, 2};
  config.composition[kRed] = {{0, 2}, {1, 1}};
  config.composition[kBlue] = {{0, 1}};
  config.tactics = &tactics;
  // The searcher starts Advancing; without the stickiness bonus the
  // fresh-contact Search score (0.75) can beat Advance (0.715) exactly at
  // the delay boundary.
  config.action_stickiness = 0.0;
  std::vector<sim::Personality> personalities;
  for (std::size_t i = 0; i < units.size(); ++i) {
    personalities.push_back({0.9, 0.2, 0.05, 0.9, sim::kCustomArchetype});
  }
  sim::Simulation simulation(config, units, personalities);
  std::string actions;
  for (int tick = 1; tick <= 12; ++tick) {
    simulation.tick();
    actions += std::to_string(static_cast<int>(static_cast<sim::Action>(
                  simulation.world().units[2].last_action)));
  }
  // Delay 5: nothing readable before tick 6 — the searcher advances.
  for (int tick = 0; tick < 5; ++tick) {
    EXPECT_EQ(actions[tick], '2') << "Search before the share delay; "
                                     "actions: " << actions;
  }
  // From tick 6 the published contact is readable; with a decision
  // interval of at most 5 the searcher must have re-decided by tick 11.
  bool searched = false;
  for (int tick = 5; tick < 12; ++tick) {
    searched = searched || actions[tick] == '5';
  }
  EXPECT_TRUE(searched) << "shared contact never triggered Search; "
                           "actions: " << actions;
}

// ---------------------------------------------------------------------------
// m13 item 2: availability v2 — the regroup lift, the Help cap and
// estimate decay.

TEST(AvailabilityV2Test, RegroupNeverLiftsAnUnavailableAdvance) {
  // With tactics on and a visible enemy, Advance is unavailable (-1).
  // The M10 regroup term clamped it to a pseudo-available 0.0 (the m12
  // audit root cause: 0.0 + the m11b order bonus outranked Approach, so
  // ordered armies never closed to melee). availability_v2 keeps it
  // excluded; v1 keeps the exact M10 clamp.
  sim::ScoreInputs inputs;
  inputs.has_visible_enemy = true;
  inputs.has_squad = true;
  inputs.squad_cohesion = 0.0;  // worst cohesion: maximum regroup pull
  const sim::TacticsConfig tactics{};
  inputs.availability_v2 = false;
  const sim::ActionScores v1 = sim::score_actions(inputs, &tactics);
  EXPECT_DOUBLE_EQ(v1[static_cast<std::size_t>(sim::Action::Advance)],
                   0.0);
  inputs.availability_v2 = true;
  const sim::ActionScores v2 = sim::score_actions(inputs, &tactics);
  EXPECT_DOUBLE_EQ(v2[static_cast<std::size_t>(sim::Action::Advance)],
                   -1.0);
  // With no enemy visible Advance is genuinely available and the regroup
  // bonus applies identically in both versions.
  inputs.has_visible_enemy = false;
  inputs.availability_v2 = false;
  const sim::ActionScores v1_open = sim::score_actions(inputs, &tactics);
  inputs.availability_v2 = true;
  const sim::ActionScores v2_open = sim::score_actions(inputs, &tactics);
  EXPECT_GT(v1_open[static_cast<std::size_t>(sim::Action::Advance)], 0.5);
  EXPECT_DOUBLE_EQ(v1_open[static_cast<std::size_t>(sim::Action::Advance)],
                   v2_open[static_cast<std::size_t>(sim::Action::Advance)]);
}

TEST(HelpTest, CapBoundsHelpersPerAllyPerTick) {
  const sim::UnitTypeTable types = infantry_types();
  sim::TacticsConfig tactics = quick_tactics();
  tactics.help.max_helpers_per_ally = 2;
  // One badly wounded ally surrounded by five cautious would-be helpers
  // (the FiresForWoundedAlly personalities): under v2 at most two may
  // choose Help in one tick (unit index order decides which); v1 has no
  // cap.
  std::vector<sim::Personality> personalities = {
      {0.9, 0.9, 0.1, 0.5, sim::kCustomArchetype},
      {0.9, 0.9, 0.1, 0.5, sim::kCustomArchetype},
      {0.9, 0.9, 0.1, 0.5, sim::kCustomArchetype},
      {0.9, 0.9, 0.1, 0.5, sim::kCustomArchetype},
      {0.9, 0.9, 0.1, 0.5, sim::kCustomArchetype},
      {0.9, 0.9, 0.1, 0.5, sim::kCustomArchetype},
  };
  std::vector<sim::Unit> units{
      make_unit(1, sim::Faction::Red, 500.0, 500.0, 5.0),  // wounded ally
      make_unit(2, sim::Faction::Red, 502.0, 500.0),
      make_unit(3, sim::Faction::Red, 498.0, 500.0),
      make_unit(4, sim::Faction::Red, 500.0, 502.0),
      make_unit(5, sim::Faction::Red, 500.0, 498.0),
      make_unit(6, sim::Faction::Red, 503.0, 502.0),
  };
  const auto count_helping = [](const sim::Simulation& battle) {
    int helping = 0;
    for (const sim::Unit& unit : battle.world().units) {
      if (unit.last_action == static_cast<std::uint8_t>(sim::Action::Help)) {
        ++helping;
      }
    }
    return helping;
  };
  sim::SimulationConfig config = tactics_config(types, 3);
  config.tactics = &tactics;
  config.ai_avail_v2 = true;
  sim::Simulation v2(config, units, personalities);
  v2.tick();
  EXPECT_GE(count_helping(v2), 1) << "the cap disabled Help entirely";
  EXPECT_LE(count_helping(v2), 2) << "the per-ally cap was not enforced";
  config.ai_avail_v2 = false;
  sim::Simulation v1(config, units, personalities);
  v1.tick();
  EXPECT_GT(count_helping(v1), 2) << "v1 unexpectedly capped helpers";
}

TEST(HelpTest, RoutedAllyFarFromCombatIsNeverHelped) {
  const sim::UnitTypeTable types = infantry_types();
  sim::TacticsConfig tactics = quick_tactics();
  // A routed, badly hurt ally with no enemy within
  // routed_help_enemy_distance (30) of it is out of the fight: not a
  // Help target under v2 (the lone enemy is 400 units away); v1 still
  // helps it.
  std::vector<sim::Personality> personalities = {
      {0.9, 0.9, 0.1, 0.5, sim::kCustomArchetype},
      {0.9, 0.9, 0.1, 0.5, sim::kCustomArchetype},
      {0.5, 0.5, 0.5, 0.5, sim::kCustomArchetype},
  };
  std::vector<sim::Unit> units{
      make_unit(1, sim::Faction::Red, 100.0, 100.0, 5.0),  // routed ally
      make_unit(2, sim::Faction::Red, 104.0, 100.0),       // would help
      make_unit(3, sim::Faction::Blue, 500.0, 500.0, 1000000.0),
  };
  units[0].routed = true;
  units[0].morale = 5.0;
  units[1].morale = 60.0;  // make_unit presets morale 0: would Flee
  units[2].morale = 60.0;
  sim::SimulationConfig config = tactics_config(types, 3);
  config.tactics = &tactics;
  // Morale on: the preset routed flag only persists with morale (with
  // morale off the sim clears routing state — there is no routing).
  config.morale_enabled = true;
  config.ai_avail_v2 = true;
  sim::Simulation v2(config, units, personalities);
  v2.tick();
  EXPECT_NE(static_cast<sim::Action>(v2.world().units[1].last_action),
            sim::Action::Help);
  config.ai_avail_v2 = false;
  sim::Simulation v1(config, units, personalities);
  v1.tick();
  EXPECT_EQ(static_cast<sim::Action>(v1.world().units[1].last_action),
            sim::Action::Help);
}

TEST(HelpTest, RoutedAllyNearEnemyIsStillHelpedUnderV2) {
  const sim::UnitTypeTable types = infantry_types();
  sim::TacticsConfig tactics = quick_tactics();
  // The complement of RoutedAllyFarFromCombatIsNeverHelped: a routed,
  // badly hurt ally WITH a living enemy within routed_help_enemy_distance
  // (29 < 30) stays a Help target under v2. The enemy sits behind the
  // ally, outside every helper's sight (each helper is more than 40 from
  // it, so the helpers see no enemy and Help is available), and three
  // helpers share the same answer for the same ally — m13 item P probes
  // once per routed unit per tick, not once per helper. The per-ally cap
  // (2 of 3 helpers) still applies.
  std::vector<sim::Personality> personalities = {
      {0.9, 0.9, 0.1, 0.5, sim::kCustomArchetype},  // routed ally
      {0.9, 0.9, 0.1, 0.5, sim::kCustomArchetype},  // helpers
      {0.9, 0.9, 0.1, 0.5, sim::kCustomArchetype},
      {0.9, 0.9, 0.1, 0.5, sim::kCustomArchetype},
      {0.5, 0.5, 0.5, 0.5, sim::kCustomArchetype},  // enemy
  };
  std::vector<sim::Unit> units{
      make_unit(1, sim::Faction::Red, 100.0, 100.0, 5.0),  // routed ally
      make_unit(2, sim::Faction::Red, 100.0, 66.0),        // helper 1
      make_unit(3, sim::Faction::Red, 104.0, 66.0),        // helper 2
      make_unit(4, sim::Faction::Red, 96.0, 66.0),         // helper 3
      make_unit(5, sim::Faction::Blue, 129.0, 100.0, 1000000.0),
  };
  units[0].routed = true;
  units[0].morale = 5.0;
  for (std::size_t i = 1; i <= 3; ++i) {
    units[i].morale = 60.0;  // make_unit presets morale 0: would Flee
  }
  units[4].morale = 60.0;
  sim::SimulationConfig config = tactics_config(types, 3);
  config.tactics = &tactics;
  // Morale on: the preset routed flag only persists with morale.
  config.morale_enabled = true;
  config.ai_avail_v2 = true;
  sim::Simulation v2(config, units, personalities);
  v2.tick();
  int helping = 0;
  for (std::size_t i = 1; i <= 3; ++i) {
    if (v2.world().units[i].last_action ==
        static_cast<std::uint8_t>(sim::Action::Help)) {
      ++helping;
    }
  }
  EXPECT_GE(helping, 1) << "the gate excluded the near-combat routed ally";
  EXPECT_LE(helping, 2) << "the per-ally cap was not enforced";
}

TEST(HelpTest, RoutedAllyExclusionPicksTheNearCombatAlly) {
  const sim::UnitTypeTable types = infantry_types();
  sim::TacticsConfig tactics = quick_tactics();
  // Two routed, badly hurt allies: the western one (id 2) is needier
  // (health 2.0 vs 5.0) but has no enemy within
  // routed_help_enemy_distance (52.9 > 30); the eastern one (id 1) sits
  // 29 from the enemy. Under v2 only the near-combat ally is a Help
  // target, so the helper marches straight north to it (x stays exactly
  // 100.0); under v1 the needier western ally wins the choice and the
  // helper veers west. Pins that the memoized probe answer is per routed
  // ALLY (m13 item P), never a per-tick or per-faction answer.
  std::vector<sim::Personality> personalities = {
      {0.9, 0.9, 0.1, 0.5, sim::kCustomArchetype},  // routed allies
      {0.9, 0.9, 0.1, 0.5, sim::kCustomArchetype},
      {0.9, 0.9, 0.1, 0.5, sim::kCustomArchetype},  // helper
      {0.5, 0.5, 0.5, 0.5, sim::kCustomArchetype},  // enemy
  };
  std::vector<sim::Unit> units{
      make_unit(1, sim::Faction::Red, 100.0, 100.0, 5.0),  // near combat
      make_unit(2, sim::Faction::Red, 80.0, 80.0, 2.0),    // needier, far
      make_unit(3, sim::Faction::Red, 100.0, 66.0),        // helper
      make_unit(4, sim::Faction::Blue, 129.0, 100.0, 1000000.0),
  };
  units[0].routed = true;
  units[1].routed = true;
  units[0].morale = 5.0;
  units[1].morale = 5.0;
  units[2].morale = 60.0;
  units[3].morale = 60.0;
  sim::SimulationConfig config = tactics_config(types, 3);
  config.tactics = &tactics;
  config.morale_enabled = true;
  config.ai_avail_v2 = true;
  sim::Simulation v2(config, units, personalities);
  v2.tick();
  const sim::Unit& helper = v2.world().units[2];
  EXPECT_EQ(static_cast<sim::Action>(helper.last_action), sim::Action::Help);
  EXPECT_DOUBLE_EQ(helper.x, 100.0)
      << "the helper veered off the near-combat ally";
  EXPECT_GT(helper.y, 66.0) << "the helper did not move toward the ally";
  config.ai_avail_v2 = false;
  sim::Simulation v1(config, units, personalities);
  v1.tick();
  EXPECT_LT(v1.world().units[2].x, 100.0)
      << "v1 should help the needier far ally and move west";
}

TEST(ContactSeekTest, ReachedEstimateWithNoEnemyDecays) {
  const sim::UnitTypeTable types = infantry_types();
  sim::TacticsConfig tactics = quick_tactics();
  tactics.contact_seek.idle_ticks = 3;
  tactics.contact_seek.estimate_max_age = 400;  // outlive the march
  // The spotter reports the enemy, then the enemy dies unseen: the
  // faction estimate goes stale. The parked observer seeks to the
  // estimate, arrives (within its sight of the estimated position) and
  // sees nothing — under v2 the estimate drops (tick returns to 0);
  // under v1 it survives and the observer keeps milling about it.
  // The observer starts 80 units east of the estimate: Search only
  // heads for contacts fresher than memory.forget_ticks, so the
  // estimate must be reachable inside that window (the seeker walks
  // ~2 units/tick).
  std::vector<sim::Unit> units{
      make_unit(1, sim::Faction::Red, 800.0, 500.0),             // seeker
      make_unit(2, sim::Faction::Red, 700.0, 500.0, 1000000.0),  // spotter
      make_unit(3, sim::Faction::Blue, 720.0, 500.0, 1000000.0), // enemy
      make_unit(4, sim::Faction::Blue, 990.0, 990.0, 1000000.0), // survivor
  };
  sim::SimulationConfig config = tactics_config(types, 200);
  config.tactics = &tactics;
  config.ai_avail_v2 = true;
  sim::Simulation v2(config, units);
  v2.tick();
  ASSERT_GT(v2.faction_estimate_tick(sim::Faction::Red), 0u);
  ASSERT_TRUE(v2.intervene_kill(3));  // the estimate goes stale
  for (int i = 0; i < 150 && !v2.finished(); ++i) {
    v2.tick();
  }
  EXPECT_EQ(v2.faction_estimate_tick(sim::Faction::Red), 0u)
      << "the stale estimate was not dropped on arrival";
  config.ai_avail_v2 = false;
  sim::Simulation v1(config, units);
  v1.tick();
  ASSERT_TRUE(v1.intervene_kill(3));
  for (int i = 0; i < 150 && !v1.finished(); ++i) {
    v1.tick();
  }
  EXPECT_GT(v1.faction_estimate_tick(sim::Faction::Red), 0u)
      << "v1 should keep the stale estimate";
}
