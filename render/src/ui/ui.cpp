// m11c — the UI framework implementation. See ui.hpp for the layer
// overview. Everything iterates fixed index order and stays in integer
// pixels, so layout and drawing are byte-deterministic.
#include "render/ui/ui.hpp"

#include <algorithm>
#include <cassert>

namespace render::ui {
namespace {

// Interactive rows hug to the theme row height; labels hug to their
// text; separators hug to 1 px plus the border padding.
[[nodiscard]] int clamp_min(int value, int minimum) {
  return value < minimum ? minimum : value;
}

[[nodiscard]] std::vector<std::string> split_items(const std::string& text) {
  std::vector<std::string> items;
  std::string current;
  for (char c : text) {
    if (c == '\n') {
      items.push_back(current);
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  items.push_back(current);
  return items;
}

[[nodiscard]] Rect inset(const Rect& rect, int by) {
  return Rect{rect.x + by, rect.y + by,
              std::max(0, rect.w - 2 * by), std::max(0, rect.h - 2 * by)};
}

}  // namespace

Theme default_theme() {
  Theme theme;
  theme.panel = game::Color{34, 38, 48, 235};
  theme.panel_hover = game::Color{52, 58, 72, 245};
  theme.panel_active = game::Color{64, 72, 90, 250};
  theme.border = game::Color{74, 80, 96, 255};
  theme.text = game::Color{228, 230, 238, 255};
  theme.text_dim = game::Color{120, 126, 140, 255};
  theme.accent = game::Color{96, 170, 255, 255};
  theme.track = game::Color{58, 64, 78, 255};
  theme.thumb = game::Color{150, 158, 176, 255};
  theme.danger = game::Color{214, 92, 92, 255};
  return theme;
}

int font_scale(int font_px) {
  if (font_px < 7) {
    font_px = 7;
  }
  return std::max(1, (font_px + 3) / 7);
}

int text_advance(const std::string& text, int font_px) {
  const int scale = font_scale(font_px);
  return static_cast<int>(text.size()) * 6 * scale;
}

int text_height(int font_px) { return 7 * font_scale(font_px); }

// ---------------------------------------------------------------------------
// Tree.
// ---------------------------------------------------------------------------

Tree Tree::root_panel(int width, int height, const std::string& id) {
  Tree tree;
  Widget root;
  root.id = id;
  root.kind = Kind::Panel;
  root.fixed_w = width;
  root.fixed_h = height;
  tree.widgets.push_back(root);
  tree.parent.push_back(-1);
  return tree;
}

std::uint32_t Tree::add(std::int32_t parent_index, Widget widget) {
  const auto index = static_cast<std::int32_t>(widgets.size());
  widgets.push_back(std::move(widget));
  parent.push_back(parent_index);
  return static_cast<std::uint32_t>(index);
}

std::vector<std::int32_t> Tree::children_of(std::int32_t index) const {
  std::vector<std::int32_t> children;
  for (std::size_t i = static_cast<std::size_t>(index) + 1;
       i < widgets.size(); ++i) {
    if (parent[i] == index) {
      children.push_back(static_cast<std::int32_t>(i));
    }
  }
  return children;
}

// ---------------------------------------------------------------------------
// Layout.
// ---------------------------------------------------------------------------

int hug_width(const Widget& widget, const Theme& theme) {
  switch (widget.kind) {
    case Kind::Panel: {
      // Hug the children plus padding; titles do not force width.
      return 2 * theme.pad;
    }
    case Kind::Label:
    case Kind::Button:
      return text_advance(widget.text, theme.font_px) + 2 * theme.pad;
    case Kind::Checkbox:
      return text_advance(widget.text, theme.font_px) + 2 * theme.pad +
             theme.row_height;
    case Kind::Slider:
      return 4 * theme.slider_thumb_w + 6 * theme.pad;
    case Kind::Select: {
      int widest = 0;
      for (const std::string& item : split_items(widget.text)) {
        widest = std::max(widest, text_advance(item, theme.font_px));
      }
      return widest + 3 * theme.pad;
    }
    case Kind::Separator:
      return 0;
  }
  return 0;
}

int hug_height(const Widget& widget, const Theme& theme) {
  switch (widget.kind) {
    case Kind::Panel:
      return 2 * theme.pad;
    case Kind::Label:
      return text_height(theme.font_px) + 2;
    case Kind::Button:
    case Kind::Checkbox:
      return theme.row_height;
    case Kind::Slider:
      return theme.row_height;
    case Kind::Select: {
      const std::vector<std::string> items = split_items(widget.text);
      int height = theme.row_height;
      if (widget.on) {  // open: the dropdown shows every item
        height = theme.row_height * static_cast<int>(items.size());
      }
      return height;
    }
    case Kind::Separator:
      return theme.border_w;
  }
  return 0;
}

namespace {

// Bottom-up size pass: returns the hug size of `index` (children laid
// out relative to the parent's content origin so the parent can hug
// them); fixed sizes win, min clamps, grow only matters in the parent's
// distribution.
struct Size {
  int w = 0;
  int h = 0;
};

Size measure(Tree& tree, std::int32_t index, const Theme& theme) {
  Widget& widget = tree.at(static_cast<std::size_t>(index));
  Size size{std::max(hug_width(widget, theme), widget.min_w),
            std::max(hug_height(widget, theme), widget.min_h)};
  if (widget.kind != Kind::Panel) {
    if (widget.fixed_w > 0) {
      size.w = widget.fixed_w;
    }
    if (widget.fixed_h > 0) {
      size.h = widget.fixed_h;
    }
    widget.rect.w = size.w;
    widget.rect.h = size.h;
    return size;
  }
  // Panel: measure children, stack them, hug around the content.
  const std::vector<std::int32_t> children = tree.children_of(index);
  int content_w = 0;
  int content_h = 0;
  bool any_grow = false;
  for (std::int32_t child : children) {
    if (tree.at(static_cast<std::size_t>(child)).grow > 0.0) {
      any_grow = true;
    }
  }
  for (std::int32_t child : children) {
    const Size child_size = measure(tree, child, theme);
    if (widget.box == Box::Column) {
      content_w = std::max(content_w, child_size.w);
      content_h += child_size.h;
    } else {
      content_w += child_size.w;
      content_h = std::max(content_h, child_size.h);
    }
  }
  if (!children.empty()) {
    const int gaps = theme.gap * (static_cast<int>(children.size()) - 1);
    if (widget.box == Box::Column) {
      content_h += gaps;
    } else {
      content_w += gaps;
    }
  }
  size.w = std::max(size.w, content_w + 2 * theme.pad);
  size.h = std::max(size.h, content_h + 2 * theme.pad);
  if (widget.fixed_w > 0) {
    size.w = widget.fixed_w;
  }
  if (widget.fixed_h > 0) {
    size.h = widget.fixed_h;
  }
  widget.rect.w = size.w;
  widget.rect.h = size.h;
  (void)any_grow;
  return size;
}

// Top-down position pass: places children inside the parent's content
// rect, distributing leftover space to growing children. Integer
// remainders go to the earliest growing children (deterministic).
void place(Tree& tree, std::int32_t index, const Theme& theme) {
  Widget& widget = tree.at(static_cast<std::size_t>(index));
  if (widget.kind != Kind::Panel) {
    return;
  }
  const std::vector<std::int32_t> children = tree.children_of(index);
  const Rect content = inset(widget.rect, theme.pad);
  const int count = static_cast<int>(children.size());
  if (count == 0) {
    return;
  }
  const int gaps = theme.gap * (count - 1);
  const bool column = widget.box == Box::Column;
  // The main axis is what the children stack along: a column's children
  // stack vertically (heights), a row's horizontally (widths).
  const int available =
      (column ? content.h : content.w) - gaps;
  int hug_total = 0;
  double grow_total = 0.0;
  for (std::int32_t child : children) {
    const Widget& child_widget = tree.at(static_cast<std::size_t>(child));
    hug_total += column ? child_widget.rect.h : child_widget.rect.w;
    grow_total += child_widget.grow;
  }
  int leftover = available - hug_total;
  if (leftover < 0) {
    leftover = 0;  // overflow: children keep their hug sizes
  }
  if (grow_total <= 0.0) {
    grow_total = 0.0;  // leftover stays unused (content is left/top-aligned)
  }
  // Floor shares plus a deterministic remainder: the earliest growing
  // children receive one extra pixel until the leftover is used up.
  int assigned = 0;
  std::vector<int> extra(static_cast<std::size_t>(count), 0);
  if (grow_total > 0.0) {
    for (int i = 0; i < count; ++i) {
      const Widget& child_widget = tree.at(
          static_cast<std::size_t>(children[static_cast<std::size_t>(i)]));
      if (child_widget.grow <= 0.0) {
        continue;
      }
      extra[static_cast<std::size_t>(i)] =
          static_cast<int>(leftover * (child_widget.grow / grow_total));
      assigned += extra[static_cast<std::size_t>(i)];
    }
    int remainder = leftover - assigned;
    for (int i = 0; i < count && remainder > 0; ++i) {
      const Widget& child_widget = tree.at(
          static_cast<std::size_t>(children[static_cast<std::size_t>(i)]));
      if (child_widget.grow > 0.0) {
        extra[static_cast<std::size_t>(i)] += 1;
        --remainder;
      }
    }
  }
  int cursor = column ? content.y : content.x;
  for (int i = 0; i < count; ++i) {
    const std::int32_t child = children[static_cast<std::size_t>(i)];
    Widget& child_widget = tree.at(static_cast<std::size_t>(child));
    if (column) {
      child_widget.rect.x = content.x;
      child_widget.rect.y = cursor;
      child_widget.rect.h += extra[static_cast<std::size_t>(i)];
      if (widget.stretch_children && child_widget.fixed_w == 0) {
        child_widget.rect.w = content.w;
      }
      cursor += child_widget.rect.h + theme.gap;
    } else {
      child_widget.rect.x = cursor;
      child_widget.rect.y = content.y;
      child_widget.rect.w += extra[static_cast<std::size_t>(i)];
      if (widget.stretch_children && child_widget.fixed_h == 0) {
        child_widget.rect.h = content.h;
      }
      cursor += child_widget.rect.w + theme.gap;
    }
  }
  for (std::int32_t child : children) {
    place(tree, child, theme);
  }
}

}  // namespace

void layout(Tree& tree, const Rect& bounds, const Theme& theme) {
  if (tree.widgets.empty()) {
    return;
  }
  measure(tree, 0, theme);
  tree.at(0).rect.x = bounds.x;
  tree.at(0).rect.y = bounds.y;
  tree.at(0).rect.w = std::max(tree.at(0).rect.w, bounds.w);
  tree.at(0).rect.h = std::max(tree.at(0).rect.h, bounds.h);
  place(tree, 0, theme);
}

// ---------------------------------------------------------------------------
// Reducer.
// ---------------------------------------------------------------------------

State focus_next(State state, bool forward) {
  const std::int32_t count = static_cast<std::int32_t>(state.tree.size());
  if (count == 0) {
    state.focus = -1;
    return state;
  }
  std::int32_t index = state.focus;
  for (int step = 0; step < count; ++step) {
    if (forward) {
      index = index + 1 >= count ? 0 : index + 1;
    } else {
      index = index - 1 < 0 ? count - 1 : index - 1;
    }
    const Widget& widget =
        state.tree.at(static_cast<std::size_t>(index));
    if (widget.interactive() && widget.enabled) {
      state.focus = index;
      return state;
    }
  }
  return state;  // no interactive widget at all
}

std::int32_t hit_test(const State& state, Point point) {
  // Topmost = latest in the vector; dropdown rows extend the select.
  for (std::int32_t index = static_cast<std::int32_t>(state.tree.size()) - 1;
       index >= 0; --index) {
    const Widget& widget =
        state.tree.at(static_cast<std::size_t>(index));
    if (!widget.interactive() || !widget.enabled) {
      continue;
    }
    if (widget.rect.contains(point)) {
      return index;
    }
  }
  return -1;
}

namespace {

[[nodiscard]] double slider_value_at(const Widget& widget, Point point,
                                     const Theme& theme) {
  const int track_x = widget.rect.x + theme.slider_thumb_w / 2;
  const int track_w =
      std::max(1, widget.rect.w - theme.slider_thumb_w);
  const double raw =
      static_cast<double>(point.x - track_x) / static_cast<double>(track_w);
  return std::clamp(raw, 0.0, 1.0);
}

[[nodiscard]] int select_row_at(const Widget& widget, Point point,
                                const Theme& theme) {
  const int row = (point.y - widget.rect.y) / theme.row_height;
  const std::vector<std::string> items = split_items(widget.text);
  if (row >= 0 && row < static_cast<int>(items.size())) {
    return row;
  }
  return -1;
}

}  // namespace

State reduce(State state, const Event& event, const Theme& theme) {
  switch (event.type) {
    case Event::Type::MouseMove:
    case Event::Type::MouseDown:
    case Event::Type::MouseUp:
      state.pointer = event.pos;
      break;
    default:
      break;
  }
  switch (event.type) {
    case Event::Type::Key: {
      if (event.key == Key::Tab) {
        return focus_next(std::move(state), true);
      }
      if (state.focus < 0) {
        return state;
      }
      Widget& widget =
          state.tree.at(static_cast<std::size_t>(state.focus));
      switch (event.key) {
        case Key::Enter:
          if (!widget.enabled) {
            break;
          }
          if (widget.kind == Kind::Button) {
            state.clicked = widget.id;
            state.click_consumed = false;
          } else if (widget.kind == Kind::Checkbox) {
            widget.on = !widget.on;
          } else if (widget.kind == Kind::Select) {
            if (widget.on) {
              widget.on = false;  // confirm
            } else {
              widget.on = true;
            }
          }
          break;
        case Key::Escape:
          if (widget.kind == Kind::Select) {
            widget.on = false;
          }
          break;
        case Key::Left:
          if (widget.kind == Kind::Slider) {
            widget.value = std::clamp(widget.value - 1.0 / 32.0, 0.0, 1.0);
          } else if (widget.kind == Kind::Select && widget.on) {
            widget.on = false;
          }
          break;
        case Key::Right:
          if (widget.kind == Kind::Slider) {
            widget.value = std::clamp(widget.value + 1.0 / 32.0, 0.0, 1.0);
          }
          break;
        case Key::Up:
          if (widget.kind == Kind::Select && widget.on) {
            widget.selected = std::max(0, widget.selected - 1);
          } else {
            return focus_next(std::move(state), false);
          }
          break;
        case Key::Down:
          if (widget.kind == Kind::Select && widget.on) {
            const std::vector<std::string> items = split_items(widget.text);
            widget.selected = std::min(
                static_cast<int>(items.size()) - 1, widget.selected + 1);
          } else {
            return focus_next(std::move(state), true);
          }
          break;
        case Key::Home:
          if (widget.kind == Kind::Slider) {
            widget.value = 0.0;
          }
          break;
        case Key::End:
          if (widget.kind == Kind::Slider) {
            widget.value = 1.0;
          }
          break;
        default:
          break;
      }
      break;
    }
    case Event::Type::MouseMove: {
      const std::int32_t hit = hit_test(state, event.pos);
      state.hover = hit;
      if (state.pointer_down && state.pressed >= 0) {
        Widget& widget =
            state.tree.at(static_cast<std::size_t>(state.pressed));
        if (widget.kind == Kind::Slider) {
          widget.value = slider_value_at(widget, event.pos, theme);
        }
      }
      break;
    }
    case Event::Type::MouseDown: {
      if (event.button != 0) {
        break;
      }
      state.pointer_down = true;
      const std::int32_t hit = hit_test(state, event.pos);
      state.hover = hit;
      state.pressed = hit;
      if (hit >= 0) {
        state.focus = hit;
        Widget& widget = state.tree.at(static_cast<std::size_t>(hit));
        if (widget.kind == Kind::Button) {
          // clicked fires on MouseUp (a drag off the button cancels).
        } else if (widget.kind == Kind::Checkbox) {
          widget.on = !widget.on;
        } else if (widget.kind == Kind::Slider) {
          widget.value = slider_value_at(widget, event.pos, theme);
        } else if (widget.kind == Kind::Select) {
          if (widget.on) {
            const int row = select_row_at(widget, event.pos, theme);
            if (row >= 0) {
              widget.selected = row;
            }
            widget.on = false;
          } else {
            widget.on = true;
          }
        }
      }
      break;
    }
    case Event::Type::MouseUp: {
      if (event.button != 0) {
        break;
      }
      state.pointer_down = false;
      if (state.pressed >= 0) {
        const Widget& widget =
            state.tree.at(static_cast<std::size_t>(state.pressed));
        if (widget.kind == Kind::Button && state.hover == state.pressed) {
          state.clicked = widget.id;
          state.click_consumed = false;
        }
      }
      state.pressed = -1;
      break;
    }
    case Event::Type::Wheel: {
      if (state.focus >= 0) {
        Widget& widget =
            state.tree.at(static_cast<std::size_t>(state.focus));
        if (widget.kind == Kind::Slider) {
          const double step =
              event.delta > 0 ? 1.0 / 32.0 : -1. / 32.;
          widget.value = std::clamp(widget.value + step, 0.0, 1.0);
        }
      }
      break;
    }
  }
  // Relayout: values can change sizes (an open Select grows), and the
  // rects must track the state for the next hit test.
  layout(state.tree, Rect{0, 0, state.width, state.height}, theme);
  return state;
}

State reduce(State state, const Event& event) {
  return reduce(std::move(state), event, default_theme());
}

// ---------------------------------------------------------------------------
// Drawing.
// ---------------------------------------------------------------------------

void draw_ui(const State& state, const Theme& theme, DrawList& out) {
  for (std::size_t index = 0; index < state.tree.size(); ++index) {
    const Widget& widget = state.tree.at(index);
    const Rect& rect = widget.rect;
    if (rect.w <= 0 || rect.h <= 0) {
      continue;
    }
    const bool hovered =
        state.hover == static_cast<std::int32_t>(index);
    const bool pressed =
        state.pressed == static_cast<std::int32_t>(index);
    const bool focused =
        state.focus == static_cast<std::int32_t>(index);
    switch (widget.kind) {
      case Kind::Panel: {
        if (widget.chrome) {
          out.rect(rect.x, rect.y, rect.w, rect.h, theme.panel, true);
          out.rect(rect.x, rect.y, rect.w, rect.h, theme.border, false);
          if (!widget.text.empty()) {
            out.text(rect.x + theme.pad, rect.y + theme.pad / 2,
                     theme.font_px, widget.text, theme.text);
          }
        }
        break;
      }
      case Kind::Label: {
        out.text(rect.x, rect.y + (rect.h - text_height(theme.font_px)) / 2,
                 theme.font_px, widget.text,
                 widget.enabled ? theme.text : theme.text_dim);
        break;
      }
      case Kind::Separator: {
        out.rect(rect.x, rect.y, rect.w, theme.border_w, theme.border, true);
        break;
      }
      case Kind::Button: {
        game::Color fill = theme.panel;
        if (pressed) {
          fill = theme.panel_active;
        } else if (hovered) {
          fill = theme.panel_hover;
        }
        out.rect(rect.x, rect.y, rect.w, rect.h, fill, true);
        out.rect(rect.x, rect.y, rect.w, rect.h, theme.border, false);
        const int text_w = text_advance(widget.text, theme.font_px);
        const int text_h = text_height(theme.font_px);
        out.text(rect.x + (rect.w - text_w) / 2,
                 rect.y + (rect.h - text_h) / 2, theme.font_px,
                 widget.text,
                 widget.enabled ? theme.text : theme.text_dim);
        break;
      }
      case Kind::Checkbox: {
        const int box = std::min(rect.h, theme.row_height) - 4;
        const int box_x = rect.x + 2;
        const int box_y = rect.y + (rect.h - box) / 2;
        out.rect(box_x, box_y, box, box, theme.panel, true);
        out.rect(box_x, box_y, box, box, theme.border, false);
        if (widget.on) {
          out.rect(box_x + 2, box_y + 2, std::max(1, box - 4),
                   std::max(1, box - 4), theme.accent, true);
        }
        out.text(box_x + box + theme.pad,
                 rect.y + (rect.h - text_height(theme.font_px)) / 2,
                 theme.font_px, widget.text,
                 widget.enabled ? theme.text : theme.text_dim);
        break;
      }
      case Kind::Slider: {
        const int track_y =
            rect.y + (rect.h - theme.slider_track_h) / 2;
        const int track_x = rect.x + theme.slider_thumb_w / 2;
        const int track_w = std::max(1, rect.w - theme.slider_thumb_w);
        out.rect(track_x, track_y, track_w, theme.slider_track_h,
                 theme.track, true);
        const int thumb_x =
            track_x + static_cast<int>(std::clamp(widget.value, 0.0, 1.0) *
                                       (track_w - theme.slider_thumb_w));
        out.rect(thumb_x,
                 rect.y + (rect.h - theme.row_height / 2) / 2,
                 theme.slider_thumb_w, theme.row_height / 2, theme.thumb,
                 true);
        break;
      }
      case Kind::Select: {
        game::Color fill = theme.panel;
        if (widget.on) {
          fill = theme.panel_active;
        } else if (hovered) {
          fill = theme.panel_hover;
        }
        out.rect(rect.x, rect.y, rect.w, rect.h, fill, true);
        out.rect(rect.x, rect.y, rect.w, rect.h, theme.border, false);
        const std::vector<std::string> items = split_items(widget.text);
        const std::string& current =
            items[static_cast<std::size_t>(
                std::clamp(widget.selected, 0,
                           static_cast<int>(items.size()) - 1))];
        out.text(rect.x + theme.pad,
                 rect.y + (theme.row_height -
                           text_height(theme.font_px)) / 2,
                 theme.font_px, current, theme.text);
        if (widget.on) {
          // The dropdown: one row per item, the selected row accented.
          const int drop_y = rect.y + theme.row_height;
          const int rows = static_cast<int>(items.size()) - 1;
          if (rows > 0) {
            out.rect(rect.x, drop_y, rect.w,
                     rows * theme.row_height, theme.panel, true);
            out.rect(rect.x, drop_y, rect.w, rows * theme.row_height,
                     theme.border, false);
          }
          for (std::size_t i = 1; i < items.size(); ++i) {
            const int row_y =
                drop_y + static_cast<int>(i - 1) * theme.row_height;
            if (static_cast<int>(i) == widget.selected) {
              out.rect(rect.x, row_y, rect.w, theme.row_height,
                       theme.accent, true);
            }
            out.text(rect.x + theme.pad,
                     row_y + (theme.row_height -
                              text_height(theme.font_px)) / 2,
                     theme.font_px, items[i],
                     static_cast<int>(i) == widget.selected
                         ? game::Color{16, 18, 24, 255}
                         : theme.text);
          }
        }
        break;
      }
    }
    // Focus ring: 1px accent outline around the focused widget.
    if (focused && widget.interactive()) {
      out.rect(rect.x - 1, rect.y - 1, rect.w + 2, rect.h + 2,
               theme.accent, false);
    }
  }
}

}  // namespace render::ui
