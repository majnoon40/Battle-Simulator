// m11c — UI framework tests: shared text metrics, layout invariants at
// three resolutions, focus cycling, the pure reducer (determinism and
// keyboard/mouse interactions), draw-list determinism, and the
// --ui-script harness end to end (PNG snapshots + pinned state lines).
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "render/raster.hpp"
#include "render/ui/demo.hpp"
#include "render/ui/ui.hpp"

namespace fs = std::filesystem;

namespace {

const std::pair<int, int> kResolutions[] = {{800, 600}, {1280, 720},
                                            {1920, 1080}};

std::string read_text(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

// Every widget rect inside the window; panels hold their children.
void expect_layout_valid(const render::ui::State& state) {
  ASSERT_GT(state.tree.size(), std::size_t{10});
  for (std::size_t i = 0; i < state.tree.size(); ++i) {
    const render::ui::Widget& widget = state.tree.at(i);
    EXPECT_GE(widget.rect.x, 0) << widget.id;
    EXPECT_GE(widget.rect.y, 0) << widget.id;
    EXPECT_GT(widget.rect.w, 0) << widget.id << " empty width";
    EXPECT_GT(widget.rect.h, 0) << widget.id << " empty height";
    EXPECT_LE(widget.rect.right(), state.width) << widget.id;
    EXPECT_LE(widget.rect.bottom(), state.height) << widget.id;
  }
  // The status bar hugs the bottom of the window.
  const render::ui::Widget& bar = state.tree.at(
      static_cast<std::size_t>(state.tree.size() - 2));
  EXPECT_EQ(bar.id, "status-bar");
  EXPECT_EQ(bar.rect.bottom(), state.height - 8);
}

}  // namespace

TEST(UiTextTest, MetricsMatchTheRasterizerFont) {
  // 5x7 font: scale = (px+3)/7, advance 6*scale, height 7*scale.
  EXPECT_EQ(render::ui::font_scale(7), 1);
  EXPECT_EQ(render::ui::font_scale(14), 2);
  EXPECT_EQ(render::ui::font_scale(20), 3);
  EXPECT_EQ(render::ui::text_advance("Ready", 14), 5 * 12);
  EXPECT_EQ(render::ui::text_advance("", 14), 0);
  EXPECT_EQ(render::ui::text_height(14), 14);
}

TEST(UiLayoutTest, DemoLayoutIsValidAtThreeResolutions) {
  for (const auto& [width, height] : kResolutions) {
    render::ui::State state = render::ui::demo_state(width, height);
    ASSERT_EQ(state.width, width);
    ASSERT_EQ(state.height, height);
    expect_layout_valid(state);
    // The control panel: fixed width, at the root padding corner.
    const render::ui::Widget& controls = state.tree.at(1);
    EXPECT_EQ(controls.id, "controls");
    EXPECT_EQ(controls.rect.x, 8);
    EXPECT_EQ(controls.rect.y, 8);
    EXPECT_EQ(controls.rect.w, 240);
    // Siblings never overlap (the root's children, and every panel's).
    for (std::size_t i = 0; i < state.tree.size(); ++i) {
      const auto& widget = state.tree.at(i);
      if (widget.kind != render::ui::Kind::Panel) {
        continue;
      }
      std::vector<render::ui::Rect> rects;
      for (std::size_t j = 0; j < state.tree.size(); ++j) {
        if (state.tree.parent[j] == static_cast<std::int32_t>(i)) {
          rects.push_back(state.tree.at(j).rect);
        }
      }
      for (std::size_t a = 0; a < rects.size(); ++a) {
        for (std::size_t b = a + 1; b < rects.size(); ++b) {
          EXPECT_FALSE(rects[a].intersects(rects[b]))
              << "siblings overlap under " << widget.id << " at "
              << width << "x" << height;
        }
      }
    }
  }
}

TEST(UiFocusTest, TabCyclesThroughEveryInteractiveWidget) {
  render::ui::State state = render::ui::demo_state(1280, 720);
  // Count the interactive widgets; tabbing must visit them in tree
  // order and wrap back to the first.
  std::vector<std::string> expected;
  for (const auto& widget : state.tree.widgets) {
    if (widget.interactive()) {
      expected.push_back(widget.id);
    }
  }
  ASSERT_GE(expected.size(), std::size_t{10});
  ASSERT_EQ(state.tree.at(static_cast<std::size_t>(state.focus)).id,
            expected.front());  // the demo starts focused on Pause
  render::ui::State walked = state;
  for (std::size_t step = 0; step < expected.size(); ++step) {
    walked = render::ui::reduce(
        std::move(walked),
        render::ui::Event{render::ui::Event::Type::Key,
                          render::ui::Key::Tab});
    EXPECT_EQ(walked.tree.at(static_cast<std::size_t>(walked.focus)).id,
              expected[(step + 1) % expected.size()])
        << "tab step " << step;
  }
  // After a full cycle Up steps backward to the last interactive widget.
  render::ui::State back = render::ui::reduce(
      std::move(walked),
      render::ui::Event{render::ui::Event::Type::Key,
                        render::ui::Key::Up});
  EXPECT_EQ(back.tree.at(static_cast<std::size_t>(back.focus)).id,
            expected.back());
}

TEST(UiReducerTest, KeyboardInteracts) {
  const render::ui::Theme theme = render::ui::default_theme();
  render::ui::State state = render::ui::demo_state(1280, 720);
  // The initial focus is the Pause button: Enter clicks it.
  state = render::ui::reduce(
      std::move(state),
      render::ui::Event{render::ui::Event::Type::Key,
                        render::ui::Key::Enter}, theme);
  EXPECT_EQ(state.clicked, "pause");
  // Tab onto the first checkbox (hud — past the speed
  // buttons): Enter toggles it.
  for (int step = 0; step < 12; ++step) {
    state = render::ui::reduce(
        std::move(state),
        render::ui::Event{render::ui::Event::Type::Key,
                          render::ui::Key::Tab}, theme);
    if (state.tree.at(static_cast<std::size_t>(state.focus)).id ==
        "hud") {
      break;
    }
  }
  const std::string focused_id =
      state.tree.at(static_cast<std::size_t>(state.focus)).id;
  EXPECT_EQ(focused_id, "hud");
  state = render::ui::reduce(
      std::move(state),
      render::ui::Event{render::ui::Event::Type::Key,
                        render::ui::Key::Enter}, theme);
  // The demo checkboxes start on (the viewer defaults); Enter toggles
  // this one off.
  EXPECT_FALSE(state.tree.at(static_cast<std::size_t>(state.focus)).on);
  // Left/Right adjust the focused slider by 1/32 (clamped at 0).
  render::ui::State slider = render::ui::demo_state(1280, 720);
  // Tab until the zoom slider has focus.
  for (int step = 0; step < 20; ++step) {
    if (slider.tree.at(static_cast<std::size_t>(slider.focus)).id ==
        "zoom") {
      break;
    }
    slider = render::ui::reduce(
        std::move(slider),
        render::ui::Event{render::ui::Event::Type::Key,
                          render::ui::Key::Tab}, theme);
  }
  ASSERT_EQ(
      slider.tree.at(static_cast<std::size_t>(slider.focus)).id, "zoom");
  const double before =
      slider.tree.at(static_cast<std::size_t>(slider.focus)).value;
  slider = render::ui::reduce(
      std::move(slider),
      render::ui::Event{render::ui::Event::Type::Key,
                        render::ui::Key::Left}, theme);
  EXPECT_NEAR(slider.tree.at(static_cast<std::size_t>(slider.focus)).value,
              before - 1.0 / 32.0, 1e-12);
  for (int step = 0; step < 64; ++step) {
    slider = render::ui::reduce(
        std::move(slider),
        render::ui::Event{render::ui::Event::Type::Key,
                          render::ui::Key::Left}, theme);
  }
  EXPECT_DOUBLE_EQ(
      slider.tree.at(static_cast<std::size_t>(slider.focus)).value, 0.0);
}

TEST(UiReducerTest, MouseInteracts) {
  const render::ui::Theme theme = render::ui::default_theme();
  render::ui::State state = render::ui::demo_state(1280, 720);
  // Click the Play button: down + up over it fires `clicked`.
  render::ui::Rect play{};
  for (std::size_t i = 0; i < state.tree.size(); ++i) {
    if (state.tree.at(i).id == "play") {
      play = state.tree.at(i).rect;
    }
  }
  ASSERT_GT(play.w, 0);
  const render::ui::Point center{play.x + play.w / 2,
                                 play.y + play.h / 2};
  state = render::ui::reduce(
      std::move(state),
      render::ui::Event{render::ui::Event::Type::MouseDown, {},
                        center, 0}, theme);
  EXPECT_EQ(state.pressed, state.hover);
  state = render::ui::reduce(
      std::move(state),
      render::ui::Event{render::ui::Event::Type::MouseUp, {}, center, 0},
      theme);
  EXPECT_EQ(state.clicked, "play");

  // A drag off the button cancels the click.
  render::ui::State cancel = render::ui::demo_state(1280, 720);
  cancel = render::ui::reduce(
      std::move(cancel),
      render::ui::Event{render::ui::Event::Type::MouseDown, {},
                        center, 0}, theme);
  cancel = render::ui::reduce(
      std::move(cancel),
      render::ui::Event{render::ui::Event::Type::MouseMove, {},
                        {center.x + 400, center.y}, 0}, theme);
  cancel = render::ui::reduce(
      std::move(cancel),
      render::ui::Event{render::ui::Event::Type::MouseUp, {},
                        {center.x + 400, center.y}, 0}, theme);
  EXPECT_TRUE(cancel.clicked.empty());

  // A checkbox click toggles; a slider drag follows the pointer.
  render::ui::State toggle = render::ui::demo_state(1280, 720);
  render::ui::Rect box{};
  for (std::size_t i = 0; i < toggle.tree.size(); ++i) {
    if (toggle.tree.at(i).id == "terrain") {
      box = toggle.tree.at(i).rect;
    }
  }
  const render::ui::Point box_center{box.x + box.w / 2,
                                     box.y + box.h / 2};
  toggle = render::ui::reduce(
      std::move(toggle),
      render::ui::Event{render::ui::Event::Type::MouseDown, {},
                        box_center, 0}, theme);
  for (std::size_t i = 0; i < toggle.tree.size(); ++i) {
    if (toggle.tree.at(i).id == "terrain") {
      // Starts on; the click toggles it off.
      EXPECT_FALSE(toggle.tree.at(i).on);
    }
  }
}

TEST(UiReducerTest, SelectOpensPicksAndCloses) {
  const render::ui::Theme theme = render::ui::default_theme();
  render::ui::State state = render::ui::demo_state(1280, 720);
  render::ui::Rect select{};
  std::size_t select_index = 0;
  for (std::size_t i = 0; i < state.tree.size(); ++i) {
    if (state.tree.at(i).id == "filter") {
      select = state.tree.at(i).rect;
      select_index = i;
    }
  }
  const render::ui::Point closed_center{select.x + select.w / 2,
                                        select.y + select.h / 2};
  // Open: the rect grows to one row per item (5 items).
  state = render::ui::reduce(
      std::move(state),
      render::ui::Event{render::ui::Event::Type::MouseDown, {},
                        closed_center, 0}, theme);
  EXPECT_TRUE(state.tree.at(select_index).on);
  EXPECT_EQ(state.tree.at(select_index).rect.h, 5 * theme.row_height);
  expect_layout_valid(state);
  // Pick the third item (Archers): row index 2 below the closed row.
  const render::ui::Point item{select.x + select.w / 2,
                               select.y + 2 * theme.row_height +
                                   theme.row_height / 2};
  state = render::ui::reduce(
      std::move(state),
      render::ui::Event{render::ui::Event::Type::MouseDown, {}, item, 0},
      theme);
  EXPECT_FALSE(state.tree.at(select_index).on);
  EXPECT_EQ(state.tree.at(select_index).selected, 2);
  expect_layout_valid(state);
}

TEST(UiReducerTest, PureAndDeterministic) {
  const render::ui::Theme theme = render::ui::default_theme();
  const auto run = [&theme]() {
    render::ui::State state = render::ui::demo_state(1280, 720);
    const std::vector<render::ui::Event> events{
        {render::ui::Event::Type::Key, render::ui::Key::Tab},
        {render::ui::Event::Type::Key, render::ui::Key::Enter},
        {render::ui::Event::Type::MouseMove, {}, {60, 150}, 0},
        {render::ui::Event::Type::MouseDown, {}, {60, 150}, 0},
        {render::ui::Event::Type::MouseUp, {}, {60, 150}, 0},
        {render::ui::Event::Type::Key, render::ui::Key::Tab},
        {render::ui::Event::Type::Key, render::ui::Key::Right},
    };
    for (const render::ui::Event& event : events) {
      state = render::ui::reduce(std::move(state), event, theme);
    }
    return state;
  };
  const render::ui::State first = run();
  const render::ui::State second = run();
  // Identical event sequences reach identical states (rects included).
  ASSERT_EQ(first.tree.size(), second.tree.size());
  for (std::size_t i = 0; i < first.tree.size(); ++i) {
    EXPECT_EQ(first.tree.at(i).rect, second.tree.at(i).rect);
    EXPECT_EQ(first.tree.at(i).on, second.tree.at(i).on);
    EXPECT_EQ(first.tree.at(i).value, second.tree.at(i).value);
    EXPECT_EQ(first.tree.at(i).selected, second.tree.at(i).selected);
  }
  EXPECT_EQ(first.focus, second.focus);
  EXPECT_EQ(first.hover, second.hover);
}

TEST(UiDrawTest, DrawListIsDeterministicAndFocusMovesTheRing) {
  const render::ui::Theme theme = render::ui::default_theme();
  const auto draw = [&theme](render::ui::State state) {
    render::DrawList list;
    list.width = state.width;
    list.height = state.height;
    render::ui::draw_ui(state, theme, list);
    return list;
  };
  render::ui::State state = render::ui::demo_state(1280, 720);
  const render::DrawList first = draw(state);
  const render::DrawList again = draw(state);
  EXPECT_EQ(first, again);  // same state -> identical command list
  EXPECT_GT(first.cmds.size(), std::size_t{30});
  // Moving the focus changes the list (the focus ring moved).
  render::ui::State moved = render::ui::reduce(
      std::move(state),
      render::ui::Event{render::ui::Event::Type::Key,
                        render::ui::Key::Tab}, theme);
  const render::DrawList shifted = draw(moved);
  EXPECT_NE(first, shifted);
  // Rasterizing is deterministic too (the PNG bytes follow).
  const render::Image image_a = render::rasterize(first);
  const render::Image image_b = render::rasterize(first);
  ASSERT_EQ(image_a.pixels.size(), image_b.pixels.size());
  EXPECT_EQ(image_a.pixels, image_b.pixels);
}

TEST(UiHarnessTest, ScriptedRunWritesPngsAndStateLines) {
  // End to end through battle_cli at all three resolutions: PNG
  // snapshots exist with the PNG magic, the state lines pin the focus
  // walk and the click, and two runs produce byte-identical files.
  for (const auto& [width, height] : kResolutions) {
    const std::string dir = "/tmp/m11c_ui_test_" +
                            std::to_string(width) + "x" +
                            std::to_string(height);
    fs::create_directories(dir);
    const std::string script_path = dir + "/script.json";
    {
      std::ofstream file(script_path);
      file << R"({
        "width": )" << width << R"(, "height": )" << height
           << R"(, "tick": 100, "out": ")" << dir
           << R"(/frame-%d.png",
        "events": [
          {"frame": 0, "type": "key", "key": "tab"},
          {"frame": 1, "type": "key", "key": "tab"},
          {"frame": 2, "type": "key", "key": "enter"},
          {"frame": 3, "type": "move", "x": 60, "y": 150},
          {"frame": 4, "type": "down", "x": 60, "y": 150},
          {"frame": 5, "type": "up", "x": 60, "y": 150}
        ]
      })";
    }
    const std::string cli =
        "cd \"" TEST_DATA_DIR "/..\" && \"" BATTLE_CLI_PATH
        "\" --scenario data/scenarios/squads.json --seed 7 --ticks 100"
        " --ui-script " + script_path + " > " + dir + "/out.txt 2>&1";
    ASSERT_EQ(std::system(cli.c_str()), 0) << cli;
    const std::string out = read_text(dir + "/out.txt");
    // Two tabs: pause -> play -> speed1; Enter clicks speed1.
    EXPECT_NE(out.find("ui frame 0 screen battle focus play"),
              std::string::npos);
    EXPECT_NE(out.find("ui frame 2 screen battle focus speed1 hover - "
                  "quit 0 zoom 0.5 filter 0"),
              std::string::npos)
        << out;
    // Down/up at (60, 150) toggled the terrain checkbox: it holds the
    // focus from the click on.
    EXPECT_NE(out.find("ui frame 4 screen battle focus terrain hover terrain"),
              std::string::npos)
        << out;
    for (int frame = 0; frame < 6; ++frame) {
      const fs::path png = dir + "/frame-" + std::to_string(frame) +
                           ".png";
      ASSERT_TRUE(fs::exists(png)) << png;
      EXPECT_GT(fs::file_size(png), std::uintmax_t{5000}) << png;
      const std::string bytes = read_text(png);
      EXPECT_EQ(bytes.substr(0, 4), std::string("\x89PNG", 4)) << png;
    }
    // Determinism: the same script twice -> identical PNG bytes.
    const fs::path first_frame = dir + "/frame-2.png";
    const std::string original = read_text(first_frame);
    ASSERT_EQ(std::system(cli.c_str()), 0);
    EXPECT_EQ(read_text(first_frame), original)
        << "the harness is not deterministic";
  }
}
