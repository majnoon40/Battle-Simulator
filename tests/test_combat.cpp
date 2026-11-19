// m13 item 3 — combat depth tests: the arc math at exact angles, the
// charge trigger/reset/reduction, fatigue and experience bounds and
// effects, the commander aura radius, the defensive stance, the export
// shape (format 9), determinism, grid == brute and the v1 byte-identity
// baselines. Crafted battles use zero-variance, accuracy-1 types and
// customized CombatConfigs so the arithmetic is exact.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "wait_status.hpp"

#include <sys/wait.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "sim/combat.hpp"
#include "sim/config.hpp"
#include "sim/scenario.hpp"
#include "sim/simulation.hpp"
#include "sim/tactics.hpp"
#include "sim/unit_types.hpp"

namespace {

constexpr std::size_t kRed = static_cast<std::size_t>(sim::Faction::Red);
constexpr std::size_t kBlue = static_cast<std::size_t>(sim::Faction::Blue);
constexpr double kPi = 3.14159265358979323846;

sim::Unit make_unit(std::uint32_t id, sim::Faction faction, double x,
                    double y, double health = 100.0) {
  sim::Unit unit;
  unit.id = id;
  unit.faction = faction;
  unit.type_index = 0;
  unit.x = x;
  unit.y = y;
  unit.health = health;
  unit.stats.max_health = health;
  unit.stats.damage = 12.0;
  unit.stats.speed = 2.0;
  unit.stats.attack_range = 2.0;
  unit.stats.attack_cooldown_ticks = 1;
  unit.stats.sight_radius = 60.0;
  unit.stats.accuracy = 1.0;
  unit.alive = true;
  unit.morale = 60.0;
  return unit;
}

// A one-type table with full control over the stats.
sim::UnitTypeTable table_of(double health, double damage, double speed,
                            double range, double sight, double accuracy,
                            double variance) {
  return sim::parse_unit_types(nlohmann::json::array({
      nlohmann::json{{"id", "soldier"},
                     {"health", health},
                     {"damage", damage},
                     {"speed", speed},
                     {"range", range},
                     {"cooldown", 1},
                     {"sight_radius", sight},
                     {"accuracy", accuracy},
                     {"damage_variance", variance}},
  }).dump());
}

sim::SimulationConfig base_config(const sim::UnitTypeTable& types,
                                  std::uint64_t max_ticks,
                                  const sim::CombatConfig& combat) {
  sim::SimulationConfig config;
  config.seed = 42;
  config.max_ticks = max_ticks;
  config.battlefield_width = 2000.0;
  config.battlefield_height = 1000.0;
  config.ai_mode = sim::AiMode::Utility;
  config.unit_types = &types;
  config.morale_enabled = false;
  config.squads_enabled = false;
  config.tactics_enabled = true;
  config.composition[kRed] = {{0, 1}};
  config.composition[kBlue] = {{0, 1}};
  config.combat = &combat;
  return config;
}

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
  return exit_status_of(pclose(pipe));
}

std::string read_bytes(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

}  // namespace

// ---------------------------------------------------------------------------
// The arc math at exact angles.

TEST(CombatArcTest, ArcsAtExactAngles) {
  const sim::CombatConfig config;  // 60 / 120 degrees, x1.25 / x1.5
  // A target facing east (0): the attacker straight ahead is frontal.
  sim::AttackArc arc = sim::attack_arc(0.0, 10.0, 0.0, config);
  EXPECT_DOUBLE_EQ(arc.multiplier, 1.0);
  EXPECT_FALSE(arc.flank);
  EXPECT_FALSE(arc.rear);
  // Exactly at the flank boundary (60 degrees) is still frontal
  // (inclusive), a hair beyond is flank.
  arc = sim::attack_arc(0.0, std::cos(60.0 * kPi / 180.0),
                        std::sin(60.0 * kPi / 180.0), config);
  EXPECT_DOUBLE_EQ(arc.multiplier, 1.0);
  EXPECT_FALSE(arc.flank);
  arc = sim::attack_arc(0.0, std::cos(61.0 * kPi / 180.0),
                        std::sin(61.0 * kPi / 180.0), config);
  EXPECT_TRUE(arc.flank);
  EXPECT_DOUBLE_EQ(arc.multiplier, 1.25);
  // Exactly at the rear boundary (120 degrees) is flank; beyond is rear.
  arc = sim::attack_arc(0.0, std::cos(120.0 * kPi / 180.0),
                        std::sin(120.0 * kPi / 180.0), config);
  EXPECT_TRUE(arc.flank);
  EXPECT_FALSE(arc.rear);
  arc = sim::attack_arc(0.0, std::cos(121.0 * kPi / 180.0),
                        std::sin(121.0 * kPi / 180.0), config);
  EXPECT_TRUE(arc.rear);
  EXPECT_DOUBLE_EQ(arc.multiplier, 1.5);
  // Straight behind is rear; a coincident attacker is frontal.
  arc = sim::attack_arc(0.0, -1.0, 0.0, config);
  EXPECT_TRUE(arc.rear);
  EXPECT_DOUBLE_EQ(arc.multiplier, 1.5);
  arc = sim::attack_arc(0.0, 0.0, 0.0, config);
  EXPECT_DOUBLE_EQ(arc.multiplier, 1.0);
  // Angles wrap: a target facing west (pi or -pi) hit from the east is
  // rear; hit from the west it is frontal.
  arc = sim::attack_arc(kPi, 10.0, 0.0, config);
  EXPECT_TRUE(arc.rear);
  arc = sim::attack_arc(-kPi, -10.0, 0.0, config);
  EXPECT_DOUBLE_EQ(arc.multiplier, 1.0);  // ahead of a west-facing unit
  arc = sim::attack_arc(-kPi, 10.0, 0.0, config);
  EXPECT_TRUE(arc.rear);
  // Facing north (pi/2), an attacker to the east is on the flank
  // (90 degrees from front).
  arc = sim::attack_arc(kPi / 2.0, 10.0, 0.0, config);
  EXPECT_TRUE(arc.flank);
}

// ---------------------------------------------------------------------------
// Flank and rear damage land only from the side they name.

TEST(CombatFacingTest, RearBonusOnlyFromBehind) {
  // Two duels, zero variance, accuracy 1: red attacks blues that face
  // west (their objective). Pair 1's red is west of its blue (frontal);
  // pair 2's red is east of its blue (rear, x1.5). The blues are pinned
  // (speed 0) and too far from anything else to matter.
  const sim::UnitTypeTable types = table_of(1000000.0, 20.0, 2.0, 2.0,
                                            80.0, 1.0, 0.0);
  sim::CombatConfig combat;  // defaults: flank x1.25, rear x1.5
  sim::SimulationConfig config = base_config(types, 6, combat);
  config.composition[kRed] = {{0, 2}};
  config.composition[kBlue] = {{0, 2}};
  std::vector<sim::Unit> units{
      make_unit(1, sim::Faction::Red, 500.0, 400.0, 1000000.0),
      make_unit(2, sim::Faction::Red, 500.0, 600.0, 1000000.0),
      make_unit(3, sim::Faction::Blue, 502.0, 400.0, 1000000.0),
      make_unit(4, sim::Faction::Blue, 498.0, 600.0, 1000000.0),
  };
  units[0].stats = types.types[0].stats;
  units[1].stats = types.types[0].stats;
  units[2].stats = types.types[0].stats;
  units[3].stats = types.types[0].stats;
  units[2].stats.speed = 0.0;  // pinned targets
  units[3].stats.speed = 0.0;
  sim::Simulation simulation(config, units);
  for (int i = 0; i < 6; ++i) {
    simulation.tick();
  }
  const sim::BattleResult result = simulation.run();
  // Rear hits happened and only toward the unit facing away.
  EXPECT_GT(result.factions[kRed].combat.rear_hits, 0u);
  // The rear target lost 1.5x the frontal target's health (same damage,
  // same cooldown, same tick count).
  const double behind_lost =
      1000000.0 - simulation.world().units[3].health;
  const double front_lost =
      1000000.0 - simulation.world().units[2].health;
  EXPECT_GT(behind_lost, 0.0);
  EXPECT_NEAR(behind_lost / front_lost, 1.5, 0.01);
}

// ---------------------------------------------------------------------------
// Charge: trigger, reset, and the reduction against Hold.

TEST(CombatChargeTest, TriggerResetAndHoldReduction) {
  const sim::UnitTypeTable types = table_of(1000000.0, 20.0, 5.0, 2.0,
                                            120.0, 1.0, 0.0);
  sim::CombatConfig combat;
  const auto build = [&](std::vector<sim::Personality> personalities) {
    sim::SimulationConfig config = base_config(types, 30, combat);
    std::vector<sim::Unit> units{
        make_unit(1, sim::Faction::Red, 400.0, 500.0, 1000000.0),
        make_unit(2, sim::Faction::Blue, 460.0, 500.0, 1000000.0),
    };
    units[0].stats = types.types[0].stats;  // speed 5: charge-eligible
    units[1].stats = types.types[0].stats;
    units[1].stats.speed = 0.0;  // pinned victim
    return sim::Simulation(config, units, personalities);
  };
  // An aggressive rider: approaches (5/tick), reaches range 2 after ~12
  // ticks having run 58 toward the victim: the first hit charges.
  const std::vector<sim::Personality> rider = {
      {0.5, 0.1, 1.0, 0.5, sim::kCustomArchetype},
      {0.5, 0.5, 0.5, 0.5, sim::kCustomArchetype},
  };
  sim::Simulation charge = build(rider);
  for (int i = 0; i < 30 && !charge.finished(); ++i) {
    charge.tick();
  }
  const sim::BattleResult result = charge.run();
  EXPECT_GT(result.factions[kRed].combat.charge_hits, 0u)
      << "the run never triggered a charge";
  // The charge fired once and reset: the attacker is now stationary in
  // melee, so later hits do not charge again (a handful of ticks at
  // most — the counter stays tiny, not one per hit).
  EXPECT_LE(result.factions[kRed].combat.charge_hits, 3u);

  // The same charge against a target that holds its ground (cautious,
  // low aggression: Hold outscores Attack out of melee) takes half the
  // bonus: the first hit deals 20 x 1.25 = 25, not 30.
  const std::vector<sim::Personality> rider_and_holder = {
      {0.5, 0.1, 1.0, 0.5, sim::kCustomArchetype},
      {0.9, 1.0, 0.0, 0.5, sim::kCustomArchetype},
  };
  sim::Simulation reduced = build(rider_and_holder);
  for (int i = 0; i < 30 && !reduced.finished(); ++i) {
    reduced.tick();
  }
  const sim::BattleResult reduced_result = reduced.run();
  EXPECT_GT(reduced_result.factions[kRed].combat.charge_hits, 0u);
  // A speed-0 non-holding victim would eat x1.5; compare the two first
  // hits via the victims' health: 30 vs 25 (one hit connected in each
  // run at the same tick when both attackers arrive).
  const double plain_first =
      1000000.0 - charge.world().units[1].health;
  const double held_first =
      1000000.0 - reduced.world().units[1].health;
  EXPECT_GT(plain_first, 0.0);
  EXPECT_NEAR(plain_first / held_first, 30.0 / 25.0, 0.2)
      << "plain=" << plain_first << " held=" << held_first;
}

// ---------------------------------------------------------------------------
// Fatigue: bounds and the speed/damage effects.

TEST(CombatFatigueTest, ExhaustionSlowsTheMarchAndStaysBounded) {
  // One unit marching alone toward its objective (940 units away at
  // speed 2 = ~470 ticks); fatigue caps at 1.0 after move_gain x 500
  // ticks, and from then on the step is 0.7 x speed. A pinned blue far
  // away keeps the composition valid without ever being seen.
  const sim::UnitTypeTable types = table_of(100.0, 10.0, 2.0, 2.0, 40.0,
                                            1.0, 0.0);
  sim::CombatConfig combat;  // move gain 1/500, speed factor 0.7
  sim::SimulationConfig config = base_config(types, 800, combat);
  std::vector<sim::Unit> units{make_unit(1, sim::Faction::Red, 60.0,
                                         500.0),
                               make_unit(2, sim::Faction::Blue, 1900.0,
                                         100.0)};
  units[0].stats = types.types[0].stats;
  units[1].stats = types.types[0].stats;
  units[1].stats.speed = 0.0;
  sim::Simulation simulation(config, units);
  double fatigue = 0.0;
  double slowest_step = 10.0;
  int slow_ticks = 0;
  for (int tick = 1; tick <= 700; ++tick) {
    simulation.tick();
    const sim::Unit& unit = simulation.world().units[0];
    fatigue = unit.fatigue;
    ASSERT_LE(unit.fatigue, 1.0) << "fatigue exceeded its bound";
    ASSERT_GE(unit.fatigue, 0.0);
    if (tick > 520 && unit.last_move_x > 0.0) {
      // Past exhaustion the march runs at 0.7 x speed = 1.4/tick.
      slowest_step = std::min(slowest_step, unit.last_move_x);
      ++slow_ticks;
    }
  }
  EXPECT_GE(fatigue, 1.0) << "the unit never exhausted";
  ASSERT_GT(slow_ticks, 50) << "the exhausted unit stopped marching";
  EXPECT_LE(slowest_step, 1.4 + 1e-9)
      << "the exhausted unit moves faster than the fatigue factor";
}

TEST(CombatFatigueTest, FightingTiresAndIdleRecovers) {
  // Two huge-health neighbours in melee (a near-dead blue would flee and
  // break the exchange): 40 attacks exhaust (fight gain 1/40 per swing);
  // an intervention kill then leaves the survivor alone and idle, and
  // recovery (1/300 per tick) brings the fatigue back down.
  const sim::UnitTypeTable types = table_of(1000000.0, 1.0, 2.0, 2.0,
                                            40.0, 1.0, 0.0);
  sim::CombatConfig combat;
  sim::SimulationConfig config = base_config(types, 240, combat);
  std::vector<sim::Unit> units{
      make_unit(1, sim::Faction::Red, 500.0, 500.0, 1000000.0),
      make_unit(2, sim::Faction::Blue, 501.5, 500.0, 1000000.0),
  };
  units[0].stats = types.types[0].stats;
  units[1].stats = types.types[0].stats;
  sim::Simulation simulation(config, units);
  double peak = 0.0;
  for (int tick = 1; tick <= 200; ++tick) {
    simulation.tick();
    peak = std::max(peak, simulation.world().units[0].fatigue);
  }
  EXPECT_GE(peak, 0.9) << "fighting never exhausted the unit (peak "
                       << peak << ")";
  EXPECT_LE(peak, 1.0);
  ASSERT_TRUE(simulation.intervene_kill(2));
  for (int tick = 0; tick < 40 && !simulation.finished(); ++tick) {
    simulation.tick();
  }
  const double final_fatigue = simulation.world().units[0].fatigue;
  EXPECT_LT(final_fatigue, peak) << "idle recovery never ran";
}

// ---------------------------------------------------------------------------
// Experience: exact gains and the bound.

TEST(CombatExperienceTest, KillsAndEngagementGrowItAndItCaps) {
  // Custom config: no per-tick gain, 0.05 per kill, so three kills make
  // exactly 0.15. A one-shot hero (own type) kills three healthy, pinned
  // victims (near-dead victims would flee and break the fixture).
  const sim::UnitTypeTable types = sim::parse_unit_types(nlohmann::json::array({
      nlohmann::json{{"id", "hero"},
                     {"health", 1000000.0}, {"damage", 1000000.0},
                     {"speed", 2.0}, {"range", 2.0}, {"cooldown", 1},
                     {"sight_radius", 60.0}, {"accuracy", 1.0}},
      nlohmann::json{{"id", "foe"},
                     {"health", 1000000.0}, {"damage", 1.0},
                     {"speed", 2.0}, {"range", 2.0}, {"cooldown", 1},
                     {"sight_radius", 60.0}, {"accuracy", 1.0}},
  }).dump());
  sim::CombatConfig combat;
  combat.experience.gain_per_engaged_tick = 0.0;
  combat.experience.gain_per_kill = 0.05;
  sim::SimulationConfig config = base_config(types, 60, combat);
  config.composition[kRed] = {{0, 1}};
  config.composition[kBlue] = {{1, 3}};
  std::vector<sim::Unit> units{
      make_unit(1, sim::Faction::Red, 500.0, 500.0, 1000000.0),
      make_unit(2, sim::Faction::Blue, 501.5, 500.0, 1000000.0),
      make_unit(3, sim::Faction::Blue, 505.0, 505.0, 1000000.0),
      make_unit(4, sim::Faction::Blue, 510.0, 510.0, 1000000.0),
  };
  for (std::size_t i = 0; i < units.size(); ++i) {
    units[i].stats = types.types[i == 0 ? 0 : 1].stats;
    if (units[i].faction == sim::Faction::Blue) {
      units[i].stats.speed = 0.0;
      units[i].type_index = 1;
    }
  }
  // An aggressive hero (default personalities back off from close
  // enemies — the utility AI's standoff behavior, not a combat bug).
  const std::vector<sim::Personality> personalities = {
      {0.5, 0.1, 1.0, 0.5, sim::kCustomArchetype},
      {0.5, 0.5, 0.5, 0.5, sim::kCustomArchetype},
      {0.5, 0.5, 0.5, 0.5, sim::kCustomArchetype},
      {0.5, 0.5, 0.5, 0.5, sim::kCustomArchetype},
  };
  sim::Simulation simulation(config, units, personalities);
  for (int tick = 1; tick <= 60 && !simulation.finished(); ++tick) {
    simulation.tick();
  }
  EXPECT_FALSE(simulation.world().units[1].alive);
  EXPECT_FALSE(simulation.world().units[2].alive);
  EXPECT_FALSE(simulation.world().units[3].alive)
      << "the kills did not all happen";
  // Three kills x 0.05 exactly (engagement adds nothing in this config).
  EXPECT_NEAR(simulation.world().units[0].experience, 0.15, 1e-12);
}

// ---------------------------------------------------------------------------
// The commander aura: inside the radius, not outside.

TEST(CombatAuraTest, AuraFollowsTheLeadershipRadius) {
  // Two red attackers with identical stats hit two pinned blue targets.
  // A commander stands next to attacker A (inside its 30-unit aura);
  // attacker B is 400 away. Custom aura bonus 0.5: A deals 1.5x B.
  const sim::UnitTypeTable types = sim::parse_unit_types(nlohmann::json::array({
      nlohmann::json{{"id", "soldier"},
                     {"health", 1000000.0}, {"damage", 10.0},
                     {"speed", 2.0}, {"range", 2.0}, {"cooldown", 1},
                     {"sight_radius", 80.0}, {"accuracy", 1.0}},
      nlohmann::json{{"id", "leader"},
                     {"health", 1000000.0}, {"damage", 1.0},
                     {"speed", 2.0}, {"range", 2.0}, {"cooldown", 1},
                     {"sight_radius", 80.0}, {"accuracy", 1.0},
                     {"leadership_radius", 30.0}},
  }).dump());
  sim::CombatConfig combat;
  combat.aura.damage_bonus = 0.5;
  sim::SimulationConfig config = base_config(types, 6, combat);
  config.composition[kRed] = {{0, 2}, {1, 1}};
  config.composition[kBlue] = {{0, 2}};
  std::vector<sim::Unit> units{
      make_unit(1, sim::Faction::Red, 500.0, 400.0, 1000000.0),  // A
      make_unit(2, sim::Faction::Red, 500.0, 800.0, 1000000.0),  // B
      make_unit(3, sim::Faction::Blue, 502.0, 400.0, 1000000.0),
      make_unit(4, sim::Faction::Blue, 502.0, 800.0, 1000000.0),
      make_unit(5, sim::Faction::Red, 505.0, 405.0, 1000000.0),  // leader
  };
  for (std::size_t i = 0; i < units.size(); ++i) {
    units[i].stats = types.types[i == 4 ? 1 : 0].stats;
  }
  units[2].stats.speed = 0.0;
  units[3].stats.speed = 0.0;
  units[4].stats.speed = 0.0;  // the leader holds position
  sim::Simulation simulation(config, units);
  for (int i = 0; i < 6; ++i) {
    simulation.tick();
  }
  const double a_lost = 1000000.0 - simulation.world().units[2].health;
  const double b_lost = 1000000.0 - simulation.world().units[3].health;
  EXPECT_GT(a_lost, 0.0);
  EXPECT_GT(b_lost, 0.0);
  EXPECT_NEAR(a_lost / b_lost, 1.5, 0.01)
      << "the aura bonus did not follow the leadership radius";
}

// ---------------------------------------------------------------------------
// Export shape, determinism, grid == brute, replay flags, v1 baselines.

TEST(CombatExportTest, Format9CarriesTheCombatFields) {
  ASSERT_EQ(run_cli("--scenario data/scenarios/squads.json --seed 7 "
                    "--ticks 400 --export-json /tmp/m13_combat_v2.json"),
            0);
  const std::string text = read_bytes("/tmp/m13_combat_v2.json");
  EXPECT_NE(text.find("\"format_version\": 9"), std::string::npos);
  EXPECT_NE(text.find("\"facing\""), std::string::npos);
  EXPECT_NE(text.find("\"fatigue\""), std::string::npos);
  EXPECT_NE(text.find("\"experience\""), std::string::npos);
  EXPECT_NE(text.find("\"flank_hits\""), std::string::npos);
  EXPECT_NE(text.find("\"rear_hits\""), std::string::npos);
  EXPECT_NE(text.find("\"charge_hits\""), std::string::npos);
  // v1 keeps the pre-13.1 shape: no combat fields, format <= 8.
  ASSERT_EQ(run_cli("--scenario data/scenarios/squads.json --seed 7 "
                    "--ticks 400 --combat v1 "
                    "--export-json /tmp/m13_combat_v1.json"),
            0);
  const std::string v1 = read_bytes("/tmp/m13_combat_v1.json");
  EXPECT_EQ(v1.find("\"facing\""), std::string::npos);
  EXPECT_EQ(v1.find("\"fatigue\""), std::string::npos);
  EXPECT_EQ(v1.find("\"format_version\": 9"), std::string::npos);
}

TEST(CombatExportTest, DeterministicAndGridEqualsBrute) {
  ASSERT_EQ(run_cli("--scenario data/scenarios/squads.json --seed 7 "
                    "--ticks 300 --export-json /tmp/m13_d1.json"),
            0);
  ASSERT_EQ(run_cli("--scenario data/scenarios/squads.json --seed 7 "
                    "--ticks 300 --export-json /tmp/m13_d2.json"),
            0);
  EXPECT_EQ(read_bytes("/tmp/m13_d1.json"), read_bytes("/tmp/m13_d2.json"));
  ASSERT_EQ(run_cli("--scenario data/scenarios/squads.json --seed 7 "
                    "--ticks 300 --neighbor-mode brute "
                    "--export-json /tmp/m13_d3.json"),
            0);
  EXPECT_EQ(read_bytes("/tmp/m13_d1.json"), read_bytes("/tmp/m13_d3.json"));
}

TEST(CombatExportTest, ReplayCarriesTheCombatFlag) {
  const std::string base =
      "--scenario data/scenarios/default.json --seed 42 --ticks 300";
  ASSERT_EQ(run_cli(base + " --record /tmp/m13_combat.replay"), 0);
  const std::string replay = read_bytes("/tmp/m13_combat.replay");
  EXPECT_NE(replay.find("combat=v2"), std::string::npos);
  ASSERT_EQ(run_cli(base + " --replay /tmp/m13_combat.replay --verify"),
            0);
  // A v1-flagged record verifies with its flag restored.
  ASSERT_EQ(run_cli(base + " --combat v1 --record /tmp/m13_combat1.replay"),
            0);
  const std::string replay_v1 = read_bytes("/tmp/m13_combat1.replay");
  EXPECT_NE(replay_v1.find("combat=v1"), std::string::npos);
  ASSERT_EQ(
      run_cli(base + " --combat v1 --replay /tmp/m13_combat1.replay --verify"),
      0);
}

TEST(CombatExportTest, V1BaselinesReproduceMainByteForByte) {
  struct Pin {
    const char* name;
    const char* arguments;
  };
  const Pin pins[] = {
      {"default_s42", "--scenario data/scenarios/default.json --seed 42"},
      {"viewer-demo_s42",
       "--scenario data/scenarios/viewer-demo.json --seed 42"},
      {"demo-river_s42",
       "--scenario data/scenarios/demo-river.json --seed 42"},
      {"earth-nile-delta_s7_swap",
       "--scenario earth-nile-delta --seed 7 --swap-sides"},
  };
  for (const Pin& pin : pins) {
    const std::string out = std::string("/tmp/m13_cb_") + pin.name + ".json";
    ASSERT_EQ(run_cli(std::string(pin.arguments) + " --combat v1" +
                      " --export-json " + out),
              0)
        << pin.name;
    const std::string expected = read_bytes(
        std::string(TEST_DATA_DIR) + "/baselines/m13_combat_v1/" +
        std::string(pin.name) + ".json");
    ASSERT_FALSE(expected.empty()) << pin.name << ": baseline missing";
    EXPECT_EQ(read_bytes(out), expected)
        << pin.name << ": v1 differs from the 13.0 baseline";
  }
}

// ---------------------------------------------------------------------------
// The defensive stance: it follows the Hold ORDER, not the momentary
// action (holders fight back when engaged — their action flips to
// Attack — and the stance must survive that).

TEST(CombatStanceTest, HoldOrderGrantsTheStanceWhileEngaged) {
  // A scripted hold battle: the ordered side's squads hold from tick 100,
  // get engaged, and their stance absorbs a steady stream of hits. The
  // same battle without orders barely produces stances (only the rare
  // action-Hold standoff).
  std::ofstream script("/tmp/m13_stance_orders.json");
  script << "[{\"tick\": 100, \"faction\": \"red\", \"squads\": \"all\", "
         << "\"order\": \"hold\"}]\n";
  script.close();
  ASSERT_EQ(run_cli("--scenario data/scenarios/squads.json --seed 3 "
                    "--ticks 900 --orders on "
                    "--script /tmp/m13_stance_orders.json "
                    "--export-json /tmp/m13_stance_hold.json"),
            0);
  nlohmann::json held =
      nlohmann::json::parse(read_bytes("/tmp/m13_stance_hold.json"));
  const std::uint64_t absorbed =
      held["statistics"]["Red"]["combat"]["stance_hits_taken"]
          .get<std::uint64_t>();
  EXPECT_GT(absorbed, 100u)
      << "the Hold order's stance never engaged while under attack";
  ASSERT_EQ(run_cli("--scenario data/scenarios/squads.json --seed 3 "
                    "--ticks 900 --export-json /tmp/m13_stance_free.json"),
            0);
  nlohmann::json free =
      nlohmann::json::parse(read_bytes("/tmp/m13_stance_free.json"));
  const std::uint64_t baseline =
      free["statistics"]["Red"]["combat"]["stance_hits_taken"]
          .get<std::uint64_t>();
  EXPECT_LT(baseline, 100u)
      << "without orders the stance should be rare (action-Hold only)";
}

// ---------------------------------------------------------------------------
// The config parser.

TEST(CombatConfigTest, ParsesShippedFileAndRejectsBadValues) {
  std::ifstream file(std::string(TEST_DATA_DIR) + "/combat.json");
  std::stringstream buffer;
  buffer << file.rdbuf();
  const sim::CombatConfig shipped = sim::parse_combat(buffer.str());
  EXPECT_DOUBLE_EQ(shipped.facing.flank_multiplier, 1.25);
  EXPECT_DOUBLE_EQ(shipped.facing.rear_multiplier, 1.5);
  EXPECT_EQ(shipped.charge.window_ticks, 10u);
  EXPECT_DOUBLE_EQ(shipped.fatigue.speed_factor, 0.7);
  EXPECT_DOUBLE_EQ(shipped.stance.damage_factor, 0.8);
  // Bad values are rejected with a message naming the section.
  bool threw = false;
  try {
    sim::parse_combat(
        R"json({"facing": {"flank_degrees": 130.0, "rear_degrees": 120.0,
                           "flank_multiplier": 1.25,
                           "rear_multiplier": 1.5,
                           "rear_morale_loss": 2.0}})json");
  } catch (const std::invalid_argument& error) {
    threw = true;
    EXPECT_NE(std::string(error.what()).find("flank_degrees"),
              std::string::npos);
  }
  EXPECT_TRUE(threw) << "flank beyond rear was accepted";
  threw = false;
  try {
    // Every section is required (like tactics.json); a negative gain is
    // rejected with a message naming the section.
    sim::parse_combat(
        R"json({"facing": {"flank_degrees": 60.0, "rear_degrees": 120.0,
                           "flank_multiplier": 1.25,
                           "rear_multiplier": 1.5,
                           "rear_morale_loss": 2.0},
                "charge": {"min_speed": 4.0, "window_ticks": 10,
                           "min_distance": 20.0, "multiplier": 1.5,
                           "held_reduction": 0.5,
                           "dense_line_radius": 3.0,
                           "dense_line_allies": 3},
                "fatigue": {"move_gain_per_tick": -1.0,
                            "fight_gain_per_attack": 0.02,
                            "recover_per_tick": 0.003,
                            "exhausted_level": 1.0,
                            "speed_factor": 0.7, "damage_factor": 0.85,
                            "accuracy_factor": 0.85},
                "experience": {"gain_per_kill": 0.05,
                               "gain_per_engaged_tick": 0.0005,
                               "accuracy_bonus_max": 0.1,
                               "morale_resist_max": 0.1},
                "aura": {"damage_bonus": 0.05, "morale_reduction": 0.25},
                "stance": {"cohesion_min": 0.6, "damage_factor": 0.8,
                           "morale_resist": 0.2}})json");
  } catch (const std::invalid_argument& error) {
    threw = true;
    EXPECT_NE(std::string(error.what()).find("fatigue"), std::string::npos);
  }
  EXPECT_TRUE(threw) << "a negative gain was accepted";
}
