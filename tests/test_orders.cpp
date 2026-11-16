// Player order tests (m11b): issue_order validation and rate limit,
// courier delay through the commander system (distance, succession,
// leaderless), the bounded score terms measurably changing behavior
// (hold vs advance, MoveTo across terrain), routed units ignoring
// orders, and determinism/replay with orders.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "cli/replay.hpp"
#include "sim/events.hpp"
#include "sim/json_export.hpp"
#include "sim/morale.hpp"
#include "sim/personalities.hpp"
#include "sim/simulation.hpp"
#include "sim/squads.hpp"
#include "sim/version.hpp"

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kRed = static_cast<std::size_t>(sim::Faction::Red);
constexpr std::size_t kBlue = static_cast<std::size_t>(sim::Faction::Blue);

std::string read_text(const fs::path& path) {
  std::ifstream file(path);
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

const sim::UnitTypeTable& order_types() {
  static const sim::UnitTypeTable table = sim::parse_unit_types(
      read_text(fs::path(TEST_DATA_DIR) / "units.json"));
  return table;
}
const sim::PersonalityTable& order_personalities() {
  static const sim::PersonalityTable table = sim::parse_personalities(
      read_text(fs::path(TEST_DATA_DIR) / "personalities.json"));
  return table;
}
const sim::FormationConfig& order_formations() {
  static const sim::FormationConfig table = sim::parse_formations(
      read_text(fs::path(TEST_DATA_DIR) / "formations.json"));
  return table;
}
const sim::CommanderConfig& order_commanders() {
  static const sim::CommanderConfig table = sim::parse_commanders(
      read_text(fs::path(TEST_DATA_DIR) / "commanders.json"));
  return table;
}
const sim::MoraleConfig& order_morale() {
  static const sim::MoraleConfig table = sim::parse_morale(
      read_text(fs::path(TEST_DATA_DIR) / "morale.json"));
  return table;
}

// A grassland grid with a vertical water wall and a 2-cell gap, the
// m11a corridor fixture (cells: 'g' grassland, 'w' water).
sim::TerrainTypeTable order_table() {
  sim::TerrainTypeTable table;
  auto add = [&table](const char* id, std::uint8_t r, std::uint8_t g,
                      std::uint8_t b, double move_cost, bool passable,
                      double sight, double defense) {
    sim::TerrainType type;
    type.id = id;
    type.r = r;
    type.g = g;
    type.b = b;
    type.move_cost = move_cost;
    type.passable = passable;
    type.sight_modifier = sight;
    type.defense_modifier = defense;
    table.types.push_back(type);
  };
  add("grassland", 0x6F, 0xA3, 0x5C, 1.0, true, 1.0, 1.0);
  add("water", 0x3D, 0x6E, 0x9E, 0.0, false, 1.0, 1.0);
  add("road", 0xB8, 0xA7, 0x7E, 1.3, true, 1.0, 1.05);
  return table;
}

sim::TerrainGrid corridor_grid(const sim::TerrainTypeTable& table) {
  std::vector<std::string> rows;
  for (std::uint32_t y = 0; y < 24; ++y) {
    std::string row(40, 'g');
    if (y != 11 && y != 12) {
      row[20] = 'w';
    }
    rows.push_back(row);
  }
  std::vector<std::uint16_t> cells;
  for (const std::string& row : rows) {
    for (char cell : row) {
      cells.push_back(cell == 'w' ? 1u : 0u);
    }
  }
  return sim::build_terrain_grid("orders", 40, 24, 5.0, cells, &table);
}

// A utility-mode squads battle on the corridor grid.
sim::Simulation corridor_battle(const sim::TerrainGrid* grid,
                                sim::EventRecorder* recorder = nullptr,
                                bool orders_enabled = true) {
  sim::SimulationConfig config;
  config.seed = 11;
  config.max_ticks = 600;
  config.ai_mode = sim::AiMode::Utility;
  config.morale_enabled = false;
  config.terrain = grid;
  config.personalities = &order_personalities();
  config.unit_types = &order_types();
  config.formations = &order_formations();
  config.commanders = &order_commanders();
  config.orders_enabled = orders_enabled;
  if (grid != nullptr) {
    config.battlefield_width = grid->width * grid->cell_size;
    config.battlefield_height = grid->height * grid->cell_size;
  } else {
    config.battlefield_width = 200.0;
    config.battlefield_height = 100.0;
  }
  config.spawn_band_fraction = 0.15;
  // One squad per side: the squad count is 1 so "all" addresses it.
  config.composition[kRed].push_back(sim::TypeCount{0, 10});
  config.composition[kRed].push_back(sim::TypeCount{3, 1});
  config.composition[kBlue].push_back(sim::TypeCount{0, 10});
  config.composition[kBlue].push_back(sim::TypeCount{3, 1});
  config.squad_plans[kRed] = sim::SquadPlan{1, 10};
  config.squad_plans[kBlue] = sim::SquadPlan{1, 10};
  config.events = recorder;
  return sim::Simulation(config);
}

// Dead units of a faction (flat-map battle counters for the m12 tests).
std::uint32_t flat_dead(const sim::Simulation& battle,
                        sim::Faction faction) {
  std::uint32_t dead = 0;
  for (const sim::Unit& unit : battle.world().units) {
    if (!unit.alive && unit.faction == faction) {
      ++dead;
    }
  }
  return dead;
}

}  // namespace

// ---------------------------------------------------------------------------
// Validation and the rate limit.
// ---------------------------------------------------------------------------

TEST(PlayerOrderTest, InvalidSquadAndWrongFactionAreRejected) {
  const sim::TerrainTypeTable table = order_table();
  const sim::TerrainGrid grid = corridor_grid(table);
  sim::Simulation battle = corridor_battle(&grid);
  ASSERT_TRUE(battle.squads_active());
  // Squad 1 is Blue; Red cannot order it.
  EXPECT_FALSE(battle.issue_order(sim::Faction::Red, {1},
                                  sim::PlayerOrderType::Hold));
  // Unknown squad id.
  EXPECT_FALSE(battle.issue_order(sim::Faction::Red, {99},
                                  sim::PlayerOrderType::Hold));
  // An unknown formation id.
  EXPECT_FALSE(battle.issue_order(sim::Faction::Red, {0},
                                  sim::PlayerOrderType::ChangeFormation,
                                  0.0, 0.0, "wedge"));
  // Nothing was issued: no events, no pending state.
  EXPECT_EQ(battle.player_order_report(sim::Faction::Red).issued, 0u);
  EXPECT_EQ(battle.player_order_report(sim::Faction::Red).rejected, 3u);
}

TEST(PlayerOrderTest, RateLimitAllowsOneCallPerTick) {
  const sim::TerrainTypeTable table = order_table();
  const sim::TerrainGrid grid = corridor_grid(table);
  sim::Simulation battle = corridor_battle(&grid);
  ASSERT_TRUE(battle.issue_order(sim::Faction::Red, {0},
                                 sim::PlayerOrderType::Hold));
  EXPECT_TRUE(battle.player_order_report(sim::Faction::Red).issued == 1u);
  // The second call in the same between-tick window is rejected.
  EXPECT_FALSE(battle.issue_order(sim::Faction::Red, {0},
                                  sim::PlayerOrderType::Regroup));
  // Blue has its own budget.
  EXPECT_TRUE(battle.issue_order(sim::Faction::Blue, {1},
                                 sim::PlayerOrderType::Hold));
}

TEST(PlayerOrderTest, OrdersOffMakesIssueInert) {
  const sim::TerrainTypeTable table = order_table();
  const sim::TerrainGrid grid = corridor_grid(table);
  sim::Simulation battle = corridor_battle(&grid, nullptr, false);
  EXPECT_FALSE(battle.issue_order(sim::Faction::Red, {0},
                                  sim::PlayerOrderType::Hold));
  EXPECT_EQ(battle.player_order_report(sim::Faction::Red).rejected, 1u);
}

// ---------------------------------------------------------------------------
// Courier delay through the commander system.
// ---------------------------------------------------------------------------

TEST(PlayerOrderTest, DeliveryDelayGrowsWithDistance) {
  const sim::TerrainTypeTable table = order_table();
  const sim::TerrainGrid grid = corridor_grid(table);
  sim::EventRecorder recorder;
  // Three squads per side at different depths; one call addresses all
  // Red squads (the rate limit counts calls, not squads).
  sim::SimulationConfig wide;
  wide.seed = 11;
  wide.max_ticks = 200;
  wide.ai_mode = sim::AiMode::Utility;
  wide.morale_enabled = false;
  wide.terrain = &grid;
  wide.personalities = &order_personalities();
  wide.unit_types = &order_types();
  wide.formations = &order_formations();
  wide.commanders = &order_commanders();
  wide.battlefield_width = grid.width * grid.cell_size;
  wide.battlefield_height = grid.height * grid.cell_size;
  wide.spawn_band_fraction = 0.15;
  wide.composition[kRed].push_back(sim::TypeCount{0, 30});
  wide.composition[kRed].push_back(sim::TypeCount{3, 3});
  wide.composition[kBlue].push_back(sim::TypeCount{0, 30});
  wide.composition[kBlue].push_back(sim::TypeCount{3, 3});
  wide.squad_plans[kRed] = sim::SquadPlan{3, 10};
  wide.squad_plans[kBlue] = sim::SquadPlan{3, 10};
  wide.events = &recorder;
  sim::Simulation three(wide);
  ASSERT_TRUE(three.squads_active());
  ASSERT_TRUE(three.issue_order(sim::Faction::Red, {0, 1, 2},
                                sim::PlayerOrderType::Hold));

  // The expected delivery tick follows the courier formula exactly:
  // issue + ceil(distance(HQ, commander) * order_delay_per_unit).
  const sim::CommanderConfig& commanders = order_commanders();
  const double band = wide.battlefield_width * wide.spawn_band_fraction;
  const double hq_x = band / 2.0;
  const double hq_y = wide.battlefield_height / 2.0;
  std::vector<std::pair<double, std::uint64_t>> expected;
  for (const sim::Squad& squad : three.squads()) {
    if (squad.faction != sim::Faction::Red || !squad.command_active) {
      continue;
    }
    const sim::Unit& commander = three.world().units[squad.commander_index];
    const double dx = commander.x - hq_x;
    const double dy = commander.y - hq_y;
    const std::uint64_t delay = static_cast<std::uint64_t>(std::ceil(
        std::sqrt(dx * dx + dy * dy) * commanders.order_delay_per_unit));
    expected.emplace_back(std::sqrt(dx * dx + dy * dy), 1 + delay);
  }
  ASSERT_EQ(expected.size(), std::size_t{3});
  // Run ticks until every order has been delivered.
  std::map<std::uint32_t, std::uint64_t> delivered;
  for (int tick = 0; tick < 60 && delivered.size() < 3; ++tick) {
    three.tick();
    for (const sim::Event& event : recorder.events()) {
      if (event.type == sim::EventType::OrderDelivered) {
        delivered[event.a] = event.tick;
      }
    }
  }
  ASSERT_EQ(delivered.size(), std::size_t{3});
  // Exact formula per squad, and farther squads deliver later.
  std::size_t index = 0;
  for (const auto& [distance, tick] : expected) {
    const sim::Squad& squad = three.squads()[index];
    EXPECT_EQ(delivered[squad.id], tick)
        << "squad " << squad.id << " distance " << distance;
    ++index;
  }
  std::vector<double> distances;
  for (const auto& [distance, tick] : expected) {
    distances.push_back(distance);
  }
  std::vector<std::uint64_t> ticks;
  for (const auto& [squad_id, tick] : delivered) {
    (void)squad_id;
    ticks.push_back(tick);
  }
  std::sort(distances.begin(), distances.end());
  std::sort(ticks.begin(), ticks.end());
  // Delivery never gets earlier with distance (strictly increasing
  // whenever the courier formula's ceil gives different delays — the
  // exact per-squad assertions above pin the formula itself).
  for (std::size_t i = 1; i < distances.size(); ++i) {
    EXPECT_GE(ticks[i], ticks[i - 1]);
  }
  const std::uint64_t min_delay =
      static_cast<std::uint64_t>(std::ceil(distances.front() * 0.02));
  const std::uint64_t max_delay =
      static_cast<std::uint64_t>(std::ceil(distances.back() * 0.02));
  if (max_delay > min_delay) {
    EXPECT_GT(ticks.back(), ticks.front());
  }
}

TEST(PlayerOrderTest, CommanderDeathRoutesThroughTheSuccessor) {
  const sim::TerrainTypeTable table = order_table();
  const sim::TerrainGrid grid = corridor_grid(table);
  sim::EventRecorder recorder;
  sim::Simulation battle = corridor_battle(&grid, &recorder);
  const sim::Squad& squad = battle.squads()[0];
  const std::uint32_t commander_id =
      battle.world().units[squad.commander_index].id;
  // Kill the commander and let one tick run so command is formally
  // lost; then issue: the order goes through the successor with the
  // succession surcharge.
  ASSERT_TRUE(battle.intervene_kill(commander_id));
  battle.tick();
  ASSERT_TRUE(battle.issue_order(sim::Faction::Red, {0},
                                 sim::PlayerOrderType::Hold));
  const std::uint64_t issue_tick = battle.tick_index() + 1;
  // The base delay from the formula (HQ to the squad centroid — no
  // acting commander).
  const sim::CommanderConfig& commanders = order_commanders();
  const sim::Squad& after = battle.squads()[0];
  const double band =
      battle.world().width * battle.config().spawn_band_fraction;
  const double dx = after.centroid_x - band / 2.0;
  const double dy = after.centroid_y - battle.world().height / 2.0;
  const std::uint64_t base = static_cast<std::uint64_t>(std::ceil(
      std::sqrt(dx * dx + dy * dy) * commanders.order_delay_per_unit));
  const std::uint64_t expected =
      issue_tick + base + commanders.succession_delay_ticks;
  std::uint64_t delivered_tick = 0;
  for (int tick = 0; tick < 120 && delivered_tick == 0; ++tick) {
    battle.tick();
    for (const sim::Event& event : recorder.events()) {
      if (event.type == sim::EventType::OrderDelivered && event.a == 0) {
        delivered_tick = event.tick;
      }
    }
  }
  ASSERT_GT(delivered_tick, 0u) << "order never delivered";
  EXPECT_EQ(delivered_tick, expected);
}

// ---------------------------------------------------------------------------
// Behavior: an order measurably changes what the army does.
// ---------------------------------------------------------------------------

TEST(PlayerOrderTest, HoldOrderMeasurablySlowsTheMarch) {
  const sim::TerrainTypeTable table = order_table();
  const sim::TerrainGrid grid = corridor_grid(table);
  const auto run = [&grid](bool hold) {
    sim::Simulation battle = corridor_battle(&grid);
    if (hold) {
      EXPECT_TRUE(battle.issue_order(sim::Faction::Red, {0},
                                     sim::PlayerOrderType::Hold));
    }
    double sum_x = 0.0;
    std::uint32_t alive = 0;
    std::uint32_t holding = 0;
    for (int tick = 0; tick < 80; ++tick) {
      battle.tick();
    }
    for (const sim::Unit& unit : battle.world().units) {
      if (unit.alive && unit.faction == sim::Faction::Red) {
        sum_x += unit.x;
        ++alive;
        if (static_cast<sim::Action>(unit.last_action) ==
            sim::Action::Hold) {
          ++holding;
        }
      }
    }
    return std::pair<double, std::uint32_t>(sum_x / alive, holding);
  };
  const auto [control_x, control_holding] = run(false);
  const auto [hold_x, hold_holding] = run(true);
  // The bounded term never overrides availability: both runs stay
  // deterministic and the control marches on.
  EXPECT_EQ(control_holding, 0u);
  EXPECT_GT(hold_holding, 0u) << "hold order had no measurable effect";
  EXPECT_LT(hold_x, control_x) << "hold order did not slow the march";
}

TEST(PlayerOrderTest, MoveToReachesAPointAcrossTerrain) {
  const sim::TerrainTypeTable table = order_table();
  const sim::TerrainGrid grid = corridor_grid(table);
  // Red outnumbers Blue so it survives the meeting at the wall gap.
  sim::SimulationConfig config;
  config.seed = 11;
  config.max_ticks = 600;
  config.ai_mode = sim::AiMode::Utility;
  config.morale_enabled = false;
  config.terrain = &grid;
  config.personalities = &order_personalities();
  config.unit_types = &order_types();
  config.formations = &order_formations();
  config.commanders = &order_commanders();
  config.battlefield_width = grid.width * grid.cell_size;
  config.battlefield_height = grid.height * grid.cell_size;
  config.spawn_band_fraction = 0.15;
  config.composition[kRed].push_back(sim::TypeCount{0, 30});
  config.composition[kRed].push_back(sim::TypeCount{3, 3});
  config.composition[kBlue].push_back(sim::TypeCount{0, 6});
  config.composition[kBlue].push_back(sim::TypeCount{3, 1});
  config.squad_plans[kRed] = sim::SquadPlan{3, 10};
  config.squad_plans[kBlue] = sim::SquadPlan{1, 6};
  sim::Simulation battle(config);
  ASSERT_TRUE(battle.squads_active());
  ASSERT_TRUE(battle.issue_order(sim::Faction::Red, {0, 1, 2},
                                 sim::PlayerOrderType::MoveTo, 170.0,
                                 60.0));
  for (int tick = 0; tick < 500; ++tick) {
    battle.tick();
  }
  // The ordered squads crossed the wall (x = 100..105) through the gap
  // and closed on the point (170, 60).
  double sum_x = 0.0;
  double sum_y = 0.0;
  std::uint32_t alive = 0;
  for (const sim::Unit& unit : battle.world().units) {
    if (unit.alive && unit.faction == sim::Faction::Red) {
      sum_x += unit.x;
      sum_y += unit.y;
      ++alive;
    }
  }
  ASSERT_GT(alive, 0u);
  const double cx = sum_x / alive;
  const double cy = sum_y / alive;
  EXPECT_GT(cx, 140.0) << "the squads did not cross the wall (cx " << cx
                       << ")";
  EXPECT_LT(cx - 170.0, 40.0);
  EXPECT_LT(std::fabs(cy - 60.0), 40.0);
}

// ---------------------------------------------------------------------------
// m12 audit: ordered armies must still fight, point orders must work on
// flat maps (validation, steering, arrival) and end on squad collapse.
// ---------------------------------------------------------------------------

TEST(PlayerOrderTest, AttackMoveEngagesOnFlatMap) {
  // m11b defect (m12 audit): AttackMove mapped to Order::Advance terms
  // boosted Approach; at weight_scale 4.0 Approach saturated at 1.0 and
  // outranked Attack, so an attack-moving army crowded the enemy
  // without ever attacking (kill ratio 0.026 in the 250v250 study).
  // March orders now boost the march only: an attack-moving army must
  // fight normally when it reaches the enemy.
  const auto run = [](bool ordered) {
    sim::Simulation battle = corridor_battle(nullptr);
    if (ordered) {
      EXPECT_TRUE(battle.issue_order(sim::Faction::Red, {0},
                                     sim::PlayerOrderType::AttackMove,
                                     150.0, 50.0));
    }
    for (int tick = 0; tick < 600; ++tick) {
      battle.tick();
    }
    return flat_dead(battle, sim::Faction::Blue);
  };
  const std::uint32_t control_kills = run(false);
  const std::uint32_t attackmove_kills = run(true);
  EXPECT_GT(control_kills, 0u);
  EXPECT_GE(attackmove_kills, control_kills / 2)
      << "attack-moving army barely fought (control " << control_kills
      << " kills, attackmove " << attackmove_kills << ")";
}

TEST(PlayerOrderTest, HoldOrderFightsBackOnFlatMap) {
  // m11b defect: a delivered Hold saturates Hold and Attack at 1.0; the
  // stickiness bonus then pinned Hold over Attack, so a holding army
  // stood still while being shredded (kill ratio 0.003 in the 250v250
  // study; only the few low-obedience units ever fought back). A holding
  // unit must attack an enemy that walks into range: the ordered side's
  // kills must be comparable to the control's, not a rounding error.
  const auto run = [](bool ordered) {
    sim::Simulation battle = corridor_battle(nullptr);
    if (ordered) {
      EXPECT_TRUE(battle.issue_order(sim::Faction::Red, {0},
                                     sim::PlayerOrderType::Hold));
    }
    for (int tick = 0; tick < 600; ++tick) {
      battle.tick();
    }
    return flat_dead(battle, sim::Faction::Blue);
  };
  const std::uint32_t control_kills = run(false);
  const std::uint32_t hold_kills = run(true);
  EXPECT_GT(control_kills, 0u);
  EXPECT_GE(hold_kills, control_kills / 2)
      << "holding army barely fought back (control " << control_kills
      << " kills, hold " << hold_kills << ")";
}

TEST(PlayerOrderTest, MoveToPointWorksOnFlatMap) {
  // m12 audit: point orders were accepted and recorded on flat maps but
  // never steered — the squad flow field only exists with terrain, and
  // the flat-map Advance beelined to the faction objective, then the
  // slot logic pulled the squad back: an oscillator. A MoveTo must
  // march the squad to the point on any map and stop within the
  // kOrderArrivalDist radius instead of resuming the faction march.
  sim::Simulation battle = corridor_battle(nullptr);
  ASSERT_TRUE(battle.issue_order(sim::Faction::Red, {0},
                                 sim::PlayerOrderType::MoveTo, 60.0, 95.0));
  // Blue holds in its spawn band so the probe measures the march, not a
  // battle (a few aggressive Blue units refuse Hold by design; Red
  // engages them normally at the point).
  ASSERT_TRUE(battle.issue_order(sim::Faction::Blue, {1},
                                 sim::PlayerOrderType::Hold));
  for (int tick = 0; tick < 300; ++tick) {
    battle.tick();
  }
  const auto centroid = [&battle] {
    double sum_x = 0.0;
    double sum_y = 0.0;
    std::uint32_t alive = 0;
    for (const sim::Unit& unit : battle.world().units) {
      if (unit.alive && unit.faction == sim::Faction::Red) {
        sum_x += unit.x;
        sum_y += unit.y;
        ++alive;
      }
    }
    EXPECT_GT(alive, 0u);
    return std::pair<double, double>(sum_x / alive, sum_y / alive);
  };
  {
    const auto [cx, cy] = centroid();
    EXPECT_LT(std::fabs(cx - 60.0), 15.0)
        << "the squad did not reach the point (cx " << cx << ")";
    EXPECT_LT(std::fabs(cy - 95.0), 15.0)
        << "the squad did not reach the point (cy " << cy << ")";
  }
  // And it stays at the point: 200 more ticks must not resume the
  // march to the faction objective (the old oscillation).
  for (int tick = 0; tick < 200; ++tick) {
    battle.tick();
  }
  const auto [cx, cy] = centroid();
  EXPECT_LT(std::fabs(cx - 60.0), 15.0)
      << "the squad drifted away from the point (cx " << cx << ")";
  EXPECT_LT(std::fabs(cy - 95.0), 15.0)
      << "the squad drifted away from the point (cy " << cy << ")";
}

TEST(PlayerOrderTest, PointOrdersAreValidatedOnFlatMaps) {
  // m12 audit: flat maps validated nothing — negative, off-map and
  // non-finite points were accepted and recorded into replays.
  sim::Simulation battle = corridor_battle(nullptr);  // 200 x 100
  EXPECT_FALSE(battle.issue_order(sim::Faction::Red, {0},
                                  sim::PlayerOrderType::MoveTo, -5.0,
                                  50.0));
  EXPECT_FALSE(battle.issue_order(sim::Faction::Red, {0},
                                  sim::PlayerOrderType::MoveTo, 60.0,
                                  150.0));
  EXPECT_FALSE(battle.issue_order(sim::Faction::Red, {0},
                                  sim::PlayerOrderType::MoveTo,
                                  std::numeric_limits<double>::quiet_NaN(),
                                  50.0));
  EXPECT_FALSE(battle.issue_order(sim::Faction::Red, {0},
                                  sim::PlayerOrderType::AttackMove,
                                  std::numeric_limits<double>::infinity(),
                                  50.0));
  // An in-bounds point still goes through (the four rejections above
  // do not consume the per-tick rate limit: they return before it).
  EXPECT_TRUE(battle.issue_order(sim::Faction::Red, {0},
                                 sim::PlayerOrderType::MoveTo, 60.0, 95.0));
}

TEST(PlayerOrderTest, PointOrderEndsOnSquadCollapse) {
  // m12 audit: docs/squads.md says a player order ends on squad
  // collapse; clear_player_order was dead code until now.
  sim::EventRecorder recorder;
  sim::Simulation battle = corridor_battle(nullptr, &recorder);
  ASSERT_TRUE(battle.squads_active());
  ASSERT_TRUE(battle.issue_order(sim::Faction::Red, {0},
                                 sim::PlayerOrderType::Hold));
  for (int tick = 0; tick < 40; ++tick) {
    battle.tick();  // let the order deliver (no contact yet)
  }
  ASSERT_TRUE(battle.squads()[0].player_order_active);
  // Kill the whole squad through the public intervention API: the next
  // tick's death sweep collapses it and must clear the order.
  for (const sim::Unit& unit : battle.world().units) {
    if (unit.alive && unit.faction == sim::Faction::Red) {
      ASSERT_TRUE(battle.intervene_kill(unit.id));
    }
  }
  battle.tick();
  EXPECT_FALSE(battle.squads()[0].player_order_active)
      << "collapse left the player order active";
  bool collapsed = false;
  for (const sim::Event& event : recorder.events()) {
    if (event.type == sim::EventType::SquadCollapse && event.a == 0) {
      collapsed = true;
    }
  }
  EXPECT_TRUE(collapsed) << "no SquadCollapse event for squad 0";
}

TEST(PlayerOrderTest, ChangeFormationChangesTheSquadShape) {
  const sim::TerrainTypeTable table = order_table();
  const sim::TerrainGrid grid = corridor_grid(table);
  sim::EventRecorder recorder;
  sim::Simulation battle = corridor_battle(&grid, &recorder);
  ASSERT_EQ(battle.squads()[0].formation, sim::FormationKind::Line);
  ASSERT_TRUE(battle.issue_order(sim::Faction::Red, {0},
                                 sim::PlayerOrderType::ChangeFormation, 0.0,
                                 0.0, "column"));
  for (int tick = 0; tick < 30; ++tick) {
    battle.tick();
  }
  EXPECT_EQ(battle.squads()[0].formation, sim::FormationKind::Column);
}

// ---------------------------------------------------------------------------
// Routed units ignore orders; determinism and replay.
// ---------------------------------------------------------------------------

TEST(PlayerOrderTest, RoutedUnitsIgnoreOrders) {
  const sim::TerrainTypeTable table = order_table();
  const sim::TerrainGrid grid = corridor_grid(table);
  // A morale config where everyone breaks at the first update (the
  // breaking value sits above the spawn morale range) and never
  // recovers within the test window: every unit is routed, the squads
  // still exist, and an order must not move anyone off the forced Flee.
  sim::MoraleConfig broken = order_morale();
  broken.states.confident_min = 99.0;
  broken.states.steady_min = 97.0;
  broken.states.worried_min = 95.0;
  broken.states.shaken_min = 90.0;  // spawn morale is 40..80: all break
  broken.recovery.morale_threshold = 1000.0;  // no recovery
  sim::SimulationConfig config;
  config.seed = 5;
  config.max_ticks = 200;
  config.ai_mode = sim::AiMode::Utility;
  config.morale_enabled = true;
  config.morale = &broken;
  config.terrain = &grid;
  config.personalities = &order_personalities();
  config.unit_types = &order_types();
  config.formations = &order_formations();
  config.commanders = &order_commanders();
  config.battlefield_width = grid.width * grid.cell_size;
  config.battlefield_height = grid.height * grid.cell_size;
  config.spawn_band_fraction = 0.15;
  config.composition[kRed].push_back(sim::TypeCount{0, 10});
  config.composition[kRed].push_back(sim::TypeCount{3, 1});
  config.composition[kBlue].push_back(sim::TypeCount{0, 10});
  config.composition[kBlue].push_back(sim::TypeCount{3, 1});
  config.squad_plans[kRed] = sim::SquadPlan{1, 10};
  config.squad_plans[kBlue] = sim::SquadPlan{1, 10};
  sim::Simulation battle(config);
  ASSERT_TRUE(battle.squads_active());
  ASSERT_TRUE(battle.issue_order(sim::Faction::Red, {0},
                                 sim::PlayerOrderType::MoveTo, 170.0,
                                 60.0));
  for (int tick = 0; tick < 60; ++tick) {
    battle.tick();
  }
  std::uint32_t routed_seen = 0;
  for (const sim::Unit& unit : battle.world().units) {
    if (unit.alive && unit.faction == sim::Faction::Red && unit.routed) {
      ++routed_seen;
      EXPECT_EQ(static_cast<sim::Action>(unit.last_action),
                sim::Action::Flee)
          << "a routed unit obeyed an order";
    }
  }
  EXPECT_GT(routed_seen, 0u) << "fixture lost its routed units";
}

TEST(PlayerOrderTest, OrdersAreDeterministicAndGridEqualsBrute) {
  const sim::TerrainTypeTable table = order_table();
  const sim::TerrainGrid grid = corridor_grid(table);
  const auto run = [&grid](bool brute) {
    sim::Simulation battle = corridor_battle(&grid);
    EXPECT_TRUE(battle.issue_order(sim::Faction::Red, {0},
                                   sim::PlayerOrderType::MoveTo, 170.0,
                                   60.0));
    EXPECT_TRUE(battle.issue_order(sim::Faction::Blue, {1},
                                   sim::PlayerOrderType::Hold));
    for (int tick = 0; tick < 200; ++tick) {
      battle.tick();
    }
    // Brute mode must see the same battle: re-run with the brute flag
    // through the config the corridor battle used (perception only).
    if (brute) {
      // The corridor battle uses the grid; the neighbor mode is a
      // config field set before construction. Rebuild with brute.
      sim::SimulationConfig config;
      config.seed = 11;
      config.max_ticks = 600;
      config.ai_mode = sim::AiMode::Utility;
      config.morale_enabled = false;
      config.terrain = &grid;
      config.personalities = &order_personalities();
      config.unit_types = &order_types();
      config.formations = &order_formations();
      config.commanders = &order_commanders();
      config.battlefield_width = grid.width * grid.cell_size;
      config.battlefield_height = grid.height * grid.cell_size;
      config.spawn_band_fraction = 0.15;
      config.neighbor_mode = sim::NeighborMode::Brute;
      config.composition[kRed].push_back(sim::TypeCount{0, 10});
      config.composition[kRed].push_back(sim::TypeCount{3, 1});
      config.composition[kBlue].push_back(sim::TypeCount{0, 10});
      config.composition[kBlue].push_back(sim::TypeCount{3, 1});
      config.squad_plans[kRed] = sim::SquadPlan{1, 10};
      config.squad_plans[kBlue] = sim::SquadPlan{1, 10};
      sim::Simulation brute_battle(config);
      EXPECT_TRUE(brute_battle.issue_order(sim::Faction::Red, {0},
                                           sim::PlayerOrderType::MoveTo,
                                           170.0, 60.0));
      EXPECT_TRUE(brute_battle.issue_order(sim::Faction::Blue, {1},
                                           sim::PlayerOrderType::Hold));
      for (int tick = 0; tick < 200; ++tick) {
        brute_battle.tick();
      }
      return sim::export_state_json(brute_battle);
    }
    return sim::export_state_json(battle);
  };
  const std::string first = run(false);
  const std::string again = run(false);
  EXPECT_EQ(first, again);  // double run: byte-identical
  const std::string brute = run(true);
  EXPECT_EQ(first, brute);  // grid == brute with orders
}

TEST(PlayerOrderTest, NoOrdersLeavesBattlesByteIdentical) {
  // The machinery is inert without orders: same battle, orders on and
  // off, byte-identical exports.
  const sim::TerrainTypeTable table = order_table();
  const sim::TerrainGrid grid = corridor_grid(table);
  const auto run = [&grid](bool orders_enabled) {
    sim::Simulation battle = corridor_battle(&grid, nullptr,
                                             orders_enabled);
    for (int tick = 0; tick < 120; ++tick) {
      battle.tick();
    }
    return sim::export_state_json(battle);
  };
  EXPECT_EQ(run(true), run(false));
}

TEST(PlayerOrderTest, ScriptParserValidates) {
  const fs::path path = "/tmp/m11b_script_test.json";
  {
    std::ofstream file(path);
    file << R"([
      {"tick": 10, "faction": "red", "squads": "all", "order": "hold"},
      {"tick": 20, "faction": "blue", "squads": [1, 2],
       "order": "moveto", "x": 100.5, "y": 200.25},
      {"tick": 30, "faction": "red", "squads": [0],
       "order": "formation", "formation": "column"}
    ])";
  }
  std::vector<cli::OrderIntervention> orders;
  std::string error;
  ASSERT_TRUE(cli::parse_order_script(path.string(), orders, error))
      << error;
  ASSERT_EQ(orders.size(), std::size_t{3});
  EXPECT_EQ(orders[0].tick, 10u);
  EXPECT_TRUE(orders[0].red && orders[0].squads_all);
  EXPECT_EQ(orders[0].type, sim::PlayerOrderType::Hold);
  EXPECT_EQ(orders[1].type, sim::PlayerOrderType::MoveTo);
  EXPECT_TRUE(orders[1].has_point);
  EXPECT_DOUBLE_EQ(orders[1].x, 100.5);
  EXPECT_DOUBLE_EQ(orders[1].y, 200.25);
  EXPECT_EQ(orders[2].formation, "column");

  // Malformed entries are rejected with an error naming the problem.
  const auto check_bad = [&](const char* text, const char* needle) {
    { std::ofstream file(path); file << text; }
    std::vector<cli::OrderIntervention> bad;
    std::string bad_error;
    EXPECT_FALSE(cli::parse_order_script(path.string(), bad, bad_error));
    EXPECT_NE(bad_error.find(needle), std::string::npos) << bad_error;
  };
  check_bad(R"([{"tick": 0, "faction": "red", "squads": "all",
                 "order": "hold"}])",
            "tick");
  check_bad(R"([{"tick": 5, "faction": "green", "squads": "all",
                 "order": "hold"}])",
            "faction");
  check_bad(R"([{"tick": 5, "faction": "red", "squads": "all",
                 "order": "sideways"}])",
            "order type");
  check_bad(R"([{"tick": 5, "faction": "red", "squads": "all",
                 "order": "moveto"}])",
            "moveto");
  check_bad("not json", "JSON");
}

TEST(PlayerOrderTest, ScriptParserHardensTypesAndPoints) {
  // m12 audit: a non-string faction/order entry used to throw an
  // uncaught type_error out of get<std::string> (the try only wrapped
  // the initial parse), 1e999 overflowed to out_of_range.406 — both
  // aborted the CLI — and non-finite points were accepted. All are now
  // clean rejections with an error naming the problem.
  const fs::path path = "/tmp/m12_script_test.json";
  const auto check_bad = [&](const char* text, const char* needle) {
    { std::ofstream file(path); file << text; }
    std::vector<cli::OrderIntervention> bad;
    std::string bad_error;
    EXPECT_FALSE(cli::parse_order_script(path.string(), bad, bad_error));
    EXPECT_NE(bad_error.find(needle), std::string::npos) << bad_error;
  };
  check_bad(R"([{"tick": 5, "faction": 3, "squads": "all",
                 "order": "hold"}])",
            "faction must be a string");
  check_bad(R"([{"tick": 5, "faction": "red", "squads": "all",
                 "order": 7}])",
            "order must be a string");
  check_bad(R"([{"tick": 5, "faction": "red", "squads": "all",
                 "order": "moveto", "x": 1e999, "y": 50}])",
            "not valid JSON");
  check_bad(R"([{"tick": 5, "faction": "red", "squads": "all",
                 "order": "moveto", "x": null, "y": 50}])",
            "x and y must be numbers");
}

TEST(PlayerOrderTest, ReplayFileCarriesOrdersRoundTrip) {
  cli::ReplayData data;
  data.version = sim::kSimVersion;
  data.scenario = "default";
  data.mod = "-";
  data.seed = 42;
  data.ticks = 1000;
  data.orders = "on";
  cli::OrderIntervention order;
  order.tick = 30;
  order.red = true;
  order.squads_all = true;
  order.type = sim::PlayerOrderType::MoveTo;
  order.x = 100.5;
  order.y = 200.25;
  order.has_point = true;
  data.order_interventions.push_back(order);
  cli::OrderIntervention formation;
  formation.tick = 60;
  formation.red = false;
  formation.squads = {3, 4};
  formation.type = sim::PlayerOrderType::ChangeFormation;
  formation.formation = "column";
  data.order_interventions.push_back(formation);
  data.hashes.emplace_back(25, "aabbccddeeff00112233445566778899");
  ASSERT_TRUE(cli::write_replay("/tmp/m11b_roundtrip.replay", data));
  cli::ReplayData parsed;
  std::string error;
  ASSERT_TRUE(
      cli::parse_replay("/tmp/m11b_roundtrip.replay", parsed, error))
      << error;
  ASSERT_EQ(parsed.order_interventions.size(), std::size_t{2});
  EXPECT_EQ(parsed.orders, "on");
  const cli::OrderIntervention& first = parsed.order_interventions[0];
  EXPECT_EQ(first.tick, 30u);
  EXPECT_TRUE(first.red && first.squads_all);
  EXPECT_EQ(first.type, sim::PlayerOrderType::MoveTo);
  EXPECT_DOUBLE_EQ(first.x, 100.5);
  EXPECT_DOUBLE_EQ(first.y, 200.25);
  const cli::OrderIntervention& second = parsed.order_interventions[1];
  EXPECT_EQ(second.squads, (std::vector<std::uint32_t>{3, 4}));
  EXPECT_EQ(second.formation, "column");
  // A file from before m11b (no orders flag) parses as orders=on.
  const std::string legacy =
      "BATTLESIM REPLAY 1\nversion " + std::string(sim::kSimVersion) +
      "\nscenario default\nmod -\nseed 42\nticks 100\n"
      "flags variance=on contact_seek=on neighbor=grid squads=on "
      "swap_sides=off nav=v2\n"
      "hash 25 aabbccddeeff00112233445566778899\n";
  { std::ofstream file("/tmp/m11b_legacy.replay"); file << legacy; }
  cli::ReplayData legacy_parsed;
  ASSERT_TRUE(
      cli::parse_replay("/tmp/m11b_legacy.replay", legacy_parsed, error))
      << error;
  EXPECT_EQ(legacy_parsed.orders, "on");
  EXPECT_TRUE(legacy_parsed.order_interventions.empty());
}

TEST(PlayerOrderTest, CliAppliesScriptAndRecordsIt) {
  // End to end through battle_cli: a scripted moveto redirects the Red
  // army (its export differs from the control), the replay records the
  // order line, and --replay --verify passes.
  const std::string script = "/tmp/m11b_cli_script.json";
  { std::ofstream file(script); file << R"([
    {"tick": 20, "faction": "red", "squads": "all",
     "order": "moveto", "x": 100, "y": 900}
  ])"; }
  const int control = std::system(
      "cd \"" TEST_DATA_DIR "/..\" && \"" BATTLE_CLI_PATH
      "\" --scenario data/scenarios/earth-lebanon-v2.json --seed 3 "
      "--ticks 120 --export-json /tmp/m11b_ctl.json > /dev/null 2>&1");
  ASSERT_EQ(control, 0);
  const std::string ordered_cmd =
      "cd \"" TEST_DATA_DIR "/..\" && \"" BATTLE_CLI_PATH
      "\" --scenario data/scenarios/earth-lebanon-v2.json --seed 3 "
      "--ticks 120 --script " +
      script +
      " --export-json /tmp/m11b_ord.json --record /tmp/m11b_ord.replay "
      "> /dev/null 2>&1";
  const int ordered = std::system(ordered_cmd.c_str());
  ASSERT_EQ(ordered, 0);
  const std::string control_json = read_text("/tmp/m11b_ctl.json");
  const std::string ordered_json = read_text("/tmp/m11b_ord.json");
  EXPECT_NE(control_json, ordered_json)
      << "the scripted order changed nothing";
  EXPECT_NE(ordered_json.find("\"player_orders\""), std::string::npos);
  const std::string replay = read_text("/tmp/m11b_ord.replay");
  EXPECT_NE(replay.find("order 20 red all moveto 100 900"),
            std::string::npos);
  // And the recorded battle verifies.
  const int verify = std::system(
      "cd \"" TEST_DATA_DIR "/..\" && \"" BATTLE_CLI_PATH
      "\" --replay /tmp/m11b_ord.replay --verify > /dev/null 2>&1");
  EXPECT_EQ(verify, 0);
}
