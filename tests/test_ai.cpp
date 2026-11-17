// M4 perception and utility AI tests: personality tables and assignment,
// score formulas (bounds, monotonicity, metamorphic invariance), sight
// boundaries, dead units, personality contrast, objectives, stickiness,
// determinism, and the utility-mode export fields.
#include "sim/ai.hpp"
#include "sim/config.hpp"
#include "sim/json_export.hpp"
#include "sim/personalities.hpp"
#include "sim/rng.hpp"
#include "sim/scenario.hpp"
#include "sim/simulation.hpp"
#include "sim/unit_types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "allocation_counter.hpp"

namespace {

constexpr std::size_t kRed = static_cast<std::size_t>(sim::Faction::Red);
constexpr std::size_t kBlue = static_cast<std::size_t>(sim::Faction::Blue);

sim::UnitStats utility_stats() {
  sim::UnitStats stats;
  stats.max_health = 100.0;
  stats.speed = 2.0;
  stats.damage = 12.0;
  stats.attack_range = 2.0;
  stats.attack_cooldown_ticks = 5;
  stats.sight_radius = 40.0;
  return stats;
}

sim::Unit make_unit(std::uint32_t id, sim::Faction faction, double x, double y,
                    double health = 100.0) {
  sim::Unit unit;
  unit.id = id;
  unit.faction = faction;
  unit.x = x;
  unit.y = y;
  unit.health = health;
  unit.stats = utility_stats();
  unit.alive = true;
  return unit;
}

sim::UnitStats archer_stats() {
  sim::UnitStats stats;
  stats.max_health = 70.0;
  stats.speed = 1.6;
  stats.damage = 9.0;
  stats.attack_range = 8.0;
  stats.attack_cooldown_ticks = 8;
  stats.sight_radius = 60.0;
  return stats;
}

// A minimal utility config for explicit-state tests: small world, no cap
// issues, legacy stats are irrelevant because units carry their own stats.
sim::SimulationConfig utility_config() {
  // M10: these are M4-era reproduction tests; tactical depth (variance,
  // decision intervals, memory, Search/Help/Flank) is off so the
  // documented M4 behavior and format-3 exports are unchanged.

  sim::SimulationConfig config;
  config.seed = 42;
  config.max_ticks = 100;
  config.battlefield_width = 400.0;
  config.battlefield_height = 400.0;
  config.ai_mode = sim::AiMode::Utility;
  config.morale_enabled = false;  // M5: M4 semantics  // the default, pinned for clarity
  config.morale_enabled = false;  // M5: M4 semantics (morale off == M4 exactly)
  config.red_stats = utility_stats();
  config.blue_stats = utility_stats();
  config.tactics_enabled = false;
  return config;
}

const char* kValidPersonalities = R"json({
  "archetypes": [
    {"id": "brave", "traits": {"bravery": [0.75, 1.0], "caution": [0.0, 0.25], "aggression": [0.5, 1.0], "discipline": [0.4, 0.8]}, "spawn_weight": 1.0},
    {"id": "cautious", "traits": {"bravery": [0.0, 0.25], "caution": [0.75, 1.0], "aggression": [0.0, 0.4], "discipline": [0.6, 1.0]}, "spawn_weight": 1.0},
    {"id": "aggressive", "traits": {"bravery": [0.5, 0.9], "caution": [0.0, 0.3], "aggression": [0.8, 1.0], "discipline": [0.3, 0.7]}, "spawn_weight": 1.0},
    {"id": "balanced", "traits": {"bravery": [0.35, 0.65], "caution": [0.35, 0.65], "aggression": [0.35, 0.65], "discipline": [0.35, 0.65]}, "spawn_weight": 1.0}
  ]
})json";

void expect_reject(const std::string& json, const std::string& fragment) {
  try {
    static_cast<void>(sim::parse_personalities(json));
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find(fragment), std::string::npos)
        << "error was: " << error.what();
    return;
  }
  FAIL() << "expected rejection containing '" << fragment << "'";
}

}  // namespace

// ---------------------------------------------------------------------------
// Personality tables.
// ---------------------------------------------------------------------------

TEST(PersonalityTableTest, ParsesTheShippedFile) {
  const sim::PersonalityTable table = sim::parse_personalities(kValidPersonalities);
  ASSERT_EQ(table.size(), 4u);
  EXPECT_EQ(table.archetypes[0].id, "brave");
  EXPECT_DOUBLE_EQ(table.archetypes[0].bravery_min, 0.75);
  EXPECT_DOUBLE_EQ(table.archetypes[0].caution_max, 0.25);
  EXPECT_EQ(table.index_of("cautious"), 1);
  EXPECT_EQ(table.index_of("balanced"), 3);
  EXPECT_EQ(table.index_of("unknown"), -1);
}

TEST(PersonalityTableTest, CommittedPersonalityFileIsValid) {
  std::ifstream file(std::string(TEST_DATA_DIR) + "/personalities.json");
  ASSERT_TRUE(file.good());
  const sim::PersonalityTable table = sim::parse_personalities(file);
  EXPECT_GE(table.size(), 1u);
  // The shipped table matches the built-in default used when no file is set.
  const sim::PersonalityTable& defaults = sim::default_personality_table();
  ASSERT_EQ(table.size(), defaults.size());
  for (std::size_t i = 0; i < table.size(); ++i) {
    EXPECT_EQ(table.archetypes[i].id, defaults.archetypes[i].id);
  }
}

TEST(PersonalityTableTest, RejectsInvalidTables) {
  expect_reject("[]", "must be a JSON object");
  expect_reject("{}", "missing required field 'archetypes'");
  expect_reject(R"({"archetypes": []})", "at least one archetype");
  expect_reject(R"({"archetypes": [{"id": "a", "spawn_weight": 1.0}]})",
                "missing required field 'traits'");
  expect_reject(R"({"archetypes": [{"id": "a", "traits": {"bravery": [0, 1], "caution": [0, 1], "aggression": [0, 1]}, "spawn_weight": 1.0}]})",
                "missing required field 'discipline'");
  expect_reject(R"({"archetypes": [{"id": "a", "traits": {"bravery": [0, 1], "caution": [0, 1], "aggression": [0, 1], "discipline": [0.9, 0.1]}, "spawn_weight": 1.0}]})",
                "0 <= min <= max <= 1");
  expect_reject(R"({"archetypes": [{"id": "a", "traits": {"bravery": [0, 2], "caution": [0, 1], "aggression": [0, 1], "discipline": [0, 1]}, "spawn_weight": 1.0}]})",
                "0 <= min <= max <= 1");
  expect_reject(R"({"archetypes": [{"id": "a", "traits": {"bravery": [0, 1], "caution": [0, 1], "aggression": [0, 1], "discipline": [0, 1]}, "spawn_weight": -1.0}]})",
                "spawn_weight must be >= 0");
  expect_reject(R"({"archetypes": [{"id": "a", "traits": {"bravery": [0, 1], "caution": [0, 1], "aggression": [0, 1], "discipline": [0, 1], "cowardice": [0, 1]}, "spawn_weight": 1.0}]})",
                "unknown trait");
  expect_reject(R"({"archetypes": [{"id": "a", "traits": {"bravery": [0, 1], "caution": [0, 1], "aggression": [0, 1], "discipline": [0, 1]}, "spawn_weight": 1.0}, {"id": "a", "traits": {"bravery": [0, 1], "caution": [0, 1], "aggression": [0, 1], "discipline": [0, 1]}, "spawn_weight": 1.0}]})",
                "duplicate personality archetype id");
  expect_reject(R"({"archetypes": [{"id": "a", "traits": {"bravery": [0, 1], "caution": [0, 1], "aggression": [0, 1], "discipline": [0, 1]}, "spawn_weight": 0.0}]})",
                "at least one archetype with spawn_weight > 0");
  expect_reject(R"({"archetypes": [{"id": "a", "traits": {"bravery": [0, 1], "caution": [0, 1], "aggression": [0, 1], "discipline": [0, 1]}, "spawn_weight": 1.0}], "extra": 1})",
                "unknown field 'extra'");
}

TEST(PersonalityForTest, IsDeterministicAndInsideRanges) {
  const sim::PersonalityTable table = sim::parse_personalities(kValidPersonalities);
  // Same (seed, id): identical personality, every time.
  const sim::Personality a = sim::personality_for(42, 7, table);
  const sim::Personality b = sim::personality_for(42, 7, table);
  EXPECT_DOUBLE_EQ(a.bravery, b.bravery);
  EXPECT_DOUBLE_EQ(a.caution, b.caution);
  EXPECT_DOUBLE_EQ(a.aggression, b.aggression);
  EXPECT_DOUBLE_EQ(a.discipline, b.discipline);
  EXPECT_EQ(a.archetype, b.archetype);

  // Traits land inside their archetype's ranges, and every archetype in an
  // equal-weight table is reachable.
  std::vector<bool> seen(table.size(), false);
  for (std::uint32_t id = 1; id <= 2000; ++id) {
    const sim::Personality p = sim::personality_for(1234, id, table);
    ASSERT_LT(static_cast<std::size_t>(p.archetype), table.size());
    const sim::PersonalityArchetype& archetype =
        table.archetypes[p.archetype];
    EXPECT_GE(p.bravery, archetype.bravery_min);
    EXPECT_LE(p.bravery, archetype.bravery_max);
    EXPECT_GE(p.caution, archetype.caution_min);
    EXPECT_LE(p.caution, archetype.caution_max);
    EXPECT_GE(p.aggression, archetype.aggression_min);
    EXPECT_LE(p.aggression, archetype.aggression_max);
    // M6: discipline is drawn inside its own range too.
    EXPECT_GE(p.discipline, archetype.discipline_min);
    EXPECT_LE(p.discipline, archetype.discipline_max);
    seen[p.archetype] = true;
  }
  for (std::size_t i = 0; i < seen.size(); ++i) {
    EXPECT_TRUE(seen[i]) << "archetype " << i << " never drawn";
  }
}

// ---------------------------------------------------------------------------
// Score formulas.
// ---------------------------------------------------------------------------

TEST(ActionScoreTest, AvailableScoresStayInUnitInterval) {
  sim::Rng rng(99);
  for (int trial = 0; trial < 1000; ++trial) {
    sim::ScoreInputs inputs;
    inputs.health_fraction = rng.next_double();
    inputs.strength_ratio = rng.next_double();
    inputs.distance_fraction = rng.next_double();
    inputs.cooldown_readiness = rng.next_double();
    inputs.bravery = rng.next_double();
    inputs.caution = rng.next_double();
    inputs.aggression = rng.next_double();
    inputs.has_visible_enemy = rng.next_uint32(2) == 0;
    inputs.enemy_in_attack_range =
        inputs.has_visible_enemy && rng.next_uint32(2) == 0;
    const sim::ActionScores scores = sim::score_actions(inputs);
    for (std::size_t action = 0; action < sim::kActionCount; ++action) {
      const double score = scores[action];
      if (score >= 0.0) {
        ASSERT_GE(score, 0.0) << "action " << action;
        ASSERT_LE(score, 1.0) << "action " << action;
      } else {
        ASSERT_DOUBLE_EQ(score, -1.0) << "unavailable actions are -1";
      }
    }
    // Availability rules.
    EXPECT_EQ(scores[static_cast<std::size_t>(sim::Action::Attack)] >= 0.0,
              inputs.enemy_in_attack_range);
    EXPECT_EQ(scores[static_cast<std::size_t>(sim::Action::Approach)] >= 0.0,
              inputs.has_visible_enemy);
    EXPECT_EQ(scores[static_cast<std::size_t>(sim::Action::Advance)] >= 0.0,
              !inputs.has_visible_enemy);
    EXPECT_EQ(scores[static_cast<std::size_t>(sim::Action::Retreat)] >= 0.0,
              inputs.has_visible_enemy);
    EXPECT_GE(scores[static_cast<std::size_t>(sim::Action::Hold)], 0.0);
  }
}

TEST(ActionScoreTest, RetreatRisesAsHealthFalls) {
  sim::ScoreInputs inputs;
  inputs.strength_ratio = 0.5;
  inputs.cooldown_readiness = 1.0;
  inputs.caution = 0.5;
  inputs.has_visible_enemy = true;
  inputs.enemy_in_attack_range = false;
  double previous = -1.0;
  for (double health = 1.0; health >= 0.0; health -= 0.1) {
    inputs.health_fraction = health;
    const sim::ActionScores scores = sim::score_actions(inputs);
    const double retreat = scores[static_cast<std::size_t>(sim::Action::Retreat)];
    EXPECT_GT(retreat, previous);  // strictly rising as health falls
    previous = retreat;
  }
}

TEST(ActionScoreTest, AttackFallsAsLocalStrengthFalls) {
  sim::ScoreInputs inputs;
  inputs.health_fraction = 1.0;
  inputs.cooldown_readiness = 1.0;
  inputs.bravery = 0.5;
  inputs.enemy_in_attack_range = true;
  double previous = 2.0;
  for (double strength = 1.0; strength > 0.0; strength -= 0.05) {
    inputs.strength_ratio = strength;
    const sim::ActionScores scores = sim::score_actions(inputs);
    const double attack = scores[static_cast<std::size_t>(sim::Action::Attack)];
    EXPECT_LT(attack, previous);  // strictly falling as strength falls
    previous = attack;
  }
}

TEST(ActionScoreTest, ScoresIgnoreInvisibleEnemies) {
  // Metamorphic: an enemy outside sight radius never enters the score
  // inputs, so moving it farther away cannot change any score.
  sim::ScoreInputs inputs;
  inputs.health_fraction = 0.8;
  inputs.strength_ratio = 0.6;
  inputs.distance_fraction = 0.5;
  inputs.cooldown_readiness = 1.0;
  inputs.bravery = 0.4;
  inputs.caution = 0.3;
  inputs.aggression = 0.7;
  inputs.has_visible_enemy = true;
  inputs.enemy_in_attack_range = false;
  const sim::ActionScores before = sim::score_actions(inputs);
  // The far enemy moved from 500 to 5000 units away: nothing the scorer can
  // see changed (the perception would be identical), so neither do the scores.
  const sim::ActionScores after = sim::score_actions(inputs);
  for (std::size_t action = 0; action < sim::kActionCount; ++action) {
    EXPECT_DOUBLE_EQ(before[action], after[action]);
  }
}

TEST(ChooseActionTest, TiesBreakByFixedOrderAndStickinessHolds) {
  sim::ActionScores scores{};
  scores.fill(0.5);  // a perfect five-way tie
  // Enum order: Attack > Approach > Advance > Retreat > Hold.
  EXPECT_EQ(sim::choose_action(scores, sim::Action::None, 0.0),
            sim::Action::Attack);
  // Stickiness lifts the current action above an equal-scored rival.
  EXPECT_EQ(sim::choose_action(scores, sim::Action::Hold, 0.05),
            sim::Action::Hold);
  // ... but cannot beat a clearly better action.
  scores[static_cast<std::size_t>(sim::Action::Retreat)] = 0.9;
  EXPECT_EQ(sim::choose_action(scores, sim::Action::Hold, 0.05),
            sim::Action::Retreat);
}

// ---------------------------------------------------------------------------
// Perception boundaries (inclusive sight radius).
// ---------------------------------------------------------------------------

TEST(UtilityPerceptionTest, EnemyExactlyAtSightRadiusIsSeen) {
  sim::SimulationConfig config = utility_config();
  std::vector<sim::Unit> units = {
      make_unit(1, sim::Faction::Red, 100.0, 100.0),
      make_unit(2, sim::Faction::Blue, 140.0, 100.0),  // exactly 40.0 away
  };
  sim::Simulation simulation(config, units);
  simulation.tick();
  const sim::Perception& perception = simulation.perception(0);
  EXPECT_EQ(perception.visible_enemies, 1u);
  EXPECT_EQ(perception.nearest_visible_enemy, 1);
  EXPECT_NEAR(perception.nearest_visible_enemy_distance, 40.0, 1e-9);
}

TEST(UtilityPerceptionTest, EnemyJustBeyondSightRadiusIsNotSeen) {
  sim::SimulationConfig config = utility_config();
  std::vector<sim::Unit> units = {
      make_unit(1, sim::Faction::Red, 100.0, 100.0),
      make_unit(2, sim::Faction::Blue, 140.001, 100.0),  // 40.001 away
  };
  sim::Simulation simulation(config, units);
  simulation.tick();
  const sim::Perception& perception = simulation.perception(0);
  EXPECT_EQ(perception.visible_enemies, 0u);
  EXPECT_EQ(perception.nearest_visible_enemy, -1);
  // No enemy visible: the unit advances toward its objective (the center of
  // the opposing spawn band) instead of approaching.
  const sim::Unit& unit = simulation.world().units[0];
  EXPECT_EQ(static_cast<sim::Action>(unit.last_action), sim::Action::Advance);
}

TEST(UtilityPerceptionTest, AlliesAndEnemiesAreCountedSeparately) {
  sim::SimulationConfig config = utility_config();
  std::vector<sim::Unit> units = {
      make_unit(1, sim::Faction::Red, 100.0, 100.0),
      make_unit(2, sim::Faction::Red, 110.0, 100.0),    // ally, in sight
      make_unit(3, sim::Faction::Red, 90.0, 100.0),     // ally, in sight
      make_unit(4, sim::Faction::Blue, 120.0, 100.0),   // enemy, in sight
      make_unit(5, sim::Faction::Blue, 300.0, 300.0),   // enemy, far away
  };
  sim::Simulation simulation(config, units);
  simulation.tick();
  const sim::Perception& perception = simulation.perception(0);
  EXPECT_EQ(perception.visible_enemies, 1u);
  EXPECT_EQ(perception.visible_allies, 2u);
  EXPECT_EQ(perception.nearest_visible_enemy, 3);
}

TEST(UtilityPerceptionTest, DeadEnemiesAreNeverSeenOrTargeted) {
  sim::SimulationConfig config = utility_config();
  std::vector<sim::Unit> units = {
      make_unit(1, sim::Faction::Red, 100.0, 100.0),
      make_unit(2, sim::Faction::Blue, 110.0, 100.0),
      make_unit(3, sim::Faction::Blue, 120.0, 100.0),
  };
  units[1].alive = false;
  units[1].health = 0.0;
  sim::Simulation simulation(config, units);
  simulation.tick();
  const sim::Perception& perception = simulation.perception(0);
  EXPECT_EQ(perception.visible_enemies, 1u);  // only unit 3 counts
  EXPECT_EQ(perception.nearest_visible_enemy, 2);
}

TEST(UtilityPerceptionTest, MetamorphicFarEnemyMovementChangesNothing) {
  const auto run_with_far_x = [](double far_x) {
    sim::SimulationConfig config = utility_config();
    std::vector<sim::Unit> units = {
        make_unit(1, sim::Faction::Red, 100.0, 100.0),
        make_unit(2, sim::Faction::Blue, 120.0, 100.0),  // visible at 20
        make_unit(3, sim::Faction::Blue, far_x, 100.0),  // far outside sight
    };
    sim::Simulation simulation(config, units);
    simulation.tick();
    return std::make_pair(simulation.perception(0),
                          simulation.world().units[0].last_action);
  };
  const auto [near, near_action] = run_with_far_x(300.0);
  const auto [far, far_action] = run_with_far_x(390.0);
  EXPECT_EQ(near.visible_enemies, far.visible_enemies);
  EXPECT_EQ(near.visible_allies, far.visible_allies);
  EXPECT_EQ(near.nearest_visible_enemy, far.nearest_visible_enemy);
  EXPECT_DOUBLE_EQ(near.nearest_visible_enemy_distance,
                   far.nearest_visible_enemy_distance);
  EXPECT_EQ(near_action, far_action);
}

// ---------------------------------------------------------------------------
// Personalities in battle.
// ---------------------------------------------------------------------------

TEST(UtilityPersonalityTest, CautiousWoundedUnitRetreatsAndBraveOneFights) {
  // Same situation, two personalities: one cautious low-health unit next to
  // an overwhelming enemy force breaks away; a brave one keeps fighting.
  const auto run = [](const sim::Personality& personality) {
    sim::SimulationConfig config = utility_config();
    std::vector<sim::Unit> units;
    units.push_back(make_unit(1, sim::Faction::Red, 200.0, 200.0, 15.0));
    for (std::uint32_t id = 2; id <= 6; ++id) {  // four blue enemies in range
      units.push_back(make_unit(id, sim::Faction::Blue, 201.5, 200.0));
    }
    units.push_back(make_unit(7, sim::Faction::Red, 195.0, 200.0));  // ally
    // Explicit personalities: unit 0 gets the personality under test; the
    // others take a neutral default (the override vector covers every unit).
    std::vector<sim::Personality> all(units.size());
    all[0] = personality;
    sim::Simulation simulation(config, units, all);
    simulation.tick();
    return static_cast<sim::Action>(simulation.world().units[0].last_action);
  };

  sim::Personality cautious;
  cautious.bravery = 0.05;
  cautious.caution = 0.95;
  cautious.aggression = 0.10;
  cautious.archetype = sim::kCustomArchetype;

  sim::Personality brave;
  brave.bravery = 0.95;
  brave.caution = 0.05;
  brave.aggression = 0.80;
  brave.archetype = sim::kCustomArchetype;

  EXPECT_EQ(run(cautious), sim::Action::Retreat);
  EXPECT_EQ(run(brave), sim::Action::Attack);
}

TEST(UtilityPersonalityTest, AssignmentIsDeterministicAndSpawnIndependent) {
  // Personalities come from a hash of (seed, id), never from the spawn RNG:
  // spawn positions are identical in legacy and utility modes, and the same
  // unit gets the same personality in both modes.
  const sim::UnitTypeTable types = sim::parse_unit_types(R"json([
    {"id": "infantry", "health": 100.0, "damage": 12.0, "speed": 2.0, "range": 2.0, "sight_radius": 40.0, "cooldown": 5}
  ])json");
  sim::SimulationConfig base;
  base.seed = 42;
  base.total_units = 60;
  base.max_ticks = 400;
  base.unit_types = &types;
  base.composition[kRed] = {{0, 30}};
  base.composition[kBlue] = {{0, 30}};
  base.ai_mode = sim::AiMode::Utility;
  base.morale_enabled = false;  // M5: M4 semantics
  sim::Simulation utility(base);

  sim::SimulationConfig legacy_config = base;
  legacy_config.ai_mode = sim::AiMode::Legacy;
  sim::Simulation legacy(legacy_config);

  ASSERT_EQ(utility.world().units.size(), legacy.world().units.size());
  for (std::size_t i = 0; i < utility.world().units.size(); ++i) {
    const sim::Unit& a = utility.world().units[i];
    const sim::Unit& b = legacy.world().units[i];
    EXPECT_DOUBLE_EQ(a.x, b.x) << "spawn x differs at index " << i;
    EXPECT_DOUBLE_EQ(a.y, b.y) << "spawn y differs at index " << i;
    EXPECT_EQ(a.id, b.id);
    EXPECT_DOUBLE_EQ(a.personality.bravery, b.personality.bravery);
    EXPECT_DOUBLE_EQ(a.personality.caution, b.personality.caution);
    EXPECT_DOUBLE_EQ(a.personality.aggression, b.personality.aggression);
    EXPECT_EQ(a.personality.archetype, b.personality.archetype);
  }

  // And a second utility run reproduces them exactly.
  sim::Simulation again(base);
  for (std::size_t i = 0; i < again.world().units.size(); ++i) {
    EXPECT_DOUBLE_EQ(again.world().units[i].personality.bravery,
                     utility.world().units[i].personality.bravery);
  }
}

// ---------------------------------------------------------------------------
// Objectives.
// ---------------------------------------------------------------------------

TEST(UtilityObjectiveTest, DefaultsToOpposingSpawnBandCenter) {
  sim::SimulationConfig config = utility_config();
  config.battlefield_width = 1000.0;
  config.battlefield_height = 800.0;
  config.spawn_band_fraction = 0.15;
  // No enemy within sight (400 apart, sight 40): both units advance toward
  // their objectives, so the movement direction reveals them.
  std::vector<sim::Unit> units = {
      make_unit(1, sim::Faction::Red, 300.0, 400.0),
      make_unit(2, sim::Faction::Blue, 700.0, 400.0),
  };
  sim::Simulation simulation(config, units);
  simulation.tick();
  const sim::Unit& red = simulation.world().units[0];
  const sim::Unit& blue = simulation.world().units[1];
  // Red's objective: center of the Blue band = (1000 - 75, 400).
  EXPECT_EQ(static_cast<sim::Action>(red.last_action), sim::Action::Advance);
  EXPECT_GT(red.x, 300.0);  // moving right toward x = 925
  // Blue's objective: center of the Red band = (75, 400).
  EXPECT_EQ(static_cast<sim::Action>(blue.last_action), sim::Action::Advance);
  EXPECT_LT(blue.x, 700.0);
  EXPECT_DOUBLE_EQ(red.y, 400.0);  // objectives share y = height / 2
  EXPECT_DOUBLE_EQ(blue.y, 400.0);
}

TEST(UtilityObjectiveTest, ScenarioObjectivesOverrideTheDefault) {
  const sim::Scenario scenario = sim::parse_scenario(R"json({
    "battlefield": {"width": 1000.0, "height": 1000.0},
    "spawn_band_fraction": 0.15,
    "armies": {"red": {"infantry": 1}, "blue": {"infantry": 1}},
    "objectives": {"red": {"x": 100.0, "y": 900.0}, "blue": {"x": 900.0, "y": 100.0}}
  })json");
  ASSERT_TRUE(scenario.objectives[kRed].has_value());
  EXPECT_DOUBLE_EQ(scenario.objectives[kRed]->x, 100.0);
  EXPECT_DOUBLE_EQ(scenario.objectives[kRed]->y, 900.0);
  ASSERT_TRUE(scenario.objectives[kBlue].has_value());
  EXPECT_DOUBLE_EQ(scenario.objectives[kBlue]->x, 900.0);
  // Partial override: red only, blue keeps the default.
  const sim::Scenario partial = sim::parse_scenario(R"json({
    "battlefield": {"width": 1000.0, "height": 1000.0},
    "spawn_band_fraction": 0.15,
    "armies": {"red": {"infantry": 1}, "blue": {"infantry": 1}},
    "objectives": {"red": {"x": 0.0, "y": 0.0}}
  })json");
  EXPECT_TRUE(partial.objectives[kRed].has_value());
  EXPECT_FALSE(partial.objectives[kBlue].has_value());
}

TEST(UtilityObjectiveTest, OffBattlefieldObjectivesAreRejected) {
  EXPECT_THROW(
      sim::parse_scenario(R"json({
        "battlefield": {"width": 100.0, "height": 100.0},
        "spawn_band_fraction": 0.15,
        "armies": {"red": {"infantry": 1}, "blue": {"infantry": 1}},
        "objectives": {"red": {"x": 150.0, "y": 50.0}}
      })json"),
      std::invalid_argument);
  EXPECT_THROW(
      sim::parse_scenario(R"json({
        "battlefield": {"width": 100.0, "height": 100.0},
        "spawn_band_fraction": 0.15,
        "armies": {"red": {"infantry": 1}, "blue": {"infantry": 1}},
        "objectives": {"green": {"x": 50.0, "y": 50.0}}
      })json"),
      std::invalid_argument);
}

TEST(UtilityObjectiveTest, ExplicitObjectiveDrivesAdvanceDirection) {
  sim::SimulationConfig config = utility_config();
  config.objectives[kRed] = sim::ObjectivePoint{0.0, 0.0, true};
  std::vector<sim::Unit> units = {make_unit(1, sim::Faction::Red, 200.0, 200.0)};
  sim::Simulation simulation(config, units);
  simulation.tick();
  const sim::Unit& unit = simulation.world().units[0];
  EXPECT_EQ(static_cast<sim::Action>(unit.last_action), sim::Action::Advance);
  EXPECT_LT(unit.x, 200.0);  // toward (0, 0)
  EXPECT_LT(unit.y, 200.0);
}

// ---------------------------------------------------------------------------
// Behavior, stickiness, determinism, exports.
// ---------------------------------------------------------------------------

TEST(UtilityBehaviorTest, ArchersHoldAtTheirOwnRange) {
  // An archer (range 8) facing an approaching infantry unit (range 2): the
  // archer stops and shoots from its own range instead of closing to melee —
  // it never moves on a tick where it chose Attack, and it first attacks at
  // full range while the infantry is still far outside its own range.
  sim::SimulationConfig config = utility_config();
  config.max_ticks = 60;
  std::vector<sim::Unit> units = {
      make_unit(1, sim::Faction::Red, 200.0, 200.0),
      make_unit(2, sim::Faction::Blue, 208.0, 200.0),
  };
  units[0].stats = archer_stats();
  sim::Simulation simulation(config, units);
  std::uint64_t attack_ticks = 0;
  bool moved_while_attacking = false;
  double first_attack_distance = -1.0;
  double previous_x = units[0].x;
  double previous_y = units[0].y;
  for (int tick = 0; tick < 60 && !simulation.finished(); ++tick) {
    simulation.tick();
    const sim::Unit& archer = simulation.world().units[0];
    const sim::Unit& infantry = simulation.world().units[1];
    if (!archer.alive || !infantry.alive) {
      break;
    }
    if (static_cast<sim::Action>(archer.last_action) == sim::Action::Attack) {
      ++attack_ticks;
      if (first_attack_distance < 0.0) {
        // Decide-time distance from this tick's perception (the infantry
        // may already have moved closer by the time we read positions).
        first_attack_distance =
            simulation.perception(0).nearest_visible_enemy_distance;
      }
      if (archer.x != previous_x || archer.y != previous_y) {
        moved_while_attacking = true;
      }
    }
    previous_x = archer.x;
    previous_y = archer.y;
  }
  EXPECT_GT(attack_ticks, 0u);
  // Attacked from its own range (8), far outside the infantry's range (2).
  EXPECT_NEAR(first_attack_distance, 8.0, 1e-9);
  // Never moved on a tick it chose Attack: holding at range, not walking in.
  EXPECT_FALSE(moved_while_attacking);
}

TEST(UtilityBehaviorTest, ActionSwitchingStaysUnderDocumentedBound) {
  // Standard battle (500 infantry, seed 42, utility mode): with the default
  // +0.05 stickiness bonus, units switch actions rarely. Bound documented in
  // docs/ai.md: fewer than 8 switches per unit per 100 ticks on average.
  sim::SimulationConfig config;
  config.seed = 42;
  config.total_units = 500;
  config.max_ticks = 600;
  config.ai_mode = sim::AiMode::Utility;
  config.morale_enabled = false;  // M5: M4 semantics
  config.red_stats = utility_stats();
  config.blue_stats = utility_stats();
  sim::Simulation simulation(config);

  std::vector<std::uint8_t> previous(simulation.world().units.size(),
                                      static_cast<std::uint8_t>(sim::Action::None));
  std::uint64_t switches = 0;
  std::uint64_t ticks = 0;
  while (!simulation.finished()) {
    simulation.tick();
    ++ticks;
    for (std::size_t i = 0; i < simulation.world().units.size(); ++i) {
      const std::uint8_t action = simulation.world().units[i].last_action;
      if (action != previous[i]) {
        ++switches;
        previous[i] = action;
      }
    }
  }
  ASSERT_GT(ticks, 0u);
  const double switches_per_unit_per_100 =
      static_cast<double>(switches) /
      (static_cast<double>(simulation.world().units.size()) *
       static_cast<double>(ticks) / 100.0);
  std::printf("[info] standard battle, seed 42: %.2f action switches per unit "
              "per 100 ticks (bound: 8)\n",
              switches_per_unit_per_100);
  EXPECT_LT(switches_per_unit_per_100, 8.0)
      << "measured " << switches_per_unit_per_100 << " switches per unit per 100 ticks";
}

TEST(UtilityDeterminismTest, DoubleRunsAreByteIdentical) {
  const auto run = [] {
    sim::SimulationConfig config;
    config.seed = 42;
    config.total_units = 500;
    config.max_ticks = 600;
    config.ai_mode = sim::AiMode::Utility;
  config.morale_enabled = false;  // M5: M4 semantics
    config.red_stats = utility_stats();
    config.blue_stats = utility_stats();
    sim::Simulation simulation(config);
    static_cast<void>(simulation.run());
    return sim::export_state_json(simulation);
  };
  EXPECT_EQ(run(), run());
}

TEST(UtilityDeterminismTest, NeighborModeDoesNotAffectUtilityResults) {
  // Utility perception always uses the spatial grid, so --neighbor-mode has
  // no effect on utility battles.
  const auto run = [](sim::NeighborMode mode) {
    sim::SimulationConfig config;
    config.seed = 42;
    config.total_units = 500;
    config.max_ticks = 600;
    config.ai_mode = sim::AiMode::Utility;
  config.morale_enabled = false;  // M5: M4 semantics
    config.neighbor_mode = mode;
    config.red_stats = utility_stats();
    config.blue_stats = utility_stats();
    sim::Simulation simulation(config);
    static_cast<void>(simulation.run());
    return sim::export_state_json(simulation);
  };
  EXPECT_EQ(run(sim::NeighborMode::Grid), run(sim::NeighborMode::Brute));
}

TEST(UtilityExportTest, ContainsPersonalityLastActionAndActionCounts) {
  sim::SimulationConfig config = utility_config();
  config.combat_v2 = false;  // m13: keep the format-3 rung (no combat)
  config.max_ticks = 40;
  // In weapons range of each other from tick 1: Attack dominates every
  // personality at full health, so the battle produces attack actions.
  std::vector<sim::Unit> units = {
      make_unit(1, sim::Faction::Red, 200.0, 200.0),
      make_unit(2, sim::Faction::Blue, 201.5, 200.0),
  };
  sim::Simulation simulation(config, units);
  static_cast<void>(simulation.run());
  const std::string json = sim::export_state_json(simulation);

  EXPECT_NE(json.find("\"format_version\": 3"), std::string::npos);
  EXPECT_NE(json.find("\"personality\""), std::string::npos);
  EXPECT_NE(json.find("\"archetype\""), std::string::npos);
  EXPECT_NE(json.find("\"last_action\""), std::string::npos);
  EXPECT_NE(json.find("\"action_counts\""), std::string::npos);
  EXPECT_NE(json.find("\"attack\""), std::string::npos);
  EXPECT_NE(json.find("\"advance\""), std::string::npos);

  // The two units fought: at least one faction has attack actions.
  const sim::BattleResult result = simulation.result();
  EXPECT_GT(result.factions[kRed]
                .action_counts[static_cast<std::size_t>(sim::Action::Attack)] +
                result.factions[kBlue]
                    .action_counts[static_cast<std::size_t>(sim::Action::Attack)],
            0u);
}

TEST(UtilityDebugTest, CaptureRecordsScoresAndChoice) {
  sim::SimulationConfig config = utility_config();
  config.max_ticks = 50;
  std::vector<sim::Unit> units = {
      make_unit(1, sim::Faction::Red, 200.0, 200.0),
      make_unit(2, sim::Faction::Blue, 210.0, 200.0),
  };
  sim::Simulation simulation(config, units);
  sim::AiDebugCapture debug;
  debug.unit_id = 1;
  debug.explicit_tick = false;  // first tick it sees an enemy
  simulation.set_ai_debug(debug);
  static_cast<void>(simulation.run());
  const sim::AiDebugCapture& captured = simulation.ai_debug();
  EXPECT_TRUE(captured.unit_found);
  ASSERT_TRUE(captured.captured);
  EXPECT_EQ(captured.captured_tick, 1u);  // the enemy starts in sight
  EXPECT_EQ(captured.visible_enemies, 1u);
  EXPECT_EQ(captured.target, 1);
  for (std::size_t action = 0; action < sim::kActionCount; ++action) {
    EXPECT_TRUE(std::isfinite(captured.scores[action]));
  }
}

TEST(UtilityAllocationTest, SteadyStateTickLoopAllocatesNothing) {
  // Static walls in sight and weapons range of each other, under the
  // utility AI: perception buffers, scoring, combat and deaths all run with
  // zero steady-state allocations (shared operator-new counter, same
  // technique as the M3 test). Damage is 1.0 so neither wall is wiped out
  // before the measured window ends.
  const std::size_t* counter = allocation_counter();
  const double world = 1000.0;
  std::vector<sim::Unit> units;
  units.reserve(2000);
  std::uint32_t next_id = 1;
  for (std::uint32_t i = 0; i < 1000; ++i) {
    const double y = static_cast<double>(i) * (world / 1000.0);
    sim::Unit red = make_unit(next_id++, sim::Faction::Red, 496.0, y);
    red.stats.speed = 0.0;  // hold position: only Attack/Hold scores matter
    red.stats.damage = 1.0;
    units.push_back(red);
    sim::Unit blue = make_unit(next_id++, sim::Faction::Blue, 497.5, y);
    blue.stats.speed = 0.0;
    blue.stats.damage = 1.0;
    units.push_back(blue);
  }
  sim::SimulationConfig config = utility_config();
  config.max_ticks = 1000;
  config.battlefield_width = world;
  config.battlefield_height = world;
  sim::Simulation simulation(config, units);
  for (int i = 0; i < 10; ++i) {
    simulation.tick();  // warm-up
  }
  const std::size_t before = *counter;
  for (int i = 0; i < 100; ++i) {
    simulation.tick();
  }
  EXPECT_EQ(*counter, before) << "utility tick loop allocated "
                              << (*counter - before) << " times in steady state";
}
