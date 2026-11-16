#pragma once

// m11c — the UI framework: a small retained-mode widget set in pure
// data, a deterministic layout engine, a pure reducer over UI events
// (headless-testable without a window), a theme, and a screen-space
// DrawList layer that draws on top of the battle view. Everything is
// integer pixels and fixed iteration order, so the same tree + state +
// theme always produce the same layout and the same command list.
//
// The layers:
//   theme  — colors and metrics (one struct, default_theme()).
//   text   — the shared 5x7 font metrics the rasterizer draws with
//            (advance 6*scale, glyph 5x7*scale), so layout and drawing
//            agree on every string.
//   widget — the tree: widgets in a flat parent-before-child vector,
//            panels own children laid out as a row or a column.
//   layout — two passes (size bottom-up, position top-down) with hug /
//            fixed / grow sizing; remainder pixels go to the earliest
//            children (documented, deterministic).
//   state  — the mutable UI state (widget values, focus, pointer) and
//            the PURE reducer: reduce(state, event) -> state. No I/O,
//            no clocks, no globals — the same event sequence always
//            yields the same state.
//   draw   — screen-space drawing into a DrawList (rects, text, focus
//            rings); the rasterizer or the raylib window renders it.

#include <cstdint>
#include <string>
#include <vector>

#include "game/theme.hpp"
#include "render/draw_list.hpp"

namespace render::ui {

// ---------------------------------------------------------------------------
// Geometry (integer pixels).
// ---------------------------------------------------------------------------

struct Point {
  int x = 0;
  int y = 0;
  [[nodiscard]] bool operator==(const Point&) const = default;
};

struct Rect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  [[nodiscard]] bool operator==(const Rect&) const = default;
  [[nodiscard]] bool contains(Point p) const noexcept {
    return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h;
  }
  [[nodiscard]] bool intersects(const Rect& other) const noexcept {
    return x < other.x + other.w && other.x < x + w &&
           y < other.y + other.h && other.y < y + h;
  }
  [[nodiscard]] int right() const noexcept { return x + w; }
  [[nodiscard]] int bottom() const noexcept { return y + h; }
};

// ---------------------------------------------------------------------------
// Theme.
// ---------------------------------------------------------------------------

struct Theme {
  game::Color panel{};        // widget background
  game::Color panel_hover{};  // hovered interactive widget
  game::Color panel_active{}; // pressed button / open select
  game::Color border{};       // 1px widget border
  game::Color text{};         // normal text
  game::Color text_dim{};     // disabled text
  game::Color accent{};       // focus ring, selection, check mark
  game::Color track{};        // slider track
  game::Color thumb{};        // slider thumb
  game::Color danger{};       // destructive highlight (Pause)
  int font_px = 14;   // -> 5x7 font scale 2 (12x14 px glyphs)
  int pad = 8;        // inner padding of panels
  int gap = 6;        // gap between siblings in a box
  int border_w = 1;   // border thickness (drawn as an outline rect)
  int slider_track_h = 4;
  int slider_thumb_w = 10;
  int row_height = 22;  // one interactive row (button/checkbox height)
};

[[nodiscard]] Theme default_theme();

// ---------------------------------------------------------------------------
// Shared text metrics (the rasterizer's built-in 5x7 font: glyph
// 5x7*scale, one scale-unit space, advance 6*scale per character).
// ---------------------------------------------------------------------------

[[nodiscard]] int font_scale(int font_px);
[[nodiscard]] int text_advance(const std::string& text, int font_px);
[[nodiscard]] int text_height(int font_px);

// ---------------------------------------------------------------------------
// Widgets and the tree.
// ---------------------------------------------------------------------------

enum class Kind : std::uint8_t {
  Panel,      // a box that lays out its children; text = the title
  Label,      // plain text
  Button,     // activates (clicked) on Enter or a click
  Checkbox,   // toggles `on`
  Slider,     // `value` in [0, 1], dragged by the pointer
  Select,     // items split from `text` by '\n'; `selected` + open flag
  Separator,  // a 1px line
};

// How a Panel lays out its children.
enum class Box : std::uint8_t { Column, Row };

struct Widget {
  std::string id;
  Kind kind = Kind::Label;
  std::string text;   // Label/Button/Panel title/Select items (\n)
  Box box = Box::Column;  // Panel layout direction
  // Panels stretch their children on the cross axis (a column's
  // children fill its width, a row's children fill its height) unless
  // the child has a fixed cross size. The root turns it off.
  bool stretch_children = true;
  bool on = false;        // Checkbox; Select: the dropdown is open
  double value = 0.5;     // Slider position [0, 1]
  int selected = 0;       // Select item index
  bool enabled = true;
  // Panels draw a filled rect + border unless chrome is false —
  // layout-only boxes (the window root, spacers) draw nothing.
  bool chrome = true;
  // Sizing: fixed wins, else hug the content, else grow fills the
  // leftover space proportionally to `grow` (0 = never grows).
  int fixed_w = 0;
  int fixed_h = 0;
  int min_w = 0;
  int min_h = 0;
  double grow = 0.0;
  // Computed by layout() — the widget's rect on screen (relative to
  // the parent's content rect while laying out, absolute after the
  // position pass).
  Rect rect{};

  [[nodiscard]] bool interactive() const noexcept {
    return kind == Kind::Button || kind == Kind::Checkbox ||
           kind == Kind::Slider || kind == Kind::Select;
  }
};

// The widget tree: flat, parent-before-child (a child's index is always
// greater than its parent's). One root panel is typical.
struct Tree {
  std::vector<Widget> widgets;
  std::vector<std::int32_t> parent;  // -1 = root, else parent index

  // Builds a one-root tree; the root panel fills the given size.
  static Tree root_panel(int width, int height,
                         const std::string& id = "root");

  // Appends a child to `parent_index`; returns the child's index.
  std::uint32_t add(std::int32_t parent_index, Widget widget);

  [[nodiscard]] std::size_t size() const noexcept {
    return widgets.size();
  }
  [[nodiscard]] const Widget& at(std::size_t index) const {
    return widgets[index];
  }
  [[nodiscard]] Widget& at(std::size_t index) { return widgets[index]; }

  // Children of one parent, in index order.
  [[nodiscard]] std::vector<std::int32_t> children_of(
      std::int32_t index) const;
};

// ---------------------------------------------------------------------------
// Layout.
// ---------------------------------------------------------------------------

// Lays the tree out inside `bounds` (the whole UI area, usually the
// window): size pass bottom-up (hug/fixed/grow with the shared text
// metrics), position pass top-down. Remainder pixels from integer
// division go to the earliest growing children. Panels inset their
// content by the theme padding; rows and columns separate children by
// the theme gap. After the call every widget's rect is absolute.
void layout(Tree& tree, const Rect& bounds, const Theme& theme);

// The natural (hug) size of one widget: text size plus padding for
// panels, the theme row height for interactive rows.
[[nodiscard]] int hug_width(const Widget& widget, const Theme& theme);
[[nodiscard]] int hug_height(const Widget& widget, const Theme& theme);

// ---------------------------------------------------------------------------
// State, events and the pure reducer.
// ---------------------------------------------------------------------------

// Keyboard keys the UI understands.
enum class Key : std::uint8_t {
  None = 0,
  Up,
  Down,
  Left,
  Right,
  Enter,
  Escape,
  Tab,
  Home,
  End,
};

struct Event {
  enum class Type : std::uint8_t {
    Key,       // key press (reduce treats keys as presses)
    MouseMove, // pos
    MouseDown, // pos, button (0 = left)
    MouseUp,   // pos, button
    Wheel,     // pos, delta (positive = away from the user)
  };
  Type type = Type::Key;
  Key key = Key::None;
  Point pos{};
  int button = 0;
  int delta = 0;
};

// The UI state: the laid-out tree (widget values + rects), the focus
// index (into the widgets vector; -1 = none), the pointer and the
// one-frame interaction results the app reads (clicked id, value
// changes are read from the widgets themselves).
struct State {
  Tree tree;
  std::int32_t focus = -1;   // focused interactive widget index
  std::int32_t hover = -1;   // widget under the pointer
  std::int32_t pressed = -1; // widget the pointer went down on
  Point pointer{};
  bool pointer_down = false;
  std::string clicked;       // id of the widget activated this frame
  bool click_consumed = true;
  int width = 0;
  int height = 0;
};

// The pure reducer: applies one event, returns the new state (the input
// is untouched). Key handling: Tab / Up / Down move the focus through
// the interactive widgets in tree order (Tab wraps; Up/Down do not move
// it out of a closed Select list — Up/Down inside an open Select move
// the selection), Enter activates (Button -> clicked, Checkbox toggles,
// Select opens / confirms), Escape closes an open Select, Left / Right
// adjust a focused Slider by 1/32. Mouse: hover follows the topmost
// interactive widget under the pointer; a click activates buttons and
// checkboxes, drags sliders (value from the pointer x inside the
// track), opens a select and picks its items. The state is relaid-out
// after every event (an open Select grows its panel), keeping rects in
// sync with values.
[[nodiscard]] State reduce(State state, const Event& event,
                           const Theme& theme);
[[nodiscard]] State reduce(State state, const Event& event);

// Moves the focus to the next/previous interactive widget (tree order,
// wrapping; skips disabled widgets and panels/labels). Returns the new
// state; when no interactive widget exists the focus stays -1.
[[nodiscard]] State focus_next(State state, bool forward);

// The topmost interactive widget at a point (hit test), or -1. Select
// dropdown rows count as part of their select widget.
[[nodiscard]] std::int32_t hit_test(const State& state, Point point);

// ---------------------------------------------------------------------------
// Drawing (screen space, on top of everything else in the list).
// ---------------------------------------------------------------------------

// Appends the UI drawing commands to `out` (caller sets out.width/
// height and draws the battle view first; the UI goes on top). Panels:
// filled rect + border + title text; interactive widgets: hover/active
// fills, a 1px accent focus ring around the focused widget, slider
// track + thumb, select dropdown with the selected row highlighted.
// The same (state, theme) always appends the same commands.
void draw_ui(const State& state, const Theme& theme, DrawList& out);

}  // namespace render::ui
