#pragma once

// m13 item 5a — the battle-view input policy: multi-squad selection,
// player-order intents and minimap geometry, all pure data + pure
// functions. The raylib window (game/src/main.cpp) feeds mouse/keyboard
// facts in and applies the results (selection to ViewOptions, intents
// through apply_order, minimap clicks to the camera center); every rule
// here is headless-testable (tests/test_battle_input.cpp). The player
// side is Red, exactly like --script (m11b).
//
// The rules this module owns:
//   - selection: a sorted, unique set of global squad ids. A plain
//     click replaces it, shift-click adds or toggles one squad, a
//     rectangle selects every Red squad with an alive member inside,
//     shift adds to the current selection, clicking nothing clears
//     (shift-clicking nothing keeps it);
//   - orders: right-click issues MoveTo to the world point on the whole
//     selection (shift-right-click = AttackMove); the hotkeys
//     1 / 2 / 3 issue Hold / Retreat / Regroup; no selection -> no
//     order; apply_order routes the intent through
//     Simulation::issue_order and reports whether any squad received it;
//   - minimap geometry: a square in the bottom-left screen corner (the
//     HUD owns top-left, the inspector top-right, the squad panel
//     bottom-right), world <-> minimap-pixel mapping and the world
//     point a minimap click centers the camera on.

#include <cstdint>
#include <vector>

#include "sim/simulation.hpp"

namespace game {

// ---------------------------------------------------------------------------
// Selection.
// ---------------------------------------------------------------------------

struct SquadSelection {
  std::vector<std::uint32_t> squads;  // sorted, unique, global squad ids

  [[nodiscard]] bool empty() const noexcept { return squads.empty(); }
  [[nodiscard]] bool contains(std::uint32_t squad_id) const noexcept;
};

// A click that picked squad `picked` (a global squad id, or -1 when the
// click hit nothing).
[[nodiscard]] SquadSelection select_squad_click(
    const SquadSelection& previous, std::int32_t picked, bool shift);

// A drag rectangle in world coordinates (any corner order): selects
// every Red squad with at least one alive member inside. Shift adds.
[[nodiscard]] SquadSelection select_squad_rect(
    const sim::Simulation& simulation, const SquadSelection& previous,
    double x0, double y0, double x1, double y1, bool shift);

// ---------------------------------------------------------------------------
// Order intents.
// ---------------------------------------------------------------------------

enum class OrderType : std::uint8_t {
  MoveTo,
  AttackMove,
  Hold,
  Retreat,
  Regroup,
};

// The glyph the renderer draws for an order (minimap-sized text).
[[nodiscard]] const char* order_glyph(OrderType type) noexcept;

struct OrderIntent {
  bool valid = false;  // false = nothing to do (e.g. empty selection)
  OrderType type = OrderType::Hold;
  std::vector<std::uint32_t> squads;
  double x = 0.0;  // world target (point orders)
  double y = 0.0;
};

// Right-click on the battlefield: MoveTo (or AttackMove with the
// modifier) to the world point for the whole selection.
[[nodiscard]] OrderIntent order_from_right_click(
    const SquadSelection& selection, bool attack_modifier, double wx,
    double wy);

// A hotkey press (1 / 2 / 3): Hold / Retreat / Regroup for the whole
// selection, no point.
[[nodiscard]] OrderIntent order_from_hotkey(const SquadSelection& selection,
                                            OrderType type);

// Routes an intent through Simulation::issue_order (Red). Returns true
// when the simulation accepted it (issue_order's own result).
[[nodiscard]] bool apply_order(sim::Simulation& simulation,
                               const OrderIntent& intent);

// ---------------------------------------------------------------------------
// Minimap geometry.
// ---------------------------------------------------------------------------

// The minimap placement: a `size`-pixel square in the bottom-left
// corner with an 8 px margin, plus the world it covers.
struct MinimapGeometry {
  int x = 0;            // screen-space top-left
  int y = 0;
  int size = 200;
  double world_w = 0.0;
  double world_h = 0.0;
};

[[nodiscard]] MinimapGeometry minimap_geometry(int screen_width,
                                               int screen_height,
                                               double world_width,
                                               double world_height,
                                               int size = 200);

// World point -> minimap pixel (top-left of the map square is 0,0).
[[nodiscard]] double minimap_x(const MinimapGeometry& geometry,
                               double world_x) noexcept;
[[nodiscard]] double minimap_y(const MinimapGeometry& geometry,
                               double world_y) noexcept;

// Minimap pixel -> world point (the click-to-jump mapping; pixels
// outside the square clamp to the battlefield).
[[nodiscard]] double world_x_from_minimap(const MinimapGeometry& geometry,
                                          double map_x) noexcept;
[[nodiscard]] double world_y_from_minimap(const MinimapGeometry& geometry,
                                          double map_y) noexcept;

// True when a screen point is over the minimap square (the window must
// not pick units or start a drag there).
[[nodiscard]] bool minimap_covers(const MinimapGeometry& geometry,
                                  int screen_x, int screen_y) noexcept;

}  // namespace game
