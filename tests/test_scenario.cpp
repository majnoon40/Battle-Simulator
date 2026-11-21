// Scenario loading tests: valid scenarios, every validation failure from the
// M2 spec (malformed JSON, missing fields, invalid world/band/counts, empty
// armies, unit-cap overflow, unknown fields), composition building, and the
// committed scenario files.
#include "sim/config.hpp"
#include "sim/scenario.hpp"

#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

namespace {

void expect_reject(const std::string& text, const std::string& fragment) {
  try {
    static_cast<void>(sim::parse_scenario(text));
    FAIL() << "expected rejection, input was: " << text;
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find(fragment), std::string::npos)
        << "error was: " << error.what();
  }
}

constexpr const char* kValidScenario = R"json({
  "battlefield": {"width": 1000.0, "height": 1000.0},
  "spawn_band_fraction": 0.15,
  "armies": {
    "red": {"infantry": 100, "archer": 100, "cavalry": 50},
    "blue": {"infantry": 100, "archer": 100, "cavalry": 50}
  }
})json";

constexpr const char* kValidTable = R"json([
  {"id": "infantry", "health": 100.0, "damage": 12.0, "speed": 2.0, "range": 2.0, "cooldown": 5, "sight_radius": 40.0},
  {"id": "archer", "health": 70.0, "damage": 9.0, "speed": 1.6, "range": 8.0, "cooldown": 8, "sight_radius": 60.0},
  {"id": "cavalry", "health": 160.0, "damage": 20.0, "speed": 4.5, "range": 2.0, "cooldown": 7, "sight_radius": 50.0}
])json";

TEST(ScenarioTest, ParsesValidScenario) {
  const sim::Scenario scenario = sim::parse_scenario(kValidScenario);
  EXPECT_DOUBLE_EQ(scenario.battlefield_width, 1000.0);
  EXPECT_DOUBLE_EQ(scenario.battlefield_height, 1000.0);
  EXPECT_DOUBLE_EQ(scenario.spawn_band_fraction, 0.15);

  const auto& red = scenario.armies[static_cast<std::size_t>(sim::Faction::Red)];
  const auto& blue = scenario.armies[static_cast<std::size_t>(sim::Faction::Blue)];
  ASSERT_EQ(red.size(), 3u);
  ASSERT_EQ(blue.size(), 3u);
  // Composition entries keep file order (this is the spawn order).
  EXPECT_EQ(red[0].type_id, "infantry");
  EXPECT_EQ(red[1].type_id, "archer");
  EXPECT_EQ(red[2].type_id, "cavalry");
  EXPECT_EQ(red[2].count, 50u);
  EXPECT_EQ(blue[1].count, 100u);
}

TEST(ScenarioTest, StreamOverloadParsesTheSameScenario) {
  std::istringstream stream(kValidScenario);
  const sim::Scenario scenario = sim::parse_scenario(stream);
  EXPECT_DOUBLE_EQ(scenario.battlefield_width, 1000.0);
  EXPECT_EQ(scenario.armies[static_cast<std::size_t>(sim::Faction::Red)].size(), 3u);
}

TEST(ScenarioTest, MissingFieldsAreRejected) {
  expect_reject(R"({"spawn_band_fraction": 0.15, "armies": {"red": {"infantry": 1}, "blue": {"infantry": 1}}})",
                "missing required field 'battlefield'");
  expect_reject(R"({"battlefield": {"height": 100.0}, "spawn_band_fraction": 0.15, "armies": {"red": {"infantry": 1}, "blue": {"infantry": 1}}})",
                "missing required field 'width'");
  expect_reject(R"({"battlefield": {"width": 100.0}, "spawn_band_fraction": 0.15, "armies": {"red": {"infantry": 1}, "blue": {"infantry": 1}}})",
                "missing required field 'height'");
  expect_reject(R"({"battlefield": {"width": 100.0, "height": 100.0}, "armies": {"red": {"infantry": 1}, "blue": {"infantry": 1}}})",
                "missing required field 'spawn_band_fraction'");
  expect_reject(R"({"battlefield": {"width": 100.0, "height": 100.0}, "spawn_band_fraction": 0.15})",
                "missing required field 'armies'");
  expect_reject(R"({"battlefield": {"width": 100.0, "height": 100.0}, "spawn_band_fraction": 0.15, "armies": {"blue": {"infantry": 1}}})",
                "missing required field 'red'");
  expect_reject(R"({"battlefield": {"width": 100.0, "height": 100.0}, "spawn_band_fraction": 0.15, "armies": {"red": {"infantry": 1}}})",
                "missing required field 'blue'");
}

TEST(ScenarioTest, InvalidWorldSizeIsRejected) {
  expect_reject(R"({"battlefield": {"width": 0.0, "height": 100.0}, "spawn_band_fraction": 0.15, "armies": {"red": {"infantry": 1}, "blue": {"infantry": 1}}})",
                "'width' must be > 0");
  expect_reject(R"({"battlefield": {"width": 100.0, "height": -5.0}, "spawn_band_fraction": 0.15, "armies": {"red": {"infantry": 1}, "blue": {"infantry": 1}}})",
                "'height' must be > 0");
}

TEST(ScenarioTest, InvalidSpawnBandIsRejected) {
  expect_reject(R"({"battlefield": {"width": 100.0, "height": 100.0}, "spawn_band_fraction": 0.0, "armies": {"red": {"infantry": 1}, "blue": {"infantry": 1}}})",
                "(0, 0.5]");
  expect_reject(R"({"battlefield": {"width": 100.0, "height": 100.0}, "spawn_band_fraction": 0.75, "armies": {"red": {"infantry": 1}, "blue": {"infantry": 1}}})",
                "(0, 0.5]");
}

TEST(ScenarioTest, InvalidCountsAreRejected) {
  expect_reject(R"({"battlefield": {"width": 100.0, "height": 100.0}, "spawn_band_fraction": 0.15, "armies": {"red": {"infantry": 0}, "blue": {"infantry": 1}}})",
                "must be >= 1");
  expect_reject(R"({"battlefield": {"width": 100.0, "height": 100.0}, "spawn_band_fraction": 0.15, "armies": {"red": {"infantry": -3}, "blue": {"infantry": 1}}})",
                "must be >= 1");
  expect_reject(R"({"battlefield": {"width": 100.0, "height": 100.0}, "spawn_band_fraction": 0.15, "armies": {"red": {"infantry": 2.5}, "blue": {"infantry": 1}}})",
                "whole number");
  expect_reject(R"({"battlefield": {"width": 100.0, "height": 100.0}, "spawn_band_fraction": 0.15, "armies": {"red": {"infantry": "5"}, "blue": {"infantry": 1}}})",
                "whole number");
}

TEST(ScenarioTest, EmptyArmyIsRejected) {
  expect_reject(R"({"battlefield": {"width": 100.0, "height": 100.0}, "spawn_band_fraction": 0.15, "armies": {"red": {}, "blue": {"infantry": 1}}})",
                "at least one unit type");
}

TEST(ScenarioTest, UnitCapIsRejected) {
  // M3 raised the cap to 20,000, M15 S5 to 100,000; these stay above it.
  expect_reject(R"({"battlefield": {"width": 100.0, "height": 100.0}, "spawn_band_fraction": 0.15, "armies": {"red": {"infantry": 55000}, "blue": {"infantry": 55000}}})",
                "exceeds the 100000-unit cap");
  expect_reject(R"({"battlefield": {"width": 100.0, "height": 100.0}, "spawn_band_fraction": 0.15, "armies": {"red": {"infantry": 100001}, "blue": {"infantry": 1}}})",
                "exceeds the unit cap");
}

// m13 audit finding 4: squad plan `count` and `size` are validated
// against the uint32 destination range before conversion — the old
// unchecked static_cast truncated 4294967296 to 0.
TEST(ScenarioTest, SquadPlanCountAndSizeFitUint32) {
  const auto with_squads = [](const char* squads) {
    return std::string(
               R"({"battlefield": {"width": 100.0, "height": 100.0},)"
               R"( "spawn_band_fraction": 0.15,)"
               R"( "armies": {"red": {"infantry": 10}, "blue": {"infantry": 10}},)"
               R"( "squads": )") +
           squads + "}";
  };
  // Boundaries: 1 and uint32-max parse (semantic checks happen later,
  // when the plan meets the composition).
  {
    const sim::Scenario scenario = sim::parse_scenario(with_squads(
        R"({"red": {"count": 4294967295, "size": 4294967295}})"));
    ASSERT_TRUE(scenario.squads[0].has_value());
    EXPECT_EQ(scenario.squads[0]->count, 4294967295u);
    EXPECT_EQ(scenario.squads[0]->size, 4294967295u);
  }
  {
    const sim::Scenario scenario =
        sim::parse_scenario(with_squads(R"({"red": {"count": 1}})"));
    ASSERT_TRUE(scenario.squads[0].has_value());
    EXPECT_EQ(scenario.squads[0]->count, 1u);
    EXPECT_EQ(scenario.squads[0]->size, 25u);  // documented default
  }
  // Maximum plus one, and larger, are rejected for count...
  expect_reject(with_squads(R"({"red": {"count": 4294967296}})"),
                "'count' must not exceed 4294967295");
  expect_reject(with_squads(R"({"red": {"count": 9999999999}})"),
                "'count' must not exceed 4294967295");
  // ...and for size, consistently. (A uint64-max literal is stored as
  // unsigned by nlohmann; reading it as int64 wraps to -1, so it fails
  // the >= 1 check — the same convention as the army-count parser.)
  expect_reject(with_squads(R"({"red": {"count": 1, "size": 4294967296}})"),
                "'size' must not exceed 4294967295");
  expect_reject(with_squads(R"({"red": {"count": 1, "size": 5000000000}})"),
                "'size' must not exceed 4294967295");
  expect_reject(
      with_squads(R"({"red": {"count": 18446744073709551615}})"),
      "whole number >= 1");
}

TEST(ScenarioTest, UnknownFieldsAreRejected) {
  // A typo'd key is reported as unknown (not as a missing battlefield).
  expect_reject(R"({"battlefeild": {"width": 100.0, "height": 100.0}, "spawn_band_fraction": 0.15, "armies": {"red": {"infantry": 1}, "blue": {"infantry": 1}}})",
                "unknown scenario field 'battlefeild'");
  expect_reject(R"({"battlefield": {"width": 100.0, "height": 100.0}, "spawn_band_fraction": 0.15, "fog": true, "armies": {"red": {"infantry": 1}, "blue": {"infantry": 1}}})",
                "unknown scenario field 'fog'");
  expect_reject(R"({"battlefield": {"width": 100.0, "height": 100.0, "fog": true}, "spawn_band_fraction": 0.15, "armies": {"red": {"infantry": 1}, "blue": {"infantry": 1}}})",
                "unknown battlefield field 'fog'");
}

TEST(ScenarioTest, NonObjectRootIsRejected) {
  expect_reject("[]", "must be a JSON object");
  expect_reject("42", "must be a JSON object");
}

TEST(ScenarioTest, MalformedJsonIsRejected) {
  expect_reject("{", "not valid JSON");
  expect_reject(R"({"battlefield": )", "not valid JSON");
}

TEST(ScenarioTest, BuildCompositionResolvesTypesInFileOrder) {
  const sim::Scenario scenario = sim::parse_scenario(kValidScenario);
  const sim::UnitTypeTable table = sim::parse_unit_types(kValidTable);
  const auto composition = sim::build_composition(scenario, table);

  const auto& red = composition[static_cast<std::size_t>(sim::Faction::Red)];
  ASSERT_EQ(red.size(), 3u);
  EXPECT_EQ(red[0].type_index, table.index_of("infantry"));
  EXPECT_EQ(red[1].type_index, table.index_of("archer"));
  EXPECT_EQ(red[2].type_index, table.index_of("cavalry"));
  EXPECT_EQ(red[0].count, 100u);
  EXPECT_EQ(red[2].count, 50u);
}

TEST(ScenarioTest, BuildCompositionRejectsUnknownTypes) {
  const sim::UnitTypeTable table = sim::parse_unit_types(kValidTable);
  const sim::Scenario scenario = sim::parse_scenario(R"json({
    "battlefield": {"width": 100.0, "height": 100.0},
    "spawn_band_fraction": 0.15,
    "armies": {"red": {"catapult": 10}, "blue": {"infantry": 10}}
  })json");
  try {
    static_cast<void>(sim::build_composition(scenario, table));
    FAIL() << "expected rejection";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("unknown unit type 'catapult'"),
              std::string::npos)
        << "error was: " << error.what();
  }
}

TEST(ScenarioTest, CommittedScenarioFilesAreValid) {
  {
    std::ifstream file(TEST_DATA_DIR "/scenarios/default.json");
    ASSERT_TRUE(file.good());
    const sim::Scenario scenario = sim::parse_scenario(file);
    // Default scenario equals the M1 geometry and armies.
    EXPECT_DOUBLE_EQ(scenario.battlefield_width, 1000.0);
    EXPECT_DOUBLE_EQ(scenario.battlefield_height, 1000.0);
    EXPECT_DOUBLE_EQ(scenario.spawn_band_fraction, 0.15);
    const auto& red = scenario.armies[static_cast<std::size_t>(sim::Faction::Red)];
    const auto& blue = scenario.armies[static_cast<std::size_t>(sim::Faction::Blue)];
    ASSERT_EQ(red.size(), 1u);
    ASSERT_EQ(blue.size(), 1u);
    EXPECT_EQ(red[0].type_id, "infantry");
    EXPECT_EQ(red[0].count, 250u);
    EXPECT_EQ(blue[0].count, 250u);
  }
  {
    std::ifstream file(TEST_DATA_DIR "/scenarios/mixed.json");
    ASSERT_TRUE(file.good());
    const sim::Scenario scenario = sim::parse_scenario(file);
    const auto& red = scenario.armies[static_cast<std::size_t>(sim::Faction::Red)];
    const auto& blue = scenario.armies[static_cast<std::size_t>(sim::Faction::Blue)];
    ASSERT_EQ(red.size(), 3u);
    ASSERT_EQ(blue.size(), 3u);
    std::uint64_t total = 0;
    for (const auto& army : scenario.armies) {
      for (const sim::ScenarioEntry& entry : army) {
        total += entry.count;
      }
    }
    EXPECT_EQ(total, 500u);
  }
  {
    // M15 S5: the 100k scale point of the perf ladder — exactly the
    // raised cap, constant density with perf-20000.json (5600 = 2500 *
    // sqrt(5) to four significant digits, so 2.5x the spawn band per
    // side at the same units-per-area as the 20k scenario).
    std::ifstream file(TEST_DATA_DIR "/scenarios/perf-100000.json");
    ASSERT_TRUE(file.good());
    const sim::Scenario scenario = sim::parse_scenario(file);
    EXPECT_DOUBLE_EQ(scenario.battlefield_width, 5600.0);
    EXPECT_DOUBLE_EQ(scenario.battlefield_height, 5600.0);
    std::uint64_t total = 0;
    for (const auto& army : scenario.armies) {
      for (const sim::ScenarioEntry& entry : army) {
        total += entry.count;
      }
    }
    EXPECT_EQ(total, std::uint64_t{sim::kMaxUnits});
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// m13 item H(b): budget-form armies.

namespace {

// Costs: a = 1, b = 2.5, c = 4 (a omitted -> default 1).
constexpr const char* kCostTable = R"json([
  {"id": "a", "health": 10.0, "damage": 2.0, "speed": 1.0, "range": 3.0, "cooldown": 4, "sight_radius": 40.0},
  {"id": "b", "health": 10.0, "damage": 2.0, "speed": 1.0, "range": 3.0, "cooldown": 4, "sight_radius": 40.0, "cost": 2.5},
  {"id": "c", "health": 10.0, "damage": 2.0, "speed": 1.0, "range": 3.0, "cooldown": 4, "sight_radius": 40.0, "cost": 4.0}
])json";

sim::Scenario budget_scenario(const std::string& red_army) {
  const std::string json =
      R"({"battlefield": {"width": 1000.0, "height": 1000.0}, )"
      R"("spawn_band_fraction": 0.15, "armies": {"red": )" +
      red_army + R"(, "blue": {"a": 10}}})";
  return sim::parse_scenario(json);
}

void expect_scenario_reject(const std::string& red_army,
                            const std::string& fragment) {
  try {
    static_cast<void>(budget_scenario(red_army));
    FAIL() << "expected rejection for army: " << red_army;
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find(fragment), std::string::npos)
        << "error was: " << error.what();
  }
}

}  // namespace

TEST(ScenarioBudgetTest, ParsesBudgetForm) {
  const sim::Scenario scenario =
      budget_scenario(R"({"budget": 100, "mix": {"a": 2, "b": 1, "c": 1}})");
  const auto& budget = scenario.budgets[static_cast<std::size_t>(
      sim::Faction::Red)];
  ASSERT_TRUE(budget.has_value());
  EXPECT_DOUBLE_EQ(budget->budget, 100.0);
  ASSERT_EQ(budget->mix.size(), 3u);
  EXPECT_EQ(budget->mix[0].type_id, "a");
  EXPECT_EQ(budget->mix[0].count, 2u);  // the weight, until the fill
  EXPECT_EQ(budget->mix[2].type_id, "c");
  EXPECT_TRUE(scenario.armies[static_cast<std::size_t>(
                  sim::Faction::Red)].empty());
  // The other faction's count form is untouched.
  EXPECT_FALSE(scenario.budgets[static_cast<std::size_t>(
                   sim::Faction::Blue)].has_value());
  EXPECT_EQ(scenario.armies[static_cast<std::size_t>(sim::Faction::Blue)]
                .size(),
            1u);
}

TEST(ScenarioBudgetTest, RejectsMalformedBudgetArmies) {
  expect_scenario_reject(R"({"budget": 100})",
                         "both 'budget' and 'mix'");
  expect_scenario_reject(R"({"mix": {"a": 1}})",
                         "both 'budget' and 'mix'");
  expect_scenario_reject(R"({"budget": 100, "mix": {"a": 1}, "b": 5})",
                         "exactly 'budget' and 'mix'");
  expect_scenario_reject(R"({"budget": 0, "mix": {"a": 1}})",
                         "'budget' must be > 0");
  expect_scenario_reject(R"({"budget": -3, "mix": {"a": 1}})",
                         "'budget' must be > 0");
  expect_scenario_reject(R"({"budget": "big", "mix": {"a": 1}})",
                         "'budget' must be a number");
  expect_scenario_reject(R"({"budget": 100, "mix": {}})",
                         "non-empty object");
  expect_scenario_reject(R"({"budget": 100, "mix": {"a": 0}})",
                         "must be >= 1");
  expect_scenario_reject(R"({"budget": 100, "mix": {"a": 1.5}})",
                         "whole number");
}

TEST(ScenarioBudgetTest, DeterministicLargestRemainderFill) {
  const sim::UnitTypeTable table = sim::parse_unit_types(kCostTable);
  // budget 100, mix a:2 b:1 c:1 -> shares 50 / 25 / 25 points ->
  // floors 50 / 10 / 6 (costs 1 / 2.5 / 4), leftover 1 point is below
  // every remaining cost: {a 50, b 10, c 6}, 99 of 100 points spent.
  const sim::Scenario scenario =
      budget_scenario(R"({"budget": 100, "mix": {"a": 2, "b": 1, "c": 1}})");
  const auto composition = sim::build_composition(scenario, table, 42);
  const auto& red = composition[static_cast<std::size_t>(sim::Faction::Red)];
  ASSERT_EQ(red.size(), 3u);
  EXPECT_EQ(red[0].type_index, table.index_of("a"));
  EXPECT_EQ(red[0].count, 50u);
  EXPECT_EQ(red[1].count, 10u);
  EXPECT_EQ(red[2].count, 6u);
}

TEST(ScenarioBudgetTest, LeftoverPointsGoToLargestAffordableRemainder) {
  const sim::UnitTypeTable table = sim::parse_unit_types(kCostTable);
  // budget 103, mix a:2 b:1 c:1 -> shares 51.5 / 25.75 / 25.75 ->
  // floors 51 / 10 / 6 (spent 100), leftover 3: remainders a 0.5, b 0.75
  // (affordable, cost 2.5), c 1.75 (unaffordable, cost 4) -> one more b.
  const sim::Scenario scenario =
      budget_scenario(R"({"budget": 103, "mix": {"a": 2, "b": 1, "c": 1}})");
  const auto composition = sim::build_composition(scenario, table, 42);
  const auto& red = composition[static_cast<std::size_t>(sim::Faction::Red)];
  ASSERT_EQ(red.size(), 3u);
  EXPECT_EQ(red[0].count, 51u);
  EXPECT_EQ(red[1].count, 11u);
  EXPECT_EQ(red[2].count, 6u);
}

TEST(ScenarioBudgetTest, EveryMixTypeFieldsAtLeastOneUnit) {
  const sim::UnitTypeTable table = sim::parse_unit_types(kCostTable);
  // budget 4.5, mix a:1 b:1 -> shares 2.25 / 2.25 -> floors 2 / 0 ->
  // b lifted to 1 (spent exactly 4.5), no leftover: {a 2, b 1}.
  const sim::Scenario scenario =
      budget_scenario(R"({"budget": 4.5, "mix": {"a": 1, "b": 1}})");
  const auto composition = sim::build_composition(scenario, table, 42);
  const auto& red = composition[static_cast<std::size_t>(sim::Faction::Red)];
  ASSERT_EQ(red.size(), 2u);
  EXPECT_EQ(red[0].count, 2u);
  EXPECT_EQ(red[1].count, 1u);
  // A budget below one unit of every mix type is a data error: here
  // one a (1.0) + one b (2.5) = 3.5 points minimum.
  const sim::Scenario impossible =
      budget_scenario(R"({"budget": 3, "mix": {"a": 1, "b": 1}})");
  try {
    static_cast<void>(sim::build_composition(impossible, table, 42));
    FAIL() << "expected rejection";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("cannot field at least one"),
              std::string::npos)
        << "error was: " << error.what();
  }
}

TEST(ScenarioBudgetTest, FillIsSeedIndependentAndCapped) {
  const sim::UnitTypeTable table = sim::parse_unit_types(kCostTable);
  const sim::Scenario scenario =
      budget_scenario(R"({"budget": 100, "mix": {"a": 2, "b": 1, "c": 1}})");
  const auto with_seed_1 = sim::build_composition(scenario, table, 1);
  const auto with_seed_999 = sim::build_composition(scenario, table, 999);
  ASSERT_EQ(with_seed_1.size(), with_seed_999.size());
  for (std::size_t faction = 0; faction < with_seed_1.size(); ++faction) {
    ASSERT_EQ(with_seed_1[faction].size(), with_seed_999[faction].size());
    for (std::size_t i = 0; i < with_seed_1[faction].size(); ++i) {
      EXPECT_EQ(with_seed_1[faction][i].type_index,
                with_seed_999[faction][i].type_index)
          << "faction " << faction << " entry " << i;
      EXPECT_EQ(with_seed_1[faction][i].count,
                with_seed_999[faction][i].count)
          << "faction " << faction << " entry " << i;
    }
  }
  const sim::Scenario huge =
      budget_scenario(R"({"budget": 120000, "mix": {"a": 1}})");
  try {
    static_cast<void>(sim::build_composition(huge, table, 42));
    FAIL() << "expected rejection";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("unit cap"), std::string::npos)
        << "error was: " << error.what();
  }
}

TEST(ScenarioBudgetTest, DefaultCostSplitsByWeight) {
  // No 'cost' anywhere: every type costs 1, so a budget splits exactly
  // by weights (budget 300, mix a:1 b:1 -> 150 / 150).
  const sim::UnitTypeTable table = sim::parse_unit_types(R"json([
    {"id": "a", "health": 10.0, "damage": 2.0, "speed": 1.0, "range": 3.0, "cooldown": 4, "sight_radius": 40.0},
    {"id": "b", "health": 10.0, "damage": 2.0, "speed": 1.0, "range": 3.0, "cooldown": 4, "sight_radius": 40.0}
  ])json");
  const sim::Scenario scenario =
      budget_scenario(R"({"budget": 300, "mix": {"a": 1, "b": 1}})");
  const auto composition = sim::build_composition(scenario, table, 42);
  const auto& red = composition[static_cast<std::size_t>(sim::Faction::Red)];
  ASSERT_EQ(red.size(), 2u);
  EXPECT_EQ(red[0].count, 150u);
  EXPECT_EQ(red[1].count, 150u);
}
