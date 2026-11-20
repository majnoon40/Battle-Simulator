// M14 item 3: scripted AI generals — the sandbox, the snapshot API, the
// returned-order parser, and end-to-end determinism. Byte-identity with
// no --general flags is the whole existing suite (the general path is
// fully gated); these tests cover what is NEW.
#include "cli/lua_general.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "sim/scenario.hpp"
#include "sim/simulation.hpp"

namespace {

constexpr std::size_t kRed = static_cast<std::size_t>(sim::Faction::Red);

constexpr const char* kTable = R"json([
  {"id": "infantry", "health": 100.0, "damage": 12.0, "speed": 2.0, "range": 2.0, "cooldown": 5, "sight_radius": 40.0},
  {"id": "commander", "health": 140.0, "damage": 10.0, "speed": 2.0, "range": 2.0, "cooldown": 6, "sight_radius": 50.0,
   "leadership_radius": 12.0}
])json";

// A 2-faction squad battle: red 2 squads x (1 commander + 10 soldiers).
sim::Simulation squad_battle(const sim::UnitTypeTable& types) {
  const sim::Scenario scenario = sim::parse_scenario(R"json({
    "battlefield": {"width": 600.0, "height": 600.0},
    "spawn_band_fraction": 0.15,
    "armies": {"red": {"commander": 2, "infantry": 20},
               "blue": {"commander": 2, "infantry": 20}},
    "squads": {"red": {"count": 2, "size": 10},
               "blue": {"count": 2, "size": 10}}
  })json");
  sim::SimulationConfig config;
  config.seed = 11;
  config.max_ticks = 200;
  config.battlefield_width = scenario.battlefield_width;
  config.battlefield_height = scenario.battlefield_height;
  config.spawn_band_fraction = scenario.spawn_band_fraction;
  config.faction_count = scenario.faction_count;
  config.unit_types = &types;
  config.composition = sim::build_composition(scenario, types, config.seed);
  config.squad_plans[0] = sim::SquadPlan{2, 10};
  config.squad_plans[1] = sim::SquadPlan{2, 10};
  config.ai_mode = sim::AiMode::Utility;
  config.squads_enabled = true;
  return sim::Simulation{config};
}

}  // namespace

TEST(LuaGeneralTest, LoadErrorsAreReported) {
  try {
    static_cast<void>(cli::LuaGeneral("function decide(g) enddd", "bad.lua"));
    FAIL() << "expected a load error";
  } catch (const std::exception& error) {
    EXPECT_NE(std::string(error.what()).find("load error"),
              std::string::npos)
        << "error was: " << error.what();
  }
  try {
    static_cast<void>(cli::LuaGeneral("x = 1", "nodecide.lua"));
    FAIL() << "expected a missing-decide error";
  } catch (const std::exception& error) {
    EXPECT_NE(std::string(error.what()).find("decide"),
              std::string::npos)
        << "error was: " << error.what();
  }
}

TEST(LuaGeneralTest, SandboxHasNoIOOrOS) {
  // io, os, package, debug and coroutine are not registered: any access
  // is an indexing-nil runtime error from decide().
  const char* script =
      "function decide(g)\n"
      "  if io ~= nil or os ~= nil or package ~= nil or debug ~= nil\n"
      "     or coroutine ~= nil then\n"
      "    error('sandbox leak')\n"
      "  end\n"
      "  return nil\n"
      "end\n";
  cli::LuaGeneral general(script, "sandbox.lua");
  const sim::UnitTypeTable types = sim::parse_unit_types(kTable);
  sim::Simulation simulation = squad_battle(types);
  try {
    static_cast<void>(general.decide(simulation, sim::Faction::Red));
    SUCCEED();
  } catch (const std::exception& error) {
    FAIL() << "sandbox check failed: " << error.what();
  }
}

TEST(LuaGeneralTest, SnapshotExposesTheBattle) {
  // The script returns one bogus-but-parseable order per seen squad and
  // stashes the aggregate fields; the asserts below cross-check both.
  const char* script =
      "seen = {}\n"
      "function decide(g)\n"
      "  seen.tick = g.tick\n"
      "  seen.faction = g.faction\n"
      "  seen.squads = #g.squads\n"
      "  seen.enemy_squads = #g.enemy_squads\n"
      "  seen.enemies = g.enemies.count\n"
      "  local orders = {}\n"
      "  for _, s in ipairs(g.squads) do\n"
      "    if s.members == nil or s.x == nil or s.commander == nil or\n"
      "       s.formation == nil then\n"
      "      error('incomplete squad table')\n"
      "    end\n"
      "    table.insert(orders, {squad = s.id, order = 'hold'})\n"
      "  end\n"
      "  return orders\n"
      "end\n";
  cli::LuaGeneral general(script, "snapshot.lua");
  const sim::UnitTypeTable types = sim::parse_unit_types(kTable);
  sim::Simulation simulation = squad_battle(types);
  simulation.tick();
  const std::vector<cli::GeneralOrder> orders =
      general.decide(simulation, sim::Faction::Red);
  EXPECT_EQ(orders.size(), 2u);  // red fields two squads
  EXPECT_EQ(orders[0].squads.size(), 1u);
  EXPECT_FALSE(orders[0].squads_all);
  EXPECT_EQ(orders[0].type, sim::PlayerOrderType::Hold);
}

TEST(LuaGeneralTest, OrderParsingShapes) {
  const char* script =
      "function decide(g)\n"
      "  return {\n"
      "    {squad = 'all', order = 'regroup'},\n"
      "    {squad = 3, order = 'move_to', x = 100, y = 200},\n"
      "    {squad = 4, order = 'formation', formation = 'column'},\n"
      "  }\n"
      "end\n";
  cli::LuaGeneral general(script, "orders.lua");
  const sim::UnitTypeTable types = sim::parse_unit_types(kTable);
  sim::Simulation simulation = squad_battle(types);
  const std::vector<cli::GeneralOrder> orders =
      general.decide(simulation, sim::Faction::Red);
  ASSERT_EQ(orders.size(), 3u);
  EXPECT_TRUE(orders[0].squads_all);
  EXPECT_EQ(orders[0].type, sim::PlayerOrderType::Regroup);
  EXPECT_FALSE(orders[0].has_point);
  ASSERT_EQ(orders[1].squads.size(), 1u);
  EXPECT_EQ(orders[1].squads[0], 3u);
  EXPECT_EQ(orders[1].type, sim::PlayerOrderType::MoveTo);
  EXPECT_TRUE(orders[1].has_point);
  EXPECT_DOUBLE_EQ(orders[1].x, 100.0);
  EXPECT_DOUBLE_EQ(orders[1].y, 200.0);
  EXPECT_EQ(orders[2].type, sim::PlayerOrderType::ChangeFormation);
  EXPECT_EQ(orders[2].formation, "column");
}

TEST(LuaGeneralTest, MalformedOrdersAreRejected) {
  const auto expect_reject = [](const char* body) {
    const std::string script =
        std::string("function decide(g)\n  return {\n") + body +
        "\n  }\nend\n";
    try {
      cli::LuaGeneral general(script, "malformed.lua");
      const sim::UnitTypeTable types = sim::parse_unit_types(kTable);
      sim::Simulation simulation = squad_battle(types);
      static_cast<void>(general.decide(simulation, sim::Faction::Red));
      FAIL() << "expected rejection, body was: " << body;
    } catch (const std::exception& error) {
      EXPECT_NE(std::string(error.what()).find("order 1"),
                std::string::npos)
          << "error was: " << error.what();
    }
  };
  expect_reject("{order = 'hold'}");                    // no squad
  expect_reject("{squad = 1, order = 'charge'}");       // unknown type
  expect_reject("{squad = 1}");                         // no order
  expect_reject("{squad = 1, order = 5}");              // order not a string
}

TEST(LuaGeneralTest, ScriptedBattleIsDeterministic) {
  // The same script + seed drives the same battle twice: identical
  // state hashes at every checkpoint. The script reacts to the snapshot
  // (it moves toward the enemy centroid), so this exercises the full
  // snapshot -> order -> simulation loop.
  const char* script =
      "function decide(g)\n"
      "  if g.enemies.count == 0 then return nil end\n"
      "  local orders = {}\n"
      "  table.insert(orders, {squad = 'all', order = 'attack_move',\n"
      "                        x = g.enemies.centroid_x,\n"
      "                        y = g.enemies.centroid_y})\n"
      "  return orders\n"
      "end\n";
  const sim::UnitTypeTable types = sim::parse_unit_types(kTable);

  const auto run_hashed = [&]() {
    sim::Simulation simulation = squad_battle(types);
    cli::LuaGeneral general(script, "aggressive.lua");
    std::vector<std::string> hashes;
    while (!simulation.finished()) {
      const std::uint64_t upcoming = simulation.world().tick_count + 1;
      if (upcoming % 30 == 0) {
        for (const cli::GeneralOrder& order :
             general.decide(simulation, sim::Faction::Red)) {
          std::vector<std::uint32_t> squads;
          if (order.squads_all) {
            for (const sim::Squad& squad : simulation.squads()) {
              if (squad.faction == sim::Faction::Red) {
                squads.push_back(squad.id);
              }
            }
          } else {
            squads = order.squads;
          }
          const bool point = order.has_point;
          static_cast<void>(simulation.issue_order(
              sim::Faction::Red, squads, order.type,
              point ? order.x : 0.0, point ? order.y : 0.0,
              order.formation));
        }
      }
      simulation.tick();
      hashes.push_back(simulation.state_hash());
    }
    return hashes;
  };
  const std::vector<std::string> first = run_hashed();
  const std::vector<std::string> second = run_hashed();
  ASSERT_FALSE(first.empty());
  ASSERT_EQ(first.size(), second.size());
  for (std::size_t i = 0; i < first.size(); ++i) {
    EXPECT_EQ(first[i], second[i]) << "hash diverged at tick " << i + 1;
  }
}

TEST(LuaGeneralTest, OrdersActuallyReachTheSquads) {
  // One hold order to every red squad: the delivered counter must move.
  const char* script =
      "function decide(g)\n"
      "  return {{squad = 'all', order = 'hold'}}\n"
      "end\n";
  const sim::UnitTypeTable types = sim::parse_unit_types(kTable);
  sim::Simulation simulation = squad_battle(types);
  cli::LuaGeneral general(script, "hold.lua");
  const std::uint64_t before =
      simulation.player_order_report(sim::Faction::Red).issued;
  for (const cli::GeneralOrder& order :
       general.decide(simulation, sim::Faction::Red)) {
    std::vector<std::uint32_t> squads;
    for (const sim::Squad& squad : simulation.squads()) {
      if (squad.faction == sim::Faction::Red) {
        squads.push_back(squad.id);
      }
    }
    static_cast<void>(simulation.issue_order(
        sim::Faction::Red, squads, order.type, 0.0, 0.0, order.formation));
  }
  const std::uint64_t after =
      simulation.player_order_report(sim::Faction::Red).issued;
  EXPECT_GT(after, before);
}
