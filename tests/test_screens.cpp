// m11d — sandbox screens tests: the three screens build and lay out
// validly at three resolutions, navigation clicks walk the screen
// flow, the view options map from the battle screen, and the
// --ui-script harness renders the menu and scenario screens.
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "render/ui/screens.hpp"

namespace fs = std::filesystem;

namespace {

const std::pair<int, int> kResolutions[] = {{800, 600}, {1280, 720},
                                            {1920, 1080}};
const std::vector<std::string> kScenarios = {
    "default", "earth-lebanon-v2", "earth-nile-delta-v2", "squads"};

std::string read_text(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

void expect_screen_valid(const render::ui::State& state) {
  ASSERT_GT(state.tree.size(), std::size_t{5});
  for (std::size_t i = 0; i < state.tree.size(); ++i) {
    const render::ui::Widget& widget = state.tree.at(i);
    if (widget.kind == render::ui::Kind::Panel && !widget.chrome) {
      continue;  // layout-only spacers may be zero-sized
    }
    EXPECT_GE(widget.rect.x, 0) << widget.id;
    EXPECT_GE(widget.rect.y, 0) << widget.id;
    EXPECT_GT(widget.rect.w, 0) << widget.id;
    EXPECT_GT(widget.rect.h, 0) << widget.id;
    EXPECT_LE(widget.rect.right(), state.width) << widget.id;
    EXPECT_LE(widget.rect.bottom(), state.height) << widget.id;
  }
}

// A full click (down + up at the widget center) as two events.
struct Click {
  render::ui::Event down;
  render::ui::Event up;
};

Click click_at(const render::ui::State& state, const std::string& id) {
  for (std::size_t i = 0; i < state.tree.size(); ++i) {
    const render::ui::Widget& widget = state.tree.at(i);
    if (widget.id == id && widget.interactive()) {
      const render::ui::Point center{widget.rect.x + widget.rect.w / 2,
                                     widget.rect.y + widget.rect.h / 2};
      return Click{render::ui::Event{
                       render::ui::Event::Type::MouseDown, {}, center, 0},
                   render::ui::Event{
                       render::ui::Event::Type::MouseUp, {}, center, 0}};
    }
  }
  return Click{render::ui::Event{render::ui::Event::Type::MouseMove},
               render::ui::Event{render::ui::Event::Type::MouseMove}};
}

// Applies a full click through the screen-level reducer.
render::ui::App click_widget(render::ui::App app, const std::string& id,
                             const std::vector<std::string>& scenarios) {
  const Click click = click_at(app.active(), id);
  app = render::ui::reduce_app(std::move(app), click.down, scenarios);
  return render::ui::reduce_app(std::move(app), click.up, scenarios);
}

}  // namespace

TEST(SandboxScreensTest, AllScreensValidAtThreeResolutions) {
  for (const auto& [width, height] : kResolutions) {
    render::ui::App app =
        render::ui::sandbox_app(width, height, kScenarios);
    ASSERT_EQ(app.screen, render::ui::Screen::Menu);
    expect_screen_valid(app.menu);
    expect_screen_valid(app.scenario);
    expect_screen_valid(app.battle);
    // The menu box centers: strictly inside the window on both axes.
    for (const auto& widget : app.menu.tree.widgets) {
      if (widget.id == "menu-box") {
        EXPECT_GT(widget.rect.x, 0);
        EXPECT_GT(widget.rect.y, 0);
        EXPECT_LT(widget.rect.right(), width);
        EXPECT_LT(widget.rect.bottom(), height);
      }
    }
  }
}

TEST(SandboxScreensTest, NavigationWalksTheScreenFlow) {
  render::ui::App app = render::ui::sandbox_app(1280, 720, kScenarios);
  // Menu -> New battle -> the scenario screen.
  app = click_widget(std::move(app), "new-battle", kScenarios);
  EXPECT_EQ(app.screen, render::ui::Screen::Scenario);
  // Pick the second scenario (earth-lebanon-v2): open, walk down, open
  // again to confirm... open + click the item row.
  const Click open = click_at(app.scenario, "scenario-list");
  app = render::ui::reduce_app(std::move(app), open.down, kScenarios);
  // The select is open; click its second item (row 1 below the head).
  render::ui::Rect list{};
  for (const auto& widget : app.scenario.tree.widgets) {
    if (widget.id == "scenario-list") {
      list = widget.rect;
    }
  }
  const render::ui::Event pick{
      render::ui::Event::Type::MouseDown, {},
      {list.x + list.w / 2, list.y + render::ui::default_theme().row_height +
                                  render::ui::default_theme().row_height / 2},
      0};
  app = render::ui::reduce_app(std::move(app), pick, kScenarios);
  // Start -> the battle screen with the chosen scenario.
  app = click_widget(std::move(app), "start", kScenarios);
  EXPECT_EQ(app.screen, render::ui::Screen::Battle);
  EXPECT_EQ(app.chosen_scenario, "earth-lebanon-v2");
  // Quit on the menu sets the flag.
  render::ui::App menu = render::ui::sandbox_app(1280, 720, kScenarios);
  menu = click_widget(std::move(menu), "quit", kScenarios);
  EXPECT_TRUE(menu.quit);
}

TEST(SandboxScreensTest, OptionsMapFromTheBattleScreen) {
  render::ui::App app = render::ui::sandbox_app(1280, 720, kScenarios);
  app.screen = render::ui::Screen::Battle;  // click the battle panel
  game::ViewOptions options = render::ui::options_from(app.battle);
  EXPECT_TRUE(options.show_hud);
  EXPECT_TRUE(options.show_terrain);
  EXPECT_TRUE(options.squad_links);
  EXPECT_TRUE(options.show_grid);
  // Toggle the checkboxes off through the reducer (clicks).
  for (const std::string id : {"hud", "terrain", "squad-links", "grid"}) {
    app = click_widget(std::move(app), id, kScenarios);
  }
  options = render::ui::options_from(app.battle);
  EXPECT_FALSE(options.show_hud);
  EXPECT_FALSE(options.show_terrain);
  EXPECT_FALSE(options.squad_links);
  EXPECT_FALSE(options.show_grid);
  EXPECT_DOUBLE_EQ(render::ui::zoom_from(app.battle), 0.5);
}

TEST(SandboxScreensTest, HarnessRendersMenuAndScenarioScreens) {
  for (const char* screen : {"menu", "scenario"}) {
    const std::string dir = std::string("/tmp/m11d_") + screen;
    fs::create_directories(dir);
    {
      std::ofstream file(dir + "/script.json");
      file << R"({
        "width": 1280, "height": 720, "tick": 0,
        "screen": ")" << screen
           << R"(",
        "scenarios": ["default", "squads"],
        "out": ")" << dir << R"(/f-%d.png",
        "events": [
          {"frame": 0, "type": "move", "x": 640, "y": 360},
          {"frame": 1, "type": "down", "x": 640, "y": 360},
          {"frame": 2, "type": "up", "x": 640, "y": 360}
        ]
      })";
    }
    const std::string cli =
        "cd \"" TEST_DATA_DIR "/..\" && \"" BATTLE_CLI_PATH
        "\" --scenario data/scenarios/squads.json --seed 1 --ticks 10"
        " --ui-script " +
        dir + "/script.json > " + dir + "/out.txt 2>&1";
    ASSERT_EQ(std::system(cli.c_str()), 0);
    const std::string out = read_text(dir + "/out.txt");
    EXPECT_NE(out.find(std::string("screen ") + screen),
              std::string::npos)
        << out;
    const fs::path png = dir + "/f-2.png";
    ASSERT_TRUE(fs::exists(png));
    EXPECT_GT(fs::file_size(png), std::uintmax_t{2000});
    EXPECT_EQ(read_text(png).substr(0, 4), std::string("\x89PNG", 4));
  }
}
