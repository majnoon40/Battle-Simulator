// M8 — terrain, navigation and mods tests. All headless: the terrain
// module and grid queries, the flow field, movement rules (move_cost,
// wall sliding, the impassable invariant over long random battles),
// sight and defense modifiers, load-time validation, map loading (PNG
// via the CLI loader and JSON grids), the export v6 fields, mod loading
// (valid, invalid, partial, missing), the no-map M1-M7 export pins, the
// DrawList terrain layer, and terrain determinism.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cli/map_load.hpp"
#include "cli/mods.hpp"
#include "game/view.hpp"
#include "render/battle_view.hpp"
#include "render/png_writer.hpp"
#include "render/raster.hpp"
#include "sim/config.hpp"
#include "sim/flow_field.hpp"
#include "sim/json_export.hpp"
#include "sim/morale.hpp"
#include "sim/personalities.hpp"
#include "sim/simulation.hpp"
#include "sim/squads.hpp"
#include "sim/terrain.hpp"
#include "sim/unit_types.hpp"

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kRed = static_cast<std::size_t>(sim::Faction::Red);
constexpr std::size_t kBlue = static_cast<std::size_t>(sim::Faction::Blue);

std::string read_text(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

sim::Unit make_unit(std::uint32_t id, sim::Faction faction, double x,
                    double y) {
  sim::Unit unit;
  unit.id = id;
  unit.faction = faction;
  unit.x = x;
  unit.y = y;
  unit.health = 100.0;
  unit.stats.max_health = 100.0;
  unit.stats.damage = 12.0;
  unit.stats.speed = 2.0;
  unit.stats.attack_range = 2.0;
  unit.stats.attack_cooldown_ticks = 5;
  unit.stats.sight_radius = 40.0;
  unit.alive = true;
  unit.morale = 80.0;
  return unit;
}

// A tiny terrain table for in-process tests (ids also used by the grid
// builders below). One type per rule so effects are unambiguous.
sim::TerrainTypeTable test_table() {
  sim::TerrainTypeTable table;
  auto add = [&](const char* id, std::uint8_t r, std::uint8_t g,
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
  add("forest", 0x3E, 0x6B, 0x3A, 0.6, true, 0.65, 0.85);
  add("hills", 0xA0, 0x8A, 0x5A, 0.5, true, 1.15, 0.75);
  add("water", 0x3D, 0x6E, 0x9E, 0.0, false, 1.0, 1.0);
  add("road", 0xB8, 0xA7, 0x7E, 1.3, true, 1.0, 1.05);
  return table;
}

std::uint16_t type_index(const sim::TerrainTypeTable& table,
                         const std::string& id) {
  for (std::size_t i = 0; i < table.types.size(); ++i) {
    if (table.types[i].id == id) {
      return static_cast<std::uint16_t>(i);
    }
  }
  return sim::TerrainGrid::kNoCell;
}

// A grid from id rows (all rows same length).
sim::TerrainGrid grid_from(const sim::TerrainTypeTable& table,
                           const std::vector<std::string>& rows,
                           double cell_size,
                           const std::string& id = "test") {
  const std::uint32_t height =
      static_cast<std::uint32_t>(rows.size());
  const std::uint32_t width =
      static_cast<std::uint32_t>(rows.empty() ? 0 : rows[0].size());
  std::vector<std::uint16_t> cells;
  cells.reserve(static_cast<std::size_t>(width) * height);
  for (const std::string& row : rows) {
    for (char cell : row) {
      const char* id = cell == 'g' ? "grassland"
                       : cell == 'f' ? "forest"
                       : cell == 'h' ? "hills"
                       : cell == 'w' ? "water"
                       : cell == 'r' ? "road"
                                     : nullptr;
      assert(id != nullptr);
      cells.push_back(type_index(table, id));
    }
  }
  return sim::build_terrain_grid(id, width, height, cell_size, cells,
                                 &table);
}

// 'g' grassland, 'f' forest, 'h' hills, 'w' water, 'r' road.
sim::TerrainGrid uniform_grid(const sim::TerrainTypeTable& table,
                              const std::string& type_id,
                              std::uint32_t width, std::uint32_t height,
                              double cell_size) {
  const std::uint16_t index = type_index(table, type_id);
  return sim::build_terrain_grid(
      "uniform", width, height, cell_size,
      std::vector<std::uint16_t>(
          static_cast<std::size_t>(width) * height, index),
      &table);
}

// A legacy-mode battle of explicit units on a map (legacy targeting and
// attacks have no AI action gating, so terrain effects are isolated).
sim::Simulation legacy_battle(const sim::TerrainGrid& grid,
                              std::vector<sim::Unit> units) {
  sim::SimulationConfig config;
  config.seed = 1;
  config.max_ticks = 1000;
  config.ai_mode = sim::AiMode::Legacy;
  config.morale_enabled = false;
  config.terrain = &grid;
  config.battlefield_width = grid.width * grid.cell_size;
  config.battlefield_height = grid.height * grid.cell_size;
  config.spawn_band_fraction = 0.15;
  return sim::Simulation(config, units);
}

// Runs a CLI command; returns the exit code, with stdout+stderr in
// `output`.
int run_cli(const std::string& arguments, std::string* output = nullptr) {
  // Run from the source root so the CLI's relative data/ paths resolve.
  const std::string command =
      "cd \"" TEST_DATA_DIR "/..\" && \"" BATTLE_CLI_PATH "\" " +
      arguments + " > /tmp/m8_cli_out.txt 2>&1";
  const int code = std::system(command.c_str());
  if (output != nullptr) {
    *output = read_text("/tmp/m8_cli_out.txt");
  }
  return code;
}

std::string export_via_cli(const std::string& arguments) {
  const std::string path = "/tmp/m8_export.json";
  fs::remove(path);
  // M10: the byte-pinned M1-M9 baselines are reproduced with tactical
  // depth off (--variance off); the default (on) widens action_counts to
  // eight actions and bumps the export format to 7.
  const int code =
      run_cli(arguments + " --variance off --export-json " + path);
  EXPECT_EQ(code, 0);
  return read_text(path);
}

}  // namespace

// ---------------------------------------------------------------------------
// Terrain table parsing and validation.
// ---------------------------------------------------------------------------

TEST(TerrainTableTest, ParsesTheShippedTable) {
  const sim::TerrainTypeTable table = sim::parse_terrain_types(
      read_text(fs::path(TEST_DATA_DIR) / "terrain.json"));
  ASSERT_GE(table.types.size(), 8u);
  EXPECT_EQ(table.types[0].id, "grassland");
  // The shipped set: grassland, forest, hills, water, road, mountain,
  // obstacle and ford.
  bool has[8] = {false, false, false, false, false, false, false, false};
  const char* wanted[8] = {"grassland", "forest",    "hills",  "water",
                           "road",      "mountain", "obstacle", "ford"};
  for (const sim::TerrainType& type : table.types) {
    for (int i = 0; i < 8; ++i) {
      has[i] = has[i] || type.id == wanted[i];
    }
  }
  for (int i = 0; i < 8; ++i) {
    EXPECT_TRUE(has[i]) << wanted[i] << " missing";
  }
  // Water is impassable, road is fast, forest blocks sight.
  const sim::TerrainType& water = table.types[type_index(table, "water")];
  EXPECT_FALSE(water.passable);
  const sim::TerrainType& road = table.types[type_index(table, "road")];
  EXPECT_GT(road.move_cost, 1.0);
  const sim::TerrainType& forest = table.types[type_index(table, "forest")];
  EXPECT_LT(forest.sight_modifier, 1.0);
}

TEST(TerrainTableTest, RejectsBrokenTables) {
  const auto rejects = [](const std::string& text) {
    try {
      sim::parse_terrain_types(text);
    } catch (const std::invalid_argument& error) {
      return std::string(error.what());
    }
    return std::string();
  };
  EXPECT_NE(rejects("{}"), "");                       // no types
  EXPECT_NE(rejects("{\"types\": []}"), "");          // empty
  EXPECT_NE(rejects("{\"types\": [{\"id\": \"a\"}]}"), "");  // missing fields
  // Duplicate ids.
  EXPECT_NE(rejects("{\"types\": ["
                    "{\"id\":\"a\",\"color\":\"#111111\",\"move_cost\":1,"
                    "\"sight_modifier\":1,\"defense_modifier\":1},"
                    "{\"id\":\"a\",\"color\":\"#222222\",\"move_cost\":1,"
                    "\"sight_modifier\":1,\"defense_modifier\":1}]}"),
            "");
  // Duplicate colors.
  EXPECT_NE(rejects("{\"types\": ["
                    "{\"id\":\"a\",\"color\":\"#111111\",\"move_cost\":1,"
                    "\"sight_modifier\":1,\"defense_modifier\":1},"
                    "{\"id\":\"b\",\"color\":\"#111111\",\"move_cost\":1,"
                    "\"sight_modifier\":1,\"defense_modifier\":1}]}"),
            "");
  // Non-positive move_cost on a passable type.
  EXPECT_NE(rejects("{\"types\": ["
                    "{\"id\":\"a\",\"color\":\"#111111\",\"move_cost\":0,"
                    "\"sight_modifier\":1,\"defense_modifier\":1}]}"),
            "");
  // No passable type at all.
  EXPECT_NE(rejects("{\"types\": ["
                    "{\"id\":\"a\",\"color\":\"#111111\",\"move_cost\":1,"
                    "\"passable\":false,\"sight_modifier\":1,"
                    "\"defense_modifier\":1}]}"),
            "");
  // Malformed JSON.
  EXPECT_NE(rejects("not json"), "");
}

// ---------------------------------------------------------------------------
// Grid queries, bounds handling and grid building validation.
// ---------------------------------------------------------------------------

TEST(TerrainGridTest, LookupBoundsAndRules) {
  const sim::TerrainTypeTable table = test_table();
  const sim::TerrainGrid grid =
      grid_from(table, {"gf", "hw"}, 10.0);
  ASSERT_EQ(grid.width, 2u);
  ASSERT_EQ(grid.height, 2u);
  // O(1) lookups by cell: row 0 is grassland|forest, row 1 hills|water.
  EXPECT_TRUE(grid.passable_at(5.0, 5.0));    // grassland
  EXPECT_TRUE(grid.passable_at(15.0, 5.0));   // forest
  EXPECT_TRUE(grid.passable_at(5.0, 15.0));   // hills
  EXPECT_FALSE(grid.passable_at(15.0, 15.0)); // water
  // Out of bounds is impassable and keeps sight/defense neutral.
  EXPECT_FALSE(grid.passable_at(-1.0, 5.0));
  EXPECT_FALSE(grid.passable_at(25.0, 25.0));
  EXPECT_DOUBLE_EQ(grid.sight_scale_at(-1.0, -1.0, 2.0), 1.0);
  EXPECT_DOUBLE_EQ(grid.defense_factor_at(-1.0, -1.0), 1.0);
  // Speed multipliers: forest 0.6, hills 0.5, water 0 (impassable).
  EXPECT_DOUBLE_EQ(grid.speed_multiplier_at(15.0, 5.0), 0.6);
  EXPECT_DOUBLE_EQ(grid.speed_multiplier_at(5.0, 15.0), 0.5);
  EXPECT_DOUBLE_EQ(grid.speed_multiplier_at(15.0, 15.0), 0.0);
  // Defense: hills 0.75, grassland 1.
  EXPECT_DOUBLE_EQ(grid.defense_factor_at(5.0, 15.0), 0.75);
  EXPECT_DOUBLE_EQ(grid.defense_factor_at(5.0, 5.0), 1.0);
  // Sight scale clamps at the configured maximum.
  const sim::TerrainGrid hilly = uniform_grid(table, "hills", 4, 4, 10.0);
  EXPECT_DOUBLE_EQ(hilly.sight_scale_at(5.0, 5.0, 2.0), 1.15);
  EXPECT_DOUBLE_EQ(hilly.sight_scale_at(5.0, 5.0, 1.0), 1.0);
}

TEST(TerrainGridTest, BuildValidation) {
  const sim::TerrainTypeTable table = test_table();
  const std::vector<std::uint16_t> ok(4, 0);
  const auto build = [&](std::uint32_t w, std::uint32_t h, double cell,
                         const std::vector<std::uint16_t>& cells) {
    try {
      sim::build_terrain_grid("x", w, h, cell, cells, &table);
    } catch (const std::invalid_argument& error) {
      return std::string(error.what());
    }
    return std::string();
  };
  EXPECT_NE(build(0, 4, 10.0, ok), "");          // zero size
  EXPECT_NE(build(2, 2, 10.0, {0, 1, 2}), "");   // cell count mismatch
  EXPECT_NE(build(2, 2, 0.0, ok), "");           // cell size
  EXPECT_NE(build(2, 2, 10.0, {0, 1, 2, 99}), "");  // index out of range
  EXPECT_EQ(build(2, 2, 10.0, ok), "");          // valid
}

// ---------------------------------------------------------------------------
// Flow field: around a wall, unreachable pockets, impassable targets.
// ---------------------------------------------------------------------------

TEST(FlowFieldTest, GoesAroundAWall) {
  const sim::TerrainTypeTable table = test_table();
  // A 10x10 field, water wall at x = 5 except a gap at y = 0.
  std::vector<std::string> rows;
  for (std::uint32_t y = 0; y < 10; ++y) {
    std::string row = "gggggggggg";
    if (y != 0) {
      row[5] = 'w';
    }
    rows.push_back(row);
  }
  const sim::TerrainGrid grid = grid_from(table, rows, 1.0);
  const sim::FlowField field = sim::compute_flow_field(grid, 9, 5);
  EXPECT_TRUE(field.target_valid);
  EXPECT_GT(field.reached_cells, 0u);
  // Walking the directions from (1, 5) must reach the target without
  // ever stepping onto impassable ground.
  std::uint32_t cx = 1;
  std::uint32_t cy = 5;
  for (int step = 0; step < 100; ++step) {
    double dx = 0.0;
    double dy = 0.0;
    if (!sim::flow_direction(field, cx, cy, dx, dy)) {
      FAIL() << "no direction at (" << cx << ", " << cy << ")";
    }
    cx = static_cast<std::uint32_t>(cx + dx + 0.5);
    cy = static_cast<std::uint32_t>(cy + dy + 0.5);
    EXPECT_TRUE(grid.passable_at(cx + 0.5, cy + 0.5))
        << "stepped into impassable cell (" << cx << ", " << cy << ")";
    if (cx == 9 && cy == 5) {
      break;
    }
  }
  EXPECT_EQ(cx, 9u);
  EXPECT_EQ(cy, 5u);
  // The cost is finite everywhere passable (the gap connects the field).
  for (std::uint32_t y = 0; y < 10; ++y) {
    for (std::uint32_t x = 0; x < 10; ++x) {
      if (grid.passable_at(x + 0.5, y + 0.5)) {
        EXPECT_LT(field.cost[static_cast<std::size_t>(y) * 10 + x],
                  sim::FlowField::kUnreached)
            << "cell (" << x << ", " << y << ") unreached";
      }
    }
  }
}

TEST(FlowFieldTest, SealedPocketAndImpassableTarget) {
  const sim::TerrainTypeTable table = test_table();
  // A sealed ring of water around (5,5), target inside the ring: cells
  // outside are unreached.
  std::vector<std::string> rows;
  for (std::uint32_t y = 0; y < 11; ++y) {
    std::string row = "ggggggggggg";
    for (std::uint32_t x = 0; x < 11; ++x) {
      const std::uint32_t dx = x > 5 ? x - 5 : 5 - x;
      const std::uint32_t dy = y > 5 ? y - 5 : 5 - y;
      if (dx == 3 && dy <= 3) {
        row[x] = 'w';
      }
      if (dy == 3 && dx <= 3) {
        row[x] = 'w';
      }
    }
    rows.push_back(row);
  }
  const sim::TerrainGrid grid = grid_from(table, rows, 1.0);
  const sim::FlowField field = sim::compute_flow_field(grid, 5, 5);
  EXPECT_TRUE(field.target_valid);
  // Inside the ring (radius < 3): reached. Outside: unreached.
  EXPECT_LT(field.cost[static_cast<std::size_t>(5) * 11 + 5],
            sim::FlowField::kUnreached);
  EXPECT_GE(field.cost[static_cast<std::size_t>(0) * 11 + 0],
            sim::FlowField::kUnreached);
  EXPECT_GT(field.reached_cells, 0u);

  // A target on impassable ground has no field at all.
  const sim::FlowField water_target =
      sim::compute_flow_field(grid, 8, 5);  // the ring itself
  EXPECT_FALSE(water_target.target_valid);
}

// ---------------------------------------------------------------------------
// Movement: move_cost, impassable blocks, wall sliding, the invariant.
// ---------------------------------------------------------------------------

TEST(TerrainMovementTest, ForestSlowsTheCrossing) {
  const sim::TerrainTypeTable table = test_table();
  const auto crossed = [&table](const std::string& type) {
    const sim::TerrainGrid grid = uniform_grid(table, type, 20, 10, 10.0);
    sim::Simulation battle =
        legacy_battle(grid, {make_unit(1, sim::Faction::Red, 5.0, 50.0),
                             make_unit(2, sim::Faction::Blue, 195.0, 50.0)});
    for (int tick = 0; tick < 20; ++tick) {
      battle.tick();
    }
    const sim::Unit& red = battle.world().units[0];
    return red.x - 5.0;
  };
  const double grass = crossed("grassland");
  const double forest = crossed("forest");
  // 20 ticks at speed 2: about 40 on grassland, 24 in forest (0.6), and
  // strictly less either way.
  EXPECT_GT(grass, 38.0);
  EXPECT_LT(forest, 26.0);
  EXPECT_LT(forest, grass);
}

TEST(TerrainMovementTest, ImpassableBlocksAndWallSlides) {
  const sim::TerrainTypeTable table = test_table();
  // A water wall at x in [50, 60) with the top row open (so the bands
  // stay connected and the battle validates).
  std::vector<std::string> rows;
  for (std::uint32_t y = 0; y < 10; ++y) {
    rows.push_back(y == 0 ? "gggggggggggggggggggg"
                          : "gggggwwwwwwwwwwggggg");
  }
  const sim::TerrainGrid grid = grid_from(table, rows, 10.0);
  // Straight east: blocked, stays west of the wall.
  {
    sim::Simulation battle =
        legacy_battle(grid, {make_unit(1, sim::Faction::Red, 45.0, 50.0),
                             make_unit(2, sim::Faction::Blue, 195.0, 50.0)});
    for (int tick = 0; tick < 30; ++tick) {
      battle.tick();
      ASSERT_LT(battle.world().units[0].x, 50.0)
          << "unit entered the water";
    }
  }
  // Northeast into the wall: x is blocked, y still slides.
  {
    sim::Simulation battle =
        legacy_battle(grid, {make_unit(1, sim::Faction::Red, 48.5, 90.0),
                             make_unit(2, sim::Faction::Blue, 195.0, 10.0)});
    battle.tick();
    const sim::Unit& red = battle.world().units[0];
    EXPECT_DOUBLE_EQ(red.x, 48.5);  // the eastern step is blocked
    EXPECT_LT(red.y, 90.0);         // the northern step still slides
    EXPECT_TRUE(grid.passable_at(red.x, red.y));
  }
}

TEST(TerrainMovementTest, NoUnitEverInsideImpassableOver1000Ticks) {
  const sim::TerrainTypeTable table = test_table();
  // A messy 30x30 map: water river, obstacle clusters, forest and hills.
  std::vector<std::string> rows;
  for (std::uint32_t y = 0; y < 30; ++y) {
    std::string row(30, 'g');
    row[(y * 7 + 13) % 30] = 'w';
    if (y % 5 == 2) {
      row[(y * 3 + 4) % 30] = 'f';
      row[(y * 3 + 5) % 30] = 'h';
    }
    rows.push_back(row);
  }
  const sim::TerrainGrid grid = grid_from(table, rows, 10.0);
  const sim::PersonalityTable personalities = sim::parse_personalities(
      read_text(fs::path(TEST_DATA_DIR) / "personalities.json"));
  const sim::MoraleConfig morale =
      sim::parse_morale(read_text(fs::path(TEST_DATA_DIR) / "morale.json"));
  for (std::uint64_t seed : {3u, 41u, 99u}) {
    sim::SimulationConfig config;
    config.seed = seed;
    config.max_ticks = 1001;
    config.ai_mode = sim::AiMode::Utility;
    config.morale_enabled = true;
    config.morale = &morale;
    config.personalities = &personalities;
    config.terrain = &grid;
    config.battlefield_width = grid.width * grid.cell_size;
    config.battlefield_height = grid.height * grid.cell_size;
    config.spawn_band_fraction = 0.15;
    // 200 infantry per side via the legacy stats path.
    config.total_units = 400;
    config.red_stats = sim::UnitStats{100.0, 12.0, 2.0, 2.0, 5, 40.0};
    config.blue_stats = config.red_stats;
    sim::Simulation battle(config);
    for (int tick = 0; tick < 1000; ++tick) {
      battle.tick();
      for (const sim::Unit& unit : battle.world().units) {
        if (unit.alive) {
          ASSERT_TRUE(grid.passable_at(unit.x, unit.y))
              << "seed " << seed << " tick " << tick << " unit " << unit.id
              << " at (" << unit.x << ", " << unit.y << ")";
        }
      }
    }
    EXPECT_FALSE(battle.finished() && battle.result().ticks_elapsed == 0);
  }
}

// ---------------------------------------------------------------------------
// Sight and defense modifiers in controlled battles.
// ---------------------------------------------------------------------------

TEST(TerrainEffectsTest, ForestHidesTheObserver) {
  const sim::TerrainTypeTable table = test_table();
  // Red at 30 world units from Blue: inside the base sight (40), outside
  // the forest-scaled sight (40 * 0.65 = 26).
  const auto visible_enemies = [&table](const std::string& type) {
    const sim::TerrainGrid grid = uniform_grid(table, type, 20, 10, 10.0);
    sim::SimulationConfig config;
    config.seed = 1;
    config.max_ticks = 10;
    config.ai_mode = sim::AiMode::Utility;
    config.morale_enabled = false;
    config.terrain = &grid;
    config.battlefield_width = grid.width * grid.cell_size;
    config.battlefield_height = grid.height * grid.cell_size;
    sim::Simulation battle(config,
                           {make_unit(1, sim::Faction::Red, 50.0, 50.0),
                            make_unit(2, sim::Faction::Blue, 80.0, 50.0)});
    sim::AiDebugCapture capture;
    capture.unit_id = 1;
    capture.tick = 1;
    capture.explicit_tick = true;
    battle.set_ai_debug(capture);
    battle.tick();
    EXPECT_TRUE(battle.ai_debug().captured);
    return battle.ai_debug().visible_enemies;
  };
  EXPECT_GE(visible_enemies("grassland"), 1u);
  EXPECT_EQ(visible_enemies("forest"), 0u);
}

TEST(TerrainEffectsTest, HillsAbsorbDamage) {
  const sim::TerrainTypeTable table = test_table();
  // Two legacy units in range; the victim stands on hills (0.75) or
  // grassland (1.0). One tick, one attack: 12 vs 9 damage.
  const auto damage_taken = [&table](const std::string& type) {
    const sim::TerrainGrid grid = uniform_grid(table, type, 20, 10, 10.0);
    sim::Simulation battle =
        legacy_battle(grid, {make_unit(1, sim::Faction::Red, 50.0, 50.0),
                             make_unit(2, sim::Faction::Blue, 51.0, 50.0)});
    battle.tick();
    return 100.0 - battle.world().units[1].health;
  };
  EXPECT_DOUBLE_EQ(damage_taken("grassland"), 12.0);
  EXPECT_DOUBLE_EQ(damage_taken("hills"), 9.0);
}

// ---------------------------------------------------------------------------
// Load-time validation via the Simulation constructor.
// ---------------------------------------------------------------------------

TEST(TerrainValidationTest, RejectsBrokenBattles) {
  const sim::TerrainTypeTable table = test_table();
  const auto throws = [](const sim::TerrainGrid& grid,
                         std::vector<sim::Unit> units = {}) {
    try {
      sim::SimulationConfig config;
      config.seed = 1;
      config.max_ticks = 10;
      config.ai_mode = sim::AiMode::Utility;
      config.morale_enabled = false;
      config.terrain = &grid;
      config.battlefield_width = grid.width * grid.cell_size;
      config.battlefield_height = grid.height * grid.cell_size;
      config.spawn_band_fraction = 0.15;
      sim::Simulation(config, units);
    } catch (const std::invalid_argument&) {
      return true;
    }
    return false;
  };

  // Spawn band with no passable cells: the western 15% is all water.
  {
    std::vector<std::string> rows;
    for (std::uint32_t y = 0; y < 10; ++y) {
      rows.push_back("wwgggggggg");
    }
    EXPECT_TRUE(throws(grid_from(table, rows, 10.0)));
  }
  // Objective unreachable: a water wall seals the west band from the
  // east (default objectives are the opposing band centers).
  {
    std::vector<std::string> rows;
    for (std::uint32_t y = 0; y < 10; ++y) {
      rows.push_back("gggwwwwggg");
    }
    EXPECT_TRUE(throws(grid_from(table, rows, 10.0)));
  }
  // Explicit unit standing inside impassable terrain.
  {
    const sim::TerrainGrid grid =
        grid_from(table, {"ggggg", "gwwww", "ggggg"}, 10.0);
    EXPECT_TRUE(throws(grid, {make_unit(1, sim::Faction::Red, 25.0, 15.0)}));
  }
  // Battlefield size must match the map.
  {
    const sim::TerrainGrid grid = uniform_grid(table, "grassland", 4, 4, 10.0);
    try {
      sim::SimulationConfig config;
      config.seed = 1;
      config.max_ticks = 10;
      config.terrain = &grid;
      config.battlefield_width = 999.0;  // not 4 * 10
      config.battlefield_height = 40.0;
      sim::Simulation reject(config);
    } catch (const std::invalid_argument&) {
      SUCCEED();
      return;
    }
    FAIL() << "size mismatch was not rejected";
  }
}

// ---------------------------------------------------------------------------
// Formation slots never sit on impassable cells.
// ---------------------------------------------------------------------------

TEST(TerrainSlotsTest, SlotsNudgedOffImpassable) {
  const sim::TerrainTypeTable table = test_table();
  // A squads battle next to water: every slot of every squad must be on
  // passable ground on every tick.
  const sim::UnitTypeTable types = sim::parse_unit_types(
      read_text(fs::path(TEST_DATA_DIR) / "units.json"));
  const sim::PersonalityTable personalities = sim::parse_personalities(
      read_text(fs::path(TEST_DATA_DIR) / "personalities.json"));
  const sim::FormationConfig formations = sim::parse_formations(
      read_text(fs::path(TEST_DATA_DIR) / "formations.json"));
  const sim::CommanderConfig commanders = sim::parse_commanders(
      read_text(fs::path(TEST_DATA_DIR) / "commanders.json"));
  const sim::MoraleConfig morale =
      sim::parse_morale(read_text(fs::path(TEST_DATA_DIR) / "morale.json"));
  // Water strip in the middle column band.
  std::vector<std::string> rows;
  for (std::uint32_t y = 0; y < 20; ++y) {
    std::string row(20, 'g');
    if (y != 9 && y != 10) {  // a land bridge keeps the bands reachable
      for (std::uint32_t x = 8; x < 12; ++x) {
        row[x] = 'w';
      }
    }
    rows.push_back(row);
  }
  const sim::TerrainGrid grid = grid_from(table, rows, 10.0);
  sim::SimulationConfig config;
  config.seed = 7;
  config.max_ticks = 60;
  config.ai_mode = sim::AiMode::Utility;
  config.morale_enabled = true;
  config.morale = &morale;
  config.personalities = &personalities;
  config.unit_types = &types;
  config.terrain = &grid;
  config.battlefield_width = grid.width * grid.cell_size;
  config.battlefield_height = grid.height * grid.cell_size;
  config.spawn_band_fraction = 0.15;
  config.composition[kRed].push_back(sim::TypeCount{0, 30});
  config.composition[kRed].push_back(sim::TypeCount{3, 3});
  config.composition[kBlue].push_back(sim::TypeCount{0, 30});
  config.composition[kBlue].push_back(sim::TypeCount{3, 3});
  config.squad_plans[kRed] = sim::SquadPlan{3, 10};
  config.squad_plans[kBlue] = sim::SquadPlan{3, 10};
  config.formations = &formations;
  config.commanders = &commanders;
  sim::Simulation battle(config);
  ASSERT_TRUE(battle.squads_active());
  for (int tick = 0; tick < 30; ++tick) {
    battle.tick();
    for (const sim::Squad& squad : battle.squads()) {
      for (std::size_t m = 0; m < squad.members.size(); ++m) {
        double x = 0.0;
        double y = 0.0;
        ASSERT_TRUE(battle.squad_slot_position(
            static_cast<std::int32_t>(squad.id), m, x, y));
        ASSERT_TRUE(grid.passable_at(x, y))
            << "tick " << tick << " squad " << squad.id << " member " << m
            << " slot at (" << x << ", " << y << ")";
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Advance follows the flow field; stuck detection recovers.
// ---------------------------------------------------------------------------

TEST(TerrainNavigationTest, AdvanceCrossesAWallViaTheGap) {
  const sim::TerrainTypeTable table = test_table();
  // A wall at x in [50, 60) with one gap at y in [10, 20); a lone Red
  // unit west of the wall, no enemies: the utility AI advances toward
  // the objective (the far east band center), the straight line is
  // blocked, so the flow field must route it through the gap.
  std::vector<std::string> rows;
  for (std::uint32_t y = 0; y < 20; ++y) {
    std::string row(20, 'g');
    for (std::uint32_t x = 5; x < 6; ++x) {
      row[x] = y >= 1 && y <= 2 ? 'g' : 'w';
    }
    rows.push_back(row);
  }
  const sim::TerrainGrid grid = grid_from(table, rows, 10.0);
  sim::SimulationConfig config;
  config.seed = 5;
  config.max_ticks = 400;
  config.ai_mode = sim::AiMode::Utility;
  config.morale_enabled = false;
  config.terrain = &grid;
  config.battlefield_width = grid.width * grid.cell_size;
  config.battlefield_height = grid.height * grid.cell_size;
  config.spawn_band_fraction = 0.15;
  sim::Unit red = make_unit(1, sim::Faction::Red, 45.0, 95.0);
  red.stats.damage = 40.0;  // strong enough to survive the meeting
  sim::Unit blue = make_unit(2, sim::Faction::Blue, 185.0, 95.0);
  blue.health = 10.0;  // harmless bait far east of the wall
  sim::Simulation battle(config, {red, blue});
  for (int tick = 0; tick < 350; ++tick) {
    battle.tick();
    for (const sim::Unit& unit : battle.world().units) {
      if (unit.alive) {
        ASSERT_TRUE(grid.passable_at(unit.x, unit.y));
      }
    }
  }
  // Red got past the wall (x >= 60): only the flow field path through
  // the gap can do that, and stuck detection kept it moving.
  const sim::Unit& final_red = battle.world().units[0];
  ASSERT_TRUE(final_red.alive);
  EXPECT_GE(final_red.x, 60.0);
}

// ---------------------------------------------------------------------------
// Map loading: PNG (via the CLI loader) and JSON grids.
// ---------------------------------------------------------------------------

TEST(MapLoadingTest, PngRoundTripAndErrors) {
  const sim::TerrainTypeTable table = test_table();
  // A 3x2 map of exact palette colors, written with the render PNG
  // writer, loaded back through the CLI loader.
  render::Image image;
  image.width = 3;
  image.height = 2;
  image.pixels.resize(18);
  const std::uint8_t* colors[6] = {&table.types[0].r, &table.types[1].r,
                                   &table.types[2].r, &table.types[0].r,
                                   &table.types[3].r, &table.types[0].r};
  for (std::size_t i = 0; i < 6; ++i) {
    image.pixels[i * 3] = colors[i][0];
    image.pixels[i * 3 + 1] = colors[i][1];
    image.pixels[i * 3 + 2] = colors[i][2];
  }
  ASSERT_TRUE(render::write_png(image, "/tmp/m8_map_ok.png"));
  const sim::TerrainGrid grid =
      cli::load_map("/tmp/m8_map_ok.png", 5.0, table);
  EXPECT_EQ(grid.width, 3u);
  EXPECT_EQ(grid.height, 2u);
  EXPECT_EQ(grid.cells[0], type_index(table, "grassland"));
  EXPECT_EQ(grid.cells[1], type_index(table, "forest"));
  EXPECT_EQ(grid.cells[2], type_index(table, "hills"));
  EXPECT_EQ(grid.cells[4], type_index(table, "water"));
  EXPECT_FALSE(grid.passable_at(5.0, 15.0));  // the water pixel

  // An unknown color is a readable error naming the pixel.
  image.pixels[5] = 0xFF;  // no terrain type is #FF6B3A
  ASSERT_TRUE(render::write_png(image, "/tmp/m8_map_bad.png"));
  try {
    cli::load_map("/tmp/m8_map_bad.png", 5.0, table);
    FAIL() << "unknown color accepted";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("pixel"), std::string::npos);
    EXPECT_NE(std::string(error.what()).find("no terrain type"),
              std::string::npos);
  }

  // The shipped demo map loads and has impassable water. The table must
  // outlive the grid: the grid stores a non-owning pointer to it (a
  // temporary here would dangle and ASan catches it at demo.table below).
  const sim::TerrainTypeTable demo_table = sim::parse_terrain_types(
      read_text(fs::path(TEST_DATA_DIR) / "terrain.json"));
  const sim::TerrainGrid demo = cli::load_map(
      (fs::path(TEST_DATA_DIR) / "maps/demo-river.png").string(), 5.0,
      demo_table);
  EXPECT_EQ(demo.width, 200u);
  EXPECT_EQ(demo.height, 200u);
  // The bridge row (y = 100) is passable road across the river.
  bool road_seen = false;
  for (std::uint32_t x = 0; x < demo.width; ++x) {
    if (demo.cells[100 * demo.width + x] ==
        type_index(*demo.table, "road")) {
      road_seen = true;
    }
  }
  EXPECT_TRUE(road_seen);
}

TEST(MapLoadingTest, JsonGridMaps) {
  const sim::TerrainTypeTable table = test_table();
  // Valid (the example mod's swampy map uses the shipped table shapes;
  // here a small hand grid).
  const sim::TerrainGrid grid = sim::parse_terrain_grid_json(
      R"json({"id": "tiny", "grid": [["grassland", "water"],
                                      ["forest", "hills"]]})json",
      10.0, &table);
  EXPECT_EQ(grid.width, 2u);
  EXPECT_EQ(grid.height, 2u);
  EXPECT_FALSE(grid.passable_at(15.0, 5.0));
  EXPECT_EQ(grid.map_id, "tiny");
  // Unknown type.
  EXPECT_THROW(sim::parse_terrain_grid_json(
                   R"json({"grid": [["lava"]]})json", 10.0, &table),
               std::invalid_argument);
  // Ragged rows.
  EXPECT_THROW(sim::parse_terrain_grid_json(
                   R"json({"grid": [["grassland"], ["grassland", "water"]]})json",
                   10.0, &table),
               std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Mods: valid (behavior change without recompiling), invalid, partial,
// missing.
// ---------------------------------------------------------------------------

TEST(ModTest, ExampleModChangesBehaviorWithoutRecompiling) {
  const fs::path mod = fs::path(TEST_DATA_DIR) / "mods/example";
  // With the mod: infantry has 120 health and the swamp map loads.
  const std::string with_mod =
      export_via_cli("--mod " + mod.string() + " --scenario swamp-war "
                     "--seed 7 --ticks 1");
  EXPECT_NE(with_mod.find("\"format_version\": 6"), std::string::npos);
  // The swampy map's grid uses the 'swamp' type, so this load only
  // succeeds with the mod's terrain table (9 types) active.
  EXPECT_NE(with_mod.find("\"id\": \"swampy\""), std::string::npos);
  // Every infantry unit starts at 120 hp with the mod.
  EXPECT_NE(with_mod.find("\"health\": 120.0"), std::string::npos);
  EXPECT_EQ(with_mod.find("\"health\": 100.0"), std::string::npos);

  // Without the mod: the same scenario name does not exist and infantry
  // has 100 hp (the shipped table).
  const std::string base = export_via_cli("--scenario default --seed 7 "
                                          "--ticks 1");
  EXPECT_NE(base.find("\"health\": 100.0"), std::string::npos);
}

TEST(ModTest, InvalidModRejectedWholesale) {
  fs::path dir = fs::temp_directory_path() / "m8_bad_mod";
  fs::remove_all(dir);
  fs::create_directories(dir);
  // A broken units.json plus a valid terrain.json: the whole mod must be
  // rejected, with a readable error naming the broken file.
  std::ofstream(dir / "units.json") << "{ not json";
  std::ofstream(dir / "terrain.json")
      << read_text(fs::path(TEST_DATA_DIR) / "terrain.json");
  std::string output;
  const int code = run_cli("--mod " + dir.string() +
                               " --scenario default --seed 1",
                           &output);
  EXPECT_NE(code, 0);
  EXPECT_NE(output.find("units.json"), std::string::npos);
  fs::remove_all(dir);
}

TEST(ModTest, PartialModAndMissingFiles) {
  // A mod with only terrain.json: valid, the battle runs on built-in
  // units with the mod's terrain table available.
  fs::path dir = fs::temp_directory_path() / "m8_partial_mod";
  fs::remove_all(dir);
  fs::create_directories(dir);
  std::ofstream(dir / "terrain.json")
      << read_text(fs::path(TEST_DATA_DIR) / "terrain.json");
  EXPECT_EQ(run_cli("--mod " + dir.string() +
                    " --scenario default --seed 1 --ticks 1"),
            0);
  fs::remove_all(dir);

  // A missing mod directory is rejected by argument parsing.
  EXPECT_NE(run_cli("--mod /tmp/definitely_not_a_mod --scenario default"),
            0);

  // A scenario naming a missing map is rejected with a readable error.
  fs::path scenario = fs::temp_directory_path() / "m8_bad_scenario.json";
  std::ofstream(scenario)
      << R"json({"battlefield": {"width": 100, "height": 100},
                  "spawn_band_fraction": 0.15,
                  "map": {"file": "missing.png", "cell_size": 5},
                  "armies": {"red": {"infantry": 10}, "blue": {"infantry": 10}}})json";
  std::string output;
  EXPECT_NE(run_cli("--scenario " + scenario.string() + " --seed 1", &output),
            0);
  EXPECT_NE(output.find("missing.png"), std::string::npos);
  fs::remove(scenario);
}

// ---------------------------------------------------------------------------
// No-map runs reproduce the M1-M7 exports byte-for-byte; terrain runs are
// deterministic and neighbor-mode agnostic.
// ---------------------------------------------------------------------------

TEST(NoMapPinsTest, M7BaselinesReproduceByteForByte) {
  const fs::path baselines = fs::path(TEST_DATA_DIR) /
                             "../tests/baselines/m8";
  struct Pin {
    const char* scenario;
    const char* seed;
    const char* file;
  };
  const Pin pins[] = {
      {"default", "42", "default-s42.json"},
      {"default", "123", "default-s123.json"},
      {"default", "5", "default-s5.json"},
      {"default", "0", "default-s0.json"},
      {"default", "18446744073709551615",
       "default-s18446744073709551615.json"},
      {"mixed", "42", "mixed-s42.json"},
      {"viewer-demo", "42", "viewer-demo-s42.json"},
  };
  for (const Pin& pin : pins) {
    // m13: the M7 era is reproduced with the v1 rule flags (--ai-avail
    // v1 --end v1); the v2 defaults change scoring edge cases and end
    // conditions, so the default-flag run is no longer era-faithful.
    const std::string exported =
        export_via_cli(std::string("--scenario ") + pin.scenario +
                       " --seed " + pin.seed + " --ai-avail v1 --end v1");
    const std::string baseline =
        read_text(baselines / pin.file);
    ASSERT_FALSE(baseline.empty()) << pin.file << " missing";
    EXPECT_EQ(exported, baseline)
        << pin.scenario << " seed " << pin.seed
        << " export changed from the M7 baseline";
  }
}

TEST(TerrainDeterminismTest, DoubleRunWithTerrainAndNeighborModes) {
  const std::string a = export_via_cli(
      "--scenario demo-river --seed 42 --ticks 200");
  const std::string b = export_via_cli(
      "--scenario demo-river --seed 42 --ticks 200");
  EXPECT_EQ(a, b);  // same run twice: byte-identical
  EXPECT_NE(a.find("\"format_version\": 6"), std::string::npos);
  EXPECT_NE(a.find("terrain_kills"), std::string::npos);
  // Grid and brute neighbor modes agree exactly with terrain.
  const std::string grid_mode =
      export_via_cli("--scenario demo-river --seed 42 --ticks 200 "
                     "--neighbor-mode grid");
  EXPECT_EQ(a, grid_mode);
}

// ---------------------------------------------------------------------------
// DrawList terrain layer.
// ---------------------------------------------------------------------------

TEST(TerrainLayerTest, CountsAndDeterminism) {
  const sim::TerrainTypeTable table = test_table();
  const sim::TerrainGrid grid =
      grid_from(table, {"gfw", "ggg"}, 10.0);
  sim::SimulationConfig config;
  config.seed = 1;
  config.max_ticks = 10;
  config.ai_mode = sim::AiMode::Utility;
  config.morale_enabled = false;
  config.terrain = &grid;
  config.battlefield_width = 30.0;
  config.battlefield_height = 20.0;
  sim::Simulation battle(config,
                         {make_unit(1, sim::Faction::Red, 5.0, 5.0),
                          make_unit(2, sim::Faction::Blue, 25.0, 15.0)});
  const game::Camera camera =
      game::Camera::fit(30.0, 20.0, 1280, 720);
  game::ViewOptions options;
  options.show_grid = false;
  options.show_hud = false;
  options.show_badges = false;
  options.squad_links = false;
  const render::DrawList list =
      render::build_battle_draw_list(battle, camera, options);
  const game::Color grass{0x6F, 0xA3, 0x5C, 0xFF};
  // Terrain rows merge runs: row 0 = g|f|w (3 runs), row 1 = g (1 run).
  std::size_t terrain_rects = 0;
  for (const render::DrawCmd& cmd : list.cmds) {
    if (cmd.kind == render::CmdKind::Rect && cmd.e > 0.5 &&
        cmd.color == grass) {
      ++terrain_rects;
    }
  }
  EXPECT_EQ(terrain_rects, 2u);  // the two grassland runs
  // Deterministic output.
  const render::DrawList again =
      render::build_battle_draw_list(battle, camera, options);
  EXPECT_EQ(list, again);
  // The toggle removes the layer entirely.
  options.show_terrain = false;
  const render::DrawList off =
      render::build_battle_draw_list(battle, camera, options);
  for (const render::DrawCmd& cmd : off.cmds) {
    EXPECT_FALSE(cmd.kind == render::CmdKind::Rect && cmd.color == grass);
  }
}

// ---------------------------------------------------------------------------
// m11a: navigation v2 — first contact, member field-march, the swap fix.
// ---------------------------------------------------------------------------

namespace {

// The m11a diagnosis fixtures: tables loaded once for the section.
const sim::UnitTypeTable& m11a_types() {
  static const sim::UnitTypeTable table = sim::parse_unit_types(
      read_text(fs::path(TEST_DATA_DIR) / "units.json"));
  return table;
}
const sim::PersonalityTable& m11a_personalities() {
  static const sim::PersonalityTable table = sim::parse_personalities(
      read_text(fs::path(TEST_DATA_DIR) / "personalities.json"));
  return table;
}
const sim::FormationConfig& m11a_formations() {
  static const sim::FormationConfig table = sim::parse_formations(
      read_text(fs::path(TEST_DATA_DIR) / "formations.json"));
  return table;
}
const sim::CommanderConfig& m11a_commanders() {
  static const sim::CommanderConfig table = sim::parse_commanders(
      read_text(fs::path(TEST_DATA_DIR) / "commanders.json"));
  return table;
}

// A utility-mode battle of explicit units with a recorder attached.
sim::Simulation m11a_battle(const sim::TerrainGrid* grid,
                            std::vector<sim::Unit> units,
                            sim::EventRecorder* recorder = nullptr,
                            bool nav_v2 = true) {
  sim::SimulationConfig config;
  config.seed = 1;
  config.max_ticks = 600;
  config.ai_mode = sim::AiMode::Utility;
  config.morale_enabled = false;
  config.terrain = grid;
  config.personalities = &m11a_personalities();
  config.unit_types = &m11a_types();
  config.formations = &m11a_formations();
  config.commanders = &m11a_commanders();
  config.nav_v2 = nav_v2;
  config.events = recorder;
  if (grid != nullptr) {
    config.battlefield_width = grid->width * grid->cell_size;
    config.battlefield_height = grid->height * grid->cell_size;
  } else {
    config.battlefield_width = 200.0;
    config.battlefield_height = 100.0;
  }
  config.spawn_band_fraction = 0.15;
  return sim::Simulation(config, std::move(units));
}

}  // namespace

TEST(NavV2Test, FirstContactTickAndEvent) {
  // Two units 30 apart (sight 40): contact on tick 1, one FirstContact
  // event carrying the seer's id. Far units (150 apart on a 400-wide
  // field): no contact for 40 ticks.
  sim::EventRecorder recorder;
  sim::Simulation close = m11a_battle(
      nullptr, {make_unit(1, sim::Faction::Red, 50.0, 50.0),
                make_unit(2, sim::Faction::Blue, 80.0, 50.0)},
      &recorder);
  close.tick();
  EXPECT_EQ(close.result().first_contact_tick, 1u);
  EXPECT_EQ(recorder.count(sim::EventType::FirstContact), 1u);
  ASSERT_FALSE(recorder.events().empty());
  EXPECT_EQ(recorder.events().front().type, sim::EventType::FirstContact);
  EXPECT_EQ(recorder.events().front().tick, 1u);

  sim::Simulation far = m11a_battle(
      nullptr, {make_unit(1, sim::Faction::Red, 50.0, 50.0),
                make_unit(2, sim::Faction::Blue, 240.0, 50.0)});
  // Closing speed 4/tick, sight 40: contact would be at tick ~38; stop
  // well before it.
  for (int tick = 0; tick < 25; ++tick) {
    far.tick();
  }
  EXPECT_EQ(far.result().first_contact_tick, 0u);
}

TEST(NavV2Test, FlagIsANoOpWithoutATerrainMap) {
  // No map: v1 and v2 are byte-identical (the member field-march and
  // every other v2 path needs terrain).
  const auto run = [](bool nav_v2) {
    sim::Simulation battle = m11a_battle(
        nullptr, {make_unit(1, sim::Faction::Red, 50.0, 50.0),
                  make_unit(2, sim::Faction::Blue, 80.0, 50.0)},
        nullptr, nav_v2);
    for (int tick = 0; tick < 60; ++tick) {
      battle.tick();
    }
    return sim::export_state_json(battle);
  };
  EXPECT_EQ(run(true), run(false));
}

TEST(NavV2Test, MemberMarchCarriesTheArmyThroughACorridor) {
  // The m11a corridor fixture: a water wall spans the map with a
  // 2-cell gap; three squads per side must cross. v1 members beeline to
  // their (projected) slots and jam against the wall; v2 members far
  // from their slot march along the flow field and funnel through. The
  // v2 army's mean progress east must beat v1's by a wide margin.
  const sim::TerrainTypeTable table = test_table();
  std::vector<std::string> rows;
  for (std::uint32_t y = 0; y < 24; ++y) {
    std::string row(40, 'g');
    if (y != 11 && y != 12) {
      row[20] = 'w';  // the wall, with a 2-cell gap at y in {11, 12}
    }
    rows.push_back(row);
  }
  const sim::TerrainGrid grid = grid_from(table, rows, 5.0);

  const auto red_state = [&grid](bool nav_v2) {
    sim::SimulationConfig config;
    config.seed = 11;
    config.max_ticks = 500;
    config.ai_mode = sim::AiMode::Utility;
    config.morale_enabled = false;
    config.terrain = &grid;
    config.personalities = &m11a_personalities();
    config.unit_types = &m11a_types();
    config.formations = &m11a_formations();
    config.commanders = &m11a_commanders();
    config.nav_v2 = nav_v2;
    config.battlefield_width = grid.width * grid.cell_size;
    config.battlefield_height = grid.height * grid.cell_size;
    config.spawn_band_fraction = 0.15;
    config.composition[kRed].push_back(sim::TypeCount{0, 30});
    config.composition[kRed].push_back(sim::TypeCount{3, 3});
    config.composition[kBlue].push_back(sim::TypeCount{0, 30});
    config.composition[kBlue].push_back(sim::TypeCount{3, 3});
    config.squad_plans[kRed] = sim::SquadPlan{3, 10};
    config.squad_plans[kBlue] = sim::SquadPlan{3, 10};
    sim::Simulation battle(config);
    EXPECT_TRUE(battle.squads_active());
    for (int tick = 0; tick < 400; ++tick) {
      battle.tick();
    }
    double sum = 0.0;
    std::uint32_t alive = 0;
    for (const sim::Unit& unit : battle.world().units) {
      if (unit.alive && unit.faction == sim::Faction::Red) {
        sum += unit.x;
        ++alive;
      }
    }
    return std::pair<std::uint32_t, double>(
        alive, alive > 0 ? sum / alive : 0.0);
  };
  // The wall sits at x = 100..105 with a 2-cell gap. Under v2 the Red
  // army funnels through the gap along the flow field and keeps
  // fighting past it; under v1 the members beeline to their projected
  // slots, jam against the wall and feed into the enemy piecemeal (the
  // m11a Lebanon signature). Both outcomes are deterministic for this
  // fixture.
  const auto [v1_alive, v1_x] = red_state(false);
  const auto [v2_alive, v2_x] = red_state(true);
  EXPECT_GT(v2_alive, 0u);
  EXPECT_GT(v2_x, 140.0) << "v2 did not carry the army through";
  EXPECT_TRUE(v1_alive == 0 || v1_x < 110.0)
      << "v1 unexpectedly crossed (alive " << v1_alive << ", x " << v1_x
      << ")";
}

TEST(NavV2Test, SwapFixMakesSwappedBattlesMeet) {
  // CLI level: on earth-lebanon with --swap-sides, nav v1 reproduces
  // the M10 stall (no contact, tick-limit draw) while nav v2 swaps the
  // explicit objectives once — the armies cross and make contact.
  const auto first_contact = [](const char* nav) {
    const std::string report = "/tmp/m11a_swap_report.md";
    fs::remove(report);
    fs::remove("/tmp/m11a_swap_report.json");
    EXPECT_EQ(0, run_cli(std::string("--scenario data/scenarios/earth-"
                                     "lebanon.json --swap-sides --seed 1 "
                                     "--ticks 1500 --nav ") +
                         nav + " --report " + report));
    const std::string json = read_text("/tmp/m11a_swap_report.json");
    // The JSON twin carries outcome.first_contact_tick.
    const std::string key = "\"first_contact_tick\": ";
    const std::size_t at = json.find(key);
    EXPECT_NE(at, std::string::npos);
    return std::strtoull(json.c_str() + at + key.size(), nullptr, 10);
  };
  EXPECT_EQ(first_contact("v1"), 0u);   // the M10 stall, byte-for-byte
  EXPECT_GT(first_contact("v2"), 0u);   // the fix: the armies meet
}
