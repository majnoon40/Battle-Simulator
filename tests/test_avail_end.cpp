// m13 item 2 — the v1 byte-identity contract. The committed baselines in
// tests/data/baselines/m13_avail_v1/ were generated from main 1993a90
// (sim 11.2) with the then-default flags; running the same battles with
// --ai-avail v1 --end v1 must reproduce them byte-for-byte, and the
// recorded v2 runs must verify. The v2 behavior itself is covered by the
// availability/termination tests in test_tactics.cpp and test_morale.cpp
// and the acceptance measurements in docs/milestone-13.md.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <sys/wait.h>

#include <gtest/gtest.h>

namespace {

// Runs a battle_cli command from the data directory (like the replay
// tests) and returns its exit code.
int run_cli(const std::string& arguments) {
  const std::string command =
      "cd \"" TEST_DATA_DIR "/..\" && \"" BATTLE_CLI_PATH "\" " + arguments +
      " 2>&1";
  std::FILE* pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) {
    return -1;
  }
  char buffer[512];
  while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
  }
  const int code = pclose(pipe);
  return WEXITSTATUS(code);
}

std::string read_bytes(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

// One pinned battle: scenario + seed (+ extra CLI flags) and the
// committed baseline its v1 export must equal byte-for-byte.
struct Baseline {
  const char* name;
  const char* arguments;  // scenario/seed/extra, no --export-json
};

const std::vector<Baseline>& baselines() {
  static const std::vector<Baseline> list{
      {"default_s42", "--scenario data/scenarios/default.json --seed 42"},
      {"viewer-demo_s42",
       "--scenario data/scenarios/viewer-demo.json --seed 42"},
      {"demo-river_s42",
       "--scenario data/scenarios/demo-river.json --seed 42"},
      {"earth-nile-delta_s7", "--scenario earth-nile-delta --seed 7"},
      {"earth-nile-delta_s7_swap",
       "--scenario earth-nile-delta --seed 7 --swap-sides"},
  };
  return list;
}

}  // namespace

TEST(AvailEndV1Test, BaselinesReproduceMainByteForByte) {
  for (const Baseline& baseline : baselines()) {
    const std::string out =
        "/tmp/m13_baseline_" + std::string(baseline.name) + ".json";
    const std::string arguments =
        std::string(baseline.arguments) + " --ai-avail v1 --end v1" +
        " --export-json " + out;
    ASSERT_EQ(run_cli(arguments), 0) << baseline.name;
    const std::string produced = read_bytes(out);
    ASSERT_FALSE(produced.empty()) << baseline.name << ": empty export";
    const std::string expected = read_bytes(
        std::string(TEST_DATA_DIR) + "/baselines/m13_avail_v1/" +
        std::string(baseline.name) + ".json");
    ASSERT_FALSE(expected.empty()) << baseline.name << ": missing baseline";
    EXPECT_EQ(produced, expected)
        << baseline.name << ": v1 export differs from the main baseline";
  }
}

TEST(AvailEndV1Test, OrdersBaselineReproducesMainByteForByte) {
  // The squads battle with a hold + attackmove script (the m12 paths:
  // order delivery, engagement, point steering).
  const char* script = R"json([
    {"tick": 100, "faction": "red", "squads": "all", "order": "hold"},
    {"tick": 150, "faction": "blue", "squads": [0], "order": "attackmove",
     "x": 300.0, "y": 500.0}
  ])json";
  { std::ofstream file("/tmp/m13_orders_script.json"); file << script; }
  const std::string out = "/tmp/m13_baseline_squads_orders_s42.json";
  const std::string arguments =
      "--scenario data/scenarios/squads.json --seed 42 --ticks 1500"
      " --orders on --script /tmp/m13_orders_script.json"
      " --ai-avail v1 --end v1 --export-json " +
      out;
  ASSERT_EQ(run_cli(arguments), 0);
  const std::string produced = read_bytes(out);
  const std::string expected = read_bytes(
      std::string(TEST_DATA_DIR) +
      "/baselines/m13_avail_v1/squads_orders_s42.json");
  ASSERT_FALSE(expected.empty());
  EXPECT_EQ(produced, expected);
}

TEST(AvailEndV1Test, RecordedV2ReplayVerifies) {
  // A v2-default record/verify round trip on the swapped Nile map (the
  // acceptance configuration) with a nonzero run.
  const std::string base =
      "--scenario earth-nile-delta --seed 11 --swap-sides --ticks 3000";
  ASSERT_EQ(run_cli(base + " --record /tmp/m13_v2.replay"), 0);
  ASSERT_EQ(run_cli(base + " --replay /tmp/m13_v2.replay --verify"), 0);
  // And a v1-flagged record verifies too (flags ride in the replay).
  const std::string v1 = base + " --ai-avail v1 --end v1";
  ASSERT_EQ(run_cli(v1 + " --record /tmp/m13_v1.replay"), 0);
  ASSERT_EQ(run_cli(v1 + " --replay /tmp/m13_v1.replay --verify"), 0);
  // The v2 file carries the flags.
  const std::string replay = read_bytes("/tmp/m13_v2.replay");
  EXPECT_NE(replay.find("ai_avail=v2"), std::string::npos);
  EXPECT_NE(replay.find("end=v2"), std::string::npos);
  const std::string replay_v1 = read_bytes("/tmp/m13_v1.replay");
  EXPECT_NE(replay_v1.find("ai_avail=v1"), std::string::npos);
  EXPECT_NE(replay_v1.find("end=v1"), std::string::npos);
}
