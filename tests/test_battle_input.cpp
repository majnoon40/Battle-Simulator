// m13 item 5a — the battle-view input policy: multi-squad selection,
// order intents, apply_order and minimap geometry (game/battle_input),
// plus the renderer's selection rings, order markers and minimap.
#include "game/battle_input.hpp"
#include "game/camera.hpp"
#include "game/view.hpp"
#include "render/battle_view.hpp"
#include "sim/simulation.hpp"
#include "sim/unit_types.hpp"

#include <fstream>
#include <string>

#include <gtest/gtest.h>

namespace {

constexpr std::size_t kRed = static_cast<std::size_t>(sim::Faction::Red);

sim::UnitTypeTable squad_types() {
  return sim::parse_unit_types(R"json([
    {"id": "infantry", "health": 100.0, "damage": 12.0, "speed": 2.0,
     "range": 2.0, "cooldown": 5, "sight_radius": 40.0},
    {"id": "commander", "health": 150.0, "damage": 8.0, "speed": 1.8,
     "range": 2.0, "cooldown": 10, "sight_radius": 60.0,
     "leadership_radius": 120.0}
  ])json");
}

// A squads battle: `squads` x `size` infantry + one commander per squad
// per side, standard battlefield (the test_squads shape).
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
  config.morale_enabled = false;
  config.squads_enabled = true;
  const int infantry = types.index_of("infantry");
  const int commander = types.index_of("commander");
  config.composition[kRed].push_back(
      sim::TypeCount{static_cast<std::uint32_t>(commander), squads});
  config.composition[kRed].push_back(
      sim::TypeCount{static_cast<std::uint32_t>(infantry), squads * size});
  config.composition[static_cast<std::size_t>(sim::Faction::Blue)].push_back(
      sim::TypeCount{static_cast<std::uint32_t>(commander), squads});
  config.composition[static_cast<std::size_t>(sim::Faction::Blue)].push_back(
      sim::TypeCount{static_cast<std::uint32_t>(infantry), squads * size});
  config.squad_plans[kRed] = sim::SquadPlan{squads, size};
  config.squad_plans[static_cast<std::size_t>(sim::Faction::Blue)] =
      sim::SquadPlan{squads, size};
  return config;
}

game::SquadSelection selection_of(std::initializer_list<std::uint32_t> ids) {
  game::SquadSelection out;
  out.squads.assign(ids);
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Selection.

TEST(BattleInputSelectionTest, ClickReplacesShiftAddsAndToggles) {
  const game::SquadSelection previous = selection_of({2, 5});
  // Plain click replaces.
  game::SquadSelection out = game::select_squad_click(previous, 7, false);
  EXPECT_EQ(out.squads, (std::vector<std::uint32_t>{7}));
  // Shift adds in sorted position.
  out = game::select_squad_click(previous, 3, true);
  EXPECT_EQ(out.squads, (std::vector<std::uint32_t>{2, 3, 5}));
  // Shift toggles an existing id off.
  out = game::select_squad_click(previous, 5, true);
  EXPECT_EQ(out.squads, (std::vector<std::uint32_t>{2}));
  // Clicking nothing clears, shift-clicking nothing keeps.
  out = game::select_squad_click(previous, -1, false);
  EXPECT_TRUE(out.empty());
  out = game::select_squad_click(previous, -1, true);
  EXPECT_EQ(out.squads, previous.squads);
}

TEST(BattleInputSelectionTest, RectSelectsRedSquadsWithMembersInside) {
  const sim::UnitTypeTable types = squad_types();
  sim::Simulation simulation(squad_config(types, 4, 5));
  const auto& world = simulation.world();
  ASSERT_GE(simulation.squads().size(), 8u);  // 4 per side

  // The position of the first alive member of Red squad 0 and of the
  // first Blue squad: rect around the Red member selects its squad only
  // (Blue squads are never selectable).
  const sim::Squad* red_squad = nullptr;
  const sim::Squad* blue_squad = nullptr;
  for (const sim::Squad& squad : simulation.squads()) {
    if (squad.faction == sim::Faction::Red && red_squad == nullptr) {
      red_squad = &squad;
    }
    if (squad.faction == sim::Faction::Blue && blue_squad == nullptr) {
      blue_squad = &squad;
    }
  }
  ASSERT_NE(red_squad, nullptr);
  ASSERT_NE(blue_squad, nullptr);
  const sim::Unit& red_member =
      world.units[red_squad->members.front().unit_index];
  const sim::Unit& blue_member =
      world.units[blue_squad->members.front().unit_index];

  const game::SquadSelection both = game::select_squad_rect(
      simulation, game::SquadSelection{}, red_member.x - 1.0,
      red_member.y - 1.0, blue_member.x + 1.0, blue_member.y + 1.0, false);
  EXPECT_TRUE(both.contains(red_squad->id));
  EXPECT_FALSE(both.contains(blue_squad->id));

  // A rect around nothing keeps the previous selection empty.
  const game::SquadSelection none = game::select_squad_rect(
      simulation, game::SquadSelection{}, 1.0, 1.0, 2.0, 2.0, false);
  EXPECT_TRUE(none.empty());

  // Shift adds the rect result to the previous selection, sorted unique.
  const game::SquadSelection with_shift = game::select_squad_rect(
      simulation, selection_of({99u}), red_member.x - 1.0, red_member.y - 1.0,
      red_member.x + 1.0, red_member.y + 1.0, true);
  EXPECT_EQ(with_shift.squads,
            (std::vector<std::uint32_t>{red_squad->id, 99u}));
}

// ---------------------------------------------------------------------------
// Order intents and apply_order.

TEST(BattleInputOrderTest, IntentsNeedASelection) {
  const game::SquadSelection empty;
  EXPECT_FALSE(game::order_from_right_click(empty, false, 10.0, 10.0).valid);
  EXPECT_FALSE(
      game::order_from_hotkey(empty, game::OrderType::Hold).valid);
}

TEST(BattleInputOrderTest, RightClickMapsMoveToAndAttackMove) {
  const game::SquadSelection selection = selection_of({1, 3});
  const game::OrderIntent move =
      game::order_from_right_click(selection, false, 100.0, 200.0);
  ASSERT_TRUE(move.valid);
  EXPECT_EQ(move.type, game::OrderType::MoveTo);
  EXPECT_EQ(move.squads, selection.squads);
  EXPECT_DOUBLE_EQ(move.x, 100.0);
  EXPECT_DOUBLE_EQ(move.y, 200.0);
  const game::OrderIntent attack =
      game::order_from_right_click(selection, true, 100.0, 200.0);
  ASSERT_TRUE(attack.valid);
  EXPECT_EQ(attack.type, game::OrderType::AttackMove);
}

TEST(BattleInputOrderTest, HotkeysMapTypesAndApplyOrderReachesTheSquad) {
  const sim::UnitTypeTable types = squad_types();
  sim::Simulation simulation(squad_config(types, 2, 3));
  const sim::Squad& first_red = *std::find_if(
      simulation.squads().begin(), simulation.squads().end(),
      [](const sim::Squad& s) { return s.faction == sim::Faction::Red; });

  game::SquadSelection selection = selection_of({first_red.id});
  game::OrderIntent hold = game::order_from_hotkey(selection,
                                                   game::OrderType::Hold);
  ASSERT_TRUE(hold.valid);
  EXPECT_EQ(hold.type, game::OrderType::Hold);
  // issue_order queues the order for courier delivery and returns true
  // when accepted; player_order_active flips when it is delivered, so
  // run the battle until the squad holds the order.
  EXPECT_TRUE(game::apply_order(simulation, hold));
  bool delivered = false;
  for (int i = 0; i < 400 && !delivered; ++i) {
    simulation.tick();
    delivered = simulation.squads()[first_red.id].player_order_active;
  }
  EXPECT_TRUE(delivered) << "the Hold order never delivered";

  // A second order after a tick (the per-tick rate limit is per tick):
  // right-click MoveTo to a map point.
  game::OrderIntent move = game::order_from_right_click(selection, false,
                                                        400.0, 500.0);
  EXPECT_TRUE(game::apply_order(simulation, move));

  // An invalid intent is a no-op.
  game::OrderIntent none;
  EXPECT_FALSE(game::apply_order(simulation, none));
}

TEST(BattleInputOrderTest, Glyphs) {
  EXPECT_STREQ(game::order_glyph(game::OrderType::MoveTo), "M");
  EXPECT_STREQ(game::order_glyph(game::OrderType::AttackMove), "A");
  EXPECT_STREQ(game::order_glyph(game::OrderType::Hold), "H");
  EXPECT_STREQ(game::order_glyph(game::OrderType::Retreat), "R");
  EXPECT_STREQ(game::order_glyph(game::OrderType::Regroup), "G");
}

// ---------------------------------------------------------------------------
// Minimap geometry.

TEST(BattleInputMinimapTest, PlacementAndCoverage) {
  const game::MinimapGeometry geometry =
      game::minimap_geometry(1280, 720, 1000.0, 1000.0);
  EXPECT_EQ(geometry.x, 8);
  EXPECT_EQ(geometry.y, 720 - 200 - 8);
  EXPECT_EQ(geometry.size, 200);
  EXPECT_TRUE(game::minimap_covers(geometry, 8, 512));
  EXPECT_TRUE(game::minimap_covers(geometry, 207, 711));
  EXPECT_FALSE(game::minimap_covers(geometry, 7, 512));
  EXPECT_FALSE(game::minimap_covers(geometry, 208, 712));
}

TEST(BattleInputMinimapTest, WorldMapRoundTripAndClamping) {
  const game::MinimapGeometry geometry =
      game::minimap_geometry(1280, 720, 1000.0, 500.0);
  // Corners map to the square corners.
  EXPECT_DOUBLE_EQ(game::minimap_x(geometry, 0.0), 8.0);
  EXPECT_DOUBLE_EQ(game::minimap_x(geometry, 1000.0), 208.0);
  EXPECT_DOUBLE_EQ(game::minimap_y(geometry, 0.0), 512.0);
  EXPECT_DOUBLE_EQ(game::minimap_y(geometry, 500.0), 712.0);
  // Round trip through the click mapping.
  for (double wx : {0.0, 123.0, 555.0, 1000.0}) {
    EXPECT_NEAR(game::world_x_from_minimap(
                    geometry, game::minimap_x(geometry, wx)),
                wx, 1e-9);
  }
  for (double wy : {0.0, 99.5, 480.0, 500.0}) {
    EXPECT_NEAR(game::world_y_from_minimap(
                    geometry, game::minimap_y(geometry, wy)),
                wy, 1e-9);
  }
  // Pixels outside the square clamp to the battlefield (the window can
  // feed a sloppy click without checks of its own).
  EXPECT_DOUBLE_EQ(game::world_x_from_minimap(geometry, -50.0), 0.0);
  EXPECT_DOUBLE_EQ(game::world_x_from_minimap(geometry, 999.0), 1000.0);
  EXPECT_DOUBLE_EQ(game::world_y_from_minimap(geometry, 0.0), 0.0);
  EXPECT_DOUBLE_EQ(game::world_y_from_minimap(geometry, 800.0), 500.0);
}

// ---------------------------------------------------------------------------
// The renderer: selection rings, order markers, minimap.

TEST(BattleView5aTest, OrderMarkersAndMinimapDraw) {
  const sim::UnitTypeTable types = squad_types();
  sim::Simulation simulation(squad_config(types, 2, 3));
  const sim::Squad* first_red = nullptr;
  for (const sim::Squad& squad : simulation.squads()) {
    if (squad.faction == sim::Faction::Red && first_red == nullptr) {
      first_red = &squad;
      break;
    }
  }
  ASSERT_NE(first_red, nullptr);
  ASSERT_TRUE(simulation.issue_order(sim::Faction::Red,
                                     std::vector<std::uint32_t>{first_red->id},
                                     sim::PlayerOrderType::MoveTo, 400.0,
                                     500.0));
  // Courier delivery: run until the squad carries the order.
  bool delivered = false;
  for (int i = 0; i < 400 && !delivered; ++i) {
    simulation.tick();
    delivered = simulation.squads()[first_red->id].player_order_active;
  }
  ASSERT_TRUE(delivered) << "the MoveTo order never delivered";

  game::ViewOptions options;
  options.show_hud = false;  // keep the command stream lean
  options.show_order_markers = true;  // interactive layers (window-only)
  options.show_minimap = true;
  game::Camera camera =
      game::Camera::fit(1000.0, 1000.0, 1280, 720);
  const render::HudStats stats;
  const render::DrawList list = render::build_battle_draw_list(
      simulation, camera, options, 1280, 720, stats);

  // The MoveTo marker: a Text command with glyph "M" near the target.
  bool saw_marker = false;
  for (const render::DrawCmd& cmd : list.cmds) {
    if (cmd.kind == render::CmdKind::Text && cmd.text == "M") {
      saw_marker = true;
      break;
    }
  }
  EXPECT_TRUE(saw_marker) << "no MoveTo marker glyph in the draw list";

  // The minimap: a filled panel rect at (8, 512) of size 200.
  bool saw_minimap = false;
  for (const render::DrawCmd& cmd : list.cmds) {
    if (cmd.kind == render::CmdKind::Rect && cmd.e == 1.0 &&
        cmd.a == 8.0 && cmd.b == 512.0 && cmd.c == 200.0 && cmd.d == 200.0) {
      saw_minimap = true;
      break;
    }
  }
  EXPECT_TRUE(saw_minimap) << "no minimap panel in the draw list";

  // Toggling the options off removes both.
  options.show_minimap = false;
  options.show_order_markers = false;
  const render::DrawList lean = render::build_battle_draw_list(
      simulation, camera, options, 1280, 720, stats);
  for (const render::DrawCmd& cmd : lean.cmds) {
    if (cmd.kind == render::CmdKind::Text && cmd.text == "M") {
      FAIL() << "marker drawn with show_order_markers off";
    }
    if (cmd.kind == render::CmdKind::Rect && cmd.e == 1.0 && cmd.a == 8.0 &&
        cmd.b == 512.0 && cmd.c == 200.0 && cmd.d == 200.0) {
      FAIL() << "minimap drawn with show_minimap off";
    }
  }
}

TEST(BattleView5aTest, SelectionRingsDrawPerSquadMember) {
  const sim::UnitTypeTable types = squad_types();
  sim::Simulation simulation(squad_config(types, 2, 3));
  const sim::Squad* first_red = nullptr;
  for (const sim::Squad& squad : simulation.squads()) {
    if (squad.faction == sim::Faction::Red && first_red == nullptr) {
      first_red = &squad;
      break;
    }
  }
  ASSERT_NE(first_red, nullptr);
  simulation.tick();  // computes the squad centroid (camera target)

  game::ViewOptions options;
  options.show_hud = false;
  options.show_minimap = false;
  options.show_order_markers = false;
  options.show_badges = false;
  options.squad_links = false;
  options.show_grid = false;
  options.show_terrain = false;
  options.selected_squads.push_back(
      static_cast<std::int32_t>(first_red->id));
  // Fresh squads are still scattered across the spawn band before they
  // form up, so fit the whole battlefield and drop the point-zoom
  // threshold: every member is on screen and draws its ring (the
  // renderer skips off-screen rings by design).
  const game::Camera camera = game::Camera::fit(1000.0, 1000.0, 1280, 720);
  options.point_zoom_threshold = 0.0;
  const render::HudStats stats;
  const render::DrawList list = render::build_battle_draw_list(
      simulation, camera, options, 1280, 720, stats);

  // Outline circles in the selection color: one per alive member of the
  // selected squad (radius 3, outline-only).
  std::size_t rings = 0;
  for (const render::DrawCmd& cmd : list.cmds) {
    if (cmd.kind == render::CmdKind::Circle && cmd.e == 1.0 &&
        cmd.c == 3.0 && cmd.color2.r == game::selected_outline().r &&
        cmd.color2.g == game::selected_outline().g) {
      ++rings;
    }
  }
  EXPECT_EQ(rings, first_red->members.size())
      << "expected one ring per squad member";
}
