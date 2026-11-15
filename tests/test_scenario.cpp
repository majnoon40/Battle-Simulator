// Scenario loading tests: valid scenarios, every validation failure from the
// M2 spec (malformed JSON, missing fields, invalid world/band/counts, empty
// armies, unit-cap overflow, unknown fields), composition building, and the
// committed scenario files.
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
  // M3 raised the cap to 20,000; these stay above it.
  expect_reject(R"({"battlefield": {"width": 100.0, "height": 100.0}, "spawn_band_fraction": 0.15, "armies": {"red": {"infantry": 11000}, "blue": {"infantry": 11000}}})",
                "exceeds the 20000-unit cap");
  expect_reject(R"({"battlefield": {"width": 100.0, "height": 100.0}, "spawn_band_fraction": 0.15, "armies": {"red": {"infantry": 20001}, "blue": {"infantry": 1}}})",
                "exceeds the unit cap");
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
}

}  // namespace
