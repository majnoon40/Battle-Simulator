// m13 item 1 — the window shell policy tests: scenario listing, input
// gating (ui_covers, arrow ownership), battle-screen click parsing, the
// zoom slider mapping and the per-frame viewer-state fold. These pin the
// contract the raylib window (game/src/main.cpp, compile-verified only)
// depends on; the human checklist for the window itself is in
// docs/viewer.md.
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "game/camera.hpp"
#include "game/view.hpp"
#include "render/ui/demo.hpp"
#include "render/ui/screens.hpp"
#include "render/ui/shell.hpp"

namespace fs = std::filesystem;

namespace {

const std::vector<std::string>& test_scenarios() {
  static const std::vector<std::string> list{"default", "squads",
                                             "viewer-demo"};
  return list;
}

// The widget index with `id` in a state's tree, or -1.
std::int32_t find_id(const render::ui::State& state,
                     const std::string& id) {
  for (std::size_t i = 0; i < state.tree.size(); ++i) {
    if (state.tree.at(i).id == id) {
      return static_cast<std::int32_t>(i);
    }
  }
  return -1;
}

render::ui::Point center_of(const render::ui::State& state,
                            const std::string& id) {
  const std::int32_t index = find_id(state, id);
  EXPECT_GE(index, 0) << "widget " << id << " missing";
  const render::ui::Rect& rect = state.tree.at(static_cast<std::size_t>(index)).rect;
  return render::ui::Point{rect.x + rect.w / 2, rect.y + rect.h / 2};
}

render::ui::App click_at(render::ui::App app, render::ui::Point where) {
  const auto down = [&where] {
    render::ui::Event event;
    event.type = render::ui::Event::Type::MouseDown;
    event.pos = where;
    event.button = 0;
    return event;
  };
  const auto up = [&where] {
    render::ui::Event event;
    event.type = render::ui::Event::Type::MouseUp;
    event.pos = where;
    event.button = 0;
    return event;
  };
  app = render::ui::reduce_app(std::move(app), down(), test_scenarios());
  app = render::ui::reduce_app(std::move(app), up(), test_scenarios());
  return app;
}

render::ui::App move_to(render::ui::App app, render::ui::Point where) {
  render::ui::Event event;
  event.type = render::ui::Event::Type::MouseMove;
  event.pos = where;
  return render::ui::reduce_app(std::move(app), event, test_scenarios());
}

}  // namespace

TEST(ShellTest, ScenarioListIsSortedJsonStems) {
  const std::vector<std::string> names = render::ui::list_scenarios(
      (fs::path(TEST_DATA_DIR) / "scenarios").string());
  ASSERT_FALSE(names.empty());
  EXPECT_TRUE(std::is_sorted(names.begin(), names.end()));
  for (const std::string& name : names) {
    EXPECT_FALSE(name.empty());
    EXPECT_EQ(name.find(".json"), std::string::npos) << name;
  }
  for (const char* expected : {"default", "squads", "viewer-demo"}) {
    EXPECT_NE(std::find(names.begin(), names.end(), expected), names.end())
        << "scenario " << expected << " missing";
  }
}

TEST(ShellTest, UiCoversTheMenuBoxButNotTheMap) {
  render::ui::App app =
      render::ui::sandbox_app(800, 600, test_scenarios());
  app.screen = render::ui::Screen::Menu;
  // Inside the centered menu content box (the Quit button) is UI.
  const render::ui::Point in_box = center_of(app.menu, "quit");
  EXPECT_TRUE(render::ui::ui_covers(app, in_box));
  // The screen corners are background.
  EXPECT_FALSE(render::ui::ui_covers(app, {5, 5}));
  EXPECT_FALSE(render::ui::ui_covers(app, {795, 595}));
}

TEST(ShellTest, UiCoversTheBattlePanelsNotTheField) {
  render::ui::App app =
      render::ui::sandbox_app(1280, 720, test_scenarios());
  app.screen = render::ui::Screen::Battle;
  // The control panel's pause button is UI.
  EXPECT_TRUE(
      render::ui::ui_covers(app, center_of(app.battle, "pause")));
  // The map area dominates the screen: a coarse grid must be mostly
  // uncovered (the battle view keeps the mouse there).
  int covered = 0;
  int total = 0;
  for (int x = 40; x < 1280; x += 40) {
    for (int y = 40; y < 720; y += 40) {
      ++total;
      covered +=
          render::ui::ui_covers(app, {x, y}) ? 1 : 0;
    }
  }
  EXPECT_GT(covered, 0) << "no UI panels found on the battle screen";
  EXPECT_LT(covered, total / 2) << "the UI covers most of the map";
}

TEST(ShellTest, ArrowOwnershipFollowsScreenAndFocus) {
  render::ui::App app =
      render::ui::sandbox_app(800, 600, test_scenarios());
  // Menus own both arrow groups.
  app.screen = render::ui::Screen::Menu;
  EXPECT_TRUE(render::ui::ui_arrow_ownership(app).left_right);
  EXPECT_TRUE(render::ui::ui_arrow_ownership(app).up_down);
  app.screen = render::ui::Screen::Scenario;
  EXPECT_TRUE(render::ui::ui_arrow_ownership(app).left_right);
  EXPECT_TRUE(render::ui::ui_arrow_ownership(app).up_down);
  // The battle screen with a button focused owns neither (arrows pan).
  app.screen = render::ui::Screen::Battle;
  EXPECT_FALSE(render::ui::ui_arrow_ownership(app).left_right);
  EXPECT_FALSE(render::ui::ui_arrow_ownership(app).up_down);
  // A focused slider owns Left/Right (the zoom drag).
  const std::int32_t zoom = find_id(app.battle, "zoom");
  ASSERT_GE(zoom, 0);
  app.battle.focus = zoom;
  EXPECT_TRUE(render::ui::ui_arrow_ownership(app).left_right);
  EXPECT_FALSE(render::ui::ui_arrow_ownership(app).up_down);
  // A focused open select owns Up/Down (the filter list).
  const std::int32_t filter = find_id(app.battle, "filter");
  ASSERT_GE(filter, 0);
  app.battle.focus = filter;
  app.battle.tree.at(static_cast<std::size_t>(filter)).on = true;
  EXPECT_FALSE(render::ui::ui_arrow_ownership(app).left_right);
  EXPECT_TRUE(render::ui::ui_arrow_ownership(app).up_down);
}

TEST(ShellTest, BattleClicksParseTheMomentaryButtons) {
  render::ui::State state;
  state.clicked = "";
  EXPECT_FALSE(render::ui::battle_clicks(state).pause);
  EXPECT_FALSE(render::ui::battle_clicks(state).play);
  EXPECT_EQ(render::ui::battle_clicks(state).speed, 0);
  state.clicked = "pause";
  EXPECT_TRUE(render::ui::battle_clicks(state).pause);
  state.clicked = "play";
  EXPECT_TRUE(render::ui::battle_clicks(state).play);
  state.clicked = "speed1";
  EXPECT_EQ(render::ui::battle_clicks(state).speed, 1);
  state.clicked = "speed2";
  EXPECT_EQ(render::ui::battle_clicks(state).speed, 2);
  state.clicked = "speed4";
  EXPECT_EQ(render::ui::battle_clicks(state).speed, 4);
  // Navigation ids never look like battle controls.
  state.clicked = "start";
  EXPECT_EQ(render::ui::battle_clicks(state).speed, 0);
}

TEST(ShellTest, ZoomSliderMapsSymmetricallyAroundBase) {
  const double base = 2.0;
  EXPECT_DOUBLE_EQ(render::ui::zoom_from_slider(0.5, base), base);
  EXPECT_DOUBLE_EQ(render::ui::zoom_from_slider(1.0, base), 16.0 * base);
  EXPECT_DOUBLE_EQ(render::ui::zoom_from_slider(0.0, base), base / 16.0);
  // Clamped to the camera limits.
  EXPECT_DOUBLE_EQ(render::ui::zoom_from_slider(1.0, 32.0),
                   game::kMaxZoom);
  // The inverse round-trips every quarter step.
  for (int step = 0; step <= 4; ++step) {
    const double slider = 0.25 * step;
    const double zoom = render::ui::zoom_from_slider(slider, base);
    EXPECT_NEAR(render::ui::slider_from_zoom(zoom, base), slider, 1e-9);
  }
  // The window follows wheel zoom back into the slider exactly.
  EXPECT_DOUBLE_EQ(
      render::ui::slider_from_zoom(render::ui::zoom_from_slider(0.7, 3.0),
                                   3.0),
      0.7);
}

TEST(ShellTest, ApplyBattleFrameFoldsClicksAndViewState) {
  render::ui::App app =
      render::ui::sandbox_app(800, 600, test_scenarios());
  app.screen = render::ui::Screen::Battle;
  const double base = render::ui::zoom_from(
      app.battle);  // initial slider position -> base mapping anchor
  const double base_zoom = 2.0;

  render::ui::BattleView view =
      render::ui::apply_battle_frame(app, render::ui::BattleView{},
                                     base_zoom);
  EXPECT_FALSE(view.paused);
  EXPECT_DOUBLE_EQ(view.speed, 1.0);
  EXPECT_TRUE(view.options.show_terrain);

  // Click Pause through the real reducer path: the click lands in
  // battle.clicked and folds into the sticky viewer state.
  const render::ui::Point point_1 = center_of(app.battle, "pause");
  app = move_to(std::move(app), point_1);
  const render::ui::Point point_2 = center_of(app.battle, "pause");
  app = click_at(std::move(app), point_2);
  view = render::ui::apply_battle_frame(app, view, base_zoom);
  EXPECT_TRUE(view.paused);
  // A quiet frame keeps it paused.
  app = move_to(std::move(app), {400, 300});
  view = render::ui::apply_battle_frame(app, view, base_zoom);
  EXPECT_TRUE(view.paused);
  // Play unpauses, speed4 sets the speed.
  const render::ui::Point point_3 = center_of(app.battle, "play");
  app = click_at(std::move(app), point_3);
  view = render::ui::apply_battle_frame(app, view, base_zoom);
  EXPECT_FALSE(view.paused);
  const render::ui::Point point_4 = center_of(app.battle, "speed4");
  app = click_at(std::move(app), point_4);
  view = render::ui::apply_battle_frame(app, view, base_zoom);
  EXPECT_DOUBLE_EQ(view.speed, 4.0);
  // The terrain checkbox unchecks -> options follow every frame.
  const render::ui::Point point_5 = center_of(app.battle, "terrain");
  app = click_at(std::move(app), point_5);
  view = render::ui::apply_battle_frame(app, view, base_zoom);
  EXPECT_FALSE(view.options.show_terrain);
  // The zoom slider maps through the shell's zoom mapping.
  const std::int32_t zoom = find_id(app.battle, "zoom");
  app.battle.tree.at(static_cast<std::size_t>(zoom)).value = 1.0;
  view = render::ui::apply_battle_frame(app, view, base_zoom);
  EXPECT_DOUBLE_EQ(view.zoom, 16.0 * base_zoom);
  (void)base;
}

TEST(ShellTest, ShellSessionWalksMenuToBattle) {
  // The exact flow the window drives: menu -> picker (Start) -> battle
  // screen, with the viewer state derived through the shell policy.
  render::ui::App app =
      render::ui::sandbox_app(1280, 720, test_scenarios());
  app.screen = render::ui::Screen::Menu;
  const render::ui::Point point_6 = center_of(app.menu, "scenario");
  app = click_at(std::move(app), point_6);
  EXPECT_EQ(app.screen, render::ui::Screen::Scenario);
  // Pick "squads" in the select (second item), then Start.
  const std::int32_t list = find_id(app.scenario, "scenario-list");
  ASSERT_GE(list, 0);
  app.scenario.tree.at(static_cast<std::size_t>(list)).selected = 1;
  const render::ui::Point point_7 = center_of(app.scenario, "start");
  app = click_at(std::move(app), point_7);
  EXPECT_EQ(app.screen, render::ui::Screen::Battle);
  EXPECT_EQ(app.chosen_scenario, "squads");
  // The viewer state is derivable the moment the battle screen shows.
  const render::ui::BattleView view = render::ui::apply_battle_frame(
      app, render::ui::BattleView{}, 2.0);
  EXPECT_FALSE(view.paused);
  EXPECT_TRUE(view.options.show_hud);
  // Quit works from the menu.
  app.screen = render::ui::Screen::Menu;
  const render::ui::Point point_8 = center_of(app.menu, "quit");
  app = click_at(std::move(app), point_8);
  EXPECT_TRUE(app.quit);
}
