// M15 S1 invariant checker tests: the checker-on-by-default hook, clean
// healthy battles (with the checker running every tick), one
// corrupt-and-catch test per invariant family, the mid-battle freeze,
// and interval independence of the battle bytes.
#include "sim/config.hpp"
#include "sim/invariants.hpp"
#include "sim/simulation.hpp"
#include "sim/squads.hpp"
#include "sim/unit_types.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

constexpr std::size_t kRed = static_cast<std::size_t>(sim::Faction::Red);
constexpr std::size_t kBlue = static_cast<std::size_t>(sim::Faction::Blue);

// Tests build with the checker on by default (M15 S1): this initializer
// arms it for every Simulation constructed anywhere in the suite. The
// interval 30 keeps perf-scale tests cheap while still validating every
// battle; the whole existing suite passing unchanged under it is the
// byte-identity evidence that the checks only read state.
const bool kInvariantsDefaulted = [] {
  sim::Simulation::set_default_invariant_checks(30);
  return true;
}();

sim::UnitStats infantry_stats() {
  sim::UnitStats stats;
  stats.max_health = 100.0;
  stats.speed = 2.0;
  stats.damage = 12.0;
  stats.attack_range = 2.0;
  stats.attack_cooldown_ticks = 5;
  stats.sight_radius = 40.0;
  return stats;
}

sim::Unit make_unit(std::uint32_t id, sim::Faction faction, double x,
                    double y, double health = 100.0) {
  sim::Unit unit;
  unit.id = id;
  unit.faction = faction;
  unit.x = x;
  unit.y = y;
  unit.health = health;
  unit.stats = infantry_stats();
  unit.alive = true;
  unit.morale = 50.0;  // Steady; explicit states keep what tests set
  return unit;
}

// The type table for the plain battle (function-local static: the
// simulation borrows the pointer, so it must outlive every use).
const sim::UnitTypeTable& infantry_table() {
  static const sim::UnitTypeTable table = sim::parse_unit_types(R"json([
    {"id": "infantry", "health": 100.0, "damage": 12.0, "speed": 2.0,
     "range": 2.0, "cooldown": 5, "sight_radius": 40.0}
  ])json");
  return table;
}

// A small utility-mode battle (the mode the grid, squads and orders
// machinery runs in), two factions of two units placed apart.
sim::SimulationConfig battle_config() {
  sim::SimulationConfig config;
  config.seed = 42;
  config.max_ticks = 500;
  config.battlefield_width = 200.0;
  config.battlefield_height = 200.0;
  config.ai_mode = sim::AiMode::Utility;
  config.unit_types = &infantry_table();
  config.morale_enabled = true;
  const auto infantry =
      static_cast<std::uint32_t>(infantry_table().index_of("infantry"));
  config.composition[kRed].push_back(sim::TypeCount{infantry, 2});
  config.composition[kBlue].push_back(sim::TypeCount{infantry, 2});
  return config;
}

// The config for explicit-initial-state battles; the units go to the
// Simulation constructor.
sim::SimulationConfig explicit_state_config() {
  sim::SimulationConfig config;
  config.seed = 7;
  config.max_ticks = 300;
  config.battlefield_width = 200.0;
  config.battlefield_height = 200.0;
  config.ai_mode = sim::AiMode::Utility;
  config.morale_enabled = true;
  return config;
}

// Does the returned violation list contain the named invariant?
bool has_invariant(const std::vector<sim::InvariantViolation>& violations,
                   const char* name) {
  for (const sim::InvariantViolation& violation : violations) {
    if (std::string(violation.invariant) == name) {
      return true;
    }
  }
  return false;
}

}  // namespace

// The default hook above armed the checker for every test simulation.
TEST(InvariantsTest, CheckerOnByDefaultInTests) {
  ASSERT_TRUE(kInvariantsDefaulted);
  sim::Simulation simulation(battle_config());
  EXPECT_TRUE(simulation.invariant_checks_on());
  EXPECT_FALSE(simulation.invariant_failed());
  EXPECT_TRUE(simulation.invariant_violations().empty());
}

TEST(InvariantsTest, EnableRejectsZeroInterval) {
  sim::Simulation simulation(battle_config());
  EXPECT_THROW(simulation.enable_invariant_checks(0), std::invalid_argument);
}

// A healthy battle stays clean at every sampled tick, through
// termination, and on a fresh one-shot check afterwards.
TEST(InvariantsTest, HealthyBattleStaysClean) {
  sim::Simulation simulation(battle_config());
  const sim::BattleResult result = simulation.run();
  EXPECT_TRUE(result.finished);
  EXPECT_FALSE(simulation.invariant_failed())
      << "first violation: "
      << (simulation.invariant_violations().empty()
              ? std::string("(none)")
              : simulation.invariant_violations().front().invariant);
  EXPECT_TRUE(simulation.invariant_violations().empty());
  EXPECT_TRUE(simulation.check_invariants().empty());
}

// The same battle with the checker at different intervals (every tick
// vs every 7 vs the default 30) produces identical state hashes: the
// checks only read state.
TEST(InvariantsTest, IntervalDoesNotChangeTheBattle) {
  std::string hash_every_tick;
  std::string hash_every_seven;
  {
    sim::Simulation simulation(battle_config());
    simulation.enable_invariant_checks(1);
    for (int i = 0; i < 60; ++i) {
      simulation.tick();
    }
    hash_every_tick = simulation.state_hash();
    EXPECT_FALSE(simulation.invariant_failed());
  }
  {
    sim::Simulation simulation(battle_config());
    simulation.enable_invariant_checks(7);
    for (int i = 0; i < 60; ++i) {
      simulation.tick();
    }
    hash_every_seven = simulation.state_hash();
    EXPECT_FALSE(simulation.invariant_failed());
  }
  EXPECT_EQ(hash_every_tick, hash_every_seven);
}

// A corrupted alive position without a grid update desyncs the grid
// (the classic bug class the checker exists for).
TEST(InvariantsTest, GridDesyncIsCaught) {
  sim::Simulation simulation(battle_config());
  for (int i = 0; i < 5; ++i) {
    simulation.tick();
  }
  const double x = simulation.world().units[0].x;
  const double y = simulation.world().units[0].y;
  sim::InvariantCorruption::position(simulation, 0, x + 200.0, y);
  EXPECT_TRUE(has_invariant(simulation.check_invariants(), "grid-contents"));
}

// Position bounds are deliberately NOT checked yet (see the milestone
// finding: the unclamped move paths make out-of-bounds a legitimate
// transient state); the finite check above covers corruption to NaN
// and infinities.

TEST(InvariantsTest, NonFinitePositionIsCaught) {
  sim::Simulation simulation(battle_config());
  sim::InvariantCorruption::position(simulation, 0, std::nan(""), 100.0);
  EXPECT_TRUE(has_invariant(simulation.check_invariants(), "position-finite"));
}

TEST(InvariantsTest, ZeroIdIsCaught) {
  sim::Simulation simulation(battle_config());
  sim::InvariantCorruption::unit_id(simulation, 0, 0);
  EXPECT_TRUE(has_invariant(simulation.check_invariants(), "id-nonzero"));
}

TEST(InvariantsTest, DuplicateIdIsCaught) {
  sim::Simulation simulation(battle_config());
  const std::uint32_t other = simulation.world().units[1].id;
  sim::InvariantCorruption::unit_id(simulation, 0, other);
  EXPECT_TRUE(has_invariant(simulation.check_invariants(), "id-unique"));
}

TEST(InvariantsTest, AliveHealthAboveMaxIsCaught) {
  sim::Simulation simulation(battle_config());
  sim::InvariantCorruption::health(simulation, 0, 500.0);
  EXPECT_TRUE(has_invariant(simulation.check_invariants(), "health-range"));
}

TEST(InvariantsTest, AliveHealthAtZeroIsCaught) {
  // An explicitly constructed state: revive a dead unit at health 0.
  const std::vector<sim::Unit> units{
      make_unit(1, sim::Faction::Red, 50.0, 50.0),
      make_unit(2, sim::Faction::Red, 60.0, 50.0),
      make_unit(3, sim::Faction::Blue, 150.0, 150.0),
      make_unit(4, sim::Faction::Blue, 160.0, 150.0)};
  sim::Simulation simulation(explicit_state_config(), units);
  // Kill unit 0 through the supported intervention, let the death land.
  ASSERT_TRUE(simulation.intervene_kill(units[0].id));
  simulation.tick();
  ASSERT_FALSE(simulation.world().units[0].alive);
  sim::InvariantCorruption::revive(simulation, 0);
  EXPECT_TRUE(has_invariant(simulation.check_invariants(), "health-range"));
}

TEST(InvariantsTest, DeadHealthNonZeroIsCaught) {
  const std::vector<sim::Unit> units{
      make_unit(1, sim::Faction::Red, 50.0, 50.0),
      make_unit(2, sim::Faction::Red, 60.0, 50.0),
      make_unit(3, sim::Faction::Blue, 150.0, 150.0),
      make_unit(4, sim::Faction::Blue, 160.0, 150.0)};
  sim::Simulation simulation(explicit_state_config(), units);
  ASSERT_TRUE(simulation.intervene_kill(1));
  simulation.tick();
  ASSERT_FALSE(simulation.world().units[0].alive);
  sim::InvariantCorruption::health(simulation, 0, 30.0);
  EXPECT_TRUE(has_invariant(simulation.check_invariants(), "health-dead"));
}

TEST(InvariantsTest, NonPositiveMaxHealthIsCaught) {
  sim::Simulation simulation(battle_config());
  sim::InvariantCorruption::max_health(simulation, 0, 0.0);
  EXPECT_TRUE(has_invariant(simulation.check_invariants(), "health-max"));
}

TEST(InvariantsTest, MoraleOutOfRangeIsCaught) {
  sim::Simulation simulation(battle_config());
  sim::InvariantCorruption::morale(simulation, 0, 150.0);
  EXPECT_TRUE(has_invariant(simulation.check_invariants(), "morale-range"));
}

TEST(InvariantsTest, FatigueOutOfRangeIsCaught) {
  sim::Simulation simulation(battle_config());
  sim::InvariantCorruption::fatigue(simulation, 0, 2.0);
  EXPECT_TRUE(has_invariant(simulation.check_invariants(), "fatigue-range"));
}

TEST(InvariantsTest, ExperienceOutOfRangeIsCaught) {
  sim::Simulation simulation(battle_config());
  sim::InvariantCorruption::experience(simulation, 0, -0.5);
  EXPECT_TRUE(
      has_invariant(simulation.check_invariants(), "experience-range"));
}

TEST(InvariantsTest, NonFiniteFacingIsCaught) {
  sim::Simulation simulation(battle_config());
  sim::InvariantCorruption::facing(
      simulation, 0, std::numeric_limits<double>::infinity());
  EXPECT_TRUE(has_invariant(simulation.check_invariants(), "facing-finite"));
}

TEST(InvariantsTest, UnfieldedFactionIsCaught) {
  sim::Simulation simulation(battle_config());
  sim::InvariantCorruption::faction(simulation, 0, sim::Faction::Green);
  EXPECT_TRUE(has_invariant(simulation.check_invariants(), "faction-valid"));
}

TEST(InvariantsTest, DeadUnitWithTargetIsCaught) {
  const std::vector<sim::Unit> units{
      make_unit(1, sim::Faction::Red, 50.0, 50.0),
      make_unit(2, sim::Faction::Red, 60.0, 50.0),
      make_unit(3, sim::Faction::Blue, 150.0, 150.0),
      make_unit(4, sim::Faction::Blue, 160.0, 150.0)};
  sim::Simulation simulation(explicit_state_config(), units);
  // Interval 1: the checker snapshots the dead set at every tick start,
  // so after the death lands and one more tick runs, the dead unit is
  // in the snapshot and must be clean (the one-tick grace is over).
  simulation.enable_invariant_checks(1);
  ASSERT_TRUE(simulation.intervene_kill(1));
  simulation.tick();
  simulation.tick();
  ASSERT_FALSE(simulation.world().units[0].alive);
  sim::InvariantCorruption::target_of(simulation, 0, 2);
  EXPECT_TRUE(has_invariant(simulation.check_invariants(), "dead-no-target"));
}

TEST(InvariantsTest, AliveUnitTargetingDeadIsCaught) {
  const std::vector<sim::Unit> units{
      make_unit(1, sim::Faction::Red, 50.0, 50.0),
      make_unit(2, sim::Faction::Red, 60.0, 50.0),
      make_unit(3, sim::Faction::Blue, 150.0, 150.0),
      make_unit(4, sim::Faction::Blue, 160.0, 150.0)};
  sim::Simulation simulation(explicit_state_config(), units);
  simulation.enable_invariant_checks(1);
  ASSERT_TRUE(simulation.intervene_kill(3));
  simulation.tick();
  simulation.tick();  // the dead unit is now in the tick-start snapshot
  ASSERT_FALSE(simulation.world().units[2].alive);
  sim::InvariantCorruption::target_of(simulation, 0, 2);
  EXPECT_TRUE(has_invariant(simulation.check_invariants(), "target-alive"));
}

TEST(InvariantsTest, AliveUnitTargetingOwnFactionIsCaught) {
  sim::Simulation simulation(battle_config());
  simulation.tick();  // target_index_ is sized by the first decide phase
  sim::InvariantCorruption::target_of(simulation, 0, 1);
  EXPECT_TRUE(has_invariant(simulation.check_invariants(), "target-enemy"));
}

TEST(InvariantsTest, OrderQueueInvariantsAreCaught) {
  sim::Simulation simulation(battle_config());
  sim::PlayerOrder bogus;  // zero-initialized: issue_tick 0
  bogus.faction = sim::Faction::Red;
  bogus.squad_id = 999;  // no squads in this battle
  sim::InvariantCorruption::push_pending_order(simulation, bogus);
  const std::vector<sim::InvariantViolation> violations =
      simulation.check_invariants();
  EXPECT_TRUE(has_invariant(violations, "order-squad"));
  EXPECT_TRUE(has_invariant(violations, "order-ticks"));
}

TEST(InvariantsTest, OrderQueueSortingIsCaught) {
  sim::Simulation simulation(battle_config());
  sim::PlayerOrder late;
  late.issue_tick = 10;
  late.deliver_tick = 50;
  late.faction = sim::Faction::Red;
  sim::PlayerOrder early;
  early.issue_tick = 20;
  early.deliver_tick = 30;
  early.faction = sim::Faction::Red;
  sim::InvariantCorruption::push_pending_order(simulation, late);
  sim::InvariantCorruption::push_pending_order(simulation, early);
  EXPECT_TRUE(has_invariant(simulation.check_invariants(), "order-sorted"));
}

namespace {

// A squads battle (the state the squad and order invariants need):
// one commander type with a leadership radius, two factions.
sim::UnitTypeTable squad_types() {
  return sim::parse_unit_types(R"json([
    {"id": "infantry", "health": 100.0, "damage": 12.0, "speed": 2.0,
     "range": 2.0, "cooldown": 5, "sight_radius": 40.0},
    {"id": "commander", "health": 150.0, "damage": 8.0, "speed": 1.8,
     "range": 2.0, "cooldown": 10, "sight_radius": 60.0,
     "leadership_radius": 120.0}
  ])json");
}

sim::SimulationConfig squads_config(const sim::UnitTypeTable& types) {
  sim::SimulationConfig config;
  config.seed = 42;
  config.max_ticks = 500;
  config.battlefield_width = 400.0;
  config.battlefield_height = 400.0;
  config.ai_mode = sim::AiMode::Utility;
  config.unit_types = &types;
  config.morale_enabled = true;
  const auto infantry = static_cast<std::uint32_t>(types.index_of("infantry"));
  const auto commander =
      static_cast<std::uint32_t>(types.index_of("commander"));
  config.composition[kRed].push_back(sim::TypeCount{commander, 1});
  config.composition[kRed].push_back(sim::TypeCount{infantry, 4});
  config.composition[kBlue].push_back(sim::TypeCount{commander, 1});
  config.composition[kBlue].push_back(sim::TypeCount{infantry, 4});
  config.squad_plans[kRed] = sim::SquadPlan{1, 4};
  config.squad_plans[kBlue] = sim::SquadPlan{1, 4};
  return config;
}

}  // namespace

TEST(InvariantsTest, HealthySquadsBattleStaysClean) {
  const sim::UnitTypeTable types = squad_types();
  sim::Simulation simulation(squads_config(types));
  const sim::BattleResult result = simulation.run();
  EXPECT_TRUE(result.finished);
  EXPECT_FALSE(simulation.invariant_failed())
      << "first violation: "
      << (simulation.invariant_violations().empty()
              ? std::string("(none)")
              : simulation.invariant_violations().front().invariant);
  EXPECT_TRUE(simulation.invariant_violations().empty());
}

TEST(InvariantsTest, UnknownSquadMembershipIsCaught) {
  const sim::UnitTypeTable types = squad_types();
  sim::Simulation simulation(squads_config(types));
  for (int i = 0; i < 3; ++i) {
    simulation.tick();
  }
  ASSERT_TRUE(simulation.squads_active());
  sim::InvariantCorruption::unit_squad(simulation, 1, 99);
  EXPECT_TRUE(has_invariant(simulation.check_invariants(), "squad-exists"));
}

TEST(InvariantsTest, SquadBackReferenceIsCaught) {
  const sim::UnitTypeTable types = squad_types();
  sim::Simulation simulation(squads_config(types));
  for (int i = 0; i < 3; ++i) {
    simulation.tick();
  }
  ASSERT_TRUE(simulation.squads_active());
  sim::InvariantCorruption::squad_member_index(simulation, 1, 0);
  EXPECT_TRUE(has_invariant(simulation.check_invariants(), "squad-backref"));
}

TEST(InvariantsTest, SquadCohesionRangeIsCaught) {
  const sim::UnitTypeTable types = squad_types();
  sim::Simulation simulation(squads_config(types));
  for (int i = 0; i < 3; ++i) {
    simulation.tick();
  }
  ASSERT_TRUE(simulation.squads_active());
  sim::InvariantCorruption::squad_cohesion(simulation, 0, 1.5);
  EXPECT_TRUE(has_invariant(simulation.check_invariants(), "squad-cohesion"));
}

TEST(InvariantsTest, DeadCommanderHoldingCommandIsCaught) {
  const sim::UnitTypeTable types = squad_types();
  sim::Simulation simulation(squads_config(types));
  for (int i = 0; i < 30; ++i) {
    simulation.tick();
  }
  ASSERT_FALSE(simulation.squads().empty());
  const std::uint32_t commander = simulation.squads()[0].commander_index;
  // A battle that ends by rout leaves survivors; make sure this squad's
  // commander is really dead before handing it active command.
  if (simulation.world().units[commander].alive) {
    simulation.slay_unit(commander);
    simulation.tick();  // the death resolves
  }
  ASSERT_FALSE(simulation.world().units[commander].alive);
  sim::InvariantCorruption::squad_commander(simulation, 0, commander, true);
  EXPECT_TRUE(
      has_invariant(simulation.check_invariants(), "squad-commander-alive"));
}

// The first violation freezes the battle: finished() turns true for
// every driver loop, no further tick advances the world, and the
// violation is recorded with its tick.
TEST(InvariantsTest, ViolationFreezesTheBattle) {
  sim::Simulation simulation(battle_config());
  simulation.enable_invariant_checks(1);
  for (int i = 0; i < 10; ++i) {
    simulation.tick();
  }
  const std::uint64_t ticks = simulation.tick_index();
  ASSERT_FALSE(simulation.invariant_failed());
  sim::InvariantCorruption::health(simulation, 0, 999.0);
  simulation.tick();  // the post-tick check fires here
  EXPECT_TRUE(simulation.invariant_failed());
  EXPECT_TRUE(simulation.finished());
  EXPECT_FALSE(simulation.invariant_violations().empty());
  EXPECT_EQ(simulation.invariant_violations().front().tick, ticks + 1);
  EXPECT_EQ(simulation.invariant_violations().front().unit_id,
            simulation.world().units[0].id);
  const std::uint64_t frozen = simulation.tick_index();
  simulation.tick();
  simulation.tick();
  EXPECT_EQ(simulation.tick_index(), frozen);
  // run() on a frozen battle returns immediately.
  const sim::BattleResult result = simulation.run();
  EXPECT_FALSE(result.finished);  // it never terminated legitimately
}

// The state dump names the unit and its key numbers; the global dump
// carries the counts.
TEST(InvariantsTest, StateDumpCarriesTheState) {
  sim::Simulation simulation(battle_config());
  const std::string unit_dump = simulation.invariant_state_dump(0);
  EXPECT_NE(unit_dump.find("unit " +
                           std::to_string(simulation.world().units[0].id)),
            std::string::npos);
  EXPECT_NE(unit_dump.find("health"), std::string::npos);
  const std::string global_dump =
      simulation.invariant_state_dump(sim::InvariantViolation::kNoUnit);
  EXPECT_NE(global_dump.find("tick"), std::string::npos);
  EXPECT_NE(global_dump.find("units"), std::string::npos);
}
