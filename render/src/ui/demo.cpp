// m11c — the demo screen builder. See demo.hpp.
#include "render/ui/demo.hpp"

namespace render::ui {

State demo_state(int width, int height) {
  const Theme theme = default_theme();
  Tree tree = Tree::root_panel(width, height, "root");
  // Root: an invisible column of (the left control panel, a growing
  // spacer, the bottom status bar).
  tree.at(0).box = Box::Column;
  tree.at(0).chrome = false;

  // --- Left control panel (a fixed-width column of rows). ---
  const std::int32_t panel =
      static_cast<std::int32_t>(tree.add(0, [&] {
        Widget w;
        w.id = "controls";
        w.kind = Kind::Panel;
        w.text = "Battle controls";
        w.box = Box::Column;
        w.fixed_w = 240;
        return w;
      }()));

  // Row 1: playback (Pause, Play).
  const std::int32_t playback = static_cast<std::int32_t>(tree.add(
      panel, [&] {
        Widget w;
        w.id = "playback";
        w.kind = Kind::Panel;
        w.box = Box::Row;
        w.fixed_h = 0;
        return w;
      }()));
  tree.add(playback, [&] {
    Widget w;
    w.id = kIdPause;
    w.kind = Kind::Button;
    w.text = "Pause";
    w.grow = 1.0;
    return w;
  }());
  tree.add(playback, [&] {
    Widget w;
    w.id = kIdPlay;
    w.kind = Kind::Button;
    w.text = "Play";
    w.grow = 1.0;
    return w;
  }());

  // Row 2: speed (x1, x2, x4).
  const std::int32_t speed = static_cast<std::int32_t>(tree.add(
      panel, [&] {
        Widget w;
        w.id = "speed-row";
        w.kind = Kind::Panel;
        w.box = Box::Row;
        return w;
      }()));
  for (const auto& [id, label] :
       std::initializer_list<std::pair<const char*, const char*>>{
           {kIdSpeed1, "x1"}, {kIdSpeed2, "x2"}, {kIdSpeed4, "x4"}}) {
    tree.add(speed, [&] {
      Widget w;
      w.id = id;
      w.kind = Kind::Button;
      w.text = label;
      w.grow = 1.0;
      return w;
    }());
  }

  tree.add(panel, [&] {
    Widget w;
    w.id = "sep1";
    w.kind = Kind::Separator;
    return w;
  }());

  // View options.
  for (const auto& [id, label] :
       std::initializer_list<std::pair<const char*, const char*>>{
           {kIdHud, "HUD text"},
           {kIdTerrain, "Terrain layer"},
           {kIdSquadLinks, "Squad links"},
           {kIdGrid, "Grid"}}) {
    tree.add(panel, [&] {
      Widget w;
      w.id = id;
      w.kind = Kind::Checkbox;
      w.text = label;
      w.on = true;  // the viewer defaults (options_from mirrors these)
      return w;
    }());
  }

  tree.add(panel, [&] {
    Widget w;
    w.id = "sep2";
    w.kind = Kind::Separator;
    return w;
  }());

  // Zoom slider + filter select.
  tree.add(panel, [&] {
    Widget w;
    w.id = kIdZoom;
    w.kind = Kind::Slider;
    w.value = 0.5;
    w.grow = 1.0;
    return w;
  }());
  tree.add(panel, [&] {
    Widget w;
    w.id = kIdFilter;
    w.kind = Kind::Select;
    w.text = "All units\nInfantry\nArchers\nCavalry\nCommanders";
    w.selected = 0;
    w.grow = 1.0;
    return w;
  }());

  // --- A growing spacer pushes the status bar to the bottom. ---
  tree.add(0, [&] {
    Widget w;
    w.id = "spacer";
    w.kind = Kind::Panel;
    w.grow = 1.0;
    w.chrome = false;
    return w;
  }());

  // --- The status bar along the bottom. ---
  tree.add(0, [&] {
    Widget w;
    w.id = "status-bar";
    w.kind = Kind::Panel;
    w.box = Box::Row;
    w.fixed_h = 30;
    return w;
  }());
  // The status label fills the bar.
  const std::int32_t status_bar = static_cast<std::int32_t>(tree.size()) - 1;
  tree.add(status_bar, [&] {
    Widget w;
    w.id = kIdStatus;
    w.kind = Kind::Label;
    w.text = "Ready";
    w.grow = 1.0;
    return w;
  }());

  State state;
  state.tree = std::move(tree);
  state.width = width;
  state.height = height;
  // The initial focus: the first interactive widget in tree order.
  for (std::size_t i = 0; i < state.tree.size(); ++i) {
    if (state.tree.at(i).interactive()) {
      state.focus = static_cast<std::int32_t>(i);
      break;
    }
  }
  layout(state.tree, Rect{0, 0, width, height}, theme);
  return state;
}

}  // namespace render::ui
