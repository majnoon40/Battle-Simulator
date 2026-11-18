// m11d — the sandbox screens implementation. See screens.hpp.
#include "render/ui/screens.hpp"

#include "render/ui/demo.hpp"

#include <algorithm>

namespace render::ui {
namespace {

// A centered content box: the root is a row of [spacer, column,
// spacer] and the column holds [spacer, content-box, spacer], so the
// content box centers on both axes with grow-only layout (no engine
// alignment feature needed). Returns the tree; `box_index` receives
// the content box's widget index.
[[nodiscard]] Tree centered_root(int width, int height, int content_w,
                                 const std::string& id,
                                 std::int32_t& box_index) {
  Tree tree = Tree::root_panel(width, height, id);
  tree.at(0).box = Box::Row;
  tree.at(0).chrome = false;
  tree.at(0).stretch_children = false;
  auto spacer = [](const std::string& spacer_id) {
    Widget w;
    w.id = spacer_id;
    w.kind = Kind::Panel;
    w.grow = 1.0;
    w.chrome = false;
    return w;
  };
  tree.add(0, spacer(id + "-left"));
  const std::int32_t column = static_cast<std::int32_t>(tree.add(
      0, [&] {
        Widget w;
        w.id = id + "-column";
        w.kind = Kind::Panel;
        w.box = Box::Column;
        w.fixed_w = content_w + 2 * 8;  // + the column's own padding
        w.stretch_children = false;
        return w;
      }()));
  tree.add(0, spacer(id + "-right"));
  tree.add(column, spacer(id + "-top"));
  box_index = static_cast<std::int32_t>(tree.add(column, [&] {
    Widget w;
    w.id = id + "-box";
    w.kind = Kind::Panel;
    w.box = Box::Column;
    w.fixed_w = content_w;
    return w;
  }()));
  tree.add(column, spacer(id + "-bottom"));
  return tree;
}

}  // namespace

App sandbox_app(int width, int height,
                const std::vector<std::string>& scenarios,
                const std::vector<std::string>& replays) {
  const Theme theme = default_theme();

  // --- The menu: a centered column of buttons. ---
  std::int32_t menu_box = 0;
  Tree menu_tree =
      centered_root(width, height, 260, "menu", menu_box);
  menu_tree.add(menu_box, [&] {
    Widget w;
    w.id = "title";
    w.kind = Kind::Label;
    w.text = "AUTOBATTLE";
    return w;
  }());
  for (const auto& [id, label] :
       std::initializer_list<std::pair<const char*, const char*>>{
           {kIdNewBattle, "New battle"}, {kIdScenario, "Scenarios"},
           {kIdReplays, "Replays"}, {kIdQuit, "Quit"}}) {
    menu_tree.add(menu_box, [&] {
      Widget w;
      w.id = id;
      w.kind = Kind::Button;
      w.text = label;
      w.grow = 1.0;
      return w;
    }());
  }

  // --- The scenario picker: a select over the scenario names plus
  // Start / Back. ---
  std::string items;
  for (const std::string& name : scenarios) {
    if (!items.empty()) {
      items.push_back('\n');
    }
    items += name;
  }
  if (items.empty()) {
    items = "default";
  }
  std::int32_t scenario_box = 0;
  Tree scenario_tree =
      centered_root(width, height, 300, "scenario", scenario_box);
  scenario_tree.add(scenario_box, [&] {
    Widget w;
    w.id = "scenario-title";
    w.kind = Kind::Label;
    w.text = "Pick a scenario";
    return w;
  }());
  scenario_tree.add(scenario_box, [&] {
    Widget w;
    w.id = kIdScenarioList;
    w.kind = Kind::Select;
    w.text = items;
    w.grow = 1.0;
    return w;
  }());
  const std::int32_t row = static_cast<std::int32_t>(scenario_tree.add(
      scenario_box, [&] {
        Widget w;
        w.id = "scenario-row";
        w.kind = Kind::Panel;
        w.box = Box::Row;
        return w;
      }()));
  scenario_tree.add(row, [&] {
    Widget w;
    w.id = kIdBack;
    w.kind = Kind::Button;
    w.text = "Back";
    w.grow = 1.0;
    return w;
  }());
  scenario_tree.add(row, [&] {
    Widget w;
    w.id = kIdStart;
    w.kind = Kind::Button;
    w.text = "Start";
    w.grow = 1.0;
    return w;
  }());

  // --- m13 item 5b: the end screen (result + casualties labels the
  // window fills; the curves draw into the battle DrawList behind the
  // UI; Rematch / Back / Save replay / Save report). ---
  std::int32_t end_box = 0;
  Tree end_tree = centered_root(width, height, 320, "end", end_box);
  end_tree.add(end_box, [&] {
    Widget w;
    w.id = kIdEndTitle;
    w.kind = Kind::Label;
    w.text = "BATTLE OVER";
    return w;
  }());
  end_tree.add(end_box, [&] {
    Widget w;
    w.id = kIdEndResult;
    w.kind = Kind::Label;
    w.text = " ";  // the window fills the result line
    return w;
  }());
  end_tree.add(end_box, [&] {
    Widget w;
    w.id = kIdEndCasualties;
    w.kind = Kind::Label;
    w.text = " ";  // ... and the casualties line
    return w;
  }());
  const std::int32_t end_row1 = static_cast<std::int32_t>(end_tree.add(
      end_box, [&] {
        Widget w;
        w.id = "end-row-1";
        w.kind = Kind::Panel;
        w.box = Box::Row;
        return w;
      }()));
  end_tree.add(end_row1, [&] {
    Widget w;
    w.id = kIdBack;
    w.kind = Kind::Button;
    w.text = "Back to battle";
    w.grow = 1.0;
    return w;
  }());
  end_tree.add(end_row1, [&] {
    Widget w;
    w.id = kIdRematch;
    w.kind = Kind::Button;
    w.text = "Rematch";
    w.grow = 1.0;
    return w;
  }());
  const std::int32_t end_row2 = static_cast<std::int32_t>(end_tree.add(
      end_box, [&] {
        Widget w;
        w.id = "end-row-2";
        w.kind = Kind::Panel;
        w.box = Box::Row;
        return w;
      }()));
  end_tree.add(end_row2, [&] {
    Widget w;
    w.id = kIdSaveReplay;
    w.kind = Kind::Button;
    w.text = "Save replay";
    w.grow = 1.0;
    return w;
  }());
  end_tree.add(end_row2, [&] {
    Widget w;
    w.id = kIdSaveReport;
    w.kind = Kind::Button;
    w.text = "Save report";
    w.grow = 1.0;
    return w;
  }());

  // --- m13 item 5b: the replay viewer (list + status + Back / Verify /
  // Load; loading and verifying are window actions). ---
  std::string replay_items;
  for (const std::string& name : replays) {
    if (!replay_items.empty()) {
      replay_items.push_back('\n');
    }
    replay_items += name;
  }
  if (replay_items.empty()) {
    replay_items = "(no .replay files)";
  }
  std::int32_t replay_box = 0;
  Tree replay_tree =
      centered_root(width, height, 300, "replay", replay_box);
  replay_tree.add(replay_box, [&] {
    Widget w;
    w.id = "replay-title";
    w.kind = Kind::Label;
    w.text = "Replay viewer";
    return w;
  }());
  replay_tree.add(replay_box, [&] {
    Widget w;
    w.id = kIdReplayList;
    w.kind = Kind::Select;
    w.text = replay_items;
    w.grow = 1.0;
    return w;
  }());
  replay_tree.add(replay_box, [&] {
    Widget w;
    w.id = kIdReplayStatus;
    w.kind = Kind::Label;
    w.text = " ";  // the window fills verify results here
    return w;
  }());
  const std::int32_t replay_row = static_cast<std::int32_t>(replay_tree.add(
      replay_box, [&] {
        Widget w;
        w.id = "replay-row";
        w.kind = Kind::Panel;
        w.box = Box::Row;
        return w;
      }()));
  replay_tree.add(replay_row, [&] {
    Widget w;
    w.id = kIdBack;
    w.kind = Kind::Button;
    w.text = "Back";
    w.grow = 1.0;
    return w;
  }());
  replay_tree.add(replay_row, [&] {
    Widget w;
    w.id = kIdVerify;
    w.kind = Kind::Button;
    w.text = "Verify";
    w.grow = 1.0;
    return w;
  }());
  replay_tree.add(replay_row, [&] {
    Widget w;
    w.id = kIdLoad;
    w.kind = Kind::Button;
    w.text = "Load";
    w.grow = 1.0;
    return w;
  }());

  App app;
  app.width = width;
  app.height = height;
  app.menu.tree = std::move(menu_tree);
  app.menu.width = width;
  app.menu.height = height;
  app.scenario.tree = std::move(scenario_tree);
  app.scenario.width = width;
  app.scenario.height = height;
  app.battle = demo_state(width, height);
  app.end.tree = std::move(end_tree);
  app.end.width = width;
  app.end.height = height;
  app.replay.tree = std::move(replay_tree);
  app.replay.width = width;
  app.replay.height = height;
  for (std::size_t i = 0; i < app.menu.tree.size() && app.menu.focus < 0;
       ++i) {
    if (app.menu.tree.at(i).interactive()) {
      app.menu.focus = static_cast<std::int32_t>(i);
    }
  }
  for (std::size_t i = 0; i < app.end.tree.size() && app.end.focus < 0;
       ++i) {
    if (app.end.tree.at(i).interactive()) {
      app.end.focus = static_cast<std::int32_t>(i);
    }
  }
  for (std::size_t i = 0; i < app.replay.tree.size() && app.replay.focus < 0;
       ++i) {
    if (app.replay.tree.at(i).interactive()) {
      app.replay.focus = static_cast<std::int32_t>(i);
    }
  }
  for (std::size_t i = 0;
       i < app.scenario.tree.size() && app.scenario.focus < 0; ++i) {
    if (app.scenario.tree.at(i).interactive()) {
      app.scenario.focus = static_cast<std::int32_t>(i);
    }
  }
  layout(app.menu.tree, Rect{0, 0, width, height}, theme);
  layout(app.scenario.tree, Rect{0, 0, width, height}, theme);
  layout(app.end.tree, Rect{0, 0, width, height}, theme);
  layout(app.replay.tree, Rect{0, 0, width, height}, theme);
  return app;
}

App reduce_app(App app, const Event& event,
               const std::vector<std::string>& scenarios) {
  const Theme theme = default_theme();
  State& active = app.active();
  const std::string clicked_before = active.clicked;
  active = reduce(std::move(active), event, theme);
  // Navigation acts on a click THIS event produced; every click is
  // consumed exactly once (older leftovers are cleared).
  const bool new_click =
      !active.clicked.empty() && active.clicked != clicked_before;
  if (!new_click) {
    return app;
  }
  const std::string clicked = active.clicked;
  // Navigation clicks are consumed here; anything else (pause, play,
  // the speed buttons) stays in active.clicked for the app layer.
  const auto consume = [&active]() { active.clicked.clear(); };
  if (app.screen == Screen::Menu) {
    if (clicked == kIdNewBattle || clicked == kIdScenario) {
      app.screen = Screen::Scenario;
      consume();
    } else if (clicked == kIdReplays) {
      app.screen = Screen::Replay;
      consume();
    } else if (clicked == kIdQuit) {
      app.quit = true;
      consume();
    }
  } else if (app.screen == Screen::End) {
    if (clicked == kIdBack) {
      app.screen = Screen::Battle;
      consume();
    }
    // Rematch / Save replay / Save report: window actions
    // (render::ui::end_clicks); the click stays in active.clicked.
  } else if (app.screen == Screen::Replay) {
    if (clicked == kIdBack) {
      app.screen = Screen::Menu;
      consume();
    }
    // Load / Verify: window actions (render::ui::replay_clicks).
  } else if (app.screen == Screen::Scenario) {
    if (clicked == kIdBack) {
      app.screen = Screen::Menu;
      consume();
    } else if (clicked == kIdStart) {
      std::size_t selected = 0;
      for (std::size_t i = 0; i < app.scenario.tree.size(); ++i) {
        if (app.scenario.tree.at(i).id == kIdScenarioList) {
          selected = static_cast<std::size_t>(
              std::max(0, app.scenario.tree.at(i).selected));
          break;
        }
      }
      app.chosen_scenario =
          selected < scenarios.size() ? scenarios[selected] : "";
      app.screen = Screen::Battle;
      consume();
    }
  }
  return app;
}

game::ViewOptions options_from(const State& battle) {
  game::ViewOptions options;
  const auto value_of = [&](const char* id, bool fallback) {
    for (const Widget& widget : battle.tree.widgets) {
      if (widget.id == id) {
        return widget.on;
      }
    }
    return fallback;
  };
  options.show_hud = value_of("hud", true);
  options.show_terrain = value_of("terrain", true);
  options.squad_links = value_of("squad-links", true);
  options.show_grid = value_of("grid", true);
  return options;
}

double zoom_from(const State& battle) {
  for (const Widget& widget : battle.tree.widgets) {
    if (widget.id == "zoom") {
      return widget.value;
    }
  }
  return 0.5;
}

}  // namespace render::ui
