#pragma once

// m11c — the demo screen: the battle control panel that exercises every
// widget kind (the seed of the m11d screens). Built once from a theme;
// the reducer drives it. Left: the control panel (play/pause buttons,
// speed buttons, view-option checkboxes, a zoom slider, a unit-type
// filter select, a status label). Bottom: a status bar with a growing
// label. The same (width, height) always builds the same tree.

#include <string>

#include "render/ui/ui.hpp"

namespace render::ui {

// The control panel, anchored to fill the window (a root panel with a
// transparent look is the simplest deterministic base). Returns the
// initial state (laid out, focus on the first button).
[[nodiscard]] State demo_state(int width, int height);

// The widget ids the demo screen uses (stable, for scripts and tests).
inline constexpr const char* kIdPause = "pause";
inline constexpr const char* kIdPlay = "play";
inline constexpr const char* kIdSpeed1 = "speed1";
inline constexpr const char* kIdSpeed2 = "speed2";
inline constexpr const char* kIdSpeed4 = "speed4";
inline constexpr const char* kIdHud = "hud";
inline constexpr const char* kIdTerrain = "terrain";
inline constexpr const char* kIdSquadLinks = "squad-links";
inline constexpr const char* kIdGrid = "grid";
inline constexpr const char* kIdZoom = "zoom";
inline constexpr const char* kIdFilter = "filter";
inline constexpr const char* kIdStatus = "status";

}  // namespace render::ui
