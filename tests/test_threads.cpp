// m13 item 4: determinism across thread counts. The --threads contract
// is byte-identical output for every thread count — verified here at
// three levels:
//   1. Library: one battle exercising the order-dependent decision
//      paths (contact seeking + the faction estimate decay, the Help
//      cap, player orders, squad mechanics, interventions) runs with
//      1/2/3/4/8 threads; every 25-tick state hash and the final JSON
//      export must be identical.
//   2. CLI: the four acceptance scenarios (default, viewer-demo,
//      demo-river, earth map) export byte-identical JSON across
//      1/2/3/4/8 threads; grid == brute holds under threads; a replay
//      recorded with 4 threads verifies under 1 and 4.
//   3. Profiling: --profile-phases reads the clock only — the battle
//      hash is unchanged with it on or off.
#include <sys/wait.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "sim/config.hpp"
#include "sim/json_export.hpp"
#include "sim/simulation.hpp"
#include "sim/unit_types.hpp"

namespace {

sim::UnitTypeTable battle_types() {
  // Infantry plus a commander type (leadership radius > 0) so squad
  // plans validate: one commander per squad, count*size soldiers.
  return sim::parse_unit_types(nlohmann::json::array({
      nlohmann::json{{"id", "infantry"},
                     {"health", 800.0},
                     {"damage", 12.0},
                     {"speed", 2.0},
                     {"range", 2.0},
                     {"cooldown", 1},
                     {"sight_radius", 40.0}},
      nlohmann::json{{"id", "commander"},
                     {"health", 900.0},
                     {"damage", 12.0},
                     {"speed", 2.0},
                     {"range", 2.0},
                     {"cooldown", 1},
                     {"sight_radius", 50.0},
                     {"leadership_radius", 60.0}},
  }).dump());
}

sim::Unit make_unit(std::uint32_t id, sim::Faction faction, double x,
                    double y, std::uint32_t type_index = 0) {
  sim::Unit unit;
  unit.id = id;
  unit.faction = faction;
  unit.type_index = type_index;
  unit.x = x;
  unit.y = y;
  unit.health = type_index == 1 ? 900.0 : 800.0;
  unit.stats.max_health = type_index == 1 ? 900.0 : 800.0;
  unit.stats.damage = 12.0;
  unit.stats.speed = 2.0;
  unit.stats.attack_range = 2.0;
  unit.stats.attack_cooldown_ticks = 1;
  unit.stats.sight_radius = type_index == 1 ? 50.0 : 40.0;
  unit.stats.leadership_radius = type_index == 1 ? 60.0 : 0.0;
  unit.stats.accuracy = 1.0;
  unit.alive = true;
  return unit;
}

// The shared battle config: utility + tactics + squads (2 squads of 4
// with commanders) + contact seek, two separated clusters so the early
// ticks are contact-less (streaks, seeking) before contact forms the
// faction estimates, a Hold and a MoveTo player order at tick 50, and
// two mid-run kills.
sim::SimulationConfig battle_config(const sim::UnitTypeTable& types,
                                    std::vector<sim::Unit>& units) {
  for (int i = 0; i < 8; ++i) {
    units.push_back(make_unit(i + 1, sim::Faction::Red,
                              10.0 + (i % 4) * 2.0, 480.0 + (i / 4) * 4.0));
    units.push_back(make_unit(i + 101, sim::Faction::Blue,
                              160.0 + (i % 4) * 2.0,
                              480.0 + (i / 4) * 4.0));
  }
  // Two commanders per faction lead the squads.
  units.push_back(make_unit(9, sim::Faction::Red, 12.0, 482.0, 1));
  units.push_back(make_unit(10, sim::Faction::Red, 12.0, 486.0, 1));
  units.push_back(make_unit(109, sim::Faction::Blue, 162.0, 482.0, 1));
  units.push_back(make_unit(110, sim::Faction::Blue, 162.0, 486.0, 1));

  sim::SimulationConfig config;
  config.seed = 5;
  config.max_ticks = 600;
  config.battlefield_width = 1000.0;
  config.battlefield_height = 1000.0;
  config.spawn_band_fraction = 0.15;
  config.unit_types = &types;
  config.composition[0] = {sim::TypeCount{1, 2}, sim::TypeCount{0, 8}};
  config.composition[1] = {sim::TypeCount{1, 2}, sim::TypeCount{0, 8}};
  config.ai_mode = sim::AiMode::Utility;
  // Morale off for THIS battle: with 10-unit clusters the default
  // morale config collapses a whole faction to a rout-cascade end at
  // tick 30 (verified identical on the pre-threads binary — pre-existing
  // behavior for this shape, not a threading artifact). The morale and
  // routing machinery is determinism-covered by the CLI scenarios
  // below (default 250v250 runs it for 500 ticks across all thread
  // counts); this battle focuses on squads, player orders, the Help
  // cap, contact seeking and interventions.
  config.morale_enabled = false;
  config.tactics_enabled = true;
  config.squads_enabled = true;
  config.squad_plans[0] = sim::SquadPlan{2, 4};  // Red: 2 squads of 4
  config.squad_plans[1] = sim::SquadPlan{2, 4};  // Blue
  return config;
}

// One battle tick-by-tick with periodic state hashes; the player orders
// and interventions ride along between ticks.
struct CheckpointRun {
  std::vector<std::pair<std::uint64_t, std::string>> hashes;
  std::string export_json;
  sim::BattleResult result;
  sim::Simulation::PhaseTimings timings;
  bool orders_ok = true;
};

CheckpointRun run_checkpoint_battle(const sim::UnitTypeTable& types,
                                    std::uint32_t threads,
                                    bool profile = false) {
  std::vector<sim::Unit> units;
  sim::SimulationConfig config = battle_config(types, units);
  config.thread_count = threads;
  sim::Simulation simulation(config, units);
  simulation.enable_phase_profiling(profile);
  CheckpointRun run;
  while (!simulation.finished()) {
    const std::uint64_t upcoming = simulation.world().tick_count + 1;
    // Player orders through the commanders (Red squads 0-1, Blue squads
    // 2-3 — assign_squads numbers them per faction in order), one call
    // per faction per tick: the commanders config rate-limits
    // issue_order to max_per_tick successful calls.
    if (upcoming == 50) {
      run.orders_ok = run.orders_ok &&
                      simulation.issue_order(sim::Faction::Red, {0},
                                             sim::PlayerOrderType::Hold);
    }
    if (upcoming == 51) {
      run.orders_ok = run.orders_ok &&
                      simulation.issue_order(sim::Faction::Red, {1},
                                             sim::PlayerOrderType::MoveTo,
                                             80.0, 500.0);
    }
    if (upcoming == 52) {
      run.orders_ok = run.orders_ok &&
                      simulation.issue_order(sim::Faction::Blue, {2, 3},
                                             sim::PlayerOrderType::Hold);
    }
    if (upcoming == 120) {
      static_cast<void>(simulation.intervene_kill(3));
      static_cast<void>(simulation.intervene_kill(104));
    }
    simulation.tick();
    if (simulation.world().tick_count % 25 == 0 || simulation.finished()) {
      run.hashes.emplace_back(simulation.world().tick_count,
                              simulation.state_hash());
    }
  }
  run.export_json = sim::export_state_json(simulation);
  run.result = simulation.result();
  run.timings = simulation.phase_timings();
  return run;
}

// Runs a battle_cli command from the repo root and returns its exit
// code (`output` receives stdout+stderr).
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
  return WEXITSTATUS(pclose(pipe));
}

bool files_equal(const std::string& a, const std::string& b) {
  std::ifstream left(a, std::ios::binary);
  std::ifstream right(b, std::ios::binary);
  if (!left || !right) {
    return false;
  }
  std::ostringstream ls, rs;
  ls << left.rdbuf();
  rs << right.rdbuf();
  return ls.str() == rs.str();
}

}  // namespace

// Level 1: the library battle across 1/2/3/4/8 threads. The reference
// is the inline (threads=1) run — which is the pre-threads code path.
TEST(ThreadsDeterminismTest, SameBattleAcrossThreadCounts) {
  const sim::UnitTypeTable types = battle_types();
  const CheckpointRun reference = run_checkpoint_battle(types, 1);
  ASSERT_TRUE(reference.orders_ok) << "the scripted orders were rejected";
  ASSERT_GE(reference.hashes.size(), std::size_t{8});
  for (std::uint32_t threads : {2u, 3u, 4u, 8u}) {
    const CheckpointRun threaded = run_checkpoint_battle(types, threads);
    ASSERT_TRUE(threaded.orders_ok) << "threads=" << threads;
    ASSERT_EQ(threaded.hashes.size(), reference.hashes.size())
        << "threads=" << threads;
    for (std::size_t i = 0; i < reference.hashes.size(); ++i) {
      ASSERT_EQ(threaded.hashes[i].first, reference.hashes[i].first)
          << "threads=" << threads << " checkpoint " << i;
      ASSERT_EQ(threaded.hashes[i].second, reference.hashes[i].second)
          << "threads=" << threads << " first divergence at checkpoint "
          << i << " (tick " << reference.hashes[i].first << ")";
    }
    EXPECT_EQ(threaded.export_json, reference.export_json)
        << "threads=" << threads;
    EXPECT_EQ(threaded.result.winner, reference.result.winner)
        << "threads=" << threads;
    EXPECT_EQ(threaded.result.ticks_elapsed, reference.result.ticks_elapsed)
        << "threads=" << threads;
  }
}

// Level 2a: the acceptance scenarios export byte-identical JSON across
// thread counts (the export contains every unit, all counters and the
// timeline — any divergence shows up here).
TEST(ThreadsDeterminismTest, ScenarioExportsAreByteIdentical) {
  const struct {
    const char* scenario;
    const char* extra;
  } cases[] = {
      {"data/scenarios/default.json", ""},
      {"data/scenarios/viewer-demo.json", ""},
      {"data/scenarios/demo-river.json", ""},
      {"data/scenarios/earth-lebanon.json", "--swap-sides"},
  };
  for (const auto& item : cases) {
    const std::string base_arguments =
        std::string("--scenario ") + item.scenario +
        " --seed 42 --ticks 500 " + item.extra + " ";
    std::string output;
    ASSERT_EQ(run_cli(base_arguments + "--threads 1 --export-json "
                                  "/tmp/m13_threads_ref.json",
                      output),
              0)
        << item.scenario << " (threads 1): " << output;
    for (const char* threads : {"2", "3", "4", "8"}) {
      const std::string path = "/tmp/m13_threads_t.json";
      ASSERT_EQ(run_cli(base_arguments + "--threads " + threads +
                            " --export-json " + path,
                        output),
                0)
          << item.scenario << " (threads " << threads << "): " << output;
      EXPECT_TRUE(files_equal("/tmp/m13_threads_ref.json", path))
          << item.scenario << " diverged with --threads " << threads;
    }
  }
}

// Level 2b: grid == brute neighbor modes hold under threads (legacy
// targeting scans run on the pool too).
TEST(ThreadsDeterminismTest, GridAndBruteAgreeUnderThreads) {
  std::string output;
  ASSERT_EQ(run_cli("--units 200 --seed 7 --ticks 200 --threads 4 "
                    "--neighbor-mode grid --export-json /tmp/m13_g.json",
                    output),
            0)
      << output;
  ASSERT_EQ(run_cli("--units 200 --seed 7 --ticks 200 --threads 4 "
                    "--neighbor-mode brute --export-json /tmp/m13_b.json",
                    output),
            0)
      << output;
  EXPECT_TRUE(files_equal("/tmp/m13_g.json", "/tmp/m13_b.json"));
  // And threads do not change either mode's result.
  ASSERT_EQ(run_cli("--units 200 --seed 7 --ticks 200 --threads 1 "
                    "--neighbor-mode grid --export-json /tmp/m13_g1.json",
                    output),
            0)
      << output;
  EXPECT_TRUE(files_equal("/tmp/m13_g1.json", "/tmp/m13_g.json"));
}

// Level 2c: a replay recorded with 4 threads verifies under 1 and 4 —
// the checkpoint hashes are thread-independent, both directions.
TEST(ThreadsDeterminismTest, ReplayRecordedWithThreadsVerifies) {
  std::string output;
  ASSERT_EQ(run_cli("--scenario data/scenarios/default.json --seed 42 "
                    "--ticks 400 --threads 4 --record /tmp/m13_th.replay",
                    output),
            0)
      << output;
  ASSERT_EQ(run_cli("--replay /tmp/m13_th.replay --verify --threads 1",
                    output),
            0)
      << output;
  EXPECT_NE(output.find("replay verified"), std::string::npos) << output;
  ASSERT_EQ(run_cli("--replay /tmp/m13_th.replay --verify --threads 4",
                    output),
            0)
      << output;
  EXPECT_NE(output.find("replay verified"), std::string::npos) << output;
}

// Level 3: --profile-phases must not change the battle (clock reads
// only), and the accumulators must actually accumulate.
TEST(PhaseProfilingTest, ProfilingDoesNotChangeOutput) {
  const sim::UnitTypeTable types = battle_types();
  const CheckpointRun plain = run_checkpoint_battle(types, 1);
  // Same battle with profiling on (threads 2 to also profile the pool
  // path).
  const CheckpointRun profiled = run_checkpoint_battle(types, 2, true);

  ASSERT_FALSE(profiled.hashes.empty());
  EXPECT_EQ(profiled.hashes.back().second, plain.hashes.back().second)
      << "profiling changed the final state hash";
  EXPECT_EQ(profiled.export_json, plain.export_json)
      << "profiling changed the export";
  EXPECT_EQ(profiled.result.ticks_elapsed, plain.result.ticks_elapsed)
      << "profiling changed the run length";

  const sim::Simulation::PhaseTimings& timings = profiled.timings;
  EXPECT_GT(timings.ticks, std::uint64_t{0});
  EXPECT_GT(timings.perception_ns, std::uint64_t{0});
  EXPECT_GT(timings.decision_ns, std::uint64_t{0});
  EXPECT_GT(timings.resolution_ns, std::uint64_t{0});
  EXPECT_GT(timings.commit_ns, std::uint64_t{0});
  EXPECT_GE(timings.total_ns,
            timings.perception_ns + timings.decision_ns +
                timings.resolution_ns + timings.commit_ns);
}
