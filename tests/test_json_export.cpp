// Deterministic JSON serialization tests: byte-identical exports for
// identical runs, required fields, stable ordering, fixed float formatting,
// no NaN/infinity/negative zero.
#include "sim/json_export.hpp"
#include "sim/simulation.hpp"

#include <cstdint>
#include <regex>
#include <string>

#include <nlohmann/json.hpp>
#include <gtest/gtest.h>

namespace {

sim::SimulationConfig battle_config(std::uint64_t seed) {
  sim::SimulationConfig config;
  config.ai_mode = sim::AiMode::Legacy;  // M4: old tests pin legacy
  config.seed = seed;
  config.total_units = 80;
  config.max_ticks = 400;
  return config;
}

std::string run_and_export(std::uint64_t seed) {
  sim::Simulation simulation(battle_config(seed));
  static_cast<void>(simulation.run());
  return sim::export_state_json(simulation);
}

TEST(JsonExportTest, IdenticalRunsExportByteIdenticalJson) {
  const std::string a = run_and_export(42);
  const std::string b = run_and_export(42);
  EXPECT_EQ(a, b);
}

TEST(JsonExportTest, DifferentSeedsExportDifferentJson) {
  const std::string a = run_and_export(42);
  const std::string b = run_and_export(43);
  EXPECT_NE(a, b);
}

TEST(JsonExportTest, ExportContainsAllRequiredFields) {
  const std::string text = run_and_export(42);
  const nlohmann::json root = nlohmann::json::parse(text);

  ASSERT_TRUE(root.is_object());
  EXPECT_EQ(root["format_version"].get<int>(), 3);  // M4: v3 adds AI fields
  EXPECT_TRUE(root.contains("seed"));
  EXPECT_TRUE(root.contains("ticks_elapsed"));
  EXPECT_TRUE(root.contains("config"));
  EXPECT_TRUE(root.contains("factions"));
  EXPECT_TRUE(root.contains("units"));
  EXPECT_TRUE(root.contains("statistics"));
  EXPECT_TRUE(root.contains("termination"));

  EXPECT_EQ(root["seed"].get<std::uint64_t>(), 42u);
  EXPECT_EQ(root["factions"].size(), 2u);

  const nlohmann::json& termination = root["termination"];
  EXPECT_TRUE(termination.contains("reason"));
  EXPECT_TRUE(termination.contains("winner"));

  const nlohmann::json& config = root["config"];
  EXPECT_TRUE(config.contains("max_ticks"));
  EXPECT_TRUE(config.contains("total_units"));
  EXPECT_TRUE(config.contains("unit_stats"));

  ASSERT_TRUE(root["units"].is_array());
  ASSERT_EQ(root["units"].size(), 80u);
  const nlohmann::json& unit = root["units"][0];
  EXPECT_TRUE(unit.contains("id"));
  EXPECT_TRUE(unit.contains("faction"));
  EXPECT_TRUE(unit.contains("x"));
  EXPECT_TRUE(unit.contains("y"));
  EXPECT_TRUE(unit.contains("health"));
  EXPECT_TRUE(unit.contains("max_health"));
  EXPECT_TRUE(unit.contains("alive"));
  // M4 fields: every unit carries its personality and last action. Legacy
  // battles never chose a utility action, so last_action is "none".
  EXPECT_TRUE(unit.contains("personality"));
  ASSERT_TRUE(unit["personality"].is_object());
  EXPECT_TRUE(unit["personality"].contains("archetype"));
  EXPECT_TRUE(unit["personality"].contains("bravery"));
  EXPECT_TRUE(unit["personality"].contains("caution"));
  EXPECT_TRUE(unit["personality"].contains("aggression"));
  EXPECT_EQ(unit["last_action"].get<std::string>(), "none");

  const nlohmann::json& statistics = root["statistics"];
  EXPECT_TRUE(statistics.contains("Red"));
  EXPECT_TRUE(statistics.contains("Blue"));
  EXPECT_TRUE(statistics.contains("total_deaths"));
  // M4: per-faction cumulative action counts exist and are zero in legacy
  // mode (no utility actions were ever chosen).
  for (const char* faction : {"Red", "Blue"}) {
    ASSERT_TRUE(statistics[faction].contains("action_counts"));
    const nlohmann::json& counts = statistics[faction]["action_counts"];
    EXPECT_EQ(counts["attack"].get<std::uint64_t>(), 0u);
    EXPECT_EQ(counts["approach"].get<std::uint64_t>(), 0u);
    EXPECT_EQ(counts["advance"].get<std::uint64_t>(), 0u);
    EXPECT_EQ(counts["retreat"].get<std::uint64_t>(), 0u);
    EXPECT_EQ(counts["hold"].get<std::uint64_t>(), 0u);
  }
}

TEST(JsonExportTest, UnitsAreExportedInAscendingIdOrder) {
  const std::string text = run_and_export(42);
  const nlohmann::json root = nlohmann::json::parse(text);

  std::uint64_t previous_id = 0;
  for (const nlohmann::json& unit : root["units"]) {
    const std::uint64_t id = unit["id"].get<std::uint64_t>();
    EXPECT_GT(id, previous_id);
    previous_id = id;
  }
  EXPECT_EQ(previous_id, 80u);
}

TEST(JsonExportTest, UnitStateIsConsistent) {
  const std::string text = run_and_export(42);
  const nlohmann::json root = nlohmann::json::parse(text);

  for (const nlohmann::json& unit : root["units"]) {
    const double health = unit["health"].get<double>();
    const double max_health = unit["max_health"].get<double>();
    EXPECT_GE(health, 0.0);
    EXPECT_LE(health, max_health);
    if (unit["alive"].get<bool>()) {
      EXPECT_GT(health, 0.0);
    } else {
      EXPECT_DOUBLE_EQ(health, 0.0);
    }
    const std::string faction = unit["faction"].get<std::string>();
    EXPECT_TRUE(faction == "Red" || faction == "Blue");
  }
}

TEST(JsonExportTest, StatisticsMatchUnitStates) {
  const std::string text = run_and_export(42);
  const nlohmann::json root = nlohmann::json::parse(text);

  std::uint64_t red_alive = 0;
  std::uint64_t blue_alive = 0;
  std::uint64_t red_total = 0;
  std::uint64_t blue_total = 0;
  for (const nlohmann::json& unit : root["units"]) {
    const bool alive = unit["alive"].get<bool>();
    if (unit["faction"].get<std::string>() == "Red") {
      ++red_total;
      red_alive += alive ? 1 : 0;
    } else {
      ++blue_total;
      blue_alive += alive ? 1 : 0;
    }
  }

  EXPECT_EQ(root["statistics"]["Red"]["survivors"].get<std::uint64_t>(), red_alive);
  EXPECT_EQ(root["statistics"]["Blue"]["survivors"].get<std::uint64_t>(), blue_alive);
  EXPECT_EQ(root["statistics"]["Red"]["initial"].get<std::uint64_t>(), red_total);
  EXPECT_EQ(root["statistics"]["Blue"]["initial"].get<std::uint64_t>(), blue_total);
  EXPECT_EQ(root["statistics"]["Red"]["deaths"].get<std::uint64_t>(),
            red_total - red_alive);
  EXPECT_EQ(root["statistics"]["Blue"]["deaths"].get<std::uint64_t>(),
            blue_total - blue_alive);
}

TEST(JsonExportTest, NoNanInfinityOrNegativeZero) {
  const std::string text = run_and_export(42);
  EXPECT_EQ(text.find("nan"), std::string::npos);
  EXPECT_EQ(text.find("NaN"), std::string::npos);
  EXPECT_EQ(text.find("inf"), std::string::npos);
  EXPECT_EQ(text.find("-0.0"), std::string::npos);
}

TEST(JsonExportTest, FloatsUsePlainDotDecimalFormat) {
  // Locale independence: every exported number must be a plain
  // dot-decimal token (no comma separators, no scientific notation for
  // battlefield-scale values). Checked on the raw serialized tokens.
  const std::string text = run_and_export(42);
  const nlohmann::json root = nlohmann::json::parse(text);
  const std::regex plain_number(R"(^-?\d+(\.\d+)?$)");
  for (const nlohmann::json& unit : root["units"]) {
    for (const char* key : {"x", "y", "health", "max_health"}) {
      const std::string token = unit[key].dump();
      EXPECT_TRUE(std::regex_match(token, plain_number))
          << key << " serialized as '" << token << "'";
    }
  }
}

TEST(JsonExportTest, ExportIsStableAcrossRepeatedCalls) {
  sim::Simulation simulation(battle_config(42));
  static_cast<void>(simulation.run());
  const std::string a = sim::export_state_json(simulation);
  const std::string b = sim::export_state_json(simulation);
  EXPECT_EQ(a, b);
}

TEST(JsonExportTest, MidBattleExportDiffersFromFinalExport) {
  sim::Simulation simulation(battle_config(42));
  const std::string before = sim::export_state_json(simulation);
  static_cast<void>(simulation.run());
  const std::string after = sim::export_state_json(simulation);
  EXPECT_NE(before, after);
}

}  // namespace
