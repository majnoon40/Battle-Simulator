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

// m13 audit finding 5: known-answer pins for the canonical state hash.
// The encoding is defined as little-endian integers and IEEE-754 double
// bit patterns (never native byte order), so these digests are the same
// on every supported platform. The values were cross-checked against
// the pre-canonicalization code (native-order serialization, built from
// the git history and linked against the same library): the digests
// match byte-for-byte, proving the canonicalization changed no hash
// value — every recorded replay still verifies. Pinning them guards the
// encoding against any future change (which would be a versioned
// replay-compatibility break).
TEST(ReplayTest, StateHashKnownAnswers) {
  const sim::UnitTypeTable types = infantry_types();
  // A fresh battle (before the first tick).
  {
    std::vector<sim::Unit> units;
    const sim::SimulationConfig config = melee_config(types, 5, units);
    const sim::Simulation simulation(config, units);
    EXPECT_EQ(simulation.state_hash(),
              "9d7bfbf96c4486fd290ee9f5dc40e4a4");
  }
  // The first checkpoint of the standard checkpoint run (tick 25).
  {
    const auto checkpoints = run_with_checkpoints(types, 5);
    ASSERT_GE(checkpoints.size(), std::size_t{2});
    EXPECT_EQ(checkpoints[0].first, std::uint64_t{25});
    EXPECT_EQ(checkpoints[0].second, "1a3f5e099ce45892369a0a222fa90113");
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

// m11a: the nav flag records the navigation generation; files from
// before m11a carry no flag and replay under v1 (the exact behavior
// they were recorded with).
TEST(ReplayTest, NavFlagRoundTripAndLegacyDefault) {
  cli::ReplayData data;
  data.version = sim::kSimVersion;
  data.scenario = "default";
  data.mod = "-";
  data.seed = 42;
  data.ticks = 3000;
  data.nav = "v2";
  data.hashes.emplace_back(25, "aabbccddeeff00112233445566778899");
  ASSERT_TRUE(cli::write_replay("/tmp/m11a-nav.replay", data));
  cli::ReplayData parsed;
  std::string error;
  ASSERT_TRUE(cli::parse_replay("/tmp/m11a-nav.replay", parsed, error))
      << error;
  EXPECT_EQ(parsed.nav, "v2");

  // A pre-m11a file (no nav flag) parses as v1.
  const std::string legacy =
      "BATTLESIM REPLAY 1\nversion " + std::string(sim::kSimVersion) +
      "\nscenario default\nmod -\nseed 42\nticks 100\n"
      "flags variance=on contact_seek=on neighbor=grid squads=on "
      "swap_sides=off\nhash 25 aabbccddeeff00112233445566778899\n";
  { std::ofstream file("/tmp/m11a-legacy.replay"); file << legacy; }
  cli::ReplayData legacy_parsed;  // fresh: parse must not inherit v2
  ASSERT_TRUE(
      cli::parse_replay("/tmp/m11a-legacy.replay", legacy_parsed, error))
      << error;
  EXPECT_EQ(legacy_parsed.nav, "v1");

  // An unknown nav value is rejected.
  const std::string bad =
      "BATTLESIM REPLAY 1\nversion " + std::string(sim::kSimVersion) +
      "\nscenario default\nmod -\nseed 42\nticks 100\n"
      "flags nav=v3\nhash 25 aabbccddeeff00112233445566778899\n";
  { std::ofstream file("/tmp/m11a-bad.replay"); file << bad; }
  cli::ReplayData bad_parsed;
  EXPECT_FALSE(cli::parse_replay("/tmp/m11a-bad.replay", bad_parsed, error));
  EXPECT_NE(error.find("nav"), std::string::npos);
}

TEST(ReplayTest, MalformedFilesAreRejected) {  cli::ReplayData parsed;
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

// m13 audit finding 2: every floating-point replay field must be a
// complete, finite decimal — std::stod happily parses "inf", "nan" and
// (via exceptions, previously uncaught-by-design) overflowed exponents.
TEST(ReplayTest, NonFiniteOrderPointsAreRejected) {
  const auto check = [](const std::string& order_line) {
    const std::string content =
        "BATTLESIM REPLAY 1\nversion " + std::string(sim::kSimVersion) +
        "\nscenario default\nmod -\nseed 42\nticks 100\n" + order_line +
        "\n";
    { std::ofstream file("/tmp/m13-nan.replay"); file << content; }
    cli::ReplayData parsed;
    std::string error;
    EXPECT_FALSE(cli::parse_replay("/tmp/m13-nan.replay", parsed, error))
        << "order line: '" << order_line << "'";
  };
  // Every non-finite spelling, in either coordinate, for moveto.
  for (const std::string& bad : {"inf", "-inf", "nan", "NaN", "-NaN",
                                 "infinity", "-infinity", "INF", "NAN",
                                 "1e999", "-1e999", "1e309"}) {
    check("order 10 red all moveto " + bad + " 10");
    check("order 10 red all moveto 10 " + bad);
  }
  // Retreat's optional point is validated the same way.
  check("order 10 red all retreat nan 10");
  check("order 10 red all retreat 10 inf");
  // Malformed numerics (trailing characters, hex floats) still fail.
  check("order 10 red all moveto 25junk 10");
  check("order 10 red all moveto 0x10 10");
  check("order 10 red all moveto 1.5e 10");
  // Finite values still parse (the strictness rejects nothing valid).
  {
    const std::string content =
        "BATTLESIM REPLAY 1\nversion " + std::string(sim::kSimVersion) +
        "\nscenario default\nmod -\nseed 42\nticks 100\n"
        "order 10 red all moveto -12.5 1e3\n";
    { std::ofstream file("/tmp/m13-nan.replay"); file << content; }
    cli::ReplayData parsed;
    std::string error;
    ASSERT_TRUE(cli::parse_replay("/tmp/m13-nan.replay", parsed, error))
        << error;
    ASSERT_EQ(parsed.order_interventions.size(), std::size_t{1});
    EXPECT_DOUBLE_EQ(parsed.order_interventions[0].x, -12.5);
    EXPECT_DOUBLE_EQ(parsed.order_interventions[0].y, 1000.0);
    EXPECT_TRUE(parsed.order_interventions[0].has_point);
  }
}

// m13 audit finding 7: parse_replay has replacement semantics and a
// failed parse must leave a reused output object completely unchanged
// (the old code mutated it line by line, exposing partial state).
TEST(ReplayTest, FailedParseLeavesReplayDataUnchanged) {
  // Sentinel state in a reused object.
  cli::ReplayData reused;
  reused.version = "sentinel";
  reused.scenario = "sentinel-scenario";
  reused.seed = 777;
  reused.ticks = 555;
  reused.interventions.push_back(cli::Intervention{9, 9});
  reused.hashes.emplace_back(25, "aabbccddeeff00112233445566778899");

  // A file that parses several valid lines and then hits an error: the
  // version/scenario/seed lines and one order all validate before the
  // bad hash line fails.
  const std::string content =
      "BATTLESIM REPLAY 1\nversion " + std::string(sim::kSimVersion) +
      "\nscenario default\nmod -\nseed 42\nticks 100\n"
      "order 10 red all moveto 5 5\n"
      "hash 25 notthirtytwochars\n";
  { std::ofstream file("/tmp/m13-reuse.replay"); file << content; }
  std::string error;
  EXPECT_FALSE(cli::parse_replay("/tmp/m13-reuse.replay", reused, error));
  EXPECT_NE(error.find("bad hash line"), std::string::npos) << error;
  // Nothing changed: no partial state leaked in.
  EXPECT_EQ(reused.version, "sentinel");
  EXPECT_EQ(reused.scenario, "sentinel-scenario");
  EXPECT_EQ(reused.seed, 777u);
  EXPECT_EQ(reused.ticks, 555u);
  ASSERT_EQ(reused.interventions.size(), std::size_t{1});
  EXPECT_EQ(reused.interventions[0].unit, 9u);
  EXPECT_TRUE(reused.order_interventions.empty());
  ASSERT_EQ(reused.hashes.size(), std::size_t{1});
  EXPECT_EQ(reused.hashes[0].first, 25u);

  // A successful parse fully replaces the object (not merges into it).
  const std::string good =
      "BATTLESIM REPLAY 1\nversion " + std::string(sim::kSimVersion) +
      "\nscenario other\nmod -\nseed 1\nticks 7\n"
      "hash 25 aabbccddeeff00112233445566778899\n";
  { std::ofstream file("/tmp/m13-reuse.replay"); file << good; }
  ASSERT_TRUE(cli::parse_replay("/tmp/m13-reuse.replay", reused, error))
      << error;
  EXPECT_EQ(reused.version, sim::kSimVersion);
  EXPECT_EQ(reused.scenario, "other");
  EXPECT_EQ(reused.seed, 1u);
  EXPECT_EQ(reused.ticks, 7u);
  EXPECT_TRUE(reused.interventions.empty());
  ASSERT_EQ(reused.hashes.size(), std::size_t{1});
}

// ---------------------------------------------------------------------------
// CLI level: the acceptance scenarios.
// ---------------------------------------------------------------------------

// m13 reliability review: recording without an explicit --scenario used
// to write an empty scenario line (the raw flag value instead of the
// resolved path), so every default-scenario recording failed --verify
// with "no scenario line".
TEST(ReplayCliTest, RecordWithoutScenarioFlagVerifies) {
  std::string output;
  ASSERT_EQ(run_cli("--seed 42 --ticks 120 --record /tmp/m13_default.replay",
                    output),
            0)
      << output;
  EXPECT_EQ(run_cli("--replay /tmp/m13_default.replay --verify", output), 0)
      << output;
  EXPECT_NE(output.find("replay verified"), std::string::npos) << output;
}

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

// m13 item 2: the ai_avail / end flags round-trip through replay files.
TEST(ReplayTest, AvailEndFlagRoundTripAndLegacyDefault) {
  cli::ReplayData data;
  data.version = sim::kSimVersion;
  data.scenario = "default";
  data.mod = "-";
  data.seed = 42;
  data.ticks = 3000;
  data.ai_avail = "v1";
  data.end = "v1";
  data.hashes.emplace_back(25, "aabbccddeeff00112233445566778899");
  ASSERT_TRUE(cli::write_replay("/tmp/m13-avail.replay", data));
  cli::ReplayData parsed;
  std::string error;
  ASSERT_TRUE(cli::parse_replay("/tmp/m13-avail.replay", parsed, error))
      << error;
  EXPECT_EQ(parsed.ai_avail, "v1");
  EXPECT_EQ(parsed.end, "v1");

  // A pre-13.0 file (no ai_avail/end flags) parses as the v2 defaults
  // (older files are refused by the version check before flags matter).
  const std::string legacy =
      "BATTLESIM REPLAY 1\nversion " + std::string(sim::kSimVersion) +
      "\nscenario default\nmod -\nseed 42\nticks 100\n"
      "flags variance=on contact_seek=on neighbor=grid squads=on "
      "swap_sides=off nav=v2 orders=on\n"
      "hash 25 aabbccddeeff00112233445566778899\n";
  { std::ofstream file("/tmp/m13-legacy.replay"); file << legacy; }
  cli::ReplayData legacy_parsed;
  ASSERT_TRUE(
      cli::parse_replay("/tmp/m13-legacy.replay", legacy_parsed, error))
      << error;
  EXPECT_EQ(legacy_parsed.ai_avail, "v2");
  EXPECT_EQ(legacy_parsed.end, "v2");

  // Unknown values are rejected.
  const std::string bad =
      "BATTLESIM REPLAY 1\nversion " + std::string(sim::kSimVersion) +
      "\nscenario default\nmod -\nseed 42\nticks 100\n"
      "flags ai_avail=v3\nhash 25 aabbccddeeff00112233445566778899\n";
  { std::ofstream file("/tmp/m13-bad.replay"); file << bad; }
  cli::ReplayData bad_parsed;
  EXPECT_FALSE(cli::parse_replay("/tmp/m13-bad.replay", bad_parsed, error));
  EXPECT_NE(error.find("ai_avail"), std::string::npos);
}
