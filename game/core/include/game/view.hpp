#pragma once

#include <cstdint>
#include <vector>

namespace game {

// What the viewer draws (M7). One struct shared by the window, the PNG
// renderer and the tests, so "what the agent tests is what the window
// draws". All toggles default to a good demo view; -1 selections mean
// "none".
struct ViewOptions {
  bool squad_links = true;        // line from each member to its commander
  bool show_sight_radius = false; // sight circle of the selected unit
  bool formation_slots = false;   // every member's formation slot
  bool show_grid = true;          // world grid every 100 units
  bool show_terrain = true;       // the terrain layer (M8 maps)
  bool show_hud = true;           // HUD text block
  bool show_badges = true;        // clickable squad id badges at centroids
  std::int32_t selected_unit = -1;  // unit index, or -1
  std::int32_t selected_squad = -1; // global squad id, or -1
  // m13 item 5a: the multi-squad selection (sorted global squad ids —
  // game::SquadSelection) the battle view highlights, the order markers
  // on squads with active player orders, and the minimap (bottom-left).
  std::vector<std::int32_t> selected_squads;
  // The interactive battle view (window) turns these on; the pinned
  // PNG/report baselines keep their exact command streams, so the
  // defaults stay off.
  bool show_order_markers = false;
  bool show_minimap = false;
  // Below this zoom (pixels per world unit) units draw as single pixels so
  // 20,000-unit battles stay readable at full-view.
  double point_zoom_threshold = 1.0;
};

}  // namespace game
