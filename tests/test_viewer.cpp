// M7 — viewer pipeline tests (all headless, no display): camera math, the
// fixed-timestep accumulator, picking, the pure DrawList, the software
// rasterizer and PNG encoding (byte-identical), HUD/inspector text and the
// color/shape mapping, the ascii map, and the squad slot accessor.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cli/cli_args.hpp"
#include "game/accumulator.hpp"
#include "game/ascii.hpp"
#include "game/camera.hpp"
#include "game/picking.hpp"
#include "game/theme.hpp"
#include "game/view.hpp"
#include "render/battle_view.hpp"
#include "render/png_writer.hpp"
#include "render/raster.hpp"
#include "sim/config.hpp"
#include "sim/simulation.hpp"
#include "sim/unit_types.hpp"

namespace {

constexpr std::size_t kRed = static_cast<std::size_t>(sim::Faction::Red);
constexpr std::size_t kBlue = static_cast<std::size_t>(sim::Faction::Blue);

// A minimal battle with explicit unit positions (legacy stats mode, no
// type table: every unit draws as the fallback circle).
sim::Simulation tiny_battle(const std::vector<sim::Unit>& units) {
  sim::SimulationConfig config;
  config.seed = 1;
  config.max_ticks = 100;
  config.battlefield_width = 400.0;
  config.battlefield_height = 400.0;
  config.ai_mode = sim::AiMode::Utility;
  // Morale on: explicit initial states keep their routed values (with
  // morale off the constructor clears routed, and the styling tests need
  // routed units).
  config.morale_enabled = true;
  config.red_stats.sight_radius = 40.0;
  config.blue_stats.sight_radius = 40.0;
  return sim::Simulation(config, units);
}

sim::Unit make_unit(std::uint32_t id, sim::Faction faction, double x,
                    double y, bool alive = true, bool routed = false) {
  sim::Unit unit;
  unit.id = id;
  unit.faction = faction;
  unit.x = x;
  unit.y = y;
  unit.health = alive ? 100.0 : 0.0;
  unit.alive = alive;
  unit.routed = routed;
  unit.morale = 60.0;
  return unit;
}

}  // namespace

// ---------------------------------------------------------------------------
// Camera.
// ---------------------------------------------------------------------------

TEST(CameraTest, FitCoversTheWorldCentered) {
  const game::Camera camera = game::Camera::fit(1000.0, 1000.0, 1280, 720);
  EXPECT_DOUBLE_EQ(camera.center_x, 500.0);
  EXPECT_DOUBLE_EQ(camera.center_y, 500.0);
  // The whole battlefield fits in the smaller screen dimension.
  EXPECT_DOUBLE_EQ(camera.zoom, 720.0 / 1000.0);
  // All four world corners are inside the screen (height binds here, so
  // the world is centered with left/right margins).
  EXPECT_GE(camera.screen_x(0.0, 1280), 0.0);
  EXPECT_LE(camera.screen_x(1000.0, 1280), 1280.0);
  EXPECT_GE(camera.screen_y(0.0, 720), 0.0);
  EXPECT_LE(camera.screen_y(1000.0, 720), 720.0);
  EXPECT_DOUBLE_EQ(camera.screen_y(0.0, 720), 0.0);
}

TEST(CameraTest, WorldScreenRoundTrip) {
  const game::Camera camera = game::Camera::fit(1000.0, 1000.0, 1280, 720);
  const double points[][2] = {{0.0, 0.0},   {1000.0, 1000.0}, {500.0, 500.0},
                              {123.0, 45.0}, {777.5, 250.25}};
  for (const auto& point : points) {
    const double sx = camera.screen_x(point[0], 1280);
    const double sy = camera.screen_y(point[1], 720);
    EXPECT_NEAR(camera.world_x(sx, 1280), point[0], 1e-9);
    EXPECT_NEAR(camera.world_y(sy, 720), point[1], 1e-9);
  }
  // Round trip survives zoom and pan.
  game::Camera moved = camera;
  moved.zoom = 7.5;
  moved.center_x = 321.0;
  moved.center_y = 123.0;
  const double sx = moved.screen_x(654.0, 1280);
  const double sy = moved.screen_y(321.0, 720);
  EXPECT_NEAR(moved.world_x(sx, 1280), 654.0, 1e-9);
  EXPECT_NEAR(moved.world_y(sy, 720), 321.0, 1e-9);
}

TEST(CameraTest, ZoomLimitsAndZoomAtKeepTheCursorPoint) {
  game::Camera camera = game::Camera::fit(1000.0, 1000.0, 1280, 720);
  // Zoom clamps at both ends.
  camera.zoom = 100.0;
  camera.clamp_zoom();
  EXPECT_DOUBLE_EQ(camera.zoom, game::kMaxZoom);
  camera.zoom = 0.0001;
  camera.clamp_zoom();
  EXPECT_DOUBLE_EQ(camera.zoom, game::kMinZoom);
  // zoom_at keeps the world point under the cursor.
  camera = game::Camera::fit(1000.0, 1000.0, 1280, 720);
  const double before_x = camera.world_x(640.0, 1280);
  const double before_y = camera.world_y(360.0, 720);
  camera.zoom_at(640.0, 360.0, 2.0, 1280, 720);
  EXPECT_NEAR(camera.world_x(640.0, 1280), before_x, 1e-9);
  EXPECT_NEAR(camera.world_y(360.0, 720), before_y, 1e-9);
  // Pan moves the center opposite to the drag, in world units.
  const double center_x = camera.center_x;
  camera.pan(100.0, 0.0);
  EXPECT_NEAR(camera.center_x, center_x - 100.0 / camera.zoom, 1e-9);
}

// ---------------------------------------------------------------------------
// Accumulator.
// ---------------------------------------------------------------------------

TEST(AccumulatorTest, TicksPerFrameAtEachSpeed) {
  // 60 fps frames, 30 base ticks per second: 0.5 ticks per frame at 1x.
  for (const int speed : game::kSpeeds) {
    game::TickAccumulator accumulator(30.0, 256);
    accumulator.set_speed(speed);
    std::uint64_t total = 0;
    for (int frame = 0; frame < 120; ++frame) {  // two seconds
      bool limited = true;
      const int ticks = accumulator.advance(1.0 / 60.0, limited);
      EXPECT_FALSE(limited);
      total += static_cast<std::uint64_t>(ticks);
    }
    // Two seconds at speed x: 60 * speed ticks (the carry never loses more
    // than one tick of fraction across the whole run).
    EXPECT_NEAR(static_cast<double>(total), 60.0 * speed, 1.0)
        << "speed " << speed;
  }
  // Exact pacing: 30 tps at 1x over 60 frames of 1/30 s is exactly 1 tick
  // per frame, with no leftover carry.
  game::TickAccumulator exact(30.0, 256);
  for (int frame = 0; frame < 60; ++frame) {
    bool limited = true;
    EXPECT_EQ(exact.advance(1.0 / 30.0, limited), 1);
    EXPECT_FALSE(limited);
  }
  EXPECT_DOUBLE_EQ(exact.carry(), 0.0);
}

TEST(AccumulatorTest, FrameCapReportsLimited) {
  game::TickAccumulator accumulator(30.0, 256);
  accumulator.set_speed(16);
  bool limited = false;
  // A ten-second stall wants 4800 ticks; the cap trims to 256.
  EXPECT_EQ(accumulator.advance(10.0, limited), 256);
  EXPECT_TRUE(limited);
  // The remainder is kept (still behind), and a normal frame reports it.
  limited = false;
  const int ticks = accumulator.advance(1.0 / 60.0, limited);
  EXPECT_TRUE(limited);
  EXPECT_EQ(ticks, 256);  // still draining the backlog at the cap
  accumulator.reset();
  accumulator.set_speed(1);  // reset clears the carry, not the speed
  limited = false;
  EXPECT_EQ(accumulator.advance(1.0 / 60.0, limited), 0);  // half a tick
  EXPECT_FALSE(limited);
  EXPECT_NEAR(accumulator.carry(), 0.5, 1e-9);
  EXPECT_EQ(accumulator.advance(1.0 / 60.0, limited), 1);  // runs on the next
}

TEST(AccumulatorTest, SpeedCyclingHelpers) {
  EXPECT_EQ(game::TickAccumulator::speed_up(1), 2);
  EXPECT_EQ(game::TickAccumulator::speed_up(2), 4);
  EXPECT_EQ(game::TickAccumulator::speed_up(8), 16);
  EXPECT_EQ(game::TickAccumulator::speed_up(16), 16);  // clamped at the top
  EXPECT_EQ(game::TickAccumulator::speed_down(16), 8);
  EXPECT_EQ(game::TickAccumulator::speed_down(4), 2);
  EXPECT_EQ(game::TickAccumulator::speed_down(1), 1);  // clamped at the bottom
  // Odd values snap to the next step.
  EXPECT_EQ(game::TickAccumulator::speed_up(3), 4);
  EXPECT_EQ(game::TickAccumulator::speed_down(3), 2);
}

// ---------------------------------------------------------------------------
// Picking.
// ---------------------------------------------------------------------------

TEST(PickingTest, NearestAliveUnitWithinRadius) {
  const sim::Simulation simulation = tiny_battle({
      make_unit(1, sim::Faction::Red, 100.0, 100.0),
      make_unit(2, sim::Faction::Red, 200.0, 100.0),
      make_unit(3, sim::Faction::Blue, 300.0, 100.0),
  });
  const game::Camera camera = game::Camera::fit(400.0, 400.0, 1280, 720);
  // Click near the middle unit (screen point of world (200, 100)).
  const double sx = camera.screen_x(201.0, 1280);
  const double sy = camera.screen_y(101.0, 720);
  EXPECT_EQ(game::pick_unit(simulation, camera, 1280, 720, sx, sy, 8.0), 1);
  // Between two units (40 and 60 world units away at the fit zoom 1.8:
  // 72 and 108 pixels): a 75-pixel pick radius reaches only the nearer
  // one.
  const double mx = camera.screen_x(140.0, 1280);
  const double my = camera.screen_y(100.0, 720);
  EXPECT_EQ(game::pick_unit(simulation, camera, 1280, 720, mx, my, 75.0), 0);
  // Far from everything: nothing.
  EXPECT_EQ(game::pick_unit(simulation, camera, 1280, 720, 5.0, 5.0, 8.0),
            -1);
  // Dead units are never picked.
  const sim::Simulation with_dead = tiny_battle({
      make_unit(1, sim::Faction::Red, 100.0, 100.0, false),
      make_unit(2, sim::Faction::Red, 200.0, 100.0),
  });
  const game::Camera dead_camera =
      game::Camera::fit(400.0, 400.0, 1280, 720);
  EXPECT_EQ(game::pick_unit(with_dead, dead_camera, 1280, 720,
                            dead_camera.screen_x(100.0, 1280),
                            dead_camera.screen_y(100.0, 720), 8.0),
            -1);
  // Empty world: nothing.
  const sim::Simulation empty = tiny_battle({make_unit(1, sim::Faction::Red,
                                                        0.0, 0.0, false)});
  EXPECT_EQ(game::pick_unit(empty, camera, 1280, 720, 640.0, 360.0, 20.0),
            -1);
}

// ---------------------------------------------------------------------------
// DrawList.
// ---------------------------------------------------------------------------

TEST(DrawListTest, CountsMatchUnitsAndStylingRules) {
  const sim::Simulation simulation = tiny_battle({
      make_unit(1, sim::Faction::Red, 100.0, 100.0),
      make_unit(2, sim::Faction::Blue, 150.0, 100.0),
      make_unit(3, sim::Faction::Blue, 200.0, 100.0, true, true),  // routed
      make_unit(4, sim::Faction::Red, 250.0, 100.0, false),        // dead
  });
  const game::Camera camera = game::Camera::fit(400.0, 400.0, 1280, 720);
  game::ViewOptions options;
  options.show_grid = false;
  options.show_hud = false;
  options.show_badges = false;
  options.squad_links = false;

  // At the fit zoom (720/400 = 1.8 >= the 1.0 threshold) every alive unit
  // draws one shape command (the fallback circle); dead units draw none.
  const render::DrawList list =
      render::build_battle_draw_list(simulation, camera, options);
  std::size_t circles = 0;
  std::vector<game::Color> circle_colors;
  for (const render::DrawCmd& cmd : list.cmds) {
    if (cmd.kind == render::CmdKind::Circle && cmd.e == 0.0) {
      ++circles;
      circle_colors.push_back(cmd.color);
    }
  }
  ASSERT_EQ(circles, 3u);
  // Routed styling: the routed Blue unit's color differs from the active
  // Blue one (desaturated) but shares the faction's hue family (not equal
  // to Red's either).
  EXPECT_NE(circle_colors[1], circle_colors[2]);
  EXPECT_NE(circle_colors[0], circle_colors[1]);
  EXPECT_EQ(circle_colors[0], game::faction_color(sim::Faction::Red));
  EXPECT_EQ(circle_colors[1], game::faction_color(sim::Faction::Blue));
  EXPECT_EQ(circle_colors[2], game::routed_color(sim::Faction::Blue));

  // Selecting a unit adds exactly one highlight ring (outline-only circle).
  game::ViewOptions selected = options;
  selected.selected_unit = 0;
  const render::DrawList selected_list =
      render::build_battle_draw_list(simulation, camera, selected);
  std::size_t rings = 0;
  for (const render::DrawCmd& cmd : selected_list.cmds) {
    if (cmd.kind == render::CmdKind::Circle && cmd.e > 0.5) {
      ++rings;
      EXPECT_EQ(cmd.color2, game::selected_outline());
    }
  }
  EXPECT_EQ(rings, 1u);

  // Below the point threshold the same battle draws one 1x1 rect per alive
  // unit and no shapes.
  game::Camera far_camera = camera;
  far_camera.zoom = 0.5;  // < 1.0 threshold
  const render::DrawList points =
      render::build_battle_draw_list(simulation, far_camera, options);
  std::size_t point_rects = 0;
  for (const render::DrawCmd& cmd : points.cmds) {
    if (cmd.kind == render::CmdKind::Rect && cmd.c == 1.0 && cmd.d == 1.0) {
      ++point_rects;
    }
    EXPECT_NE(cmd.kind, render::CmdKind::Triangle);
  }
  EXPECT_EQ(point_rects, 3u);
}

TEST(DrawListTest, DeterministicOutputForTheSameState) {
  const sim::Simulation simulation = tiny_battle({
      make_unit(1, sim::Faction::Red, 100.0, 100.0),
      make_unit(2, sim::Faction::Blue, 300.0, 320.0, true, true),
  });
  const game::Camera camera = game::Camera::fit(400.0, 400.0, 1280, 720);
  game::ViewOptions options;
  const render::DrawList a =
      render::build_battle_draw_list(simulation, camera, options);
  const render::DrawList b =
      render::build_battle_draw_list(simulation, camera, options);
  EXPECT_EQ(a, b);
}

TEST(DrawListTest, SquadExtrasDrawWhenToggled) {
  // A squads battle: commander + 3 soldiers per side, squads active.
  const sim::UnitTypeTable types = sim::parse_unit_types(R"json([
    {"id": "infantry", "health": 100.0, "damage": 12.0, "speed": 2.0,
     "range": 2.0, "cooldown": 5, "sight_radius": 40.0},
    {"id": "commander", "health": 150.0, "damage": 8.0, "speed": 1.8,
     "range": 2.0, "cooldown": 10, "sight_radius": 60.0,
     "leadership_radius": 120.0}])json");
  sim::SimulationConfig config;
  config.seed = 42;
  config.max_ticks = 50;
  config.battlefield_width = 400.0;
  config.battlefield_height = 400.0;
  config.ai_mode = sim::AiMode::Utility;
  config.unit_types = &types;
  config.morale_enabled = false;
  config.composition[kRed].push_back(sim::TypeCount{1, 1});
  config.composition[kRed].push_back(sim::TypeCount{0, 3});
  config.composition[kBlue].push_back(sim::TypeCount{1, 1});
  config.composition[kBlue].push_back(sim::TypeCount{0, 3});
  config.squad_plans[kRed] = sim::SquadPlan{1, 3};
  config.squad_plans[kBlue] = sim::SquadPlan{1, 3};
  const sim::Simulation simulation(config);
  ASSERT_TRUE(simulation.squads_active());

  const game::Camera camera = game::Camera::fit(400.0, 400.0, 1280, 720);
  game::ViewOptions options;
  options.show_grid = false;
  options.show_hud = false;

  // Squad links: every non-commander alive member links to its commander:
  // 3 soldiers per squad x 2 squads = 6 lines.
  const render::DrawList linked =
      render::build_battle_draw_list(simulation, camera, options);
  std::size_t links = 0;
  for (const render::DrawCmd& cmd : linked.cmds) {
    if (cmd.kind == render::CmdKind::Line) {
      ++links;
    }
  }
  EXPECT_EQ(links, 6u);

  // Formation slots: one 2x2 rect per member (4 per squad, 8 total).
  game::ViewOptions slots = options;
  slots.squad_links = false;
  slots.formation_slots = true;
  const render::DrawList slotted =
      render::build_battle_draw_list(simulation, camera, slots);
  std::size_t slot_rects = 0;
  for (const render::DrawCmd& cmd : slotted.cmds) {
    if (cmd.kind == render::CmdKind::Rect && cmd.c == 2.0 && cmd.d == 2.0) {
      ++slot_rects;
    }
  }
  EXPECT_EQ(slot_rects, 8u);

  // Badges: one text per squad id.
  game::ViewOptions badges = options;
  badges.squad_links = false;
  const render::DrawList badged =
      render::build_battle_draw_list(simulation, camera, badges);
  std::size_t texts = 0;
  for (const render::DrawCmd& cmd : badged.cmds) {
    if (cmd.kind == render::CmdKind::Text) {
      ++texts;
    }
  }
  EXPECT_EQ(texts, 2u);

  // Squad slot positions: slot 0 is the commander's own position.
  double x = 0.0;
  double y = 0.0;
  ASSERT_TRUE(simulation.squad_slot_position(0, 0, x, y));
  const sim::Unit& commander =
      simulation.world().units[simulation.squads()[0].commander_index];
  EXPECT_DOUBLE_EQ(x, commander.x);
  EXPECT_DOUBLE_EQ(y, commander.y);
  // Out-of-range lookups fail.
  EXPECT_FALSE(simulation.squad_slot_position(-1, 0, x, y));
  EXPECT_FALSE(simulation.squad_slot_position(0, 99, x, y));
}

// ---------------------------------------------------------------------------
// Rasterizer and PNG.
// ---------------------------------------------------------------------------

TEST(RasterPngTest, SameFrameTwiceIsByteIdentical) {
  const sim::Simulation simulation = tiny_battle({
      make_unit(1, sim::Faction::Red, 100.0, 100.0),
      make_unit(2, sim::Faction::Blue, 200.0, 200.0),
      make_unit(3, sim::Faction::Red, 300.0, 300.0, true, true),
  });
  const game::Camera camera = game::Camera::fit(400.0, 400.0, 1280, 720);
  const render::DrawList list = render::build_battle_draw_list(
      simulation, camera, game::ViewOptions{});
  const render::Image first = render::rasterize(list);
  const render::Image second = render::rasterize(list);
  EXPECT_EQ(first, second);
  ASSERT_EQ(first.pixels.size(),
            static_cast<std::size_t>(1280) * 720u * 3u);

  // The PNG encoding of the same image is byte-identical, and carries the
  // PNG magic bytes.
  const std::vector<std::uint8_t> png_a = render::encode_png(first);
  const std::vector<std::uint8_t> png_b = render::encode_png(second);
  EXPECT_EQ(png_a, png_b);
  ASSERT_GE(png_a.size(), 8u);
  EXPECT_EQ(png_a[0], 0x89);
  EXPECT_EQ(png_a[1], 'P');
  EXPECT_EQ(png_a[2], 'N');
  EXPECT_EQ(png_a[3], 'G');

  // write_png writes exactly those bytes to disk.
  const std::string path = "/tmp/m7_png_roundtrip_test.png";
  ASSERT_TRUE(render::write_png(first, path));
  std::FILE* file = std::fopen(path.c_str(), "rb");
  ASSERT_NE(file, nullptr);
  std::vector<std::uint8_t> read_back(png_a.size());
  const std::size_t read =
      std::fread(read_back.data(), 1, read_back.size(), file);
  std::fclose(file);
  std::remove(path.c_str());
  ASSERT_EQ(read, png_a.size());
  EXPECT_EQ(read_back, png_a);
}

// ---------------------------------------------------------------------------
// HUD text, colors and shapes.
// ---------------------------------------------------------------------------

TEST(HudThemeTest, HudAndInspectorLines) {
  const sim::Simulation simulation = tiny_battle({
      make_unit(1, sim::Faction::Red, 100.0, 100.0),
      make_unit(2, sim::Faction::Red, 120.0, 100.0, true, true),  // routed
      make_unit(3, sim::Faction::Blue, 300.0, 100.0, false),      // dead
  });
  const std::vector<std::string> lines =
      game::hud_lines(simulation, 60.0, 31.5, 4, true, "demo");
  ASSERT_EQ(lines.size(), 5u);
  EXPECT_NE(lines[0].find("tick 0"), std::string::npos);
  EXPECT_NE(lines[0].find("fps 60"), std::string::npos);
  EXPECT_NE(lines[0].find("sim 31.5 t/s"), std::string::npos);
  EXPECT_NE(lines[1].find("speed 4x"), std::string::npos);
  EXPECT_NE(lines[1].find("SIM LIMITED"), std::string::npos);
  EXPECT_NE(lines[2].find("seed 1"), std::string::npos);
  EXPECT_NE(lines[3].find("Red  2 alive  1 routed  0 dead"),
            std::string::npos);
  EXPECT_NE(lines[4].find("Blue 0 alive  0 routed  1 dead"),
            std::string::npos);

  // Without fps/tps the segments are simply omitted.
  const std::vector<std::string> bare =
      game::hud_lines(simulation, 0.0, 0.0, 1, false, "");
  EXPECT_EQ(bare[0], "tick 0");
  EXPECT_EQ(bare[1], "speed 1x");

  // Inspector: unit 2 shows hp, morale state, action, personality and
  // "squad none" (no squads in this battle).
  const std::vector<std::string> inspector =
      game::inspector_lines(simulation, 1);
  ASSERT_FALSE(inspector.empty());
  EXPECT_NE(inspector[0].find("unit 2"), std::string::npos);
  EXPECT_NE(inspector[1].find("hp 100/100"), std::string::npos);
  EXPECT_NE(inspector[1].find("morale 60"), std::string::npos);
  EXPECT_NE(inspector[2].find("action"), std::string::npos);
  EXPECT_NE(inspector[3].find("personality"), std::string::npos);
  EXPECT_NE(inspector[4].find("squad none"), std::string::npos);
  // Unknown indices produce no lines.
  EXPECT_TRUE(game::inspector_lines(simulation, 99).empty());
}

TEST(HudThemeTest, ColorsAndShapes) {
  // Faction colors are distinct and routing desaturates both.
  const game::Color red = game::faction_color(sim::Faction::Red);
  const game::Color blue = game::faction_color(sim::Faction::Blue);
  EXPECT_NE(red, blue);
  const game::Color red_routed = game::routed_color(sim::Faction::Red);
  const game::Color blue_routed = game::routed_color(sim::Faction::Blue);
  EXPECT_NE(red_routed, red);
  EXPECT_NE(blue_routed, blue);
  const auto saturation = [](const game::Color& color) {
    const int max = std::max({color.r, color.g, color.b});
    const int min = std::min({color.r, color.g, color.b});
    return max - min;
  };
  EXPECT_LT(saturation(red_routed), saturation(red));
  EXPECT_LT(saturation(blue_routed), saturation(blue));

  // Shape and size mapping by type id, with the circle fallback.
  EXPECT_EQ(game::shape_of("infantry"), game::UnitShape::Circle);
  EXPECT_EQ(game::shape_of("archer"), game::UnitShape::Triangle);
  EXPECT_EQ(game::shape_of("cavalry"), game::UnitShape::Rect);
  EXPECT_EQ(game::shape_of("commander"), game::UnitShape::Rect);
  EXPECT_EQ(game::shape_of("alien"), game::UnitShape::Circle);
  EXPECT_GT(game::unit_size("cavalry"), game::unit_size("infantry"));
  EXPECT_GT(game::unit_size("commander"), game::unit_size("archer"));
}

// ---------------------------------------------------------------------------
// Ascii map.
// ---------------------------------------------------------------------------

TEST(AsciiTest, MapsUnitsToCells) {
  const sim::Simulation simulation = tiny_battle({
      make_unit(1, sim::Faction::Red, 100.0, 100.0),
      make_unit(2, sim::Faction::Red, 101.0, 101.0, true, true),  // routed
      make_unit(3, sim::Faction::Blue, 300.0, 300.0),
      make_unit(4, sim::Faction::Red, 350.0, 350.0, false),  // dead: hidden
  });
  const std::string map = game::ascii_map(simulation, 10, 5);
  // Header lines, border, five rows, legend.
  EXPECT_NE(map.find("tick 0"), std::string::npos);
  EXPECT_NE(map.find("seed 1"), std::string::npos);
  EXPECT_NE(map.find("Red 2 alive (1 routed)  Blue 1 alive (0 routed)"),
            std::string::npos);
  EXPECT_NE(map.find("+----------+"), std::string::npos);
  // Cell (100,100) in a 10x5 grid over a 400x400 world: column 2, row 1.
  // It holds one active and one routed Red unit: actives win -> 'R'.
  EXPECT_NE(map.find("|  R       |"), std::string::npos);
  // Cell (300,300): column 7, row 3 -> active Blue 'B'.
  EXPECT_NE(map.find("|       B  |"), std::string::npos);
  // The dead unit at (350,350) would land in column 8, row 4; dead units
  // are not shown, so that row has no 'R'.
  EXPECT_EQ(map.find("|        R |"), std::string::npos);
  EXPECT_EQ(map.find("|        R|"), std::string::npos);
}

// ---------------------------------------------------------------------------
// CLI render argument parsing (flag layer; the full pipeline above).
// ---------------------------------------------------------------------------

namespace {

// parse_args with a friendly signature (same idea as test_cli_args.cpp).
cli::ParseOutcome parse_cli(std::vector<std::string> arguments) {
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>("battle_cli"));
  for (std::string& argument : arguments) {
    argv.push_back(const_cast<char*>(argument.data()));
  }
  return cli::parse_args(static_cast<int>(argv.size()), argv.data());
}

}  // namespace

TEST(CliRenderArgsTest, RenderFlagsParseAndValidate) {
  cli::ParseOutcome outcome =
      parse_cli({"--render-frame", "100", "--out", "f.png"});
  ASSERT_TRUE(outcome.ok);
  EXPECT_EQ(outcome.args.render_frame, 100u);
  EXPECT_EQ(outcome.args.render_out, "f.png");

  outcome = parse_cli({"--render-frames", "0,100,250",
                        "--out-prefix", "out/frame"});
  ASSERT_TRUE(outcome.ok);
  EXPECT_EQ(outcome.args.render_frames, "0,100,250");
  EXPECT_EQ(outcome.args.render_out_prefix, "out/frame");

  outcome = parse_cli({"--ascii", "50"});
  ASSERT_TRUE(outcome.ok);
  EXPECT_EQ(outcome.args.ascii_tick, 50u);

  // --render-frame without --out is rejected.
  outcome = parse_cli({"--render-frame", "100"});
  EXPECT_FALSE(outcome.ok);
  EXPECT_NE(outcome.error.find("requires --out"), std::string::npos);
  // --render-frames without --out-prefix is rejected.
  outcome = parse_cli({"--render-frames", "1,2"});
  EXPECT_FALSE(outcome.ok);
  EXPECT_NE(outcome.error.find("requires --out-prefix"), std::string::npos);
  // The two render modes are mutually exclusive.
  outcome = parse_cli({"--render-frame", "1", "--out", "a.png",
                        "--render-frames", "2", "--out-prefix", "b"});
  EXPECT_FALSE(outcome.ok);
  EXPECT_NE(outcome.error.find("mutually exclusive"), std::string::npos);
  // Bad tick values are rejected.
  outcome = parse_cli({"--render-frame", "-5", "--out", "f"});
  EXPECT_FALSE(outcome.ok);
  outcome = parse_cli({"--ascii", "x"});
  EXPECT_FALSE(outcome.ok);
}
