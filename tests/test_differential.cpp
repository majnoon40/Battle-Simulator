// M15 S2: differential and metamorphic tests. Every property here runs
// the real binary end to end and compares runs against each other (not
// against a golden file):
//   - side-swap on a symmetric map: aggregate fairness, pinned exactly
//   - side-swap spawn semantics: the band is translated, not mirrored
//   - replay record/verify on representative shipped scenarios (the CI
//     'differential' job sweeps all 19, both plain and swapped)
//   - grid == brute neighbor modes on shipped scenarios
//   - the M7-era flag-off recipe composes with the output-neutral
//     modes (--orders off, --neighbor-mode brute, --threads 4)
//   - a script's orders == the replay's recorded interventions, exactly
//
// Measured background (docs/milestone-15.md, S2): on the symmetric
// default.json the per-seed metamorphic property "the winner flips with
// the same end tick" does NOT hold — spawn draws are allocated by
// faction index and mapped by translation into the flipped band, so
// --swap-sides produces a different random battle, not the relabeled
// mirror (100 seeds: same end tick 0/100, mirrored casualties 0/100,
// winner flip 60/100, mirror overlap of spawn positions 0/250). What
// the symmetric map does give is aggregate fairness — no faction
// advantage (Red 93 / Blue 103 / draw 4 of 200 battles, share 0.465,
// 95% CI [0.396, 0.534]) — and that is what the fairness test pins.
#include <sys/wait.h>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

namespace {

std::string read_text(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file),
                     std::istreambuf_iterator<char>());
}

// Runs a CLI command from the source root (so data/ paths resolve);
// returns the exit status, stdout+stderr land in `output`.
int run_cli(const std::string& arguments, std::string* output = nullptr) {
  const std::string command =
      "cd \"" TEST_DATA_DIR "/..\" && \"" BATTLE_CLI_PATH "\" " +
      arguments + " > /tmp/s2_cli_out.txt 2>&1";
  const int code = std::system(command.c_str());
  if (output != nullptr) {
    *output = read_text("/tmp/s2_cli_out.txt");
  }
  return code;
}

// Runs the battle and returns the export JSON text.
std::string export_via_cli(const std::string& arguments) {
  const std::string path = "/tmp/s2_export.json";
  std::string output;
  EXPECT_EQ(run_cli(arguments + " --export-json " + path, &output), 0)
      << output;
  return read_text(path);
}

// Counts the replay file lines starting with `prefix` (hash checkpoints,
// order interventions).
std::size_t count_lines(const std::string& path, const char* prefix) {
  std::ifstream file(path);
  std::string line;
  std::size_t count = 0;
  while (std::getline(file, line)) {
    if (line.rfind(prefix, 0) == 0) {
      ++count;
    }
  }
  return count;
}

}  // namespace

// ---------------------------------------------------------------------------
// Side-swap on the symmetric map.
// ---------------------------------------------------------------------------

// default.json is mirror-symmetric (1000x1000 open field, identical
// 250-infantry armies, 15% spawn bands), so any systematic faction
// advantage would skew the win count. The counts are exact pins of
// deterministic battles: a battle-behavior change (which must arrive
// behind a flag) shifts them and requires a conscious re-measure — the
// 100-seed evidence and the re-pin policy are in docs/milestone-15.md.
TEST(DifferentialTest, SideSwapAggregateFairnessOnSymmetricMap) {
  int red = 0;
  int blue = 0;
  int draws = 0;
  for (int seed = 1; seed <= 5; ++seed) {
    for (const char* extra : {"", "--swap-sides"}) {
      const std::string report = "/tmp/s2_fair.md";
      std::string output;
      ASSERT_EQ(run_cli("--scenario data/scenarios/default.json --seed " +
                            std::to_string(seed) +
                            " --ticks 2000 --report " + report + " " + extra,
                        &output),
                0)
          << "seed " << seed << " " << extra << ": " << output;
      const nlohmann::json parsed =
          nlohmann::json::parse(read_text("/tmp/s2_fair.json"));
      const std::string winner =
          parsed["outcome"]["winner"].get<std::string>();
      if (winner == "Red") {
        ++red;
      } else if (winner == "Blue") {
        ++blue;
      } else {
        ++draws;
      }
    }
  }
  // Seeds 1-5, both orientations: 10 deterministic battles (the 100-seed
  // evidence behind the pin: Red 93 / Blue 103 / draw 4 of 200).
  EXPECT_EQ(red, 6);
  EXPECT_EQ(blue, 4);
  EXPECT_EQ(draws, 0);
  EXPECT_EQ(red + blue + draws, 10);
}

// --swap-sides maps each unit's band-relative offset to the same offset
// in the opposite band (a translation): the army keeps its layout and
// changes sides. It does NOT reflect positions (a mirror). After one
// tick every unit has moved at most one step of speed 2.0 toward its
// objective, so the offset difference is bounded by two steps (measured
// max 4.000); a mirror mapping would put it at |2*offset - 150|, far
// outside the slack for most units. The y coordinates are NOT expected
// to match: the objective mirrors properly (band centers 75 vs 925)
// while the spawn only translates, so |dx| differs and the normalized
// y-step diverges from tick 1 — part of why the swapped battle is a
// different battle, not a mirror.
TEST(DifferentialTest, SideSwapTranslatesSpawnBandsNotMirrors) {
  ASSERT_EQ(run_cli("--scenario data/scenarios/default.json --seed 42 "
                    "--ticks 1 --export-json /tmp/s2_spawn_pl.json"),
            0);
  ASSERT_EQ(run_cli("--scenario data/scenarios/default.json --seed 42 "
                    "--ticks 1 --swap-sides "
                    "--export-json /tmp/s2_spawn_sw.json"),
            0);
  const nlohmann::json plain =
      nlohmann::json::parse(read_text("/tmp/s2_spawn_pl.json"));
  const nlohmann::json swapped =
      nlohmann::json::parse(read_text("/tmp/s2_spawn_sw.json"));
  std::unordered_map<std::uint32_t, const nlohmann::json*> by_id;
  for (const auto& unit : swapped["units"]) {
    by_id[unit["id"].get<std::uint32_t>()] = &unit;
  }
  // default.json: width 1000, band 15% = 150; east band starts at 850.
  constexpr double kWestLo = 0.0;
  constexpr double kEastLo = 850.0;
  std::size_t checked = 0;
  for (const auto& up : plain["units"]) {
    const auto it = by_id.find(up["id"].get<std::uint32_t>());
    ASSERT_NE(it, by_id.end()) << "unit " << up["id"];
    const nlohmann::json& us = *it->second;
    ASSERT_EQ(up["faction"], us["faction"]) << "unit " << up["id"];
    const bool red = up["faction"] == "Red";
    const double lo_plain = red ? kWestLo : kEastLo;
    const double lo_swapped = red ? kEastLo : kWestLo;
    const double delta = (up["x"].get<double>() - lo_plain) -
                         (us["x"].get<double>() - lo_swapped);
    EXPECT_LE(std::fabs(delta), 4.5)
        << "unit " << up["id"] << " offset delta " << delta;
    // Band placement after one tick of movement toward the center.
    if (red) {
      EXPECT_LT(up["x"].get<double>(), 300.0);
      EXPECT_GT(us["x"].get<double>(), 700.0);
    } else {
      EXPECT_GT(up["x"].get<double>(), 700.0);
      EXPECT_LT(us["x"].get<double>(), 300.0);
    }
    ++checked;
  }
  EXPECT_EQ(checked, std::size_t{500});  // 250 infantry per side
}

// ---------------------------------------------------------------------------
// Replay record/verify and grid == brute on the shipped scenarios.
// ---------------------------------------------------------------------------

// The CI 'differential' job records and verifies every shipped scenario
// (19, both orientations); this keeps the property in the regular suite
// for a representative set: open field, squads, three factions,
// terrain, an earth map in the swapped orientation and a perf scenario
// at a reduced tick budget. Verify re-runs the battle from the recorded
// setup and compares every checkpoint hash; two of the replays also
// verify under 4 threads (the hashes are thread-independent).
TEST(DifferentialTest, ReplayRecordVerifyOnRepresentativeScenarios) {
  struct Case {
    const char* scenario;
    const char* ticks;
    const char* extra;
    bool threads4;
  };
  const Case cases[] = {
      {"data/scenarios/default.json", "300", "", true},
      {"data/scenarios/squads.json", "300", "", false},
      {"data/scenarios/three-way.json", "300", "", false},
      {"data/scenarios/demo-river.json", "300", "", false},
      {"data/scenarios/earth-lebanon.json", "300", "--swap-sides", false},
      {"data/scenarios/perf-5000.json", "150", "", true},
  };
  for (const Case& c : cases) {
    const std::string replay = "/tmp/s2_replay.replay";
    std::string output;
    ASSERT_EQ(run_cli(std::string("--scenario ") + c.scenario +
                          " --seed 42 --ticks " + c.ticks + " " + c.extra +
                          " --record " + replay,
                      &output),
              0)
        << c.scenario << ": " << output;
    EXPECT_GE(count_lines(replay, "hash "), std::size_t{5}) << c.scenario;
    ASSERT_EQ(run_cli("--replay " + replay + " --verify", &output), 0)
        << c.scenario << ": " << output;
    EXPECT_NE(output.find("replay verified"), std::string::npos)
        << c.scenario << ": " << output;
    if (c.threads4) {
      ASSERT_EQ(run_cli("--replay " + replay + " --verify --threads 4",
                        &output),
                0)
          << c.scenario << " (threads 4): " << output;
      EXPECT_NE(output.find("replay verified"), std::string::npos)
          << c.scenario << " (threads 4): " << output;
    }
  }
}

// The spatial grid and the brute-force scan must produce the same
// battles on the shipped scenarios (the m13 tests pin this for a
// synthetic battle and demo-river under threads; this extends it to
// multi-faction faction-scoped queries and more scenarios; the CI job
// adds viewer-demo, m10-line and balanced-demo).
TEST(DifferentialTest, GridVsBruteAgreeOnShippedScenarios) {
  struct Case {
    const char* scenario;
    const char* ticks;
  };
  const Case cases[] = {
      {"data/scenarios/default.json", "300"},
      {"data/scenarios/mixed.json", "300"},
      {"data/scenarios/squads.json", "300"},
      {"data/scenarios/three-way.json", "300"},
      {"data/scenarios/demo-river.json", "200"},  // terrain
  };
  for (const Case& c : cases) {
    const std::string grid = export_via_cli(
        std::string("--scenario ") + c.scenario +
        " --seed 42 --ticks " + c.ticks + " --neighbor-mode grid");
    const std::string brute = export_via_cli(
        std::string("--scenario ") + c.scenario +
        " --seed 42 --ticks " + c.ticks + " --neighbor-mode brute");
    EXPECT_EQ(grid, brute) << c.scenario << " grid != brute";
  }
}

// ---------------------------------------------------------------------------
// Flag-off composition and script orders.
// ---------------------------------------------------------------------------

// The M7-era recipe (--variance off --ai-avail v1 --end v1 --combat v1)
// reproduces the committed baselines byte-for-byte (NoMapPinsTest pins
// that). The output-neutral modes — orders off, the brute neighbor
// scan, multithreading — must compose with the era recipe to the SAME
// bytes: turning every switchable feature off reaches the previous
// release no matter how the inert modes are set.
TEST(DifferentialTest, EraFlagsComposeWithInertModesToBaselines) {
  struct Pin {
    const char* scenario;
    const char* file;
  };
  const Pin pins[] = {
      {"data/scenarios/default.json", "default-s42.json"},
      {"data/scenarios/mixed.json", "mixed-s42.json"},
  };
  for (const Pin& pin : pins) {
    const std::string base_args =
        std::string("--scenario ") + pin.scenario +
        " --seed 42 --variance off --ai-avail v1 --end v1 --combat v1";
    const std::string baseline = read_text(
        std::string(TEST_DATA_DIR) + "/../tests/baselines/m8/" + pin.file);
    ASSERT_FALSE(baseline.empty()) << pin.file << " missing";
    EXPECT_EQ(export_via_cli(base_args), baseline)
        << pin.scenario << " era recipe drifted";
    for (const char* extra :
         {"--orders off", "--neighbor-mode brute", "--threads 4"}) {
      EXPECT_EQ(export_via_cli(base_args + " " + extra), baseline)
          << pin.scenario << " " << extra
          << " is not inert under the era flags";
    }
  }
}

// A script using every order flavor — both factions, all/specific
// squads, with and without points — must be recorded into the replay
// as exactly the same interventions (count, order, fields), and the
// replay must verify. (CliAppliesScriptAndRecordsIt checks one moveto
// line's presence; this pins the full bijection.)
TEST(DifferentialTest, ScriptOrdersMatchReplayInterventionsExactly) {
  const std::string script = "/tmp/s2_script.json";
  {
    std::ofstream file(script);
    file << R"([
      {"tick": 20, "faction": "red", "squads": "all",
       "order": "moveto", "x": 300, "y": 500},
      {"tick": 40, "faction": "blue", "squads": [0],
       "order": "attackmove", "x": 700, "y": 500},
      {"tick": 60, "faction": "red", "squads": [1, 2], "order": "hold"},
      {"tick": 80, "faction": "blue", "squads": "all", "order": "retreat"},
      {"tick": 100, "faction": "red", "squads": [3],
       "order": "retreat", "x": 100, "y": 100},
      {"tick": 120, "faction": "blue", "squads": [4], "order": "regroup"},
      {"tick": 140, "faction": "red", "squads": "all",
       "order": "formation", "formation": "column"},
      {"tick": 160, "faction": "blue", "squads": [5, 6],
       "order": "formation", "formation": "loose"}
    ])";
  }
  const std::string replay = "/tmp/s2_script.replay";
  std::string output;
  ASSERT_EQ(run_cli("--scenario data/scenarios/squads.json --seed 42 "
                    "--ticks 400 --script " + script +
                    " --record " + replay,
                    &output),
            0)
      << output;
  const std::vector<std::string> expected = {
      "order 20 red all moveto 300 500",
      "order 40 blue 0 attackmove 700 500",
      "order 60 red 1,2 hold",
      "order 80 blue all retreat",
      "order 100 red 3 retreat 100 100",
      "order 120 blue 4 regroup",
      "order 140 red all formation column",
      "order 160 blue 5,6 formation loose",
  };
  std::vector<std::string> got;
  {
    std::ifstream file(replay);
    std::string line;
    while (std::getline(file, line)) {
      if (line.rfind("order ", 0) == 0) {
        got.push_back(line);
      }
    }
  }
  ASSERT_EQ(got.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(got[i], expected[i]) << "intervention " << i;
  }
  ASSERT_EQ(run_cli("--replay " + replay + " --verify", &output), 0)
      << output;
  EXPECT_NE(output.find("replay verified"), std::string::npos) << output;
}
