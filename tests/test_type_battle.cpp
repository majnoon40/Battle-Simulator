// M2 battle behavior tests: archer/cavalry properties, per-type statistics,
// the M1 regression (seeds 42 and 123, legacy and composition paths),
// mixed-type determinism, composition validation, and the temp-file
// integration test (editing a JSON file changes the result without
// recompiling).
#include "sim/json_export.hpp"
#include "sim/scenario.hpp"
#include "sim/simulation.hpp"
#include "sim/unit_types.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

constexpr std::size_t kRed = static_cast<std::size_t>(sim::Faction::Red);
constexpr std::size_t kBlue = static_cast<std::size_t>(sim::Faction::Blue);

// The three shipped types, mirroring data/units.json.
constexpr const char* kTableText = R"json([
  {"id": "infantry", "health": 100.0, "damage": 12.0, "speed": 2.0, "range": 2.0, "cooldown": 5, "sight_radius": 40.0},
  {"id": "archer", "health": 70.0, "damage": 9.0, "speed": 1.6, "range": 8.0, "cooldown": 8, "sight_radius": 60.0},
  {"id": "cavalry", "health": 160.0, "damage": 20.0, "speed": 4.5, "range": 2.0, "cooldown": 7, "sight_radius": 50.0}
])json";

sim::Unit make_unit(std::uint32_t id, sim::Faction faction,
                    std::uint32_t type_index, double x, double y,
                    const sim::UnitStats& stats) {
  sim::Unit unit;
  unit.id = id;
  unit.faction = faction;
  unit.type_index = type_index;
  unit.x = x;
  unit.y = y;
  unit.health = stats.max_health;
  unit.stats = stats;
  unit.alive = true;
  return unit;
}

// Composition-mode config on the M1 battlefield with per-type armies. Types
// with count 0 are left out entirely (they would be rejected by validation).
sim::SimulationConfig composed_config(const sim::UnitTypeTable& types,
                                      std::uint64_t seed,
                                      std::uint32_t infantry,
                                      std::uint32_t archers,
                                      std::uint32_t cavalry) {
  sim::SimulationConfig config;
  config.ai_mode = sim::AiMode::Legacy;  // M4: old tests pin legacy
  config.seed = seed;
  config.max_ticks = 3000;
  config.unit_types = &types;
  const std::pair<const char*, std::uint32_t> wanted[] = {
      {"infantry", infantry}, {"archer", archers}, {"cavalry", cavalry}};
  for (const auto& [type_id, count] : wanted) {
    if (count == 0) {
      continue;
    }
    const int index = types.index_of(type_id);
    EXPECT_GE(index, 0) << "table is missing type " << type_id;
    config.composition[kRed].push_back(
        {static_cast<std::uint32_t>(index), count});
    config.composition[kBlue].push_back(
        {static_cast<std::uint32_t>(index), count});
  }
  std::uint32_t total = 0;
  for (const auto& army : config.composition) {
    for (const sim::TypeCount& entry : army) {
      total += entry.count;
    }
  }
  config.total_units = total;
  return config;
}

const sim::TypeReport* find_type(const sim::FactionReport& report,
                                 const std::string& type_id) {
  for (const sim::TypeReport& type : report.by_type) {
    if (type.type_id == type_id) {
      return &type;
    }
  }
  return nullptr;
}

TEST(TypeBehaviorTest, ArcherOutrangesInfantry) {
  const sim::UnitTypeTable types = sim::parse_unit_types(kTableText);
  const int archer = types.index_of("archer");
  const int infantry = types.index_of("infantry");
  ASSERT_GE(archer, 0);
  ASSERT_GE(infantry, 0);
  ASSERT_GT(types.types[archer].stats.attack_range,
            types.types[infantry].stats.attack_range);

  sim::SimulationConfig config;
  config.ai_mode = sim::AiMode::Legacy;  // M4: old tests pin legacy
  config.seed = 1;
  config.max_ticks = 200;
  config.battlefield_width = 100.0;
  config.battlefield_height = 100.0;
  config.unit_types = &types;

  // Archer and immobile infantry start 5.0 apart — inside archer range (8),
  // outside infantry range (2). Both hold position (speed 0), so only the
  // archer can ever land a hit.
  sim::UnitStats archer_stats = types.types[archer].stats;
  archer_stats.speed = 0.0;
  sim::UnitStats infantry_stats = types.types[infantry].stats;
  infantry_stats.speed = 0.0;
  sim::Simulation simulation(
      config, {make_unit(1, sim::Faction::Red, archer, 0.0, 0.0, archer_stats),
               make_unit(2, sim::Faction::Blue, infantry, 5.0, 0.0, infantry_stats)});

  const sim::BattleResult result = simulation.run();

  EXPECT_TRUE(result.finished);
  EXPECT_TRUE(result.has_winner);
  EXPECT_EQ(result.winner, sim::Faction::Red);
  EXPECT_EQ(result.reason, sim::TerminationReason::FactionEliminated);
  EXPECT_EQ(result.factions[kRed].deaths, 0u);
  EXPECT_EQ(result.factions[kBlue].deaths, 1u);
  EXPECT_DOUBLE_EQ(simulation.world().find_by_id(1)->health, 70.0);

  const sim::TypeReport* red_archer =
      find_type(result.factions[kRed], "archer");
  ASSERT_NE(red_archer, nullptr);
  EXPECT_EQ(red_archer->spawned, 1u);
  EXPECT_EQ(red_archer->survivors, 1u);
  EXPECT_EQ(red_archer->kills, 1u);
  // 100 health, 9 damage per shot: 12 shots, the last one overkills.
  EXPECT_DOUBLE_EQ(red_archer->damage_dealt, 108.0);
  const sim::TypeReport* blue_infantry =
      find_type(result.factions[kBlue], "infantry");
  ASSERT_NE(blue_infantry, nullptr);
  EXPECT_EQ(blue_infantry->spawned, 1u);
  EXPECT_EQ(blue_infantry->survivors, 0u);
  EXPECT_EQ(blue_infantry->kills, 0u);
  EXPECT_DOUBLE_EQ(blue_infantry->damage_dealt, 0.0);
}

TEST(TypeBehaviorTest, CavalryReachesTheEnemyFirst) {
  // One immobile, harmless Blue victim 40 world units away. Red fields either
  // a cavalry (speed 4.5) or an infantry (speed 2.0) unit from the same spot;
  // both have range 2.0. First damage tick: 9 for cavalry, 19 for infantry —
  // cavalry engages first, as the M2 spec requires.
  auto first_damage_tick = [&kTableText](bool cavalry) -> int {
    const sim::UnitTypeTable types = sim::parse_unit_types(kTableText);
    const std::uint32_t type = static_cast<std::uint32_t>(
        cavalry ? types.index_of("cavalry") : types.index_of("infantry"));
    sim::SimulationConfig config;
  config.ai_mode = sim::AiMode::Legacy;  // M4: old tests pin legacy
    config.seed = 1;
    config.max_ticks = 100;
    config.battlefield_width = 200.0;
    config.battlefield_height = 200.0;
    config.unit_types = &types;

    sim::UnitStats victim_stats;
    victim_stats.max_health = 1000.0;
    victim_stats.speed = 0.0;
    victim_stats.damage = 0.0;
    victim_stats.attack_range = 2.0;
    victim_stats.attack_cooldown_ticks = 5;

    sim::Simulation simulation(
        config,
        {make_unit(1, sim::Faction::Blue,
                   static_cast<std::uint32_t>(types.index_of("infantry")),
                   100.0, 50.0, victim_stats),
         make_unit(2, sim::Faction::Red, type, 60.0, 50.0,
                   types.types[type].stats)});
    for (int tick = 1; tick <= 100; ++tick) {
      simulation.tick();
      if (simulation.world().find_by_id(1)->health < 1000.0) {
        return tick;
      }
    }
    return -1;
  };

  EXPECT_EQ(first_damage_tick(true), 9);
  EXPECT_EQ(first_damage_tick(false), 19);
}

TEST(CompositionTest, MixedArmyRunsToCompletionWithPerTypeStats) {
  const sim::UnitTypeTable types = sim::parse_unit_types(kTableText);
  sim::Simulation simulation(composed_config(types, 42, 30, 20, 10));
  const sim::BattleResult result = simulation.run();

  EXPECT_TRUE(result.finished);
  EXPECT_GT(result.ticks_elapsed, 0u);
  EXPECT_LE(result.ticks_elapsed, 3000u);
  EXPECT_GT(result.factions[kRed].deaths + result.factions[kBlue].deaths, 0u);

  for (std::size_t faction : {kRed, kBlue}) {
    const sim::FactionReport& report = result.factions[faction];
    ASSERT_EQ(report.by_type.size(), 3u);
    std::uint32_t spawned = 0;
    std::uint32_t survivors = 0;
    double damage = 0.0;
    for (const sim::TypeReport& type : report.by_type) {
      spawned += type.spawned;
      survivors += type.survivors;
      damage += type.damage_dealt;
      EXPECT_LE(type.survivors, type.spawned);
    }
    EXPECT_EQ(spawned, report.initial);
    EXPECT_EQ(survivors, report.survivors);
    EXPECT_GT(damage, 0.0);
    // Kills of this faction's types equal the enemy's deaths.
    std::uint32_t kills = 0;
    for (const sim::TypeReport& type : report.by_type) {
      kills += type.kills;
    }
    EXPECT_EQ(kills, result.factions[1 - faction].deaths);
  }
}

TEST(CompositionTest, KillsAndDamageAreTrackedPerType) {
  // Two cannons (damage 10) focus one 10-health victim in the same tick:
  // damage counts both attacks (M1 overkill semantics), the kill is credited
  // exactly once.
  const sim::UnitTypeTable types = sim::parse_unit_types(R"json([
    {"id": "glass", "health": 10.0, "damage": 1.0, "speed": 0.1, "range": 1.0, "cooldown": 1, "sight_radius": 40.0},
    {"id": "cannon", "health": 100.0, "damage": 10.0, "speed": 0.1, "range": 5.0, "cooldown": 1, "sight_radius": 40.0}
  ])json");
  const std::uint32_t glass =
      static_cast<std::uint32_t>(types.index_of("glass"));
  const std::uint32_t cannon =
      static_cast<std::uint32_t>(types.index_of("cannon"));

  sim::SimulationConfig config;
  config.ai_mode = sim::AiMode::Legacy;  // M4: old tests pin legacy
  config.seed = 1;
  config.max_ticks = 10;
  config.battlefield_width = 100.0;
  config.battlefield_height = 100.0;
  config.unit_types = &types;
  sim::Simulation simulation(
      config, {make_unit(1, sim::Faction::Red, cannon, 0.0, 0.0, types.types[cannon].stats),
               make_unit(2, sim::Faction::Red, cannon, 0.0, 2.0, types.types[cannon].stats),
               make_unit(3, sim::Faction::Blue, glass, 4.0, 0.0, types.types[glass].stats)});

  const sim::BattleResult result = simulation.run();
  ASSERT_TRUE(result.finished);
  EXPECT_TRUE(result.has_winner);
  EXPECT_EQ(result.winner, sim::Faction::Red);

  const sim::TypeReport* red_cannon = find_type(result.factions[kRed], "cannon");
  ASSERT_NE(red_cannon, nullptr);
  EXPECT_EQ(red_cannon->spawned, 2u);
  EXPECT_EQ(red_cannon->survivors, 2u);
  EXPECT_EQ(red_cannon->kills, 1u);       // credited once despite overkill
  EXPECT_DOUBLE_EQ(red_cannon->damage_dealt, 20.0);  // both attacks count

  const sim::TypeReport* blue_glass = find_type(result.factions[kBlue], "glass");
  ASSERT_NE(blue_glass, nullptr);
  EXPECT_EQ(blue_glass->spawned, 1u);
  EXPECT_EQ(blue_glass->survivors, 0u);
  EXPECT_EQ(blue_glass->kills, 0u);
}

TEST(RegressionTest, Seed42LegacyPathReproducesM1) {
  sim::SimulationConfig config;
  config.ai_mode = sim::AiMode::Legacy;  // M4: old tests pin legacy  // M1 defaults: seed 42, 500 units, M1 stats
  sim::Simulation simulation(config);
  const sim::BattleResult result = simulation.run();

  ASSERT_TRUE(result.finished);
  EXPECT_TRUE(result.has_winner);
  EXPECT_EQ(result.winner, sim::Faction::Blue);
  EXPECT_EQ(result.ticks_elapsed, 456u);
  EXPECT_EQ(result.factions[kRed].survivors, 0u);
  EXPECT_EQ(result.factions[kRed].deaths, 250u);
  EXPECT_EQ(result.factions[kBlue].survivors, 64u);
  EXPECT_EQ(result.factions[kBlue].deaths, 186u);
}

TEST(RegressionTest, Seed42CompositionPathMatchesLegacyBitForBit) {
  // The data-driven default (infantry = M1 stats, 250/250, M1 battlefield)
  // must reproduce the legacy M1 run exactly: same positions, same outcome,
  // byte-identical export.
  const sim::UnitTypeTable types = sim::parse_unit_types(kTableText);

  sim::SimulationConfig legacy;  // seed 42, 500 units, M1 defaults
  legacy.ai_mode = sim::AiMode::Legacy;
  sim::Simulation legacy_run(legacy);
  const sim::BattleResult legacy_result = legacy_run.run();

  sim::Simulation composed_run(composed_config(types, 42, 250, 0, 0));
  const sim::BattleResult composed_result = composed_run.run();

  ASSERT_EQ(legacy_run.world().units.size(),
            composed_run.world().units.size());
  for (std::size_t i = 0; i < legacy_run.world().units.size(); ++i) {
    const sim::Unit& a = legacy_run.world().units[i];
    const sim::Unit& b = composed_run.world().units[i];
    ASSERT_EQ(a.id, b.id);
    EXPECT_EQ(a.faction, b.faction);
    EXPECT_DOUBLE_EQ(a.x, b.x);
    EXPECT_DOUBLE_EQ(a.y, b.y);
    EXPECT_DOUBLE_EQ(a.health, b.health);
    EXPECT_EQ(a.alive, b.alive);
    ASSERT_EQ(b.type_index,
              static_cast<std::uint32_t>(types.index_of("infantry")));
  }
  EXPECT_EQ(composed_result.ticks_elapsed, legacy_result.ticks_elapsed);
  EXPECT_EQ(composed_result.has_winner, legacy_result.has_winner);
  EXPECT_EQ(composed_result.winner, legacy_result.winner);
  EXPECT_EQ(composed_result.factions[kBlue].survivors,
            legacy_result.factions[kBlue].survivors);
  EXPECT_EQ(composed_result.factions[kBlue].deaths,
            legacy_result.factions[kBlue].deaths);
}

TEST(RegressionTest, Seed123ReproducesM1) {
  sim::SimulationConfig config;
  config.ai_mode = sim::AiMode::Legacy;  // M4: old tests pin legacy
  config.seed = 123;
  sim::Simulation simulation(config);
  const sim::BattleResult result = simulation.run();

  ASSERT_TRUE(result.finished);
  EXPECT_TRUE(result.has_winner);
  EXPECT_EQ(result.winner, sim::Faction::Blue);
  EXPECT_EQ(result.ticks_elapsed, 713u);
  EXPECT_EQ(result.factions[kRed].survivors, 0u);
  EXPECT_EQ(result.factions[kRed].deaths, 250u);
  EXPECT_EQ(result.factions[kBlue].survivors, 6u);
  EXPECT_EQ(result.factions[kBlue].deaths, 244u);
}

TEST(DeterminismTest, MixedCompositionDoubleRunIsByteIdentical) {
  const sim::UnitTypeTable types = sim::parse_unit_types(kTableText);
  const sim::SimulationConfig config = composed_config(types, 42, 30, 20, 10);

  sim::Simulation a(config);
  const sim::BattleResult result_a = a.run();
  sim::Simulation b(config);
  const sim::BattleResult result_b = b.run();

  ASSERT_EQ(a.world().units.size(), b.world().units.size());
  for (std::size_t i = 0; i < a.world().units.size(); ++i) {
    EXPECT_EQ(a.world().units[i].id, b.world().units[i].id);
    EXPECT_EQ(a.world().units[i].type_index, b.world().units[i].type_index);
    EXPECT_DOUBLE_EQ(a.world().units[i].x, b.world().units[i].x);
    EXPECT_DOUBLE_EQ(a.world().units[i].y, b.world().units[i].y);
    EXPECT_DOUBLE_EQ(a.world().units[i].health, b.world().units[i].health);
    EXPECT_EQ(a.world().units[i].alive, b.world().units[i].alive);
  }
  EXPECT_EQ(result_a.ticks_elapsed, result_b.ticks_elapsed);
  EXPECT_EQ(result_a.winner, result_b.winner);
  EXPECT_EQ(sim::export_state_json(a), sim::export_state_json(b));
}

TEST(CompositionValidationTest, InvalidCompositionsAreRejected) {
  const sim::UnitTypeTable types = sim::parse_unit_types(kTableText);
  const auto valid = composed_config(types, 1, 10, 10, 10);
  EXPECT_NO_THROW(sim::Simulation{valid});

  // No unit type table.
  sim::SimulationConfig missing_table = composed_config(types, 1, 10, 10, 10);
  missing_table.unit_types = nullptr;
  EXPECT_THROW(sim::Simulation{missing_table}, std::invalid_argument);

  // Only one army has a composition.
  sim::SimulationConfig one_army = composed_config(types, 1, 10, 10, 10);
  one_army.composition[kBlue].clear();
  EXPECT_THROW(sim::Simulation{one_army}, std::invalid_argument);

  // Zero count entry.
  sim::SimulationConfig zero = composed_config(types, 1, 10, 10, 10);
  zero.composition[kRed][1].count = 0;
  EXPECT_THROW(sim::Simulation{zero}, std::invalid_argument);

  // Type index outside the table.
  sim::SimulationConfig bad_index = composed_config(types, 1, 10, 10, 10);
  bad_index.composition[kRed][0].type_index = 99;
  EXPECT_THROW(sim::Simulation{bad_index}, std::invalid_argument);

  // Over the unit cap (M3: 20,000) — 11,000 + 11,000.
  sim::SimulationConfig over_cap;
  over_cap.unit_types = &types;
  over_cap.composition[kRed] = {
      {static_cast<std::uint32_t>(types.index_of("infantry")), 11000}};
  over_cap.composition[kBlue] = {
      {static_cast<std::uint32_t>(types.index_of("infantry")), 11000}};
  EXPECT_THROW(sim::Simulation{over_cap}, std::invalid_argument);

  // Legacy path: total above the cap.
  sim::SimulationConfig legacy_over = valid;
  legacy_over.composition[kRed].clear();
  legacy_over.composition[kBlue].clear();
  legacy_over.total_units = 20001;
  EXPECT_THROW(sim::Simulation{legacy_over}, std::invalid_argument);
}

TEST(DataIntegrationTest, EditingUnitTypesOnDiskChangesTheResult) {
  // Same mixed battle, two unit type tables that differ only in archer
  // damage (9.0 vs 3.0). The second table goes through a temp file and the
  // stream overload, proving that editing the JSON data changes the
  // simulation without recompiling.
  const std::string stock = kTableText;
  const std::string variant = R"json([
  {"id": "infantry", "health": 100.0, "damage": 12.0, "speed": 2.0, "range": 2.0, "cooldown": 5, "sight_radius": 40.0},
  {"id": "archer", "health": 70.0, "damage": 3.0, "speed": 1.6, "range": 8.0, "cooldown": 8, "sight_radius": 60.0},
  {"id": "cavalry", "health": 160.0, "damage": 20.0, "speed": 4.5, "range": 2.0, "cooldown": 7, "sight_radius": 50.0}
])json";

  const std::filesystem::path temp =
      std::filesystem::temp_directory_path() /
      "autobattle_m2_units_variant.json";
  {
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(file.good());
    file << variant;
    ASSERT_TRUE(file.good());
  }
  std::ifstream read_back(temp, std::ios::binary);
  ASSERT_TRUE(read_back.good());
  const sim::UnitTypeTable variant_table = sim::parse_unit_types(read_back);

  const sim::UnitTypeTable stock_table = sim::parse_unit_types(stock);
  sim::Simulation stock_run(composed_config(stock_table, 42, 30, 20, 10));
  const sim::BattleResult stock_result = stock_run.run();
  sim::Simulation variant_run(composed_config(variant_table, 42, 30, 20, 10));
  const sim::BattleResult variant_result = variant_run.run();

  // The battle outcome is measurably different.
  EXPECT_NE(sim::export_state_json(stock_run),
            sim::export_state_json(variant_run));

  // Specifically, the archers' contribution changes.
  const sim::TypeReport* stock_archer =
      find_type(stock_result.factions[kRed], "archer");
  const sim::TypeReport* variant_archer =
      find_type(variant_result.factions[kRed], "archer");
  ASSERT_NE(stock_archer, nullptr);
  ASSERT_NE(variant_archer, nullptr);
  EXPECT_NE(stock_archer->damage_dealt, variant_archer->damage_dealt);

  std::filesystem::remove(temp);
}

}  // namespace
