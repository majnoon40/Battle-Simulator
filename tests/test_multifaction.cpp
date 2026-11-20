// M14 item 2: multi-faction battles. Scenario parsing of the extra
// fixed-index factions (green..magenta), config validation, sector spawn
// determinism, grid-vs-brute nearest enemy on 3-faction states, a 3-faction
// battle smoke run (termination + result + export), and the write_scenario
// round-trip. Every 2-faction behavior is pinned by the existing suites —
// these tests cover what is NEW.
#include "sim/scenario.hpp"
#include "sim/simulation.hpp"
#include "sim/spatial_hash.hpp"
#include "sim/neighbors.hpp"
#include "sim/json_export.hpp"
#include "sim/unit_types.hpp"

#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

constexpr std::size_t kRed = static_cast<std::size_t>(sim::Faction::Red);
constexpr std::size_t kBlue = static_cast<std::size_t>(sim::Faction::Blue);
constexpr std::size_t kGreen = static_cast<std::size_t>(sim::Faction::Green);
constexpr std::size_t kYellow = static_cast<std::size_t>(sim::Faction::Yellow);

constexpr const char* kTable = R"json([
  {"id": "infantry", "health": 100.0, "damage": 12.0, "speed": 2.0, "range": 2.0, "cooldown": 5, "sight_radius": 40.0},
  {"id": "archer", "health": 70.0, "damage": 9.0, "speed": 1.6, "range": 8.0, "cooldown": 8, "sight_radius": 60.0}
])json";

sim::UnitTypeTable load_table() { return sim::parse_unit_types(kTable); }

void expect_reject(const std::string& text, const std::string& fragment) {
  try {
    static_cast<void>(sim::parse_scenario(text));
    FAIL() << "expected rejection, input was: " << text;
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find(fragment), std::string::npos)
        << "error was: " << error.what();
  }
}

constexpr const char* kThreeFactionScenario = R"json({
  "battlefield": {"width": 1000.0, "height": 1000.0},
  "spawn_band_fraction": 0.15,
  "armies": {
    "red": {"infantry": 60, "archer": 20},
    "blue": {"infantry": 60, "archer": 20},
    "green": {"infantry": 60, "archer": 20}
  }
})json";

sim::SimulationConfig three_faction_config(const sim::UnitTypeTable& types) {
  const sim::Scenario scenario = sim::parse_scenario(kThreeFactionScenario);
  sim::SimulationConfig config;
  config.seed = 7;
  config.max_ticks = 600;
  config.battlefield_width = scenario.battlefield_width;
  config.battlefield_height = scenario.battlefield_height;
  config.spawn_band_fraction = scenario.spawn_band_fraction;
  config.faction_count = scenario.faction_count;
  config.unit_types = &types;
  config.composition = sim::build_composition(scenario, types, config.seed);
  config.ai_mode = sim::AiMode::Utility;
  return config;
}

}  // namespace

TEST(MultiFactionParseTest, ParsesThreeFactionsWithFixedIndices) {
  const sim::Scenario scenario = sim::parse_scenario(kThreeFactionScenario);
  EXPECT_EQ(scenario.faction_count, 3);
  EXPECT_EQ(scenario.armies[kRed].size(), 2u);
  EXPECT_EQ(scenario.armies[kBlue].size(), 2u);
  EXPECT_EQ(scenario.armies[kGreen].size(), 2u);
  // File order must not matter: green is index 2 even if written first.
  const std::string reordered = R"json({
    "battlefield": {"width": 1000.0, "height": 1000.0},
    "spawn_band_fraction": 0.15,
    "armies": {
      "green": {"infantry": 10},
      "blue": {"infantry": 20},
      "red": {"infantry": 30}
    }
  })json";
  const sim::Scenario swapped = sim::parse_scenario(reordered);
  EXPECT_EQ(swapped.faction_count, 3);
  EXPECT_EQ(swapped.armies[kRed][0].count, 30u);
  EXPECT_EQ(swapped.armies[kGreen][0].count, 10u);
}

TEST(MultiFactionParseTest, RejectsMissingRequiredFaction) {
  expect_reject(R"json({
    "battlefield": {"width": 1000.0, "height": 1000.0},
    "spawn_band_fraction": 0.15,
    "armies": {"red": {"infantry": 10}, "green": {"infantry": 10}}
  })json",
                "blue");
}

TEST(MultiFactionParseTest, RejectsUnknownFactionKey) {
  expect_reject(R"json({
    "battlefield": {"width": 1000.0, "height": 1000.0},
    "spawn_band_fraction": 0.15,
    "armies": {"red": {"infantry": 10}, "blue": {"infantry": 10},
               "crimson": {"infantry": 10}}
  })json",
                "unknown armies faction 'crimson'");
}

TEST(MultiFactionParseTest, RejectsHolesInTheFactionSet) {
  // magenta is index 7: fielding it implies all of 0..7, so the missing
  // green/yellow/purple/orange/cyan armies must be rejected by name.
  expect_reject(R"json({
    "battlefield": {"width": 1000.0, "height": 1000.0},
    "spawn_band_fraction": 0.15,
    "armies": {"red": {"infantry": 10}, "blue": {"infantry": 10},
               "magenta": {"infantry": 10}}
  })json",
                "green");
}

TEST(MultiFactionParseTest, RejectsPerFactionDataForUnfieldedFactions) {
  const std::string base = R"json({
    "battlefield": {"width": 1000.0, "height": 1000.0},
    "spawn_band_fraction": 0.15,
    "armies": {"red": {"infantry": 10}, "blue": {"infantry": 10}}
  })json";
  // objectives / squads / spawn_patterns for a faction that has no army.
  std::string with_objective = base.substr(0, base.size() - 2);
  with_objective +=
      ",\n  \"objectives\": {\"green\": {\"x\": 100.0, \"y\": 100.0}}\n}";
  expect_reject(with_objective, "objectives faction 'green'");
  std::string with_squads = base.substr(0, base.size() - 2);
  with_squads += ",\n  \"squads\": {\"green\": {\"count\": 1}}\n}";
  expect_reject(with_squads, "squads faction 'green'");
  std::string with_pattern = base.substr(0, base.size() - 2);
  with_pattern += ",\n  \"spawn_patterns\": {\"green\": \"line\"}\n}";
  expect_reject(with_pattern, "spawn_patterns faction 'green'");
}

TEST(MultiFactionParseTest, WriteScenarioRoundTripsThreeFactions) {
  const sim::Scenario scenario = sim::parse_scenario(kThreeFactionScenario);
  const std::string text = sim::write_scenario(scenario);
  const sim::Scenario reparsed = sim::parse_scenario(text);
  EXPECT_EQ(reparsed.faction_count, scenario.faction_count);
  for (std::size_t f = 0; f < scenario.faction_count; ++f) {
    ASSERT_EQ(reparsed.armies[f].size(), scenario.armies[f].size());
    for (std::size_t i = 0; i < scenario.armies[f].size(); ++i) {
      EXPECT_EQ(reparsed.armies[f][i].type_id, scenario.armies[f][i].type_id);
      EXPECT_EQ(reparsed.armies[f][i].count, scenario.armies[f][i].count);
    }
  }
}

TEST(MultiFactionConfigTest, ValidatesFactionCountRules) {
  const sim::UnitTypeTable types = load_table();
  sim::SimulationConfig config = three_faction_config(types);

  // Valid 3-faction composition constructs and runs one tick.
  sim::Simulation sim{config};
  sim.tick();
  EXPECT_EQ(sim.faction_count(), 3u);

  // Legacy mode (no composition) cannot field more than two armies.
  sim::SimulationConfig legacy = config;
  legacy.composition = {};
  legacy.red_stats.max_health = 100.0;
  legacy.blue_stats.max_health = 100.0;
  legacy.total_units = 30;
  try {
    static_cast<void>(sim::Simulation{legacy});
    FAIL() << "expected rejection of multi-faction legacy mode";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("composition"), std::string::npos)
        << "error was: " << error.what();
  }

  // A composition for a faction that is not fielded is a configuration
  // error, not silent data.
  sim::SimulationConfig extra = config;
  extra.composition[kYellow].push_back({0u, 5u});
  try {
    static_cast<void>(sim::Simulation{extra});
    FAIL() << "expected rejection of an unfielded faction's composition";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("not fielded"), std::string::npos)
        << "error was: " << error.what();
  }

  // faction_count itself must stay in [2, kMaxFactions].
  sim::SimulationConfig broken = config;
  broken.faction_count = 1;
  try {
    static_cast<void>(sim::Simulation{broken});
    FAIL() << "expected rejection of faction_count < 2";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("faction_count"),
              std::string::npos)
        << "error was: " << error.what();
  }
}

TEST(MultiFactionSpawnTest, SectorSpawnIsDeterministicPerSeed) {
  const sim::UnitTypeTable types = load_table();
  const sim::SimulationConfig config = three_faction_config(types);
  const sim::Simulation a{config};
  const sim::Simulation b{config};
  const sim::World& wa = a.world();
  const sim::World& wb = b.world();
  ASSERT_EQ(wa.units.size(), wb.units.size());
  for (std::size_t i = 0; i < wa.units.size(); ++i) {
    EXPECT_EQ(wa.units[i].id, wb.units[i].id);
    EXPECT_EQ(wa.units[i].faction, wb.units[i].faction);
    EXPECT_DOUBLE_EQ(wa.units[i].x, wb.units[i].x);
    EXPECT_DOUBLE_EQ(wa.units[i].y, wb.units[i].y);
  }
  // Every faction actually spawns, in separated sectors around the
  // center: red near west, blue near east, green at its own angle.
  std::size_t per_faction[3] = {0, 0, 0};
  for (const sim::Unit& unit : wa.units) {
    const auto index = static_cast<std::size_t>(unit.faction);
    ASSERT_LT(index, 3u);
    ++per_faction[index];
  }
  EXPECT_EQ(per_faction[0], 80u);
  EXPECT_EQ(per_faction[1], 80u);
  EXPECT_EQ(per_faction[2], 80u);
}

TEST(MultiFactionGridTest, NearestEnemyMatchesBruteForceOnThreeFactions) {
  // Random 3-faction state, grid vs brute oracle for every unit: the
  // nearest of ALL other factions, ties to the lower id.
  sim::World world;
  world.width = 400.0;
  world.height = 400.0;
  std::vector<sim::Unit>& units = world.units;
  sim::Rng rng{1234};
  for (std::uint32_t i = 0; i < 90; ++i) {
    sim::Unit unit;
    unit.id = i + 1;
    unit.faction = static_cast<sim::Faction>(i % 3);
    unit.x = rng.next_double() * world.width;
    unit.y = rng.next_double() * world.height;
    unit.alive = true;
    units.push_back(unit);
  }
  sim::SpatialHashGrid grid;
  grid.configure(world.width, world.height, 32.0, 8, 3);
  grid.build(units);
  for (std::size_t i = 0; i < units.size(); ++i) {
    const int grid_answer = grid.nearest_enemy(units, units[i]);
    const int brute_answer = sim::nearest_enemy_index(units, units[i]);
    EXPECT_EQ(grid_answer, brute_answer)
        << "unit " << units[i].id << " of faction "
        << static_cast<int>(static_cast<std::uint8_t>(units[i].faction));
  }
}

TEST(MultiFactionBattleTest, ThreeFactionBattleRunsToTermination) {
  const sim::UnitTypeTable types = load_table();
  const sim::SimulationConfig config = three_faction_config(types);
  sim::Simulation sim{config};
  const sim::BattleResult result = sim.run();
  EXPECT_TRUE(result.finished);
  EXPECT_EQ(result.faction_count, 3);
  std::uint32_t survivors = 0;
  for (std::size_t f = 0; f < result.faction_count; ++f) {
    survivors += result.factions[f].survivors;
    EXPECT_GT(result.factions[f].initial, 0u);
  }
  // Elimination ends with at most one standing faction; a tick-limit
  // three-way grind may legitimately leave several (the winner, if any,
  // is the most-survivors rule).
  if (result.reason == sim::TerminationReason::FactionEliminated) {
    std::size_t with_survivors = 0;
    for (std::size_t f = 0; f < result.faction_count; ++f) {
      if (result.factions[f].survivors > 0) {
        ++with_survivors;
      }
    }
    EXPECT_LE(with_survivors, 1u);
  }
  if (result.has_winner) {
    EXPECT_GT(result.factions[static_cast<std::size_t>(result.winner)]
                  .survivors,
              0u);
  }

  // The JSON export lists exactly the fielded factions, in index order.
  const std::string json = sim::export_state_json(sim);
  EXPECT_NE(json.find("\"Red\""), std::string::npos);
  EXPECT_NE(json.find("\"Blue\""), std::string::npos);
  EXPECT_NE(json.find("\"Green\""), std::string::npos);
  EXPECT_EQ(json.find("\"Yellow\""), std::string::npos);
}

TEST(MultiFactionBattleTest, TwoFactionBattlesStillFieldTwoFactions) {
  // The default config (faction_count 2) is untouched: the export lists
  // Red and Blue only.
  const sim::UnitTypeTable types = load_table();
  sim::SimulationConfig config;
  config.seed = 3;
  config.max_ticks = 50;
  config.unit_types = &types;
  const int infantry = types.index_of("infantry");
  config.composition[kRed].push_back({static_cast<std::uint32_t>(infantry), 10});
  config.composition[kBlue].push_back({static_cast<std::uint32_t>(infantry), 10});
  config.ai_mode = sim::AiMode::Legacy;
  sim::Simulation sim{config};
  EXPECT_EQ(sim.faction_count(), 2u);
  const std::string json = sim::export_state_json(sim);
  EXPECT_NE(json.find("\"Red\""), std::string::npos);
  EXPECT_NE(json.find("\"Blue\""), std::string::npos);
  EXPECT_EQ(json.find("\"Green\""), std::string::npos);
}
