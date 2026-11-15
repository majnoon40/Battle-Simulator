// M10 Part 2b tests: replay recording and verification.
// Library level: the state hash pins the future, the file format
// round-trips, malformed files are rejected with clear errors.
// CLI level (battle_cli): record then verify passes on three
// scenarios, a flipped byte reports the first mismatching tick (or a
// format error), a version mismatch is refused, and an intervention is
// reproduced by the replay.
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "cli/replay.hpp"
#include "sim/simulation.hpp"
#include "sim/unit_types.hpp"
#include "sim/version.hpp"

namespace {

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
                    double y) {
  sim::Unit unit;
  unit.id = id;
  unit.faction = faction;
  unit.type_index = 0;
  unit.x = x;
  unit.y = y;
  unit.health = 100.0;
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

sim::SimulationConfig melee_config(const sim::UnitTypeTable& types,
                                   std::uint32_t seed,
                                   std::vector<sim::Unit>& units) {
  for (int i = 0; i < 8; ++i) {
    units.push_back(make_unit(i + 1, sim::Faction::Red,
                              10.0 + i * 2.0, 500.0));
    units.push_back(make_unit(i + 101, sim::Faction::Blue,
                              30.0 + i * 2.0, 500.0));
  }
  sim::SimulationConfig config;
  config.seed = seed;
  config.max_ticks = 300;
  config.battlefield_width = 1000.0;
  config.battlefield_height = 1000.0;
  config.spawn_band_fraction = 0.15;
  config.unit_types = &types;
  config.ai_mode = sim::AiMode::Utility;
  config.morale_enabled = true;
  config.tactics_enabled = true;
  return config;
}

// Runs a battle tick-by-tick (like the --record path), collecting the
// state hash at every checkpoint; optionally applies one intervention.
std::vector<std::pair<std::uint64_t, std::string>> run_with_checkpoints(
    const sim::UnitTypeTable& types, std::uint32_t seed,
    std::uint32_t kill_unit = 0, std::uint64_t kill_tick = 0) {
  std::vector<sim::Unit> units;
  const sim::SimulationConfig config = melee_config(types, seed, units);
  sim::Simulation simulation(config, units);
  std::vector<std::pair<std::uint64_t, std::string>> hashes;
  while (!simulation.finished()) {
    const std::uint64_t upcoming = simulation.world().tick_count + 1;
    if (kill_unit != 0 && kill_tick == upcoming) {
      static_cast<void>(simulation.intervene_kill(kill_unit));
    }
    simulation.tick();
    if (simulation.world().tick_count % 25 == 0 || simulation.finished()) {
      hashes.emplace_back(simulation.world().tick_count,
                          simulation.state_hash());
    }
  }
  return hashes;
}

// Runs a battle_cli command from the data directory (like the earth map
// tests) and returns its exit code; `output` receives stdout+stderr.
int run_cli(const std::string& arguments, std::string& output) {
  const std::string command =
      "cd \"" TEST_DATA_DIR "/..\" && \"" BATTLE_CLI_PATH "\" " + arguments +
      " 2>&1";
  std::FILE* pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) {
    return -1;
  }
  char buffer[512];
  output.clear();
  while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    output += buffer;
  }
  const int code = pclose(pipe);
  return WEXITSTATUS(code);
}

}  // namespace

// ---------------------------------------------------------------------------
// Library level.
// ---------------------------------------------------------------------------

TEST(ReplayTest, ReRunProducesIdenticalHashes) {
  const sim::UnitTypeTable types = infantry_types();
  const auto first = run_with_checkpoints(types, 5);
  const auto second = run_with_checkpoints(types, 5);
  ASSERT_EQ(first.size(), second.size());
  ASSERT_GE(first.size(), std::size_t{2});
  for (std::size_t i = 0; i < first.size(); ++i) {
    EXPECT_EQ(first[i].first, second[i].first);
    EXPECT_EQ(first[i].second, second[i].second);
  }
}

TEST(ReplayTest, HashDetectsDivergence) {
  const sim::UnitTypeTable types = infantry_types();
  const auto plain = run_with_checkpoints(types, 5);
  // Killing unit 1 at tick 10 must change every checkpoint from tick 25
  // on (the battle can end by rout soon after, so the kill must land
  // before the first checkpoint).
  const auto killed = run_with_checkpoints(types, 5, 1, 10);
  ASSERT_EQ(plain.size(), killed.size());
  bool diverged = false;
  for (std::size_t i = 0; i < plain.size(); ++i) {
    if (plain[i].second != killed[i].second) {
      diverged = true;
    }
  }
  EXPECT_TRUE(diverged);
}

TEST(ReplayTest, FileRoundTrip) {
  cli::ReplayData data;
  data.version = sim::kSimVersion;
  data.scenario = "default";
  data.mod = "-";
  data.seed = 42;
  data.ticks = 3000;
  data.variance = "on";
  data.contact_seek = "on";
  data.neighbor = "grid";
  data.squads = "on";
  data.swap_sides = "off";
  data.interventions.push_back(cli::Intervention{7, 120});
  data.hashes.emplace_back(25, "aabbccddeeff00112233445566778899");
  data.hashes.emplace_back(50, "11223344556677889900aabbccddeeff");
  ASSERT_TRUE(cli::write_replay("/tmp/m10p2-roundtrip.replay", data));
  cli::ReplayData parsed;
  std::string error;
  ASSERT_TRUE(cli::parse_replay("/tmp/m10p2-roundtrip.replay", parsed,
                                error))
      << error;
  EXPECT_EQ(parsed.version, data.version);
  EXPECT_EQ(parsed.scenario, data.scenario);
  EXPECT_EQ(parsed.mod, data.mod);
  EXPECT_EQ(parsed.seed, data.seed);
  EXPECT_EQ(parsed.ticks, data.ticks);
  EXPECT_EQ(parsed.variance, data.variance);
  EXPECT_EQ(parsed.contact_seek, data.contact_seek);
  EXPECT_EQ(parsed.neighbor, data.neighbor);
  EXPECT_EQ(parsed.squads, data.squads);
  EXPECT_EQ(parsed.swap_sides, data.swap_sides);
  ASSERT_EQ(parsed.interventions.size(), std::size_t{1});
  EXPECT_EQ(parsed.interventions[0].unit, data.interventions[0].unit);
  EXPECT_EQ(parsed.interventions[0].tick, data.interventions[0].tick);
  ASSERT_EQ(parsed.hashes.size(), std::size_t{2});
  EXPECT_EQ(parsed.hashes[1].second, data.hashes[1].second);
}

TEST(ReplayTest, MalformedFilesAreRejected) {
  cli::ReplayData parsed;
  std::string error;
  const auto check = [&](const std::string& content,
                         const std::string& needle) {
    const std::string path = "/tmp/m10p2-bad.replay";
    { std::ofstream file(path); file << content; }
    EXPECT_FALSE(cli::parse_replay(path, parsed, error));
    EXPECT_NE(error.find(needle), std::string::npos)
        << "error was: " << error;
  };
  check("not a replay\n", "magic");
  check("BATTLESIM REPLAY 1\nversion 10.2\n", "scenario");
  check("BATTLESIM REPLAY 1\nversion 10.2\nscenario default\nseed x\n",
        "bad seed");
  check("BATTLESIM REPLAY 1\nversion 10.2\nscenario default\nseed 1\n"
        "nonsense 2\n",
        "unknown replay line");
  check("BATTLESIM REPLAY 1\nversion 10.2\nscenario default\nseed 1\n"
        "ticks 100\n"
        "hash 50 11223344556677889900aabbccddeeff\n"
        "hash 50 11223344556677889900aabbccddeeff\n",
        "non-ascending");
}

// ---------------------------------------------------------------------------
// CLI level: the acceptance scenarios.
// ---------------------------------------------------------------------------

TEST(ReplayCliTest, RecordThenVerifyOnThreeScenarios) {
  struct Case {
    const char* name;
    const char* extra;
  };
  const Case cases[] = {
      {"default", "--scenario default --seed 7 --ticks 400"},
      {"demo-river", "--scenario demo-river --seed 3 --ticks 400"},
      {"earth-lebanon", "--scenario earth-lebanon --seed 3 --ticks 400"},
  };
  for (const Case& one : cases) {
    std::string output;
    const std::string replay = std::string("/tmp/m10p2-") + one.name +
                               ".replay";
    EXPECT_EQ(run_cli(std::string(one.extra) + " --record " + replay,
                      output),
              0)
        << one.name << ": " << output;
    EXPECT_EQ(run_cli("--replay " + replay + " --verify", output), 0)
        << one.name << ": " << output;
    EXPECT_NE(output.find("replay verified"), std::string::npos)
        << one.name << ": " << output;
  }
}

TEST(ReplayCliTest, FlippedHashByteReportsFirstMismatchTick) {
  std::string output;
  ASSERT_EQ(run_cli("--scenario default --seed 7 --ticks 400 --record "
                    "/tmp/m10p2-flip.replay",
                    output),
            0);
  // Flip one hex digit inside the first checkpoint hash.
  std::string content;
  { std::ifstream file("/tmp/m10p2-flip.replay"); std::getline(file, content, '\0'); }
  const std::size_t hash_line = content.find("hash 25 ");
  ASSERT_NE(hash_line, std::string::npos);
  const std::size_t digit = hash_line + 8;  // first hex digit of the hash
  content[digit] = content[digit] == '0' ? '1' : '0';
  { std::ofstream file("/tmp/m10p2-flip.replay"); file << content; }
  EXPECT_EQ(run_cli("--replay /tmp/m10p2-flip.replay --verify", output),
            2);
  EXPECT_NE(output.find("first mismatching tick: 25"),
            std::string::npos)
      << output;
}

TEST(ReplayCliTest, VersionMismatchIsRefused) {
  std::string output;
  ASSERT_EQ(run_cli("--scenario default --seed 7 --ticks 400 --record "
                    "/tmp/m10p2-old.replay",
                    output),
            0);
  std::string content;
  {
    std::ifstream file("/tmp/m10p2-old.replay");
    std::getline(file, content, '\0');
  }
  const std::size_t version_line = content.find("version ");
  ASSERT_NE(version_line, std::string::npos);
  const std::size_t eol = content.find('\n', version_line);
  content =
      content.substr(0, version_line) + "version 0.1" + content.substr(eol);
  {
    std::ofstream file("/tmp/m10p2-old.replay");
    file << content;
  }
  EXPECT_EQ(run_cli("--replay /tmp/m10p2-old.replay --verify", output), 1);
  EXPECT_NE(output.find("replaying across versions is not supported"),
            std::string::npos)
      << output;
  EXPECT_NE(output.find("sim version"), std::string::npos) << output;
}

TEST(ReplayCliTest, InterventionIsReproducedByReplay) {
  std::string output;
  // Record WITH an intervention; the replay must re-apply it and pass.
  ASSERT_EQ(run_cli("--scenario default --seed 7 --ticks 400 "
                    "--intervene kill:1:100 --record "
                    "/tmp/m10p2-iv.replay",
                    output),
            0)
      << output;
  EXPECT_EQ(run_cli("--replay /tmp/m10p2-iv.replay --verify", output), 0)
      << output;
  EXPECT_NE(output.find("replay verified"), std::string::npos) << output;
  // The same battle WITHOUT the intervention must fail against this
  // replay: the recorded hashes encode the intervention's effect.
  // (Sanity that the intervention actually changed the battle: record
  // it without the kill and compare the two files' hash lines.)
  ASSERT_EQ(run_cli("--scenario default --seed 7 --ticks 400 --record "
                    "/tmp/m10p2-noiv.replay",
                    output),
            0);
  const auto hash_lines = [](const std::string& path) {
    std::ifstream file(path);
    std::string line;
    std::string joined;
    while (std::getline(file, line)) {
      if (line.rfind("hash ", 0) == 0) {
        joined += line + "\n";
      }
    }
    return joined;
  };
  EXPECT_NE(hash_lines("/tmp/m10p2-iv.replay"),
            hash_lines("/tmp/m10p2-noiv.replay"));
}

TEST(ReplayCliTest, SeekStopsAtTheRequestedTick) {
  std::string output;
  ASSERT_EQ(run_cli("--scenario default --seed 7 --ticks 400 --record "
                    "/tmp/m10p2-seek.replay",
                    output),
            0);
  // --seek re-runs from the start and stops at T; the export shows the
  // state at exactly T.
  ASSERT_EQ(run_cli("--replay /tmp/m10p2-seek.replay --seek 150 "
                    "--export-json /tmp/m10p2-seek150.json",
                    output),
            0)
      << output;
  std::ifstream json("/tmp/m10p2-seek150.json");
  std::stringstream buffer;
  buffer << json.rdbuf();
  EXPECT_NE(buffer.str().find("\"ticks_elapsed\": 150"), std::string::npos)
      << buffer.str().substr(0, 200);
}
