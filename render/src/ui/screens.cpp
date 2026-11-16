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
                const std::vector<std::string>& scenarios) {
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
           {kIdQuit, "Quit"}}) {
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
  for (std::size_t i = 0; i < app.menu.tree.size() && app.menu.focus < 0;
       ++i) {
    if (app.menu.tree.at(i).interactive()) {
      app.menu.focus = static_cast<std::int32_t>(i);
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
    } else if (clicked == kIdQuit) {
      app.quit = true;
      consume();
    }
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
