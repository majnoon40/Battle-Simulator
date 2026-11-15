// M6 — squads, formations, commanders.
//
// Covers the data layer (formations.json / commanders.json parsing and their
// built-in defaults, scenario squad plans, the commander unit type), the
// squad machinery (deterministic assignment covering every unit exactly
// once, formation slot shapes, separation in a crowded squad, order
// propagation and decay, obedience, commander death shock and succession at
// the exact documented tick, routed commanders losing command), the
// behavioral effect of orders (hold keeps squads closer than advance), and
// the reproduction guarantees (--squads off == M5 exactly, no-plan scenarios
// are no-ops, squads-on double runs byte-identical).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "sim/config.hpp"
#include "sim/factions.hpp"
#include "sim/json_export.hpp"
#include "sim/scenario.hpp"
#include "sim/simulation.hpp"
#include "sim/squads.hpp"
#include "sim/unit_types.hpp"

namespace {

constexpr std::size_t kRed = static_cast<std::size_t>(sim::Faction::Red);
constexpr std::size_t kBlue = static_cast<std::size_t>(sim::Faction::Blue);

// The two shipped types these tests need, mirroring data/units.json.
sim::UnitTypeTable squad_types() {
  return sim::parse_unit_types(R"json([
    {"id": "infantry", "health": 100.0, "damage": 12.0, "speed": 2.0,
     "range": 2.0, "cooldown": 5, "sight_radius": 40.0},
    {"id": "commander", "health": 150.0, "damage": 8.0, "speed": 1.8,
     "range": 2.0, "cooldown": 10, "sight_radius": 60.0,
     "leadership_radius": 120.0}
  ])json");
}

std::string read_data_file(const std::string& name) {
  std::ifstream file(std::string(TEST_DATA_DIR) + "/" + name);
  EXPECT_TRUE(file.good());
  return std::string(std::istreambuf_iterator<char>(file),
                     std::istreambuf_iterator<char>());
}

// A squads battle configuration: `red_squads` x `size` infantry plus one
// commander per squad, same for Blue, on the standard battlefield.
// M10: M6-era reproduction tests; tactics off keeps the documented
// behavior and exports unchanged.
sim::SimulationConfig squad_config(const sim::UnitTypeTable& types,
                                   std::uint32_t squads,
                                   std::uint32_t size) {
  sim::SimulationConfig config;
  config.seed = 42;
  config.max_ticks = 3000;
  config.battlefield_width = 1000.0;
  config.battlefield_height = 1000.0;
  config.ai_mode = sim::AiMode::Utility;
  config.unit_types = &types;
  config.morale_enabled = true;
  const int infantry = types.index_of("infantry");
  const int commander = types.index_of("commander");
  config.composition[kRed].push_back(
      sim::TypeCount{static_cast<std::uint32_t>(commander), squads});
  config.composition[kRed].push_back(
      sim::TypeCount{static_cast<std::uint32_t>(infantry), squads * size});
  config.composition[kBlue].push_back(
      sim::TypeCount{static_cast<std::uint32_t>(commander), squads});
  config.composition[kBlue].push_back(
      sim::TypeCount{static_cast<std::uint32_t>(infantry), squads * size});
  config.squad_plans[kRed] = sim::SquadPlan{squads, size};
  config.squad_plans[kBlue] = sim::SquadPlan{squads, size};
  config.tactics_enabled = false;
  return config;
}

// Mean distance of a squad's alive members to their centroid.
double mean_distance_to_centroid(const sim::Simulation& simulation,
                                 const sim::Squad& squad) {
  double sum_x = 0.0;
  double sum_y = 0.0;
  std::size_t alive = 0;
  for (const sim::SquadMemberState& member : squad.members) {
    const sim::Unit& unit =
        simulation.world().units[member.unit_index];
    if (unit.alive) {
      sum_x += unit.x;
      sum_y += unit.y;
      ++alive;
    }
  }
  if (alive == 0) {
    return 0.0;
  }
  const double cx = sum_x / static_cast<double>(alive);
  const double cy = sum_y / static_cast<double>(alive);
  double distance_sum = 0.0;
  for (const sim::SquadMemberState& member : squad.members) {
    const sim::Unit& unit =
        simulation.world().units[member.unit_index];
    if (unit.alive) {
      distance_sum += std::sqrt((unit.x - cx) * (unit.x - cx) +
                                (unit.y - cy) * (unit.y - cy));
    }
  }
  return distance_sum / static_cast<double>(alive);
}

}  // namespace

// ---------------------------------------------------------------------------
// Configuration: formations.json and commanders.json.
// ---------------------------------------------------------------------------

TEST(FormationConfigTest, ParsesTheShippedFileAndMatchesDefaults) {
  const sim::FormationConfig parsed =
      sim::parse_formations(read_data_file("formations.json"));
  const sim::FormationConfig& defaults = sim::default_formation_config();
  ASSERT_EQ(parsed.formations.size(), defaults.formations.size());
  for (std::size_t i = 0; i < parsed.formations.size(); ++i) {
    EXPECT_EQ(parsed.formations[i].id, defaults.formations[i].id);
    EXPECT_EQ(parsed.formations[i].kind, defaults.formations[i].kind);
    EXPECT_DOUBLE_EQ(parsed.formations[i].spacing,
                     defaults.formations[i].spacing);
  }
  EXPECT_EQ(parsed.default_formation, defaults.default_formation);
  EXPECT_DOUBLE_EQ(parsed.steering.separation, defaults.steering.separation);
  EXPECT_DOUBLE_EQ(parsed.steering.cohesion, defaults.steering.cohesion);
  EXPECT_DOUBLE_EQ(parsed.steering.alignment, defaults.steering.alignment);
  EXPECT_DOUBLE_EQ(parsed.steering.slot_seeking,
                   defaults.steering.slot_seeking);
  EXPECT_DOUBLE_EQ(parsed.steering.separation_radius,
                   defaults.steering.separation_radius);
  EXPECT_DOUBLE_EQ(parsed.steering.cohesion_reference,
                   defaults.steering.cohesion_reference);
  // The shipped file defines exactly the three documented shapes and the
  // default names one of them.
  ASSERT_EQ(parsed.formations.size(), 3u);
  EXPECT_EQ(parsed.default_formation, "line");
}

TEST(FormationConfigTest, RejectsInvalidConfigs) {
  const auto reject = [](const std::string& json, const std::string& what) {
    try {
      static_cast<void>(sim::parse_formations(json));
    } catch (const std::invalid_argument& error) {
      EXPECT_NE(std::string(error.what()).find(what), std::string::npos)
          << "error was: " << error.what();
      return;
    }
    FAIL() << "expected rejection containing '" << what << "'";
  };
  reject("[]", "must be a JSON object");
  reject("{}", "missing required field 'formations'");
  reject(R"({"formations": {}, "default_formation": "line",
           "steering": {"separation": 1.5, "cohesion": 0.5, "alignment": 0.3,
           "slot_seeking": 2.0, "separation_radius": 3.0,
           "cohesion_reference": 60.0}})",
         "non-empty object");
  reject(R"({"formations": {"wedge": {"spacing": 3.0}}, "default_formation": "wedge",
           "steering": {"separation": 1.5, "cohesion": 0.5, "alignment": 0.3,
           "slot_seeking": 2.0, "separation_radius": 3.0,
           "cohesion_reference": 60.0}})",
         "unknown formation 'wedge'");
  reject(R"({"formations": {"line": {"spacing": 3.0}}, "default_formation": "column",
           "steering": {"separation": 1.5, "cohesion": 0.5, "alignment": 0.3,
           "slot_seeking": 2.0, "separation_radius": 3.0,
           "cohesion_reference": 60.0}})",
         "is not a defined formation");
  reject(R"({"formations": {"line": {"spacing": 0.0}}, "default_formation": "line",
           "steering": {"separation": 1.5, "cohesion": 0.5, "alignment": 0.3,
           "slot_seeking": 2.0, "separation_radius": 3.0,
           "cohesion_reference": 60.0}})",
         "spacing must be > 0");
  reject(R"({"formations": {"line": {"spacing": 3.0}}, "default_formation": "line",
           "steering": {"separation": -1.0, "cohesion": 0.5, "alignment": 0.3,
           "slot_seeking": 2.0, "separation_radius": 3.0,
           "cohesion_reference": 60.0}})",
         "weights must be >= 0");
  reject(R"({"formations": {"line": {"spacing": 3.0}}, "default_formation": "line",
           "steering": {"separation": 1.5, "cohesion": 0.5, "alignment": 0.3,
           "slot_seeking": 2.0, "separation_radius": 0.0,
           "cohesion_reference": 60.0}})",
         "separation_radius must be > 0");
  reject(R"({"formations": {"line": {"spacing": 3.0}}, "default_formation": "line",
           "extra": 1})",
         "unknown field 'extra'");
}

TEST(CommanderConfigTest, ParsesTheShippedFileAndMatchesDefaults) {
  const sim::CommanderConfig parsed =
      sim::parse_commanders(read_data_file("commanders.json"));
  const sim::CommanderConfig& defaults = sim::default_commander_config();
  EXPECT_DOUBLE_EQ(parsed.rules.retreat_cohesion_below,
                   defaults.rules.retreat_cohesion_below);
  EXPECT_DOUBLE_EQ(parsed.rules.retreat_strength_below,
                   defaults.rules.retreat_strength_below);
  EXPECT_DOUBLE_EQ(parsed.rules.retreat_morale_below,
                   defaults.rules.retreat_morale_below);
  EXPECT_DOUBLE_EQ(parsed.rules.regroup_cohesion_below,
                   defaults.rules.regroup_cohesion_below);
  EXPECT_DOUBLE_EQ(parsed.rules.hold_strength_above,
                   defaults.rules.hold_strength_above);
  EXPECT_DOUBLE_EQ(parsed.order_weights.advance, defaults.order_weights.advance);
  EXPECT_DOUBLE_EQ(parsed.order_weights.hold, defaults.order_weights.hold);
  EXPECT_DOUBLE_EQ(parsed.order_weights.regroup, defaults.order_weights.regroup);
  EXPECT_DOUBLE_EQ(parsed.order_weights.retreat, defaults.order_weights.retreat);
  EXPECT_EQ(parsed.order_min_duration_ticks,
            defaults.order_min_duration_ticks);
  EXPECT_DOUBLE_EQ(parsed.order_delay_per_unit,
                   defaults.order_delay_per_unit);
  EXPECT_EQ(parsed.order_decay_ticks, defaults.order_decay_ticks);
  EXPECT_EQ(parsed.succession_delay_ticks, defaults.succession_delay_ticks);
  EXPECT_DOUBLE_EQ(parsed.commander_death_morale_loss,
                   defaults.commander_death_morale_loss);
  EXPECT_DOUBLE_EQ(parsed.obedience.morale_factor_confident,
                   defaults.obedience.morale_factor_confident);
  EXPECT_DOUBLE_EQ(parsed.obedience.morale_factor_steady,
                   defaults.obedience.morale_factor_steady);
  EXPECT_DOUBLE_EQ(parsed.obedience.morale_factor_worried,
                   defaults.obedience.morale_factor_worried);
  EXPECT_DOUBLE_EQ(parsed.obedience.morale_factor_shaken,
                   defaults.obedience.morale_factor_shaken);
  EXPECT_DOUBLE_EQ(parsed.obedience.cohesion_floor,
                   defaults.obedience.cohesion_floor);
}

TEST(CommanderConfigTest, RejectsInvalidConfigs) {
  const auto reject = [](const std::string& json, const std::string& what) {
    try {
      static_cast<void>(sim::parse_commanders(json));
    } catch (const std::invalid_argument& error) {
      EXPECT_NE(std::string(error.what()).find(what), std::string::npos)
          << "error was: " << error.what();
      return;
    }
    FAIL() << "expected rejection containing '" << what << "'";
  };
  reject("[]", "must be a JSON object");
  reject("{}", "missing required field 'orders'");
  // Valid skeleton to mutate.
  const nlohmann::json base = nlohmann::json::parse(R"json({
    "orders": {
      "retreat": {"cohesion_below": 0.15, "strength_below": 0.3, "morale_below": 15.0},
      "regroup": {"cohesion_below": 0.45},
      "hold": {"strength_above": 0.65}
    },
    "order_weights": {"advance": 0.3, "hold": 0.3, "regroup": 0.3, "retreat": 0.3},
    "order_min_duration_ticks": 40,
    "order_delay_per_unit": 0.02,
    "order_decay_ticks": 60,
    "succession_delay_ticks": 20,
    "commander_death_morale_loss": 18.0,
    "obedience": {
      "morale_factor": {"confident": 1.1, "steady": 1.0, "worried": 0.85, "shaken": 0.6},
      "cohesion_floor": 0.5
    }
  })json");
  const auto reject_variant = [&](nlohmann::json variant,
                                  const std::string& what) {
    reject(variant.dump(), what);
  };
  {
    nlohmann::json variant = base;
    variant["orders"]["skirmish"] = {{"strength_above", 0.5}};
    reject_variant(variant, "unknown order 'skirmish'");
  }
  {
    nlohmann::json variant = base;
    variant["order_weights"]["hold"] = 1.5;
    reject_variant(variant, "order weights must be in [0, 1]");
  }
  {
    nlohmann::json variant = base;
    variant["succession_delay_ticks"] = 0;
    reject_variant(variant, "must be >= 1");
  }
  {
    nlohmann::json variant = base;
    variant["commander_death_morale_loss"] = -1.0;
    reject_variant(variant, "commander_death_morale_loss must be >= 0");
  }
  {
    nlohmann::json variant = base;
    variant["obedience"]["cohesion_floor"] = 2.0;
    reject_variant(variant, "cohesion_floor must be in [0, 1]");
  }
  {
    nlohmann::json variant = base;
    variant["extra"] = 1;
    reject_variant(variant, "unknown field 'extra'");
  }
}

// ---------------------------------------------------------------------------
// Configuration: scenario squad plans and the commander unit type.
// ---------------------------------------------------------------------------

TEST(ScenarioSquadsTest, ParsesSquadPlans) {
  const sim::Scenario scenario = sim::parse_scenario(R"json({
    "battlefield": {"width": 1000.0, "height": 1000.0},
    "spawn_band_fraction": 0.15,
    "armies": {
      "red": {"commander": 2, "infantry": 50},
      "blue": {"commander": 2, "infantry": 50}
    },
    "squads": {"red": {"count": 2, "size": 25}, "blue": {"count": 2}}
  })json");
  ASSERT_TRUE(scenario.squads[kRed].has_value());
  EXPECT_EQ(scenario.squads[kRed]->count, 2u);
  EXPECT_EQ(scenario.squads[kRed]->size, 25u);
  // size omitted -> the documented default 25.
  ASSERT_TRUE(scenario.squads[kBlue].has_value());
  EXPECT_EQ(scenario.squads[kBlue]->count, 2u);
  EXPECT_EQ(scenario.squads[kBlue]->size, 25u);
}

TEST(ScenarioSquadsTest, RejectsInvalidPlans) {
  const auto reject = [](const std::string& squads_json,
                         const std::string& what) {
    const std::string json = R"json({
      "battlefield": {"width": 1000.0, "height": 1000.0},
      "spawn_band_fraction": 0.15,
      "armies": {"red": {"infantry": 10}, "blue": {"infantry": 10}},
      "squads": )json" + squads_json + "}";
    try {
      static_cast<void>(sim::parse_scenario(json));
    } catch (const std::invalid_argument& error) {
      EXPECT_NE(std::string(error.what()).find(what), std::string::npos)
          << "error was: " << error.what();
      return;
    }
    FAIL() << "expected rejection containing '" << what << "'";
  };
  reject(R"({"green": {"count": 1}})", "unknown squads faction 'green'");
  reject(R"({"red": {"size": 25}})", "missing required field 'count'");
  reject(R"({"red": {"count": 0}})", "'count' must be a whole number >= 1");
  reject(R"({"red": {"count": 2, "size": 0}})",
         "'size' must be a whole number >= 1");
  reject(R"({"red": 2})", "must be a JSON object");
  reject(R"([])", "must be a JSON object");
  // A scenario with no squads section is fine (no squads for anyone).
  const sim::Scenario plain = sim::parse_scenario(R"json({
    "battlefield": {"width": 1000.0, "height": 1000.0},
    "spawn_band_fraction": 0.15,
    "armies": {"red": {"infantry": 10}, "blue": {"infantry": 10}}
  })json");
  EXPECT_FALSE(plain.squads[kRed].has_value());
  EXPECT_FALSE(plain.squads[kBlue].has_value());
}

TEST(CommanderTypeTest, LeadershipRadiusIsOptionalButValidated) {
  // Present and positive: parsed.
  const sim::UnitTypeTable table = squad_types();
  ASSERT_EQ(table.size(), 2u);
  EXPECT_DOUBLE_EQ(table.types[1].stats.leadership_radius, 120.0);
  // Absent: not a commander (0).
  EXPECT_DOUBLE_EQ(table.types[0].stats.leadership_radius, 0.0);
  // Present but zero or negative: rejected.
  const auto reject = [](const std::string& json, const std::string& what) {
    try {
      static_cast<void>(sim::parse_unit_types(json));
    } catch (const std::invalid_argument& error) {
      EXPECT_NE(std::string(error.what()).find(what), std::string::npos)
          << "error was: " << error.what();
      return;
    }
    FAIL() << "expected rejection containing '" << what << "'";
  };
  reject(R"([{"id": "x", "health": 1, "damage": 1, "speed": 1, "range": 1,
             "cooldown": 1, "sight_radius": 10, "leadership_radius": 0.0}])",
         "field 'leadership_radius' must be > 0");
  reject(R"([{"id": "x", "health": 1, "damage": 1, "speed": 1, "range": 1,
             "cooldown": 1, "sight_radius": 10, "leadership_radius": -5.0}])",
         "field 'leadership_radius' must be > 0");
}

TEST(SquadConfigValidationTest, PlansMustMatchTheComposition) {
  const sim::UnitTypeTable types = squad_types();
  // Exact match (2 squads of 25 + 2 commanders) is accepted.
  {
    const sim::SimulationConfig config = squad_config(types, 2, 25);
    sim::Simulation simulation(config);  // must not throw
    EXPECT_TRUE(simulation.squads_active());
  }
  // Wrong commander count: rejected.
  {
    sim::SimulationConfig config = squad_config(types, 2, 25);
    config.composition[kRed][0].count = 3;  // 3 commanders, 2 squads
    EXPECT_THROW(static_cast<void>(sim::Simulation(config)),
                 std::invalid_argument);
  }
  // Wrong soldier count: rejected.
  {
    sim::SimulationConfig config = squad_config(types, 2, 25);
    config.composition[kRed][1].count = 49;  // 49 soldiers, 2 x 25 wanted
    EXPECT_THROW(static_cast<void>(sim::Simulation(config)),
                 std::invalid_argument);
  }
  // No commander type at all: rejected (0 commanders for 1 squad).
  {
    sim::SimulationConfig config = squad_config(types, 1, 5);
    config.composition[kRed].clear();
    config.composition[kRed].push_back(sim::TypeCount{0, 5});  // infantry only
    EXPECT_THROW(static_cast<void>(sim::Simulation(config)),
                 std::invalid_argument);
  }
  // Plans with squads disabled are inert: no validation, no squads, and the
  // battle still runs (this is how --squads off reproduces M5).
  {
    sim::SimulationConfig config = squad_config(types, 2, 25);
    config.squads_enabled = false;
    EXPECT_NO_THROW(static_cast<void>(sim::Simulation(config)));
    sim::Simulation simulation(config);
    EXPECT_FALSE(simulation.squads_active());
  }
  // Plans in legacy mode are inert too (legacy reproduces M1-M3).
  {
    sim::SimulationConfig config = squad_config(types, 2, 25);
    config.ai_mode = sim::AiMode::Legacy;
    sim::Simulation simulation(config);
    EXPECT_FALSE(simulation.squads_active());
  }
}

// ---------------------------------------------------------------------------
// Squad assignment: every unit in exactly one squad, deterministic,
// spatially contiguous.
// ---------------------------------------------------------------------------

TEST(SquadAssignmentTest, EveryUnitInExactlyOneSquadDeterministically) {
  const sim::UnitTypeTable types = squad_types();
  const sim::SimulationConfig config = squad_config(types, 4, 10);

  const sim::Simulation a(config);
  const sim::Simulation b(config);
  ASSERT_TRUE(a.squads_active());
  ASSERT_EQ(a.squads().size(), 8u);  // 4 Red + 4 Blue
  EXPECT_EQ(b.squads().size(), 8u);

  // Assignment is deterministic: the same config yields the same squads
  // (ids, factions, formations, member unit indices) every time.
  ASSERT_EQ(a.squads().size(), b.squads().size());
  for (std::size_t s = 0; s < a.squads().size(); ++s) {
    EXPECT_EQ(a.squads()[s].id, b.squads()[s].id);
    EXPECT_EQ(a.squads()[s].faction, b.squads()[s].faction);
    EXPECT_EQ(a.squads()[s].formation, b.squads()[s].formation);
    ASSERT_EQ(a.squads()[s].members.size(), b.squads()[s].members.size());
    for (std::size_t m = 0; m < a.squads()[s].members.size(); ++m) {
      EXPECT_EQ(a.squads()[s].members[m].unit_index,
                b.squads()[s].members[m].unit_index);
    }
  }

  // Every unit is in exactly one squad: each squad membership is claimed by
  // exactly one (squad, member) pair, and the counts cover the whole army.
  std::vector<int> claims(a.world().units.size(), 0);
  std::size_t members_total = 0;
  for (const sim::Squad& squad : a.squads()) {
    // One commander plus `size` soldiers each.
    EXPECT_EQ(squad.members.size(), 11u);
    ++members_total;
    for (const sim::SquadMemberState& member : squad.members) {
      ASSERT_LT(member.unit_index, claims.size());
      ++claims[member.unit_index];
      // squad_of agrees with the membership list.
      EXPECT_EQ(a.squad_of(member.unit_index),
                static_cast<std::int32_t>(squad.id));
    }
    // The commander is member 0 (slot 0: line center / column front) and is
    // a commander-type unit with the squad's leadership radius.
    const sim::Unit& commander =
        a.world().units[squad.commander_index];
    EXPECT_EQ(squad.members[0].unit_index, squad.commander_index);
    EXPECT_GT(commander.stats.leadership_radius, 0.0);
    EXPECT_DOUBLE_EQ(squad.leadership_radius,
                     commander.stats.leadership_radius);
    EXPECT_EQ(commander.faction, squad.faction);
  }
  EXPECT_EQ(members_total, 8u);
  for (std::size_t i = 0; i < claims.size(); ++i) {
    EXPECT_EQ(claims[i], 1) << "unit index " << i << " claimed "
                            << claims[i] << " times";
  }
  EXPECT_EQ(members_total * 11u, a.world().units.size());
}

TEST(SquadAssignmentTest, SquadsAreSpatiallyContiguous) {
  const sim::UnitTypeTable types = squad_types();
  const sim::SimulationConfig config = squad_config(types, 5, 10);
  const sim::Simulation simulation(config);
  ASSERT_TRUE(simulation.squads_active());

  // Within one faction, each squad's soldiers occupy a contiguous band of
  // the spawn order sorted by (y, x, id): no soldier of another squad sits
  // between two of this squad's soldiers in that order.
  for (const sim::Faction faction : sim::kAllFactions) {
    const std::size_t index = static_cast<std::size_t>(faction);
    std::vector<std::uint32_t> soldiers;  // world indices
    for (std::size_t i = 0; i < simulation.world().units.size(); ++i) {
      const sim::Unit& unit = simulation.world().units[i];
      if (unit.faction == faction &&
          unit.stats.leadership_radius <= 0.0) {
        soldiers.push_back(static_cast<std::uint32_t>(i));
      }
    }
    const auto by_position = [&](std::uint32_t p, std::uint32_t q) {
      const sim::Unit& a = simulation.world().units[p];
      const sim::Unit& b = simulation.world().units[q];
      if (a.y != b.y) return a.y < b.y;
      if (a.x != b.x) return a.x < b.x;
      return a.id < b.id;
    };
    std::sort(soldiers.begin(), soldiers.end(), by_position);
    // Map each soldier to its squad and check each squad's soldiers form
    // one contiguous run in this order.
    std::vector<std::int32_t> run_of(soldiers.size(), -1);
    for (std::size_t s = 0; s < simulation.squads().size(); ++s) {
      if (simulation.squads()[s].faction != faction) {
        continue;
      }
      bool seen = false;
      bool closed = false;
      for (std::size_t i = 0; i < soldiers.size(); ++i) {
        const bool mine =
            simulation.squad_of(soldiers[i]) ==
            static_cast<std::int32_t>(simulation.squads()[s].id);
        if (mine) {
          ASSERT_FALSE(closed) << "squad " << s << " is not contiguous";
          seen = true;
          run_of[i] = static_cast<std::int32_t>(s);
        } else if (seen) {
          closed = true;
        }
      }
    }
    (void)index;
  }
}

TEST(SquadAssignmentTest, SquadlessFactionsAndNoPlanScenarios) {
  const sim::UnitTypeTable types = squad_types();
  // Red has squads, Blue does not: Blue units are squadless, Red is covered.
  sim::SimulationConfig config = squad_config(types, 2, 5);
  config.squad_plans[kBlue].reset();
  const sim::Simulation simulation(config);
  ASSERT_TRUE(simulation.squads_active());
  EXPECT_EQ(simulation.squads().size(), 2u);  // Red squads only
  for (const sim::Squad& squad : simulation.squads()) {
    EXPECT_EQ(squad.faction, sim::Faction::Red);
  }
  for (std::size_t i = 0; i < simulation.world().units.size(); ++i) {
    const sim::Unit& unit = simulation.world().units[i];
    if (unit.faction == sim::Faction::Blue) {
      EXPECT_EQ(simulation.squad_of(i), -1);
      EXPECT_EQ(simulation.unit_squad_info(i).squad_id, -1);
    } else {
      EXPECT_GE(simulation.squad_of(i), 0);
    }
  }

  // A scenario without squad plans is a complete no-op even with squads
  // enabled (the default flag): no squads, nothing changes.
  sim::SimulationConfig plain = squad_config(types, 2, 5);
  plain.squad_plans[kRed].reset();
  plain.squad_plans[kBlue].reset();
  const sim::Simulation plain_simulation(plain);
  EXPECT_FALSE(plain_simulation.squads_active());
  EXPECT_TRUE(plain_simulation.squads().empty());
}

// ---------------------------------------------------------------------------
// Formation slots.
// ---------------------------------------------------------------------------

TEST(FormationSlotTest, SlotsDoNotOverlap) {
  // For every kind and a range of member counts, all slots are distinct and
  // no two slots are closer than the formation's spacing.
  for (const sim::FormationKind kind :
       {sim::FormationKind::Line, sim::FormationKind::Column,
        sim::FormationKind::Loose}) {
    for (std::size_t count = 1; count <= 100; ++count) {
      const double spacing = 3.0;
      std::vector<sim::SlotOffset> slots;
      slots.reserve(count);
      for (std::size_t i = 0; i < count; ++i) {
        slots.push_back(sim::formation_slot(kind, count, spacing, i));
      }
      for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t j = i + 1; j < count; ++j) {
          const double dx = slots[i].forward - slots[j].forward;
          const double dy = slots[i].lateral - slots[j].lateral;
          const double distance = std::sqrt(dx * dx + dy * dy);
          EXPECT_GE(distance, spacing - 1e-9)
              << "kind " << static_cast<int>(kind) << " count " << count
              << " slots " << i << " and " << j << " overlap";
        }
      }
    }
  }
}

TEST(FormationSlotTest, CommanderTakesTheCenterOrFront) {
  // Slot 0 (the commander's) is the center of a line, the front of a column
  // and the front rank of a loose block.
  EXPECT_DOUBLE_EQ(
      sim::formation_slot(sim::FormationKind::Line, 7, 3.0, 0).lateral, 0.0);
  EXPECT_DOUBLE_EQ(
      sim::formation_slot(sim::FormationKind::Line, 7, 3.0, 0).forward, 0.0);
  EXPECT_DOUBLE_EQ(
      sim::formation_slot(sim::FormationKind::Column, 7, 3.0, 0).forward,
      0.0);
  EXPECT_DOUBLE_EQ(
      sim::formation_slot(sim::FormationKind::Column, 7, 3.0, 0).lateral,
      0.0);
  EXPECT_DOUBLE_EQ(
      sim::formation_slot(sim::FormationKind::Loose, 9, 3.0, 0).forward,
      0.0);
  // Line fills center-out: +1 right, -1 left, +2 right, ...
  EXPECT_DOUBLE_EQ(
      sim::formation_slot(sim::FormationKind::Line, 5, 3.0, 1).lateral, 3.0);
  EXPECT_DOUBLE_EQ(
      sim::formation_slot(sim::FormationKind::Line, 5, 3.0, 2).lateral,
      -3.0);
  EXPECT_DOUBLE_EQ(
      sim::formation_slot(sim::FormationKind::Line, 5, 3.0, 3).lateral, 6.0);
  // Column files front to back: the commander leads, the squad trails.
  EXPECT_DOUBLE_EQ(
      sim::formation_slot(sim::FormationKind::Column, 5, 3.0, 4).forward,
      -12.0);
}

TEST(SquadSeparationTest, SeparationKeepsMinimumDistanceInCrowd) {
  // A whole squad spawns piled onto one point and stays there (its objective
  // is its own position, so nothing marches): the members assemble into
  // their formation and separation keeps them apart.
  const sim::UnitTypeTable types = squad_types();
  sim::SimulationConfig config = squad_config(types, 1, 15);
  config.max_ticks = 400;
  config.morale_enabled = false;
  // Objectives at the squads' own positions: nothing marches anywhere.
  config.objectives[kRed] = sim::ObjectivePoint{400.0, 500.0, true};
  config.objectives[kBlue] = sim::ObjectivePoint{990.0, 990.0, true};

  // Explicit state: the Red squad clustered at one point, Blue elsewhere.
  std::vector<sim::Unit> units;
  const int commander = types.index_of("commander");
  const int infantry = types.index_of("infantry");
  sim::Unit unit;
  unit.id = 1;
  unit.faction = sim::Faction::Red;
  unit.type_index = static_cast<std::uint32_t>(commander);
  unit.stats = types.types[commander].stats;
  unit.x = 400.0;
  unit.y = 500.0;
  unit.health = unit.stats.max_health;
  unit.alive = true;
  units.push_back(unit);
  for (std::uint32_t i = 0; i < 15; ++i) {
    sim::Unit soldier;
    soldier.id = 2 + i;
    soldier.faction = sim::Faction::Red;
    soldier.type_index = static_cast<std::uint32_t>(infantry);
    soldier.stats = types.types[infantry].stats;
    soldier.x = 400.0 + 0.001 * static_cast<double>(i);  // near-pile
    soldier.y = 500.0;
    soldier.health = soldier.stats.max_health;
    soldier.alive = true;
    units.push_back(soldier);
  }
  for (std::uint32_t i = 0; i < 16; ++i) {
    sim::Unit blue;
    blue.id = 100 + i;
    blue.faction = sim::Faction::Blue;
    blue.type_index = static_cast<std::uint32_t>(i == 0 ? commander : infantry);
    blue.stats =
        types.types[i == 0 ? commander : infantry].stats;
    blue.x = 990.0;
    blue.y = 990.0;
    blue.health = blue.stats.max_health;
    blue.alive = true;
    units.push_back(blue);
  }
  sim::Simulation simulation(config, units);
  ASSERT_TRUE(simulation.squads_active());
  ASSERT_EQ(simulation.squads().size(), 2u);
  const sim::Squad& red_squad =
      simulation.squads()[0];  // Red squads come first

  // At spawn everyone is piled up: the minimum pairwise distance is ~0.
  const auto min_pairwise = [&]() {
    double best = 1e9;
    for (const sim::SquadMemberState& a : red_squad.members) {
      for (const sim::SquadMemberState& b : red_squad.members) {
        if (a.unit_index == b.unit_index) {
          continue;
        }
        const sim::Unit& ua = simulation.world().units[a.unit_index];
        const sim::Unit& ub = simulation.world().units[b.unit_index];
        if (!ua.alive || !ub.alive) {
          continue;
        }
        const double dx = ua.x - ub.x;
        const double dy = ua.y - ub.y;
        best = std::min(best, std::sqrt(dx * dx + dy * dy));
      }
    }
    return best;
  };
  EXPECT_LT(min_pairwise(), 0.5);

  for (int i = 0; i < 300; ++i) {
    simulation.tick();
  }
  // After assembly, separation keeps every pair further apart than half the
  // separation radius (the squad may still be settling, but nobody may be
  // piled on anybody).
  EXPECT_GT(min_pairwise(), 1.5)
      << "separation failed to keep the crowded squad apart";
  // And the squad actually formed: most members are in formation.
  std::size_t in_formation = 0;
  for (const sim::SquadMemberState& member : red_squad.members) {
    if (member.in_formation) {
      ++in_formation;
    }
  }
  EXPECT_GE(in_formation, 12u);
}
// ---------------------------------------------------------------------------
// Pure decision functions: order rules and obedience.
// ---------------------------------------------------------------------------

TEST(OrderRuleTest, PicksOrdersByDocumentedPriority) {
  const sim::CommanderConfig::Rules& rules =
      sim::default_commander_config().rules;
  // Healthy, cohesive, unseen enemy: advance.
  EXPECT_EQ(sim::pick_order(0.9, 0.9, 80.0, false, rules), sim::Order::Advance);
  // Enemies visible and locally winning: hold.
  EXPECT_EQ(sim::pick_order(0.9, 0.9, 80.0, true, rules), sim::Order::Hold);
  // No enemies visible but strong: still advance (hold needs contact).
  EXPECT_EQ(sim::pick_order(0.5, 0.9, 50.0, false, rules), sim::Order::Advance);
  // Scattered but not collapsing: regroup.
  EXPECT_EQ(sim::pick_order(0.3, 0.9, 80.0, true, rules), sim::Order::Regroup);
  // Low local strength: retreat (cohesion still fine).
  EXPECT_EQ(sim::pick_order(0.9, 0.2, 80.0, true, rules), sim::Order::Retreat);
  // Collapsed morale: retreat.
  EXPECT_EQ(sim::pick_order(0.9, 0.9, 10.0, true, rules), sim::Order::Retreat);
  // Dissolved squad: retreat beats regroup.
  EXPECT_EQ(sim::pick_order(0.05, 0.9, 80.0, true, rules), sim::Order::Retreat);
}

TEST(ObedienceTest, FallsWithDistanceAndRisesWithDiscipline) {
  const sim::CommanderConfig& config = sim::default_commander_config();
  // Falls with distance (same member, same state).
  const double near_ =
      sim::obedience_weight(0.8, 10.0, 120.0, sim::MoraleState::Steady,
                            0.9, config);
  const double far_ =
      sim::obedience_weight(0.8, 100.0, 120.0, sim::MoraleState::Steady,
                            0.9, config);
  EXPECT_GT(near_, far_);
  EXPECT_GT(far_, 0.0);
  EXPECT_DOUBLE_EQ(
      sim::obedience_weight(0.8, 120.0, 120.0, sim::MoraleState::Steady,
                            0.9, config),
      0.0);  // at the radius edge: no obedience
  EXPECT_DOUBLE_EQ(
      sim::obedience_weight(0.8, 130.0, 120.0, sim::MoraleState::Steady,
                            0.9, config),
      0.0);  // beyond the radius: none at all
  // Rises with discipline.
  const double undisciplined =
      sim::obedience_weight(0.1, 10.0, 120.0, sim::MoraleState::Steady,
                            0.9, config);
  const double disciplined =
      sim::obedience_weight(1.0, 10.0, 120.0, sim::MoraleState::Steady,
                            0.9, config);
  EXPECT_GT(disciplined, undisciplined);
  // Rises with squad cohesion, bounded by the cohesion floor.
  const double scattered =
      sim::obedience_weight(0.8, 10.0, 120.0, sim::MoraleState::Steady,
                            0.0, config);
  const double together =
      sim::obedience_weight(0.8, 10.0, 120.0, sim::MoraleState::Steady,
                            1.0, config);
  EXPECT_GT(together, scattered);
  EXPECT_DOUBLE_EQ(scattered, 0.8 * (110.0 / 120.0) * 1.0 * 0.5);
  // Morale state: confident obeys more than steady, shaken less, broken
  // never.
  const double confident =
      sim::obedience_weight(0.8, 10.0, 120.0, sim::MoraleState::Confident,
                            0.9, config);
  const double steady =
      sim::obedience_weight(0.8, 10.0, 120.0, sim::MoraleState::Steady,
                            0.9, config);
  const double worried =
      sim::obedience_weight(0.8, 10.0, 120.0, sim::MoraleState::Worried,
                            0.9, config);
  const double shaken =
      sim::obedience_weight(0.8, 10.0, 120.0, sim::MoraleState::Shaken,
                            0.9, config);
  const double broken =
      sim::obedience_weight(0.8, 10.0, 120.0, sim::MoraleState::Broken,
                            0.9, config);
  EXPECT_GT(confident, steady);
  EXPECT_GT(steady, worried);
  EXPECT_GT(worried, shaken);
  EXPECT_GT(shaken, broken);
  EXPECT_DOUBLE_EQ(broken, 0.0);
  // The exact documented formula for one input set:
  // discipline * distance factor * state factor * cohesion factor.
  EXPECT_DOUBLE_EQ(
      steady, 0.8 * (1.0 - 10.0 / 120.0) * 1.0 * (0.5 + 0.5 * 0.9));
  // No commander radius: no obedience.
  EXPECT_DOUBLE_EQ(
      sim::obedience_weight(1.0, 0.0, 0.0, sim::MoraleState::Steady, 1.0,
                            config),
      0.0);
}

TEST(OrderScoreBonusTest, BoostsOnlyTheDocumentedActions) {
  const sim::CommanderConfig::OrderWeights weights{0.3, 0.4, 0.2, 0.5};
  const auto bonus = [](sim::Order order, double obedience,
                        const sim::CommanderConfig::OrderWeights& w) {
    return sim::order_score_bonus(order, obedience, w);
  };
  const std::size_t attack = static_cast<std::size_t>(sim::Action::Attack);
  const std::size_t approach =
      static_cast<std::size_t>(sim::Action::Approach);
  const std::size_t advance =
      static_cast<std::size_t>(sim::Action::Advance);
  const std::size_t retreat =
      static_cast<std::size_t>(sim::Action::Retreat);
  const std::size_t hold = static_cast<std::size_t>(sim::Action::Hold);

  auto advance_bonus = bonus(sim::Order::Advance, 1.0, weights);
  EXPECT_DOUBLE_EQ(advance_bonus[advance], 0.3);
  EXPECT_DOUBLE_EQ(advance_bonus[approach], 0.3);
  EXPECT_DOUBLE_EQ(advance_bonus[hold], 0.0);

  auto hold_bonus = bonus(sim::Order::Hold, 0.5, weights);
  EXPECT_DOUBLE_EQ(hold_bonus[hold], 0.2);  // 0.5 obedience * 0.4 weight
  EXPECT_DOUBLE_EQ(hold_bonus[attack], 0.2);

  auto regroup_bonus = bonus(sim::Order::Regroup, 1.0, weights);
  EXPECT_DOUBLE_EQ(regroup_bonus[hold], 0.2);

  auto retreat_bonus = bonus(sim::Order::Retreat, 1.0, weights);
  EXPECT_DOUBLE_EQ(retreat_bonus[retreat], 0.5);

  auto none_bonus = bonus(sim::Order::None, 1.0, weights);
  for (double value : none_bonus) {
    EXPECT_DOUBLE_EQ(value, 0.0);
  }
}

// ---------------------------------------------------------------------------
// Order propagation: distance-based delay, decay out of range.
// ---------------------------------------------------------------------------

namespace {

// One commander plus one immobile soldier, no enemies: the commander's
// order (advance — no enemies visible, strength 1, morale neutral) reaches
// the soldier after the documented delay. Retreat and regroup are disabled
// so the two-man squad's naturally low cohesion can never change the order.
sim::CommanderConfig propagation_commanders(double delay_per_unit,
                                            std::uint64_t decay_ticks) {
  sim::CommanderConfig config;
  config.order_delay_per_unit = delay_per_unit;
  config.order_decay_ticks = decay_ticks;
  config.rules.retreat_cohesion_below = 0.0;
  config.rules.retreat_strength_below = 0.0;
  config.rules.retreat_morale_below = 0.0;
  config.rules.regroup_cohesion_below = 0.0;
  return config;
}

// Builds a two-faction explicit battle: Red squad (commander + one
// immobile soldier at `soldier_offset` behind it), Blue parked far away.
// Both Red units are essentially immobile unless the test needs the
// commander to march away (speed 2); the soldier is always immobile.
sim::Simulation propagation_battle(const sim::UnitTypeTable& types,
                                   const sim::CommanderConfig& commanders,
                                   double soldier_x,
                                   double commander_speed = 0.001) {
  sim::SimulationConfig config;
  config.seed = 7;
  config.max_ticks = 5000;
  config.battlefield_width = 1000.0;
  config.battlefield_height = 1000.0;
  config.ai_mode = sim::AiMode::Utility;
  config.unit_types = &types;
  config.morale_enabled = false;
  config.commanders = &commanders;
  config.composition[kRed].push_back(sim::TypeCount{1, 1});   // commander
  config.composition[kRed].push_back(sim::TypeCount{0, 1});   // soldier
  config.composition[kBlue].push_back(sim::TypeCount{1, 1});
  config.composition[kBlue].push_back(sim::TypeCount{0, 1});
  config.squad_plans[kRed] = sim::SquadPlan{1, 1};
  config.squad_plans[kBlue] = sim::SquadPlan{1, 1};

  std::vector<sim::Unit> units;
  auto make = [&](std::uint32_t id, sim::Faction faction, int type_index,
                  double x, double y, double speed) {
    sim::Unit unit;
    unit.id = id;
    unit.faction = faction;
    unit.type_index = static_cast<std::uint32_t>(type_index);
    unit.stats = types.types[type_index].stats;
    unit.stats.speed = speed;
    unit.x = x;
    unit.y = y;
    unit.health = unit.stats.max_health;
    unit.alive = true;
    units.push_back(unit);
  };
  make(1, sim::Faction::Red, 1, 500.0, 500.0, commander_speed);
  make(2, sim::Faction::Red, 0, soldier_x, 500.0, 0.001);  // immobile soldier
  make(3, sim::Faction::Blue, 1, 990.0, 990.0, 0.001);
  make(4, sim::Faction::Blue, 0, 991.0, 991.0, 0.001);
  return sim::Simulation(config, units);
}

}  // namespace

TEST(OrderPropagationTest, ReachesMembersAfterTheDistanceDelay) {
  const sim::UnitTypeTable types = squad_types();
  // Delay: 0.5 ticks per world unit. The soldier sits 95 behind the
  // commander (inside the 120 leadership radius): delay = ceil(95*0.5) = 48
  // ticks after the issue tick (tick 1), so the order arrives during tick
  // 49. (95*0.5 = 47.5 stays safely away from an integer boundary, so the
  // ceil cannot flip on floating-point dust.)
  const sim::CommanderConfig commanders =
      propagation_commanders(0.5, 60);
  sim::Simulation simulation =
      propagation_battle(types, commanders, 405.0);
  ASSERT_TRUE(simulation.squads_active());
  const sim::Squad& squad = simulation.squads()[0];
  ASSERT_EQ(squad.faction, sim::Faction::Red);

  // The commander issues its first order (advance: no enemies, strong,
  // cohesive) on tick 1.
  for (int tick = 0; tick < 48; ++tick) {
    simulation.tick();
  }
  EXPECT_EQ(squad.order, sim::Order::Advance);
  // 48 ticks done: the delay (issue tick 1 + 48) has not elapsed yet.
  EXPECT_EQ(simulation.unit_squad_info(1).order, sim::Order::None)
      << "order must not arrive before the documented delay";
  simulation.tick();  // tick 49: 49 >= 1 + 48
  EXPECT_EQ(simulation.unit_squad_info(1).order, sim::Order::Advance)
      << "order must arrive exactly at issue tick + delay";
}

TEST(OrderPropagationTest, NeverReachesMembersOutsideTheRadius) {
  const sim::UnitTypeTable types = squad_types();
  const sim::CommanderConfig commanders =
      propagation_commanders(0.0, 60);  // zero delay: pure range test
  // The soldier sits 300 behind the commander: outside the 120 radius. The
  // order never arrives and nothing ever decays (nothing was received).
  sim::Simulation simulation =
      propagation_battle(types, commanders, 200.0);
  const sim::Squad& squad = simulation.squads()[0];
  for (int tick = 0; tick < 200; ++tick) {
    simulation.tick();
  }
  EXPECT_EQ(squad.order, sim::Order::Advance);
  EXPECT_EQ(simulation.unit_squad_info(1).order, sim::Order::None);
}

TEST(OrderPropagationTest, DecaysAfterLeavingTheRadius) {
  const sim::UnitTypeTable types = squad_types();
  // Short decay so the test runs quickly: 40 ticks out of range suffice.
  const sim::CommanderConfig commanders =
      propagation_commanders(0.0, 40);
  // The soldier starts 100 behind the commander (in range) and cannot move;
  // the commander marches east at speed 2 and leaves the radius behind.
  sim::Simulation simulation =
      propagation_battle(types, commanders, 400.0, 2.0);
  const sim::Squad& squad = simulation.squads()[0];
  for (int tick = 0; tick < 10; ++tick) {
    simulation.tick();
  }
  EXPECT_EQ(simulation.unit_squad_info(1).order, sim::Order::Advance);
  // The commander is now ~420+; the soldier at 400 is still within 120
  // until the commander passes 520, then the decay timer starts. Wait long
  // enough for both (the commander reaches 520 after ~10 more ticks, then
  // 40 decay ticks, then some margin).
  for (int tick = 0; tick < 190; ++tick) {
    simulation.tick();
  }
  EXPECT_GT(simulation.world().units[0].x, 520.0);  // really out of range
  EXPECT_EQ(simulation.unit_squad_info(1).order, sim::Order::None)
      << "the last order must decay after order_decay_ticks out of range";
}

// ---------------------------------------------------------------------------
// Commander death: morale shock and succession at the exact tick.
// ---------------------------------------------------------------------------

namespace {

// One Red squad (commander id 1 + soldiers 2, 3, 4 around it) and one Blue
// assassin adjacent to the commander. The assassin has damage 60: it kills
// the 50-health commander in one hit but cannot one-shot the 100-health
// soldiers.
struct DeathFixture {
  sim::UnitTypeTable types = squad_types();
  sim::CommanderConfig commanders;
  std::vector<sim::Unit> units;

  DeathFixture(std::uint64_t succession_delay,
               double death_morale_loss) {
    commanders = sim::default_commander_config();
    commanders.succession_delay_ticks = succession_delay;
    commanders.commander_death_morale_loss = death_morale_loss;
  }

  sim::SimulationConfig config() {
    sim::SimulationConfig config;
    config.seed = 11;
    config.max_ticks = 5000;
    config.battlefield_width = 1000.0;
    config.battlefield_height = 1000.0;
    config.ai_mode = sim::AiMode::Utility;
    config.unit_types = &types;
    config.morale_enabled = true;
    config.tactics_enabled = false;  // M10: M6-era reproduction fixture
    config.commanders = &commanders;
    // Composition mirrors the explicit units (squad plan validation).
    config.composition[kRed].push_back(sim::TypeCount{1, 1});
    config.composition[kRed].push_back(sim::TypeCount{0, 3});
    config.composition[kBlue].push_back(sim::TypeCount{0, 1});
    config.squad_plans[kRed] = sim::SquadPlan{1, 3};
    return config;
  }

  void build() {
    auto make = [&](std::uint32_t id, sim::Faction faction, int type_index,
                    double x, double y, double health) {
      sim::Unit unit;
      unit.id = id;
      unit.faction = faction;
      unit.type_index = static_cast<std::uint32_t>(type_index);
      unit.stats = types.types[type_index].stats;
      unit.x = x;
      unit.y = y;
      unit.health = health;
      unit.alive = true;
      units.push_back(unit);
    };
    // Soldier 2 (the destined successor) sits closest to the assassin so
    // the second and third strikes deterministically land on it; soldiers
    // 3 and 4 start 40 away and cannot interfere before tick 13.
    make(1, sim::Faction::Red, 1, 500.0, 500.0, 50.0);  // the commander
    make(2, sim::Faction::Red, 0, 505.0, 500.0, 100.0);
    make(3, sim::Faction::Red, 0, 540.0, 500.0, 100.0);
    make(4, sim::Faction::Red, 0, 500.0, 540.0, 100.0);
    make(9, sim::Faction::Blue, 0, 502.0, 500.0, 100.0);  // the assassin
    units.back().stats.damage = 60.0;
    units.back().stats.speed = 0.001;
    units.back().stats.max_health = 400.0;  // tanky: survives the fight
    units.back().health = 400.0;
    // Morale is enabled: explicit initial states carry their own morale, so
    // set healthy values (the default 0 would break everyone at tick 1).
    for (sim::Unit& unit : units) {
      unit.morale = 60.0;  // matches bravery 0.5 -> 40 + 40*0.5
    }
    units.back().morale = 80.0;  // the brave assassin: 40 + 40*1.0
  }

  // Neutral Red personalities (loss scale exactly 1) and a decisive
  // assassin, so the commander reliably dies at tick 1 regardless of the
  // hash-derived personalities.
  std::vector<sim::Personality> personalities() const {
    std::vector<sim::Personality> personalities;
    for (std::uint32_t i = 0; i < 4; ++i) {
      sim::Personality personality;
      personality.bravery = 0.5;
      personality.caution = 0.5;
      personality.aggression = 0.5;
      personality.discipline = 0.5;
      personalities.push_back(personality);
    }
    sim::Personality assassin;
    assassin.bravery = 1.0;
    assassin.caution = 0.0;
    assassin.aggression = 1.0;
    assassin.discipline = 0.5;
    personalities.push_back(assassin);
    return personalities;
  }
};

}  // namespace

TEST(CommanderDeathTest, KillsCommanderAtTickOneAndShocksMembers) {
  // Differential design: two identical battles whose only difference is the
  // commander death morale loss (18 vs 0). Every other tick-1 morale
  // contribution is identical, so the morale difference after tick 1 is
  // exactly the shock (personality scale 1: hash-derived personalities are
  // replaced by explicit neutral ones).
  const auto run = [](double death_loss) {
    DeathFixture fixture(20, death_loss);
    fixture.build();
    sim::SimulationConfig config = fixture.config();
    sim::Simulation simulation(config, fixture.units,
                               fixture.personalities());
    simulation.tick();  // the assassin strikes at tick 1
    // Precondition: the commander died during tick 1.
    EXPECT_FALSE(simulation.world().units[0].alive);
    return simulation;
  };
  const sim::Simulation shocked = run(18.0);
  const sim::Simulation control = run(0.0);

  // Members 2, 3, 4 are within the 120 leadership radius of the dying
  // commander: their morale dropped by exactly the 18-point shock (every
  // other contribution is shared with the control battle).
  const std::size_t members[] = {1, 2, 3};  // unit indices of ids 2, 3, 4
  for (const std::size_t index : members) {
    const double delta =
        control.world().units[index].morale -
        shocked.world().units[index].morale;
    EXPECT_NEAR(delta, 18.0, 1e-9)
        << "member " << shocked.world().units[index].id
        << " did not take the exact commander-death shock";
  }
}

TEST(CommanderDeathTest, SuccessionHappensAtTheExactDocumentedTick) {
  DeathFixture fixture(5, 18.0);  // succession delay: 5 ticks
  fixture.build();
  sim::Simulation simulation(fixture.config(), fixture.units,
                             fixture.personalities());
  ASSERT_TRUE(simulation.squads_active());
  const sim::Squad& squad = simulation.squads()[0];

  simulation.tick();  // tick 1: the commander dies, command is lost
  EXPECT_FALSE(simulation.world().units[0].alive);
  EXPECT_FALSE(squad.command_active);
  EXPECT_EQ(squad.command_lost_tick, 1u);
  // All orders dropped: the squad drifts with no order term.
  EXPECT_EQ(simulation.unit_squad_info(1).order, sim::Order::None);
  EXPECT_EQ(simulation.unit_squad_info(2).order, sim::Order::None);

  // Ticks 2..5: still leaderless (succession at 1 + 5 = 6, not earlier).
  for (int i = 0; i < 3; ++i) {
    simulation.tick();
  }
  EXPECT_FALSE(squad.command_active)
      << "succession must not happen before the documented tick";

  simulation.tick();  // tick 5
  EXPECT_FALSE(squad.command_active)
      << "succession must not happen on tick 5 (delay 5 from tick 1)";

  simulation.tick();  // tick 6: exactly succession_delay after the loss
  ASSERT_TRUE(squad.command_active)
      << "succession must happen exactly at loss tick + delay";
  // The successor is the lowest-id alive, non-routed member: soldier id 2
  // (unit index 1), not the dead commander (id 1) and not ids 3/4.
  EXPECT_EQ(simulation.world().units[squad.commander_index].id, 2u);
  // The successor counted as a succession and picked a fresh order (which
  // order depends on the tactical picture; the point is that it commands).
  EXPECT_EQ(simulation.result().factions[kRed].squad.successions, 1u);
  EXPECT_NE(squad.order, sim::Order::None);
}

TEST(CommanderDeathTest, DeathOfADeposedCommanderIsJustACasualty) {
  // Only acting commanders count: the original commander's death costs the
  // squad its command; when the successor (soldier 2) later dies to the
  // same assassin, that is the second commander death and id 3 succeeds.
  DeathFixture fixture(2, 0.0);
  fixture.build();
  sim::Simulation simulation(fixture.config(), fixture.units,
                             fixture.personalities());
  const sim::Squad& squad = simulation.squads()[0];
  // Tick 1: original commander dies. Tick 3: successor (id 2) takes command
  // (succession delay 2).
  for (int i = 0; i < 3; ++i) {
    simulation.tick();
  }
  ASSERT_TRUE(squad.command_active);
  ASSERT_EQ(simulation.world().units[squad.commander_index].id, 2u);
  EXPECT_EQ(simulation.result().factions[kRed].squad.commander_deaths, 1u);
  // The assassin (damage 60, cooldown 5) strikes again on tick 6 and tick
  // 11: soldier id 2 (100 health) dies on tick 11 — a second commander
  // death — and id 3 succeeds two ticks later (tick 13).
  for (int i = 0; i < 10; ++i) {
    simulation.tick();
  }
  EXPECT_FALSE(simulation.world().units[1].alive) << "successor should die";
  const auto report = simulation.result().factions[kRed].squad;
  EXPECT_EQ(report.commander_deaths, 2u);
  EXPECT_EQ(report.successions, 2u);
  EXPECT_EQ(simulation.world().units[squad.commander_index].id, 3u);
}

TEST(RoutedCommanderTest, RoutedCommandersLoseCommand) {
  // The commander starts routed (explicit state carries morale and routed):
  // command is lost from tick 1 and succession follows.
  const sim::UnitTypeTable types = squad_types();
  sim::CommanderConfig commanders = sim::default_commander_config();
  commanders.succession_delay_ticks = 10;
  sim::SimulationConfig config;
  config.seed = 13;
  config.max_ticks = 5000;
  config.battlefield_width = 1000.0;
  config.battlefield_height = 1000.0;
  config.ai_mode = sim::AiMode::Utility;
  config.unit_types = &types;
  config.morale_enabled = true;
  config.commanders = &commanders;
  config.composition[kRed].push_back(sim::TypeCount{1, 1});
  config.composition[kRed].push_back(sim::TypeCount{0, 2});
  config.composition[kBlue].push_back(sim::TypeCount{0, 1});
  config.squad_plans[kRed] = sim::SquadPlan{1, 2};

  std::vector<sim::Unit> units;
  auto make = [&](std::uint32_t id, sim::Faction faction, int type_index,
                  double x, double y) {
    sim::Unit unit;
    unit.id = id;
    unit.faction = faction;
    unit.type_index = static_cast<std::uint32_t>(type_index);
    unit.stats = types.types[type_index].stats;
    unit.x = x;
    unit.y = y;
    unit.health = unit.stats.max_health;
    unit.alive = true;
    units.push_back(unit);
  };
  make(1, sim::Faction::Red, 1, 500.0, 500.0);
  make(2, sim::Faction::Red, 0, 510.0, 500.0);
  make(3, sim::Faction::Red, 0, 490.0, 500.0);
  make(9, sim::Faction::Blue, 0, 990.0, 990.0);
  // Morale is enabled: healthy soldiers and a Broken, routed commander.
  for (sim::Unit& unit : units) {
    unit.morale = 60.0;
  }
  units[0].morale = 5.0;    // Broken
  units[0].routed = true;   // routing from the start

  sim::Simulation simulation(config, units);
  ASSERT_TRUE(simulation.squads_active());
  const sim::Squad& squad = simulation.squads()[0];
  simulation.tick();
  EXPECT_TRUE(simulation.world().units[0].routed);
  EXPECT_FALSE(squad.command_active)
      << "a routed commander must lose command";
  EXPECT_EQ(squad.command_lost_tick, 1u);
  // Still leaderless at tick 10; successor at tick 11 (delay 10).
  for (int i = 0; i < 9; ++i) {
    simulation.tick();
  }
  EXPECT_FALSE(squad.command_active);
  simulation.tick();
  ASSERT_TRUE(squad.command_active);
  // Soldiers 2 and 3 are eligible: the lowest id (2) succeeds. The routed
  // former commander (id 1) is not eligible while routed.
  EXPECT_EQ(simulation.world().units[squad.commander_index].id, 2u);
  EXPECT_EQ(simulation.result().factions[kRed].squad.successions, 1u);
}

// ---------------------------------------------------------------------------
// Orders measurably change behavior: hold keeps squads closer than advance.
// ---------------------------------------------------------------------------

namespace {

// Owns the battle AND the configs it points at (the simulation keeps raw
// pointers into them; they must outlive it).
struct OrderBattle {
  sim::CommanderConfig commanders;
  sim::FormationConfig formations;
  std::unique_ptr<sim::Simulation> simulation;
};

// One Red squad (an immobile commander + 10 disciplined soldiers) and five
// immobile, harmless Blue "beacons" spread vertically in front of it. The
// commander sees the beacons (hold can fire); the soldiers see three of
// them (approach is available). A hold order keeps the soldiers at their
// column slots behind the commander; an advance order pushes Approach and
// they stream toward the spread-out beacons, far from the commander.
OrderBattle order_battle(const sim::UnitTypeTable& types,
                         double hold_strength_above) {
  OrderBattle battle;
  battle.commanders = sim::default_commander_config();
  battle.commanders.rules.hold_strength_above = hold_strength_above;
  battle.commanders.order_weights = {1.0, 1.0, 1.0, 1.0};
  // Never retreat or regroup in this controlled test.
  battle.commanders.rules.retreat_cohesion_below = 0.0;
  battle.commanders.rules.retreat_strength_below = 0.0;
  battle.commanders.rules.retreat_morale_below = 0.0;
  battle.commanders.rules.regroup_cohesion_below = 0.0;

  battle.formations = sim::default_formation_config();
  battle.formations.default_formation = "column";

  sim::SimulationConfig config;
  config.seed = 21;
  config.max_ticks = 5000;
  config.battlefield_width = 1000.0;
  config.battlefield_height = 1000.0;
  config.ai_mode = sim::AiMode::Utility;
  config.unit_types = &types;
  config.morale_enabled = false;  // no routing noise in a controlled test
  config.commanders = &battle.commanders;
  config.formations = &battle.formations;
  config.composition[kRed].push_back(sim::TypeCount{1, 1});
  config.composition[kRed].push_back(sim::TypeCount{0, 10});
  config.composition[kBlue].push_back(sim::TypeCount{0, 5});
  config.squad_plans[kRed] = sim::SquadPlan{1, 10};

  std::vector<sim::Unit> units;
  auto make = [&](std::uint32_t id, sim::Faction faction, int type_index,
                  double x, double y) {
    sim::Unit unit;
    unit.id = id;
    unit.faction = faction;
    unit.type_index = static_cast<std::uint32_t>(type_index);
    unit.stats = types.types[type_index].stats;
    unit.x = x;
    unit.y = y;
    unit.health = unit.stats.max_health;
    unit.alive = true;
    units.push_back(unit);
  };
  // The commander is pinned (speed ~0) so the squad's anchor never moves.
  make(1, sim::Faction::Red, 1, 400.0, 500.0);
  units.back().stats.speed = 0.001;
  for (std::uint32_t i = 0; i < 10; ++i) {
    make(2 + i, sim::Faction::Red, 0, 395.0, 500.0);
  }
  // Five beacons spread over 100 of y, 25 apart; the middle three are
  // visible to the soldiers from the start (distance ~38 <= sight 40).
  for (std::uint32_t i = 0; i < 5; ++i) {
    make(100 + i, sim::Faction::Blue, 0, 425.0,
         450.0 + 25.0 * static_cast<double>(i));
  }
  // Beacons: immobile, harmless, tough (they exist to be seen).
  for (std::size_t i = units.size() - 5; i < units.size(); ++i) {
    units[i].stats.speed = 0.001;
    units[i].stats.damage = 0.001;
    units[i].stats.attack_range = 0.5;
    units[i].stats.attack_cooldown_ticks = 100;
  }
  // Disciplined, steady soldiers: obedience near 1 so the order term bites.
  std::vector<sim::Personality> personalities;
  sim::Personality soldier;
  soldier.bravery = 0.9;
  soldier.caution = 0.1;
  soldier.aggression = 0.3;
  soldier.discipline = 1.0;
  for (std::uint32_t i = 0; i < 11; ++i) {
    personalities.push_back(soldier);
  }
  for (std::uint32_t i = 0; i < 5; ++i) {
    personalities.push_back(soldier);
  }
  battle.simulation = std::make_unique<sim::Simulation>(
      config, units, personalities);
  return battle;
}

// Mean distance of a squad's alive soldiers to their commander.
double mean_distance_to_commander(const sim::Simulation& simulation,
                                  const sim::Squad& squad) {
  if (!squad.command_active ||
      squad.commander_index >= simulation.world().units.size()) {
    return 0.0;
  }
  const sim::Unit& commander =
      simulation.world().units[squad.commander_index];
  double distance_sum = 0.0;
  std::size_t alive = 0;
  for (const sim::SquadMemberState& member : squad.members) {
    if (member.unit_index == squad.commander_index) {
      continue;
    }
    const sim::Unit& unit = simulation.world().units[member.unit_index];
    if (unit.alive) {
      const double dx = unit.x - commander.x;
      const double dy = unit.y - commander.y;
      distance_sum += std::sqrt(dx * dx + dy * dy);
      ++alive;
    }
  }
  return alive == 0 ? 0.0 : distance_sum / static_cast<double>(alive);
}

}  // namespace

TEST(OrderEffectTest, HoldKeepsSquadsCloserThanAdvance) {
  const sim::UnitTypeTable types = squad_types();
  // Hold config: the commander sees enemies and is locally strong, so it
  // orders hold (strength (10+1)/(10+5+1) = 0.69 > 0.6).
  OrderBattle holding = order_battle(types, 0.6);
  // Advance config: hold can never fire (threshold above 1), so the
  // commander orders advance.
  OrderBattle advancing = order_battle(types, 2.0);
  sim::Simulation& holding_sim = *holding.simulation;
  sim::Simulation& advancing_sim = *advancing.simulation;
  ASSERT_TRUE(holding_sim.squads_active());
  ASSERT_TRUE(advancing_sim.squads_active());
  ASSERT_EQ(holding_sim.squads().size(), 1u);
  ASSERT_EQ(advancing_sim.squads().size(), 1u);

  for (int tick = 0; tick < 150; ++tick) {
    holding_sim.tick();
    advancing_sim.tick();
  }
  // Sanity: the orders actually diverged.
  EXPECT_EQ(holding_sim.squads()[0].order, sim::Order::Hold);
  EXPECT_EQ(advancing_sim.squads()[0].order, sim::Order::Advance);
  // Most members obey: holding soldiers hold at their slots, advancing
  // soldiers stream toward the beacons.
  std::uint64_t holding_holds = 0;
  std::uint64_t advancing_approaches = 0;
  for (std::size_t i = 0; i < holding_sim.world().units.size(); ++i) {
    if (holding_sim.world().units[i].faction == sim::Faction::Red &&
        holding_sim.world().units[i].last_action ==
            static_cast<std::uint8_t>(sim::Action::Hold)) {
      ++holding_holds;
    }
    if (advancing_sim.world().units[i].faction == sim::Faction::Red &&
        advancing_sim.world().units[i].last_action ==
            static_cast<std::uint8_t>(sim::Action::Approach)) {
      ++advancing_approaches;
    }
  }
  EXPECT_GE(holding_holds, 8u);
  EXPECT_GE(advancing_approaches, 8u);
  // The measurable difference: mean distance of the soldiers to their
  // commander. Holding soldiers sit in their column slots behind the pinned
  // commander; advancing soldiers stream to the beacon line far away.
  const double hold_spread = mean_distance_to_commander(
      holding_sim, holding_sim.squads()[0]);
  const double advance_spread = mean_distance_to_commander(
      advancing_sim, advancing_sim.squads()[0]);
  EXPECT_LT(hold_spread, advance_spread)
      << "hold spread " << hold_spread << " vs advance spread "
      << advance_spread;
  EXPECT_LT(hold_spread, advance_spread * 0.9)
      << "the difference must be substantial, not marginal (hold "
      << hold_spread << " vs advance " << advance_spread << ")";
}

// ---------------------------------------------------------------------------
// Reproduction and determinism.
// ---------------------------------------------------------------------------

TEST(SquadReproductionTest, SquadsOffIsBitIdenticalToNoSquads) {
  const sim::UnitTypeTable types = squad_types();
  // The same squads battle, run once with squads disabled and once with no
  // plans at all: identical battles, byte-identical exports (format 4).
  const auto run = [&](bool squads_enabled, bool with_plans) {
    sim::SimulationConfig config = squad_config(types, 2, 10);
    config.max_ticks = 120;
    if (!with_plans) {
      config.squad_plans[kRed].reset();
      config.squad_plans[kBlue].reset();
    }
    config.squads_enabled = squads_enabled;
    sim::Simulation simulation(config);
    simulation.run();
    return sim::export_state_json(simulation);
  };
  EXPECT_EQ(run(false, true), run(false, false))
      << "--squads off must reproduce the no-squad battle exactly";
  EXPECT_EQ(run(false, true), run(true, false))
      << "a no-plan scenario must be a no-op even with squads enabled";
}

TEST(SquadReproductionTest, DefaultScenarioSquadsOnEqualsSquadsOff) {
  // The shipped default scenario has no squad plans: with the default
  // --squads on, nothing changes relative to --squads off (M5 behavior),
  // byte for byte, with morale on and off.
  const sim::UnitTypeTable types = squad_types();
  const auto run = [&types](bool squads_enabled, bool morale) {
    sim::SimulationConfig config;
    config.seed = 42;
    config.max_ticks = 200;
    config.battlefield_width = 1000.0;
    config.battlefield_height = 1000.0;
    config.ai_mode = sim::AiMode::Utility;
    config.unit_types = &types;
    config.morale_enabled = morale;
    config.squads_enabled = squads_enabled;
    const int infantry = types.index_of("infantry");
    config.composition[kRed].push_back(sim::TypeCount{
        static_cast<std::uint32_t>(infantry), 30});
    config.composition[kBlue].push_back(sim::TypeCount{
        static_cast<std::uint32_t>(infantry), 30});
    sim::Simulation simulation(config);
    simulation.run();
    return sim::export_state_json(simulation);
  };
  EXPECT_EQ(run(true, true), run(false, true));
  EXPECT_EQ(run(true, false), run(false, false));
}

TEST(SquadDeterminismTest, SquadsOnDoubleRunsAreByteIdentical) {
  const sim::UnitTypeTable types = squad_types();
  const auto run = [&types] {
    sim::SimulationConfig config = squad_config(types, 2, 12);
    config.max_ticks = 300;
    sim::Simulation simulation(config);
    simulation.run();
    return sim::export_state_json(simulation);
  };
  EXPECT_EQ(run(), run());
}

TEST(SquadDeterminismTest, SquadReportsAndExportCarrySquadData) {
  const sim::UnitTypeTable types = squad_types();
  sim::SimulationConfig config = squad_config(types, 2, 8);
  config.max_ticks = 250;
  sim::Simulation simulation(config);
  const sim::BattleResult result = simulation.run();
  ASSERT_TRUE(simulation.squads_active());

  // The faction reports carry the squad statistics.
  for (const sim::Faction faction : sim::kAllFactions) {
    const sim::SquadStats& stats =
        result.factions[static_cast<std::size_t>(faction)].squad;
    EXPECT_EQ(stats.squads, 2u);
    EXPECT_EQ(stats.commander_deaths + stats.successions >= 0, true);
    std::uint64_t total_orders = 0;
    for (std::size_t order = 0; order < sim::kOrderCount; ++order) {
      total_orders += stats.order_counts[order];
    }
    EXPECT_GE(total_orders, 2u);  // each commander picked an initial order
  }

  // The export is format 5 and carries the per-unit squad fields and the
  // per-faction squad statistics.
  const nlohmann::json state =
      nlohmann::json::parse(sim::export_state_json(simulation));
  EXPECT_EQ(state["format_version"], 5);
  std::uint64_t with_squad = 0;
  for (const nlohmann::json& unit : state["units"]) {
    ASSERT_TRUE(unit.contains("squad"));
    ASSERT_TRUE(unit.contains("formation"));
    ASSERT_TRUE(unit.contains("order"));
    ASSERT_TRUE(unit.contains("cohesion"));
    if (unit["squad"] >= 0) {
      ++with_squad;
      EXPECT_NE(unit["formation"], "none");
    }
  }
  EXPECT_EQ(with_squad, 36u);  // 2 squads x (8 + 1) x 2 factions
  for (const std::string faction : {"Red", "Blue"}) {
    const nlohmann::json& stats = state["statistics"][faction]["squads"];
    EXPECT_EQ(stats["squads"], 2);
    EXPECT_TRUE(stats.contains("commander_deaths"));
    EXPECT_TRUE(stats.contains("successions"));
    EXPECT_TRUE(stats.contains("drops_from_formation"));
    EXPECT_TRUE(stats["order_counts"].contains("advance"));
    EXPECT_TRUE(stats["order_counts"].contains("hold"));
    EXPECT_TRUE(stats["order_counts"].contains("regroup"));
    EXPECT_TRUE(stats["order_counts"].contains("retreat"));
  }
  // The personality blocks carry the discipline trait in format 5.
  EXPECT_TRUE(state["units"][0]["personality"].contains("discipline"));
  // The unit type table carries leadership_radius in format 5.
  for (const nlohmann::json& type : state["config"]["unit_types"]) {
    EXPECT_TRUE(type.contains("leadership_radius"));
  }
}
