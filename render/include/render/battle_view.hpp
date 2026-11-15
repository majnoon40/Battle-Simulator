#pragma once

#include "game/camera.hpp"
#include "game/view.hpp"
#include "render/draw_list.hpp"
#include "sim/simulation.hpp"

namespace render {

// Default PNG frame size (also the window's default size).
inline constexpr int kFrameWidth = 1280;
inline constexpr int kFrameHeight = 720;

// Live playback numbers for the HUD (the window fills them; the headless
// PNG renderer keeps the defaults and the fps/tps segments are omitted).
struct HudStats {
  double fps = 0.0;      // frames per second; <= 0 hides the segment
  double sim_tps = 0.0;  // measured simulation ticks per second
  int speed = 1;
  bool limited = false;  // the accumulator capped the frame: "sim limited"
  std::string scenario_name;
};

// Builds the whole frame for a battle snapshot: background, world grid,
// formation slots, squad links, sight radius of the selected unit, units
// (points at low zoom, shapes by type otherwise; routed desaturated; dead
// units not drawn; selected highlighted; commanders outlined), squad
// badges, then the HUD / inspector / squad-panel text from the view
// options. Deterministic: the same simulation state, camera and options
// always produce the same list. No raylib, no file I/O.
[[nodiscard]] DrawList build_battle_draw_list(
    const sim::Simulation& simulation, const game::Camera& camera,
    const game::ViewOptions& options, int screen_width = kFrameWidth,
    int screen_height = kFrameHeight,
    const HudStats& stats = HudStats{});

}  // namespace render
