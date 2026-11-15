// M10 Part 2a tests: event recording, timeline sampling, report
// generation, CSV and report PNG. The core invariants:
//  - a recorder (or timeline sampling) never changes a battle;
//  - every recorded event type maps to a measurable outcome;
//  - every number the report prints comes from recorded state.
#include <cstdint>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cli/report.hpp"
#include <nlohmann/json.hpp>
#include "sim/events.hpp"
#include "sim/json_export.hpp"
#include "sim/simulation.hpp"
#include "sim/timeline.hpp"
#include "sim/unit_types.hpp"

namespace {

#include "sim/unit_types.hpp"
#include <sstream>

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

// A small deterministic melee: two ranks of 6 units each, armies 20
// apart. Variance + morale + tactics on (the M10 default feature set).
// `one_hit` raises the damage so kills happen before morale breaks
// (the default 12-damage setup usually ends in a bloodless rout).
sim::Simulation make_battle(const sim::UnitTypeTable& types,
                            sim::EventRecorder* recorder,
                            std::uint32_t timeline_interval,
                            bool one_hit = false,
                            bool morale = true) {
  std::vector<sim::Unit> units;
  for (int i = 0; i < 6; ++i) {
    sim::Unit red_unit = make_unit(static_cast<std::uint32_t>(i + 1),
                                   sim::Faction::Red, 10.0 + i * 2.0, 500.0);
    sim::Unit blue_unit = make_unit(static_cast<std::uint32_t>(i + 101),
                                    sim::Faction::Blue, 30.0 + i * 2.0,
                                    500.0);
    if (one_hit) {
      red_unit.stats.damage = 100.0;
      blue_unit.stats.damage = 100.0;
    }
    units.push_back(red_unit);
    units.push_back(blue_unit);
  }
  sim::SimulationConfig config;
  config.seed = 7;
  config.max_ticks = 400;
  config.battlefield_width = 1000.0;
  config.battlefield_height = 1000.0;
  config.spawn_band_fraction = 0.15;
  config.unit_types = &types;
  config.ai_mode = sim::AiMode::Utility;
  config.morale_enabled = morale;
  config.tactics_enabled = true;
  config.events = recorder;
  config.timeline_interval = timeline_interval;
  sim::Simulation simulation(config, units);
  return simulation;
}

// ---------------------------------------------------------------------------
// Recording changes nothing.
// ---------------------------------------------------------------------------

TEST(EventsTest, RecorderAndTimelineDoNotChangeTheBattle) {
  const sim::UnitTypeTable types = infantry_types();
  std::string without;
  {
    sim::Simulation simulation = make_battle(types, nullptr, 0);
    static_cast<void>(simulation.run());
    without = sim::export_state_json(simulation);
  }
  std::string with;
  {
    sim::EventRecorder recorder;
    sim::Simulation simulation = make_battle(types, &recorder, 5);
    static_cast<void>(simulation.run());
    with = sim::export_state_json(simulation);
    EXPECT_GT(recorder.events().size(), std::size_t{0});
  }
  EXPECT_EQ(without, with);
}

// ---------------------------------------------------------------------------
// Events map to measurable outcomes.
// ---------------------------------------------------------------------------

TEST(EventsTest, DeathsRecordKillerAndMatchCasualties) {
  const sim::UnitTypeTable types = infantry_types();
  sim::EventRecorder recorder;
  sim::Simulation simulation =
      make_battle(types, &recorder, 0, true, false);
  const sim::BattleResult result = simulation.run();

  const std::size_t deaths = recorder.count(sim::EventType::Death);
  ASSERT_GT(deaths, std::size_t{0});
  ASSERT_EQ(deaths, result.factions[0].deaths + result.factions[1].deaths);

  // Every death names a victim of the right faction, and a killer that
  // existed on the other side (or 0 for unknown killers).
  std::map<std::uint32_t, sim::Faction> factions;
  for (const sim::Unit& unit : simulation.world().units) {
    factions[unit.id] = unit.faction;
  }
  std::size_t with_killer = 0;
  for (const sim::Event& event : recorder.events()) {
    if (event.type != sim::EventType::Death) {
      continue;
    }
    ASSERT_NE(factions.count(event.a), std::size_t{0});
    if (event.b != 0) {
      ASSERT_NE(factions.count(event.b), std::size_t{0});
      ASSERT_NE(factions.at(event.a), factions.at(event.b));
      ++with_killer;
    }
    ASSERT_GT(event.tick, std::uint64_t{0});
    ASSERT_LE(event.tick, result.ticks_elapsed);
  }
  EXPECT_EQ(with_killer, deaths);  // combat deaths always carry a killer
}

TEST(EventsTest, RoutsAndRecoveriesMatchRoutingStats) {
  const sim::UnitTypeTable types = infantry_types();
  sim::EventRecorder recorder;
  sim::Simulation simulation = make_battle(types, &recorder, 0);
  const sim::BattleResult result = simulation.run();
  EXPECT_EQ(recorder.count(sim::EventType::Rout),
            result.factions[0].routing.total_routs +
                result.factions[1].routing.total_routs);
  EXPECT_EQ(recorder.count(sim::EventType::Recovery),
            result.factions[0].routing.recoveries +
                result.factions[1].routing.recoveries);
}

// ---------------------------------------------------------------------------
// Timeline.
// ---------------------------------------------------------------------------

TEST(TimelineTest, SamplesOnCadenceAndMatchTheFinalState) {
  const sim::UnitTypeTable types = infantry_types();
  sim::Simulation simulation = make_battle(types, nullptr, 10);
  const sim::BattleResult result = simulation.run();
  const std::vector<sim::TimelineRow>& rows = simulation.timeline();
  ASSERT_GE(rows.size(), std::size_t{2});
  // Every-10 cadence plus the final tick.
  for (std::size_t i = 0; i + 1 < rows.size(); ++i) {
    EXPECT_EQ(rows[i + 1].tick - rows[i].tick, std::uint64_t{10});
  }
  EXPECT_EQ(rows.back().tick, result.ticks_elapsed);
  // The final row matches the result: survivors, dead, actives.
  const sim::TimelineRow& last = rows.back();
  for (std::size_t f = 0; f < 2; ++f) {
    EXPECT_EQ(last.factions[f].alive, result.factions[f].survivors);
    EXPECT_EQ(last.factions[f].dead, result.factions[f].deaths);
    EXPECT_EQ(last.factions[f].routed,
              result.factions[f].survivors - result.factions[f].active);
  }
  // Deaths are monotone and alive + dead == initial.
  for (std::size_t i = 1; i < rows.size(); ++i) {
    for (std::size_t f = 0; f < 2; ++f) {
      EXPECT_GE(rows[i].factions[f].dead, rows[i - 1].factions[f].dead);
      EXPECT_EQ(rows[i].factions[f].alive + rows[i].factions[f].dead,
                result.factions[f].initial);
    }
  }
}

// ---------------------------------------------------------------------------
// Report generation: every printed number maps to a measured field.
// ---------------------------------------------------------------------------

TEST(ReportTest, MarkdownAndJsonMapToMeasuredFields) {
  const sim::UnitTypeTable types = infantry_types();
  sim::EventRecorder recorder;
  sim::Simulation simulation = make_battle(types, &recorder, 10);
  const sim::BattleResult result = simulation.run();
  const cli::ReportMeta meta{"melee", 7};
  const std::string markdown =
      cli::generate_report_markdown(simulation, &recorder, types, meta);
  const std::string json =
      cli::generate_report_json(simulation, &recorder, types, meta);

  // Outcome sentences carry the recorded values.
  EXPECT_NE(markdown.find("Ticks elapsed: " +
                          std::to_string(result.ticks_elapsed) + "."),
            std::string::npos);
  for (std::size_t f = 0; f < 2; ++f) {
    const std::string faction = f == 0 ? "Red" : "Blue";
    const std::string casualties =
        faction + ": " + std::to_string(result.factions[f].initial) +
        " units, " + std::to_string(result.factions[f].deaths) +
        " deaths, " + std::to_string(result.factions[f].survivors) +
        " survivors";
    EXPECT_NE(markdown.find(casualties), std::string::npos);
    EXPECT_NE(markdown.find(faction + ": " +
                            std::to_string(result.factions[f].routing
                                               .total_routs) +
                            " routs"),
              std::string::npos);
  }

  // The turning points in the JSON are exactly what the rules compute
  // from the recorded events (recomputed here, independently).
  //  - first commander death
  std::uint64_t first_commander_death = 0;
  for (const sim::Event& event : recorder.events()) {
    if (event.type == sim::EventType::CommanderDeath) {
      first_commander_death = event.tick;
      break;
    }
  }
  if (first_commander_death != 0) {
    EXPECT_NE(json.find("\"first_commander_death\": {\"tick\": " +
                        std::to_string(first_commander_death)),
              std::string::npos);
  } else {
    EXPECT_NE(json.find("\"first_commander_death\": null"),
              std::string::npos);
  }
  //  - mass rout: the first 20-tick window with >= 10% of a faction
  //    routed (both factions have 6 units -> threshold 1).
  std::map<std::uint32_t, std::size_t> faction_of;
  for (const sim::Unit& unit : simulation.world().units) {
    faction_of[unit.id] = static_cast<std::size_t>(unit.faction);
  }
  std::vector<std::uint64_t> rout_ticks[2];
  for (const sim::Event& event : recorder.events()) {
    if (event.type == sim::EventType::Rout) {
      rout_ticks[faction_of.at(event.a)].push_back(event.tick);
    }
  }
  std::uint64_t expected_mass_rout = 0;
  for (int f = 0; f < 2; ++f) {
    for (std::uint64_t t : rout_ticks[f]) {
      std::size_t in_window = 0;
      for (std::uint64_t u : rout_ticks[f]) {
        if (u <= t && (t < 20 || u > t - 20)) {
          ++in_window;
        }
      }
      if (in_window >= 1) {  // 10% of 6 = 0.6 -> 1
        expected_mass_rout = expected_mass_rout == 0 ? t : expected_mass_rout;
        break;
      }
    }
  }
  if (expected_mass_rout != 0) {
    EXPECT_NE(json.find("\"first_mass_rout\": {\"tick\": " +
                        std::to_string(expected_mass_rout)),
              std::string::npos);
  }

  // The rout counts in the JSON equal the recorded rout events per
  // faction (0 = Red, 1 = Blue).
  for (int f = 0; f < 2; ++f) {
    std::size_t routs = 0;
    for (const sim::Event& event : recorder.events()) {
      if (event.type == sim::EventType::Rout &&
          faction_of.at(event.a) == static_cast<std::size_t>(f)) {
        ++routs;
      }
    }
    EXPECT_NE(json.find("\"routs\": " + std::to_string(routs) + ","),
              std::string::npos);
  }
}

TEST(ReportTest, CsvHasColumnsAndRows) {
  const sim::UnitTypeTable types = infantry_types();
  sim::Simulation simulation = make_battle(types, nullptr, 10);
  static_cast<void>(simulation.run());
  const std::string csv = cli::generate_timeline_csv(simulation, types);
  const std::size_t rows = simulation.timeline().size();
  std::size_t lines = 0;
  std::size_t pos = 0;
  while ((pos = csv.find('\n', pos)) != std::string::npos) {
    ++lines;
    ++pos;
  }
  EXPECT_EQ(lines, rows + 1);  // header + one line per sample
  EXPECT_NE(csv.find("tick,red_alive,red_routed,red_dead,red_avg_morale,"
                     "red_engagements"),
            std::string::npos);
  EXPECT_NE(csv.find("blue_engagements"), std::string::npos);
  EXPECT_NE(csv.find(",red_infantry_alive"), std::string::npos);
  EXPECT_NE(csv.find(",blue_infantry_alive"), std::string::npos);
}

TEST(ReportTest, PngIsWritten) {
  const sim::UnitTypeTable types = infantry_types();
  sim::Simulation simulation = make_battle(types, nullptr, 10);
  static_cast<void>(simulation.run());
  const std::string path = "/tmp/m10p2-report-test.png";
  EXPECT_TRUE(cli::write_report_png(path, simulation));
  std::ifstream file(path, std::ios::binary);
  ASSERT_TRUE(file.good());
  char magic[8] = {};
  file.read(magic, 8);
  EXPECT_EQ(magic[0], static_cast<char>(0x89));
  EXPECT_EQ(magic[1], 'P');
  EXPECT_EQ(magic[2], 'N');
  EXPECT_EQ(magic[3], 'G');
  // And a battle without timeline samples cannot produce a chart.
  sim::Simulation bare = make_battle(types, nullptr, 0);
  static_cast<void>(bare.run());
  EXPECT_FALSE(cli::write_report_png("/tmp/m10p2-should-not-exist.png",
                                     bare));
}

// ---------------------------------------------------------------------------
// Squad collapse (a squad's last alive member dies).
// ---------------------------------------------------------------------------

TEST(EventsTest, SquadCollapseFiresWhenTheLastMemberDies) {
  const sim::UnitTypeTable types = sim::parse_unit_types(nlohmann::json::array({
      nlohmann::json{{"id", "infantry"},
                     {"health", 100.0},
                     {"damage", 100.0},
                     {"speed", 2.0},
                     {"range", 2.0},
                     {"cooldown", 1},
                     {"sight_radius", 40.0}},
      nlohmann::json{{"id", "commander"},
                     {"health", 100.0},
                     {"damage", 100.0},
                     {"speed", 2.0},
                     {"range", 2.0},
                     {"cooldown", 1},
                     {"sight_radius", 40.0},
                     {"leadership_radius", 30.0}},
  }).dump());
  std::ifstream commanders_file(TEST_DATA_DIR "/commanders.json");
  const sim::CommanderConfig commanders =
      sim::parse_commanders(commanders_file);

  std::vector<sim::Unit> units;
  auto make = [&](std::uint32_t id, sim::Faction faction, int type_index,
                  double x, double y) {
    sim::Unit unit;
    unit.id = id;
    unit.faction = faction;
    unit.type_index = static_cast<std::uint32_t>(type_index);
    unit.x = x;
    unit.y = y;
    unit.health = 100.0;
    unit.stats = types.types[static_cast<std::size_t>(type_index)].stats;
    unit.stats.damage = 100.0;  // one-hit exchange
    unit.alive = true;
    units.push_back(unit);
  };
  make(1, sim::Faction::Red, 1, 500.0, 500.0);   // commander
  make(2, sim::Faction::Red, 0, 502.0, 500.0);   // soldier
  make(3, sim::Faction::Blue, 1, 512.0, 500.0);  // commander
  make(4, sim::Faction::Blue, 0, 514.0, 500.0);  // soldier
  // Blue outlasts the one-hit exchange: only Red's squad dies out.
  units[2].health = 1000.0;
  units[3].health = 1000.0;

  sim::SimulationConfig config;
  config.seed = 11;
  config.max_ticks = 200;
  config.battlefield_width = 1000.0;
  config.battlefield_height = 1000.0;
  config.spawn_band_fraction = 0.15;
  config.unit_types = &types;
  config.ai_mode = sim::AiMode::Utility;
  config.morale_enabled = false;
  config.commanders = &commanders;
  // The composition mirrors the explicit units (validation requires it
  // whenever squad plans are set): one commander + one soldier each.
  config.composition[0].push_back(sim::TypeCount{1, 1});
  config.composition[0].push_back(sim::TypeCount{0, 1});
  config.composition[1].push_back(sim::TypeCount{1, 1});
  config.composition[1].push_back(sim::TypeCount{0, 1});
  config.squad_plans[0] = sim::SquadPlan{1, 1};
  config.squad_plans[1] = sim::SquadPlan{1, 1};
  sim::EventRecorder recorder;
  config.events = &recorder;
  sim::Simulation simulation(config, units);
  static_cast<void>(simulation.run());

  EXPECT_GE(recorder.count(sim::EventType::Death), std::size_t{2});
  EXPECT_EQ(recorder.count(sim::EventType::SquadCollapse),
            std::size_t{1});  // Red's squad died out
}

}  // namespace
