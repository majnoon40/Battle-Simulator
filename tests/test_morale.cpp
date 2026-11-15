// M5 morale and routing tests: config validation, bounded and directed
// morale deltas, personality scaling, break/recover hysteresis, routing
// behavior and FACTION_ROUTED termination, morale-off reproduction of M4,
// legacy indifference, exports and determinism.
#include "sim/ai.hpp"
#include "sim/config.hpp"
#include "sim/json_export.hpp"
#include "sim/morale.hpp"
#include "sim/simulation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <gtest/gtest.h>

#include "allocation_counter.hpp"

namespace {

constexpr std::size_t kRed = static_cast<std::size_t>(sim::Faction::Red);
constexpr std::size_t kBlue = static_cast<std::size_t>(sim::Faction::Blue);

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

sim::Unit make_unit(std::uint32_t id, sim::Faction faction, double x, double y,
                    double health = 100.0) {
  sim::Unit unit;
  unit.id = id;
  unit.faction = faction;
  unit.x = x;
  unit.y = y;
  unit.health = health;
  unit.stats = infantry_stats();
  unit.alive = true;
  // Explicit initial states carry their morale as constructed (the
  // simulation does not re-derive it), so tests must set a sane default.
  unit.morale = 50.0;  // Steady
  return unit;
}

// A utility config with morale on and a small world for explicit-state
// tests.
sim::SimulationConfig morale_config() {
  // M10: M5-era reproduction tests; tactics off keeps the documented
  // behavior and exports unchanged.

  sim::SimulationConfig config;
  config.seed = 42;
  config.max_ticks = 1000;
  config.battlefield_width = 400.0;
  config.battlefield_height = 400.0;
  config.ai_mode = sim::AiMode::Utility;
  config.morale_enabled = true;
  config.red_stats = infantry_stats();
  config.blue_stats = infantry_stats();
  config.tactics_enabled = false;
  return config;
}

// The M4 reference values for seed 42, 500 infantry, utility mode, morale
// off — extracted from the M4-era export (also byte-compared in the
// acceptance run): ticks 3000, winner Blue, survivors 83/114, exact action
// totals.
constexpr std::uint64_t kM4Seed42Ticks = 3000;
constexpr std::uint64_t kM4Seed42RedAttack = 6355;
constexpr std::uint64_t kM4Seed42RedAdvance = 271566;
constexpr std::uint64_t kM4Seed42BlueAttack = 7626;
constexpr std::uint64_t kM4Seed42BlueAdvance = 343610;

std::string valid_morale_json() {
  std::ifstream file(std::string(TEST_DATA_DIR) + "/morale.json");
  EXPECT_TRUE(file.good());
  return std::string(std::istreambuf_iterator<char>(file),
                     std::istreambuf_iterator<char>());
}

}  // namespace

// ---------------------------------------------------------------------------
// Configuration.
// ---------------------------------------------------------------------------

TEST(MoraleConfigTest, ParsesTheShippedFile) {
  const sim::MoraleConfig config = sim::parse_morale(valid_morale_json());
  EXPECT_DOUBLE_EQ(config.start.base, 40.0);
  EXPECT_DOUBLE_EQ(config.states.shaken_min, 12.0);
  EXPECT_DOUBLE_EQ(config.loss.ally_death, 10.0);
  EXPECT_DOUBLE_EQ(config.loss.ally_rout, 3.0);
  EXPECT_DOUBLE_EQ(config.loss.routed_ally_presence_per_tick, 0.3);
  EXPECT_DOUBLE_EQ(config.recovery.morale_threshold, 35.0);
  EXPECT_EQ(config.recovery.safe_ticks, 120u);
  EXPECT_EQ(config.rout_timeout_ticks, 30u);

  // The shipped file matches the built-in default exactly.
  const sim::MoraleConfig& defaults = sim::default_morale_config();
  EXPECT_DOUBLE_EQ(config.loss.ally_death, defaults.loss.ally_death);
  EXPECT_DOUBLE_EQ(config.loss.routed_ally_presence_per_tick,
                   defaults.loss.routed_ally_presence_per_tick);
  EXPECT_DOUBLE_EQ(config.recovery.morale_threshold,
                   defaults.recovery.morale_threshold);
  EXPECT_EQ(config.recovery.safe_ticks, defaults.recovery.safe_ticks);
}

// M6 pre-step 4: the built-in defaults (used when config.morale is nullptr)
// must equal the shipped data/morale.json in EVERY field, so a missing file
// never silently changes battles. Field-by-field, no spot-checking.
TEST(MoraleConfigTest, BuiltInDefaultsMatchShippedFileExactly) {
  const sim::MoraleConfig parsed = sim::parse_morale(valid_morale_json());
  const sim::MoraleConfig& defaults = sim::default_morale_config();
  EXPECT_DOUBLE_EQ(parsed.start.base, defaults.start.base);
  EXPECT_DOUBLE_EQ(parsed.start.bravery_scale, defaults.start.bravery_scale);
  EXPECT_DOUBLE_EQ(parsed.states.confident_min, defaults.states.confident_min);
  EXPECT_DOUBLE_EQ(parsed.states.steady_min, defaults.states.steady_min);
  EXPECT_DOUBLE_EQ(parsed.states.worried_min, defaults.states.worried_min);
  EXPECT_DOUBLE_EQ(parsed.states.shaken_min, defaults.states.shaken_min);
  EXPECT_DOUBLE_EQ(parsed.loss.ally_death, defaults.loss.ally_death);
  EXPECT_DOUBLE_EQ(parsed.loss.ally_rout, defaults.loss.ally_rout);
  EXPECT_DOUBLE_EQ(parsed.loss.routed_ally_presence_per_tick,
                   defaults.loss.routed_ally_presence_per_tick);
  EXPECT_DOUBLE_EQ(parsed.loss.outnumbered_per_tick,
                   defaults.loss.outnumbered_per_tick);
  EXPECT_DOUBLE_EQ(parsed.loss.damage_per_health_fraction,
                   defaults.loss.damage_per_health_fraction);
  EXPECT_DOUBLE_EQ(parsed.loss.isolation_per_tick,
                   defaults.loss.isolation_per_tick);
  EXPECT_DOUBLE_EQ(parsed.loss.caution_amplifier,
                   defaults.loss.caution_amplifier);
  EXPECT_DOUBLE_EQ(parsed.loss.bravery_shield, defaults.loss.bravery_shield);
  EXPECT_DOUBLE_EQ(parsed.loss.min_scale, defaults.loss.min_scale);
  EXPECT_DOUBLE_EQ(parsed.gain.kill, defaults.gain.kill);
  EXPECT_DOUBLE_EQ(parsed.gain.superiority_per_tick,
                   defaults.gain.superiority_per_tick);
  EXPECT_DOUBLE_EQ(parsed.gain.allies_present_per_tick,
                   defaults.gain.allies_present_per_tick);
  EXPECT_DOUBLE_EQ(parsed.gain.safe_per_tick, defaults.gain.safe_per_tick);
  EXPECT_DOUBLE_EQ(parsed.thresholds.outnumbered_below,
                   defaults.thresholds.outnumbered_below);
  EXPECT_DOUBLE_EQ(parsed.thresholds.superiority_above,
                   defaults.thresholds.superiority_above);
  EXPECT_DOUBLE_EQ(parsed.recovery.morale_threshold,
                   defaults.recovery.morale_threshold);
  EXPECT_EQ(parsed.recovery.safe_ticks, defaults.recovery.safe_ticks);
  EXPECT_DOUBLE_EQ(parsed.ai.confident_bonus, defaults.ai.confident_bonus);
  EXPECT_DOUBLE_EQ(parsed.ai.worried_penalty, defaults.ai.worried_penalty);
  EXPECT_DOUBLE_EQ(parsed.ai.shaken_penalty, defaults.ai.shaken_penalty);
  EXPECT_DOUBLE_EQ(parsed.max_loss_per_tick, defaults.max_loss_per_tick);
  EXPECT_DOUBLE_EQ(parsed.max_gain_per_tick, defaults.max_gain_per_tick);
  EXPECT_EQ(parsed.rout_timeout_ticks, defaults.rout_timeout_ticks);
}

TEST(MoraleConfigTest, RejectsInvalidConfigs) {
  // Build variants from the shipped (valid) file by mutation, so every
  // fixture is well-formed JSON except for the field under test.
  const nlohmann::json base = nlohmann::json::parse(valid_morale_json());
  const auto reject = [](const nlohmann::json& variant,
                         const std::string& fragment) {
    try {
      static_cast<void>(sim::parse_morale(variant.dump()));
    } catch (const std::invalid_argument& error) {
      EXPECT_NE(std::string(error.what()).find(fragment), std::string::npos)
          << "error was: " << error.what();
      return;
    }
    FAIL() << "expected rejection containing '" << fragment << "'";
  };

  nlohmann::json missing = base;
  missing["states"].erase("shaken_min");
  reject(missing, "missing required field 'shaken_min'");

  nlohmann::json missing_section = base;
  missing_section.erase("recovery");
  reject(missing_section, "missing required field 'recovery'");

  nlohmann::json unordered = base;
  unordered["states"]["confident_min"] = 50.0;
  unordered["states"]["steady_min"] = 75.0;
  reject(unordered, "shaken_min < worried_min < steady_min < confident_min");

  // Hysteresis: the recovery threshold must be strictly above the breaking
  // value.
  nlohmann::json no_hysteresis = base;
  no_hysteresis["recovery"]["morale_threshold"] = 12.0;
  reject(no_hysteresis, "strictly greater");

  nlohmann::json no_safe_ticks = base;
  no_safe_ticks["recovery"]["safe_ticks"] = 0;
  reject(no_safe_ticks, "safe_ticks' must be >= 1");

  nlohmann::json negative_rate = base;
  negative_rate["loss"]["ally_death"] = -1.0;
  reject(negative_rate, "'loss.ally_death' must be >=");

  nlohmann::json unknown = base;
  unknown["extra"] = 1;
  reject(unknown, "unknown field 'extra'");

  nlohmann::json unknown_trait = base;
  unknown_trait["loss"]["cowardice"] = 1.0;
  reject(unknown_trait, "unknown field 'cowardice'");

  nlohmann::json bad_start = base;
  bad_start["start"]["base"] = 90.0;
  reject(bad_start, "start.base + start.bravery_scale must be <= 100");

  nlohmann::json inverted_thresholds = base;
  inverted_thresholds["thresholds"]["outnumbered_below"] = 0.9;
  reject(inverted_thresholds, "outnumbered_below must be <");
}

// ---------------------------------------------------------------------------
// Delta bounds and directions (pure function).
// ---------------------------------------------------------------------------

TEST(MoraleDeltaTest, BoundedAndFiniteOverRandomInputs) {
  const sim::MoraleConfig& config = sim::default_morale_config();
  sim::Rng rng(7);
  for (int trial = 0; trial < 2000; ++trial) {
    sim::MoraleInputs inputs;
    inputs.enemies_visible = rng.next_uint32(2) == 0;
    inputs.allies_visible = rng.next_uint32(2) == 0;
    inputs.strength_ratio = rng.next_double();
    inputs.damage_fraction = rng.next_double();
    inputs.kills = rng.next_uint32(3);
    inputs.ally_deaths = rng.next_uint32(5);
    inputs.ally_routs = rng.next_uint32(5);
    inputs.routed_allies = rng.next_uint32(5);
    inputs.bravery = rng.next_double();
    inputs.caution = rng.next_double();
    const double delta = sim::morale_delta(inputs, config);
    ASSERT_TRUE(std::isfinite(delta));
    ASSERT_GE(delta, -config.max_loss_per_tick);
    ASSERT_LE(delta, config.max_gain_per_tick);
  }
}

TEST(MoraleDeltaTest, ClampedSequenceStaysInRange) {
  const sim::MoraleConfig& config = sim::default_morale_config();
  sim::Rng rng(11);
  double morale = 50.0;
  for (int tick = 0; tick < 10000; ++tick) {
    sim::MoraleInputs inputs;
    inputs.enemies_visible = rng.next_uint32(2) == 0;
    inputs.allies_visible = rng.next_uint32(2) == 0;
    inputs.strength_ratio = rng.next_double();
    inputs.damage_fraction = rng.next_double() * 0.5;
    inputs.kills = rng.next_uint32(2);
    inputs.ally_deaths = rng.next_uint32(4);
    inputs.bravery = rng.next_double();
    inputs.caution = rng.next_double();
    morale = std::clamp(morale + sim::morale_delta(inputs, config), 0.0,
                        100.0);
    ASSERT_GE(morale, 0.0);
    ASSERT_LE(morale, 100.0);
    ASSERT_TRUE(std::isfinite(morale));
  }
}

TEST(MoraleDeltaTest, EverySourceMovesMoraleInTheDocumentedDirection) {
  const sim::MoraleConfig& config = sim::default_morale_config();
  const auto delta = [&config](sim::MoraleInputs inputs) {
    return sim::morale_delta(inputs, config);
  };

  sim::MoraleInputs neutral;  // safe, no events: +safe_per_tick
  // Ally deaths: more deaths, lower delta.
  sim::MoraleInputs with_death = neutral;
  with_death.ally_deaths = 1;
  EXPECT_LT(delta(with_death), delta(neutral));
  // Ally routs (contagion) lower it too.
  sim::MoraleInputs with_rout = neutral;
  with_rout.ally_routs = 1;
  EXPECT_LT(delta(with_rout), delta(neutral));
  // Routed allies nearby lower it every tick.
  sim::MoraleInputs with_presence = neutral;
  with_presence.routed_allies = 2;
  EXPECT_LT(delta(with_presence), delta(neutral));
  // Damage taken lowers it.
  sim::MoraleInputs with_damage = neutral;
  with_damage.damage_fraction = 0.12;
  EXPECT_LT(delta(with_damage), delta(neutral));
  // Outnumbered (enemies visible, strength below the threshold).
  sim::MoraleInputs mid = neutral;
  mid.enemies_visible = true;
  mid.allies_visible = true;
  mid.strength_ratio = 0.5;
  sim::MoraleInputs outnumbered = mid;
  outnumbered.strength_ratio = 0.3;  // < outnumbered_below (0.4)
  EXPECT_LT(delta(outnumbered), delta(mid));
  // Isolation: enemies visible but no allies.
  sim::MoraleInputs isolated = mid;
  isolated.allies_visible = false;
  EXPECT_LT(delta(isolated), delta(mid));
  // Kills raise it.
  sim::MoraleInputs with_kill = mid;
  with_kill.kills = 1;
  EXPECT_GT(delta(with_kill), delta(mid));
  // Superiority (strength above the threshold) raises it.
  sim::MoraleInputs superior = mid;
  superior.strength_ratio = 0.9;  // > superiority_above (0.75)
  EXPECT_GT(delta(superior), delta(mid));
  // Allies present raise it (isolation is the negative counterpart).
  sim::MoraleInputs no_allies = mid;
  no_allies.allies_visible = false;
  EXPECT_GT(delta(mid), delta(no_allies));
  // Safe time (no enemies visible) is a gain.
  EXPECT_GT(delta(neutral), 0.0);
}

TEST(MoraleDeltaTest, BraveryShieldsAndCautionAmplifies) {
  const sim::MoraleConfig& config = sim::default_morale_config();
  // One loss source (an ally death), sized so the raw loss stays below the
  // per-tick cap and the personality scaling remains visible.
  sim::MoraleInputs losing = [] {
    sim::MoraleInputs inputs;
    inputs.enemies_visible = true;
    inputs.allies_visible = true;
    inputs.strength_ratio = 0.5;
    inputs.ally_deaths = 1;
    return inputs;
  }();
  sim::MoraleInputs brave = losing;
  brave.bravery = 0.9;
  brave.caution = 0.1;
  sim::MoraleInputs cowardly = losing;
  cowardly.bravery = 0.1;
  cowardly.caution = 0.9;
  EXPECT_GT(sim::morale_delta(brave, config),
            sim::morale_delta(cowardly, config));
  EXPECT_GT(sim::morale_delta(brave, config),
            sim::morale_delta(losing, config));
}

// ---------------------------------------------------------------------------
// States and score modifiers.
// ---------------------------------------------------------------------------

TEST(MoraleStateTest, ThresholdsMapValuesToStates) {
  const sim::MoraleConfig& config = sim::default_morale_config();
  EXPECT_EQ(sim::state_from_morale(90.0, config.states),
            sim::MoraleState::Confident);
  EXPECT_EQ(sim::state_from_morale(75.0, config.states),
            sim::MoraleState::Confident);
  EXPECT_EQ(sim::state_from_morale(74.999, config.states),
            sim::MoraleState::Steady);
  EXPECT_EQ(sim::state_from_morale(50.0, config.states),
            sim::MoraleState::Steady);
  EXPECT_EQ(sim::state_from_morale(30.0, config.states),
            sim::MoraleState::Worried);
  EXPECT_EQ(sim::state_from_morale(12.0, config.states),
            sim::MoraleState::Shaken);
  EXPECT_EQ(sim::state_from_morale(11.999, config.states),
            sim::MoraleState::Broken);
  // Starting morale scales with bravery and stays in range.
  EXPECT_DOUBLE_EQ(sim::starting_morale(0.0, config.start), 40.0);
  EXPECT_DOUBLE_EQ(sim::starting_morale(1.0, config.start), 80.0);
}

TEST(MoraleStateTest, StartingMoraleScalesWithBraveryInBattle) {
  sim::SimulationConfig config = morale_config();
  config.max_ticks = 1;
  sim::Simulation simulation(config);
  double brave_morale = 0.0;
  double coward_morale = 101.0;
  for (const sim::Unit& unit : simulation.world().units) {
    if (unit.personality.bravery >= 0.8) {
      brave_morale = std::max(brave_morale, unit.morale);
    }
    if (unit.personality.bravery <= 0.2) {
      coward_morale = std::min(coward_morale, unit.morale);
    }
  }
  EXPECT_GT(brave_morale, coward_morale);
  for (const sim::Unit& unit : simulation.world().units) {
    EXPECT_GE(unit.morale, 0.0);
    EXPECT_LE(unit.morale, 100.0);
  }
}

TEST(MoraleModifierTest, StatesShiftScoresAsDocumented) {
  const sim::MoraleConfig& config = sim::default_morale_config();
  const auto scored = [] {
    sim::ScoreInputs inputs;
    inputs.health_fraction = 0.8;
    inputs.strength_ratio = 0.6;
    inputs.cooldown_readiness = 1.0;
    inputs.bravery = 0.5;
    inputs.caution = 0.3;
    inputs.aggression = 0.5;
    inputs.has_visible_enemy = true;
    inputs.enemy_in_attack_range = true;
    return sim::score_actions(inputs);
  };
  const sim::ActionScores base = scored();
  const double base_attack =
      base[static_cast<std::size_t>(sim::Action::Attack)];
  const double base_approach =
      base[static_cast<std::size_t>(sim::Action::Approach)];
  const double base_retreat =
      base[static_cast<std::size_t>(sim::Action::Retreat)];

  sim::ActionScores worried = base;
  sim::apply_morale_modifiers(worried, sim::MoraleState::Worried, config.ai);
  EXPECT_LT(worried[static_cast<std::size_t>(sim::Action::Attack)],
            base_attack);
  EXPECT_LT(worried[static_cast<std::size_t>(sim::Action::Approach)],
            base_approach);
  EXPECT_GT(worried[static_cast<std::size_t>(sim::Action::Retreat)],
            base_retreat);

  sim::ActionScores shaken = base;
  sim::apply_morale_modifiers(shaken, sim::MoraleState::Shaken, config.ai);
  EXPECT_LT(shaken[static_cast<std::size_t>(sim::Action::Attack)],
            worried[static_cast<std::size_t>(sim::Action::Attack)]);

  sim::ActionScores confident = base;
  sim::apply_morale_modifiers(confident, sim::MoraleState::Confident,
                              config.ai);
  EXPECT_GT(confident[static_cast<std::size_t>(sim::Action::Attack)],
            base_attack);
  EXPECT_LT(confident[static_cast<std::size_t>(sim::Action::Retreat)],
            base_retreat);

  sim::ActionScores steady = base;
  sim::apply_morale_modifiers(steady, sim::MoraleState::Steady, config.ai);
  EXPECT_DOUBLE_EQ(steady[static_cast<std::size_t>(sim::Action::Attack)],
                   base_attack);
}

// ---------------------------------------------------------------------------
// Routing behavior (explicit initial states).
// ---------------------------------------------------------------------------

TEST(RoutingTest, BrokenUnitNeverAttacks) {
  sim::SimulationConfig config = morale_config();
  std::vector<sim::Unit> units = {
      make_unit(1, sim::Faction::Red, 200.0, 200.0),
      make_unit(2, sim::Faction::Blue, 201.0, 200.0),  // in attack range
  };
  units[0].morale = 5.0;  // below shaken_min: breaks on the first update
  sim::Simulation simulation(config, units);
  static_cast<void>(simulation.run());
  const sim::Unit& red = simulation.world().units[0];
  // Unit 1 broke and never attacked (the Blue unit is untouched) — with
  // speed 2 it also fled some distance.
  EXPECT_TRUE(red.routed);
  EXPECT_EQ(static_cast<sim::Action>(red.last_action), sim::Action::Flee);
  EXPECT_DOUBLE_EQ(simulation.world().units[1].health, 100.0);
  EXPECT_LT(red.x, 200.0);  // fled toward Red's own side (x = 0)
}

TEST(RoutingTest, RoutedUnitFleesAtFullSpeedTowardOwnSide) {
  sim::SimulationConfig config = morale_config();
  std::vector<sim::Unit> units = {
      make_unit(1, sim::Faction::Red, 200.0, 200.0),
      make_unit(2, sim::Faction::Blue, 210.0, 200.0),  // visible enemy
  };
  units[0].morale = 5.0;
  units[0].routed = true;  // already routed at construction
  sim::Simulation simulation(config, units);
  simulation.tick();
  const sim::Unit& red = simulation.world().units[0];
  EXPECT_EQ(static_cast<sim::Action>(red.last_action), sim::Action::Flee);
  EXPECT_NEAR(red.x, 198.0, 1e-9);  // full speed (2.0) toward x = 0
  EXPECT_DOUBLE_EQ(red.y, 200.0);
}

TEST(RoutingTest, RecoveryNeedsSafeTimeAndThreshold) {
  const sim::MoraleConfig& morale = sim::default_morale_config();
  // The routed unit needs a same-faction active unit to keep the battle
  // alive: a faction with no active units ends by FACTION_ROUTED after 30
  // ticks, long before the 120-tick recovery.
  const auto run = [&](double preset_morale, int ticks, bool enemy_visible) {
    sim::SimulationConfig config = morale_config();
    config.max_ticks = static_cast<std::uint64_t>(ticks) + 5;
    // The routed unit flees down the y = 350 lane; the Blue unit's advance
    // path ends at its objective (30, 200), far outside the 40-unit sight
    // of that lane, so safe time keeps accruing once the enemy is away.
    std::vector<sim::Unit> units = {
        make_unit(1, sim::Faction::Red, 100.0, 350.0),
        make_unit(2, sim::Faction::Red, 60.0, 60.0),  // active, far away
        make_unit(3, sim::Faction::Blue,
                  enemy_visible ? 110.0 : 380.0, enemy_visible ? 350.0 : 380.0),
    };
    units[0].morale = preset_morale;
    units[0].routed = true;
    sim::Simulation simulation(config, units);
    for (int i = 0; i < ticks; ++i) {
      simulation.tick();
    }
    return simulation.world().units[0].routed;
  };
  const int need = static_cast<int>(morale.recovery.safe_ticks);
  // Above the threshold and unthreatened: recovers exactly after the safe
  // time, not one tick earlier.
  EXPECT_TRUE(run(50.0, need - 1, false));
  EXPECT_FALSE(run(50.0, need, false));
  // Threatened: safe time never accrues, so it never recovers.
  EXPECT_TRUE(run(50.0, need * 2, true));

  // The threshold itself, isolated with a custom config whose safe gain is
  // too slow to lift morale past the recovery threshold within the safe
  // time: safe time is served, the threshold is not, so no recovery.
  sim::MoraleConfig slow = morale;
  slow.gain.safe_per_tick = 0.05;
  slow.recovery.safe_ticks = 50;
  slow.recovery.morale_threshold = 60.0;
  const auto run_slow = [&](int ticks) {
    sim::SimulationConfig config = morale_config();
    config.max_ticks = static_cast<std::uint64_t>(ticks) + 5;
    config.morale = &slow;
    std::vector<sim::Unit> units = {
        make_unit(1, sim::Faction::Red, 100.0, 350.0),
        make_unit(2, sim::Faction::Red, 60.0, 60.0),
        make_unit(3, sim::Faction::Blue, 380.0, 380.0),
    };
    units[0].morale = 40.0;
    units[0].routed = true;
    sim::Simulation simulation(config, units);
    for (int i = 0; i < ticks; ++i) {
      simulation.tick();
    }
    return simulation.world().units[0].routed;
  };
  EXPECT_TRUE(run_slow(50));     // safe time served, morale still below 60
  EXPECT_FALSE(run_slow(410));   // +0.05/tick crossed 60 (strictly) by now
}

TEST(RoutingTest, RoutedTransitionsStayBounded) {
  // Standard battle (500 infantry, seed 42, morale on): routed-flag changes
  // are bounded — hysteresis (break at 12, recover above 35 after 120 safe
  // ticks) makes flip-flopping impossible in practice.
  sim::SimulationConfig config;
  config.seed = 42;
  config.total_units = 500;
  config.max_ticks = 600;
  config.ai_mode = sim::AiMode::Utility;
  config.morale_enabled = true;
  config.red_stats = infantry_stats();
  config.blue_stats = infantry_stats();
  sim::Simulation simulation(config);
  std::uint64_t transitions = 0;
  std::uint64_t ticks = 0;
  std::vector<bool> previous(simulation.world().units.size(), false);
  while (!simulation.finished()) {
    simulation.tick();
    ++ticks;
    for (std::size_t i = 0; i < simulation.world().units.size(); ++i) {
      const bool routed = simulation.world().units[i].routed;
      if (routed != previous[i]) {
        ++transitions;
        previous[i] = routed;
      }
    }
  }
  ASSERT_GT(ticks, 0u);
  const double per_unit_per_100 =
      static_cast<double>(transitions) /
      (static_cast<double>(simulation.world().units.size()) *
       static_cast<double>(ticks) / 100.0);
  std::printf("[info] standard battle, seed 42: %.3f routed-state changes "
              "per unit per 100 ticks (bound: 3)\n",
              per_unit_per_100);
  EXPECT_LT(per_unit_per_100, 3.0);
}

TEST(RoutingTest, FactionRoutedEndsTheBattle) {
  // A faction with only routed units loses after the rout timeout, even
  // though its routed units are alive. The routed Blue unit sits far off
  // Red's advance path (Red heads for the Blue band center near y = 200),
  // so nobody runs it down first.
  sim::SimulationConfig config = morale_config();
  config.max_ticks = 500;
  std::vector<sim::Unit> units = {
      make_unit(1, sim::Faction::Red, 60.0, 200.0),
      make_unit(2, sim::Faction::Red, 65.0, 205.0),
      make_unit(3, sim::Faction::Blue, 390.0, 390.0),
  };
  units[2].routed = true;
  units[2].morale = 5.0;
  sim::Simulation simulation(config, units);
  static_cast<void>(simulation.run());
  EXPECT_TRUE(simulation.finished());
  EXPECT_EQ(simulation.result().reason, sim::TerminationReason::FactionRouted);
  EXPECT_TRUE(simulation.result().has_winner);
  EXPECT_EQ(simulation.result().winner, sim::Faction::Red);
  EXPECT_GT(simulation.result().factions[kBlue].survivors, 0u);  // alive...
  EXPECT_EQ(simulation.result().factions[kBlue].active, 0u);     // ...not active
}

TEST(RoutingTest, FactionDoesNotRoutWhileActiveUnitsRemain) {
  // Blue has one active unit besides two routed ones: no FACTION_ROUTED.
  sim::SimulationConfig config = morale_config();
  config.max_ticks = 500;
  std::vector<sim::Unit> units = {
      make_unit(1, sim::Faction::Red, 200.0, 200.0),
      make_unit(2, sim::Faction::Red, 205.0, 205.0),
      make_unit(3, sim::Faction::Blue, 300.0, 200.0),
      make_unit(4, sim::Faction::Blue, 350.0, 250.0),
      make_unit(5, sim::Faction::Blue, 330.0, 150.0),
  };
  units[3].routed = true;
  units[3].morale = 5.0;
  units[4].routed = true;
  units[4].morale = 5.0;
  sim::Simulation simulation(config, units);
  for (int i = 0; i < 60 && !simulation.finished(); ++i) {
    simulation.tick();
  }
  // Unit 3 is still active: the battle must not have ended by routing (the
  // tick limit is far away).
  EXPECT_FALSE(simulation.finished());
}

TEST(RoutingTest, MismatchProducesRoutNotAnnihilation) {
  // 3:1 mismatch, a seed from the acceptance sweep: the smaller army is
  // routed out, not eliminated.
  const sim::UnitTypeTable types = sim::parse_unit_types(R"json([
    {"id": "infantry", "health": 100.0, "damage": 12.0, "speed": 2.0, "range": 2.0, "cooldown": 5, "sight_radius": 40.0}
  ])json");
  sim::SimulationConfig config;
  config.seed = 1;
  config.max_ticks = 3000;
  config.unit_types = &types;
  config.ai_mode = sim::AiMode::Utility;
  config.morale_enabled = true;
  config.tactics_enabled = false;  // M10: M5-era reproduction test
  config.composition[kRed] = {{0, 375}};
  config.composition[kBlue] = {{0, 125}};
  sim::Simulation simulation(config);
  const sim::BattleResult result = simulation.run();
  EXPECT_EQ(result.reason, sim::TerminationReason::FactionRouted);
  EXPECT_TRUE(result.has_winner);
  EXPECT_EQ(result.winner, sim::Faction::Red);
  // The smaller army was routed, not wiped out.
  EXPECT_GT(result.factions[kBlue].survivors, 0u);
  EXPECT_GT(result.factions[kBlue].routing.total_routs, 0u);
  EXPECT_GT(result.factions[kBlue].routing.peak_routed, 0u);
}

// ---------------------------------------------------------------------------
// Morale off == M4; legacy ignores morale; determinism.
// ---------------------------------------------------------------------------

TEST(MoraleOffTest, ReproducesM4UtilityResultsExactly) {
  const auto run = [] {
    sim::SimulationConfig config;
    config.seed = 42;
    config.total_units = 500;
    config.max_ticks = 3000;
    config.ai_mode = sim::AiMode::Utility;
    config.morale_enabled = false;  // M4 semantics
    config.tactics_enabled = false;  // M10: M4 reproduction
    config.red_stats = infantry_stats();
    config.blue_stats = infantry_stats();
    sim::Simulation simulation(config);
    return simulation.run();
  };
  const sim::BattleResult result = run();
  EXPECT_EQ(result.ticks_elapsed, kM4Seed42Ticks);
  EXPECT_TRUE(result.has_winner);
  EXPECT_EQ(result.winner, sim::Faction::Blue);
  EXPECT_EQ(result.factions[kRed].survivors, 83u);
  EXPECT_EQ(result.factions[kBlue].survivors, 114u);
  EXPECT_EQ(result.factions[kRed]
                .action_counts[static_cast<std::size_t>(sim::Action::Attack)],
            kM4Seed42RedAttack);
  EXPECT_EQ(result.factions[kRed]
                .action_counts[static_cast<std::size_t>(sim::Action::Advance)],
            kM4Seed42RedAdvance);
  EXPECT_EQ(result.factions[kBlue]
                .action_counts[static_cast<std::size_t>(sim::Action::Attack)],
            kM4Seed42BlueAttack);
  EXPECT_EQ(result.factions[kBlue]
                .action_counts[static_cast<std::size_t>(sim::Action::Advance)],
            kM4Seed42BlueAdvance);
  // No routing happened and the export is the exact v3 shape.
  EXPECT_EQ(result.factions[kRed].routing.total_routs, 0u);
  const std::string json = [] {
    sim::SimulationConfig config;
    config.seed = 42;
    config.total_units = 500;
    config.max_ticks = 3000;
    config.ai_mode = sim::AiMode::Utility;
    config.morale_enabled = false;
    config.tactics_enabled = false;  // M10: M4 reproduction
    config.red_stats = infantry_stats();
    config.blue_stats = infantry_stats();
    sim::Simulation simulation(config);
    static_cast<void>(simulation.run());
    return sim::export_state_json(simulation);
  }();
  const nlohmann::json root = nlohmann::json::parse(json);
  EXPECT_EQ(root["format_version"].get<int>(), 3);
  EXPECT_FALSE(root["units"][0].contains("morale"));
  EXPECT_FALSE(root["units"][0].contains("state"));
  EXPECT_FALSE(root["units"][0].contains("routed"));
  EXPECT_FALSE(root["statistics"]["Red"].contains("routing"));
}

TEST(MoraleOffTest, DoubleRunsRemainByteIdentical) {
  const auto export_run = [] {
    sim::SimulationConfig config;
    config.seed = 42;
    config.total_units = 500;
    config.max_ticks = 600;
    config.ai_mode = sim::AiMode::Utility;
    config.morale_enabled = false;
    config.tactics_enabled = false;  // M10: M4 reproduction
    config.red_stats = infantry_stats();
    config.blue_stats = infantry_stats();
    sim::Simulation simulation(config);
    static_cast<void>(simulation.run());
    return sim::export_state_json(simulation);
  };
  EXPECT_EQ(export_run(), export_run());
}

TEST(MoraleOnTest, DoubleRunsAreByteIdentical) {
  const auto export_run = [] {
    sim::SimulationConfig config;
    config.seed = 42;
    config.total_units = 500;
    config.max_ticks = 600;
    config.ai_mode = sim::AiMode::Utility;
    config.morale_enabled = true;
    config.red_stats = infantry_stats();
    config.blue_stats = infantry_stats();
    sim::Simulation simulation(config);
    static_cast<void>(simulation.run());
    return sim::export_state_json(simulation);
  };
  EXPECT_EQ(export_run(), export_run());
}

TEST(MoraleOnTest, ExportCarriesMoraleStateAndRoutingStats) {
  const sim::UnitTypeTable types = sim::parse_unit_types(R"json([
    {"id": "infantry", "health": 100.0, "damage": 12.0, "speed": 2.0, "range": 2.0, "cooldown": 5, "sight_radius": 40.0}
  ])json");
  sim::SimulationConfig config;
  config.seed = 1;
  config.max_ticks = 3000;
  config.unit_types = &types;
  config.ai_mode = sim::AiMode::Utility;
  config.morale_enabled = true;
  config.tactics_enabled = false;  // M10: M5-era reproduction test
  config.composition[kRed] = {{0, 375}};
  config.composition[kBlue] = {{0, 125}};
  sim::Simulation simulation(config);
  static_cast<void>(simulation.run());
  const std::string json = sim::export_state_json(simulation);
  const nlohmann::json root = nlohmann::json::parse(json);
  EXPECT_EQ(root["format_version"].get<int>(), 4);
  ASSERT_TRUE(root["units"][0].contains("morale"));
  ASSERT_TRUE(root["units"][0].contains("state"));
  ASSERT_TRUE(root["units"][0].contains("routed"));
  const std::string state = root["units"][0]["state"].get<std::string>();
  EXPECT_TRUE(state == "confident" || state == "steady" ||
              state == "worried" || state == "shaken" || state == "broken");
  const nlohmann::json& routing = root["statistics"]["Blue"]["routing"];
  EXPECT_TRUE(routing.contains("first_rout_tick"));
  EXPECT_TRUE(routing.contains("peak_routed"));
  EXPECT_TRUE(routing.contains("total_routs"));
  EXPECT_TRUE(routing.contains("recoveries"));
  EXPECT_GT(routing["total_routs"].get<std::uint64_t>(), 0u);
  // last_action may be "flee" for routed units — check at least one exists.
  bool any_flee = false;
  for (const nlohmann::json& unit : root["units"]) {
    if (unit["last_action"] == "flee") {
      any_flee = true;
      EXPECT_TRUE(unit["routed"].get<bool>());
    }
  }
  EXPECT_TRUE(any_flee);
}

TEST(LegacyMoraleTest, LegacyModeIgnoresMorale) {
  // Legacy + morale on still reproduces the M1 reference (seed 42: Blue,
  // 456 ticks, 0/64 survivors).
  sim::SimulationConfig config;
  config.seed = 42;
  config.total_units = 500;
  config.max_ticks = 3000;
  config.ai_mode = sim::AiMode::Legacy;
  config.morale_enabled = true;  // must be ignored
  config.red_stats = infantry_stats();
  config.blue_stats = infantry_stats();
  sim::Simulation simulation(config);
  const sim::BattleResult result = simulation.run();
  EXPECT_EQ(result.reason, sim::TerminationReason::FactionEliminated);
  EXPECT_TRUE(result.has_winner);
  EXPECT_EQ(result.winner, sim::Faction::Blue);
  EXPECT_EQ(result.ticks_elapsed, 456u);
  EXPECT_EQ(result.factions[kRed].survivors, 0u);
  EXPECT_EQ(result.factions[kBlue].survivors, 64u);
  EXPECT_EQ(result.factions[kRed].routing.total_routs, 0u);
  for (const sim::Unit& unit : simulation.world().units) {
    EXPECT_FALSE(unit.routed);
    EXPECT_DOUBLE_EQ(unit.morale, 0.0);
  }
}

TEST(MoraleDebugTest, CaptureCarriesMoraleFields) {
  sim::SimulationConfig config = morale_config();
  std::vector<sim::Unit> units = {
      make_unit(1, sim::Faction::Red, 200.0, 200.0),
      make_unit(2, sim::Faction::Blue, 210.0, 200.0),
  };
  units[0].morale = 5.0;
  units[0].routed = true;
  sim::Simulation simulation(config, units);
  sim::AiDebugCapture debug;
  debug.unit_id = 1;
  debug.tick = 1;
  debug.explicit_tick = true;
  simulation.set_ai_debug(debug);
  static_cast<void>(simulation.run());
  const sim::AiDebugCapture& captured = simulation.ai_debug();
  ASSERT_TRUE(captured.captured);
  EXPECT_NEAR(captured.morale, 5.0, 1e-9);
  EXPECT_TRUE(captured.routed);
  EXPECT_EQ(captured.chosen, sim::Action::Flee);
}

TEST(MoraleAllocationTest, SteadyStateTickLoopAllocatesNothing) {
  // Static walls with zero damage (no deaths, no routs) under morale-on
  // utility AI: morale updates, perception, scoring and combat all run with
  // zero steady-state allocations.
  const std::size_t* counter = allocation_counter();
  const double world = 1000.0;
  std::vector<sim::Unit> units;
  units.reserve(2000);
  std::uint32_t next_id = 1;
  for (std::uint32_t i = 0; i < 1000; ++i) {
    const double y = static_cast<double>(i) * (world / 1000.0);
    sim::Unit red = make_unit(next_id++, sim::Faction::Red, 496.0, y);
    red.stats.speed = 0.0;
    red.stats.damage = 0.0;
    units.push_back(red);
    sim::Unit blue = make_unit(next_id++, sim::Faction::Blue, 497.5, y);
    blue.stats.speed = 0.0;
    blue.stats.damage = 0.0;
    units.push_back(blue);
  }
  sim::SimulationConfig config = morale_config();
  config.max_ticks = 1000;
  config.battlefield_width = world;
  config.battlefield_height = world;
  sim::Simulation simulation(config, units);
  for (int i = 0; i < 10; ++i) {
    simulation.tick();  // warm-up
  }
  const std::size_t before = *counter;
  for (int i = 0; i < 100; ++i) {
    simulation.tick();
  }
  EXPECT_EQ(*counter, before) << "morale tick loop allocated "
                               << (*counter - before)
                               << " times in steady state";
}
