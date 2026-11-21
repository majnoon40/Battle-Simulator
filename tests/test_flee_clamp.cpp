// M15 flee-clamp tests: the S1 finding (routed/flanking units leave the
// battlefield — the Advance/Approach/Retreat family never clamped)
// fixed behind --flee-clamp (default on; off reproduces 13.1
// byte-for-byte, pinned by the CLI tests below).
//
// The repro: a 200x100 edge battle — objectives at y=85 pull the armies
// together near the top border, flank points get computed beside
// enemies close to it and (unclamped) a unit marches straight past the
// edge and stays outside (measured with the clamp off: 1 unit, 0.65
// overshoot, still outside at the final tick; seed 1).
#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "sim/config.hpp"
#include "sim/simulation.hpp"
#include "sim/unit_types.hpp"

namespace {

constexpr std::size_t kRed = static_cast<std::size_t>(sim::Faction::Red);
constexpr std::size_t kBlue = static_cast<std::size_t>(sim::Faction::Blue);

std::string read_text(const std::string& path) {
  std::ifstream file(path);
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

const sim::UnitTypeTable& edge_types() {
  static const sim::UnitTypeTable table = sim::parse_unit_types(
      read_text(std::string(TEST_DATA_DIR) + "/units.json"));
  return table;
}
const sim::PersonalityTable& edge_personalities() {
  static const sim::PersonalityTable table = sim::parse_personalities(
      read_text(std::string(TEST_DATA_DIR) + "/personalities.json"));
  return table;
}
const sim::FormationConfig& edge_formations() {
  static const sim::FormationConfig table = sim::parse_formations(
      read_text(std::string(TEST_DATA_DIR) + "/formations.json"));
  return table;
}
const sim::CommanderConfig& edge_commanders() {
  static const sim::CommanderConfig table = sim::parse_commanders(
      read_text(std::string(TEST_DATA_DIR) + "/commanders.json"));
  return table;
}
const sim::MoraleConfig& edge_morale() {
  static const sim::MoraleConfig table = sim::parse_morale(
      read_text(std::string(TEST_DATA_DIR) + "/morale.json"));
  return table;
}

// The 200x100 edge-clash battle (see the file comment).
sim::SimulationConfig edge_config(bool clamp) {
  sim::SimulationConfig config;
  config.seed = 1;
  config.max_ticks = 500;
  config.ai_mode = sim::AiMode::Utility;
  config.morale_enabled = true;
  config.morale = &edge_morale();
  config.personalities = &edge_personalities();
  config.unit_types = &edge_types();
  config.formations = &edge_formations();
  config.commanders = &edge_commanders();
  config.flee_clamp_enabled = clamp;
  config.battlefield_width = 200.0;
  config.battlefield_height = 100.0;
  config.spawn_band_fraction = 0.15;
  config.composition[kRed].push_back(sim::TypeCount{0, 15});
  config.composition[kRed].push_back(sim::TypeCount{3, 1});
  config.composition[kBlue].push_back(sim::TypeCount{0, 15});
  config.composition[kBlue].push_back(sim::TypeCount{3, 1});
  config.squad_plans[kRed] = sim::SquadPlan{1, 15};
  config.squad_plans[kBlue] = sim::SquadPlan{1, 15};
  config.objectives[kRed] = sim::ObjectivePoint{150.0, 85.0, true};
  config.objectives[kBlue] = sim::ObjectivePoint{50.0, 85.0, true};
  return config;
}

struct BoundsScan {
  std::size_t outside = 0;
  std::size_t alive_outside = 0;
  double max_overshoot = 0.0;
};

BoundsScan scan_bounds(const sim::Simulation& simulation) {
  BoundsScan scan;
  for (const sim::Unit& unit : simulation.world().units) {
    const double over =
        std::max(-unit.x, std::max(-unit.y,
                     std::max(unit.x - simulation.world().width,
                              unit.y - simulation.world().height)));
    if (over > 0.0) {
      ++scan.outside;
      scan.max_overshoot = std::max(scan.max_overshoot, over);
      if (unit.alive) {
        ++scan.alive_outside;
      }
    }
  }
  return scan;
}

int run_cli(const std::string& arguments, std::string* output = nullptr) {
  const std::string command =
      "cd \"" TEST_DATA_DIR "/..\" && \"" BATTLE_CLI_PATH "\" " +
      arguments + " > /tmp/m15_flee_cli.txt 2>&1";
  const int code = std::system(command.c_str());
  if (output != nullptr) {
    *output = read_text("/tmp/m15_flee_cli.txt");
  }
  return code;
}

}  // namespace

// With the clamp on, no unit of the edge battle ever leaves the field.
TEST(FleeClampTest, EdgeBattleStaysInsideWithClampOn) {
  sim::Simulation simulation(edge_config(true));
  for (std::uint32_t t = 0; t < 500; ++t) {
    simulation.tick();
    const BoundsScan scan = scan_bounds(simulation);
    ASSERT_EQ(scan.outside, std::size_t{0})
        << "tick " << t << ": " << scan.alive_outside << " alive outside, "
        << "overshoot " << scan.max_overshoot;
  }
}

// The same battle with the clamp off reproduces the 13.1 behavior —
// an alive unit ends up past the border and stays there (the S1
// finding). This pins the bug the flag fixes; if a future change makes
// the unclamped paths stay inside on its own, this test failing is the
// signal to revisit the flag.
TEST(FleeClampTest, UnclampedPathsLeaveTheField) {
  sim::Simulation simulation(edge_config(false));
  BoundsScan worst;
  for (std::uint32_t t = 0; t < 500; ++t) {
    simulation.tick();
    const BoundsScan scan = scan_bounds(simulation);
    worst.outside = std::max(worst.outside, scan.outside);
    worst.alive_outside = std::max(worst.alive_outside, scan.alive_outside);
    worst.max_overshoot = std::max(worst.max_overshoot,
                                   scan.max_overshoot);
  }
  const BoundsScan final_scan = scan_bounds(simulation);
  EXPECT_GE(worst.outside, std::size_t{1});
  EXPECT_GE(worst.alive_outside, std::size_t{1});
  EXPECT_GT(worst.max_overshoot, 0.0);
  // The unclamped unit does not come back on its own.
  EXPECT_GE(final_scan.outside, std::size_t{1});
}

// OFF reaches the previous release: the edge battle's export with
// --flee-clamp off matches the committed 13.1 reference bytes, and the
// default (on) export differs from it (the clamp only bites where a
// unit actually leaves).
TEST(FleeClampTest, OffReproducesThe131ReferenceBytes) {
  const std::string scenario = "/tmp/m15_flee_edge.json";
  {
    std::ofstream file(scenario);
    file << R"json({
      "battlefield": {"width": 200.0, "height": 100.0},
      "spawn_band_fraction": 0.15,
      "armies": {"red": {"infantry": 15, "commander": 1},
                 "blue": {"infantry": 15, "commander": 1}},
      "objectives": {"red": {"x": 150.0, "y": 85.0},
                     "blue": {"x": 50.0, "y": 85.0}},
      "squads": {"red": {"count": 1, "size": 15},
                 "blue": {"count": 1, "size": 15}}
    })json";
  }
  const std::string base = "--scenario " + scenario + " --seed 1 --ticks 500 ";
  std::string output;
  ASSERT_EQ(run_cli(base + "--flee-clamp off --export-json "
                          "/tmp/m15_flee_off.json", &output),
            0)
      << output;
  ASSERT_EQ(run_cli(base + "--export-json /tmp/m15_flee_on.json", &output),
            0)
      << output;
  const std::string off = read_text("/tmp/m15_flee_off.json");
  const std::string on = read_text("/tmp/m15_flee_on.json");
  const std::string reference = read_text(
      std::string(TEST_DATA_DIR) +
      "/../tests/baselines/flee-clamp/edge-s1.json");
  ASSERT_FALSE(reference.empty()) << "13.1 reference baseline missing";
  EXPECT_EQ(off, reference)
      << "--flee-clamp off drifted from the 13.1 bytes";
  EXPECT_NE(on, off) << "the clamp changed nothing in the edge battle";
}

// The flag rides in replays both ways and verifies.
TEST(FleeClampTest, ReplayCarriesTheFlagAndVerifies) {
  std::string output;
  ASSERT_EQ(run_cli("--scenario data/scenarios/default.json --seed 42 "
                    "--ticks 200 --flee-clamp off --record "
                    "/tmp/m15_flee.replay", &output),
            0)
      << output;
  const std::string replay =
      read_text("/tmp/m15_flee.replay");
  EXPECT_NE(replay.find("flee_clamp=off"), std::string::npos);
  EXPECT_NE(replay.find("version 13.2"), std::string::npos);
  ASSERT_EQ(run_cli("--replay /tmp/m15_flee.replay --verify", &output), 0)
      << output;
  EXPECT_NE(output.find("replay verified"), std::string::npos);
  // A 13.1 replay is refused by version (the clean refusal the bump
  // buys; previously a counters-era same-version replay failed at
  // checkpoint verify instead).
  const std::string old = "BATTLESIM REPLAY 1\nversion 13.1\nscenario "
                          "default\nmod -\nseed 42\nticks 100\nflags "
                          "variance=on\nhash 25 aabbccddeeff001122334455"
          "66778899\n";
  { std::ofstream file("/tmp/m15_flee_old.replay"); file << old; }
  const int code = run_cli("--replay /tmp/m15_flee_old.replay --verify",
                           &output);
  EXPECT_NE(code, 0);
  EXPECT_NE(output.find("version"), std::string::npos) << output;
}
