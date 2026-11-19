#pragma once

// m13 item 1 — the window shell policy: everything the raylib window
// (game/src/main.cpp) needs around the sandbox App to map OS input to
// UI events, gate the battle view's mouse and arrow keys, sync the zoom
// slider with the camera, and apply the battle screen's widgets to the
// viewer state. Pure data in, pure data out — the window feeds raylib
// values through these functions and renders the result, so every rule
// here is testable headlessly (tests/test_shell.cpp). What remains for
// a human to verify in the window itself is listed in docs/viewer.md.
//
// The rules the shell owns:
//   - the scenario list (sorted .json stems of a directory);
//   - input gating: while the pointer is over drawn UI chrome the
//     battle view must not pick units or zoom; arrow keys belong to the
//     UI on menu screens and on the battle screen when the focused
//     widget consumes them (slider: Left/Right, open select: Up/Down);
//   - the battle screen's momentary buttons (pause/play/speed) and the
//     view checkboxes map onto the viewer state every frame;
//   - the zoom slider [0,1] maps to camera zoom symmetric in log space
//     around the fit zoom: x16 at the right end, /16 at the left.

#include <string>
#include <vector>

#include "game/settings.hpp"
#include "game/view.hpp"
#include "render/ui/screens.hpp"

namespace render::ui {

// The .json stems in `dir`, sorted (the picker list; the same rule the
// --ui-script harness uses when the script pins no list).
[[nodiscard]] std::vector<std::string> list_scenarios(
    const std::string& dir);

// m13 item 5b: the .replay stems in `dir`, sorted (the replay viewer
// list — the same rule list_scenarios uses, for .replay files).
[[nodiscard]] std::vector<std::string> list_replays(
    const std::string& dir);

// True when the point is over drawn UI chrome — a Panel widget with
// chrome on the active screen. Transparent layout scaffolding (the
// centered-root spacers, the battle screen's window root) does not
// block, so the battle view keeps the mouse outside the panels.
[[nodiscard]] bool ui_covers(const App& app, Point p);

// Which arrow-key groups the UI owns this frame (the window must not
// pan the camera with a group the UI owns). Non-battle screens own
// both; the battle screen owns Left/Right over a focused Slider and
// Up/Down over a focused open Select.
struct ArrowOwnership {
  bool left_right = false;
  bool up_down = false;
};
[[nodiscard]] ArrowOwnership ui_arrow_ownership(const App& app);

// The battle screen's momentary buttons, read from one frame's clicks
// (reduce_app leaves non-navigation clicks in state.clicked for this
// layer). At most one button registers per frame.
struct BattleClicks {
  bool pause = false;
  bool play = false;
  int speed = 0;  // 0 = none this frame, else 1 / 2 / 4
};
[[nodiscard]] BattleClicks battle_clicks(const State& battle);

// Slider [0,1] -> camera zoom (pixels per world unit): `base` at the
// middle, 16x at the right end, 1/16 at the left, clamped to the
// camera limits. slider_from_zoom is the exact inverse (also clamped),
// so the window can follow wheel zoom back into the slider.
[[nodiscard]] double zoom_from_slider(double slider, double base);
[[nodiscard]] double slider_from_zoom(double zoom, double base);

// m13 item 5b: the end screen's window actions, read from one frame's
// clicks (navigation — Back — is handled by reduce_app; these stay in
// state.clicked). At most one registers per frame.
struct EndClicks {
  bool rematch = false;
  bool save_replay = false;
  bool save_report = false;
};
[[nodiscard]] EndClicks end_clicks(const State& end);

// m13 item 5b: the replay screen's window actions. Load returns the
// selected replay's stem ("" when the list is empty).
struct ReplayClicks {
  bool load = false;
  bool verify = false;
  std::string load_stem;
};
[[nodiscard]] ReplayClicks replay_clicks(const State& replay);

// m13 item 5c: the settings screen's widgets read back as a
// game::Settings (resolution select index, checkboxes, the UI-scale
// slider mapped [0,1] -> [0.75, 2.0]).
[[nodiscard]] game::Settings settings_from(const State& settings);
[[nodiscard]] bool settings_apply_clicked(const State& settings);

// m13 item 5c: the setup screen's window actions: a stepper click
// carries the type it steps ("" = none), Save and Start.
struct SetupClicks {
  bool save = false;
  bool start = false;
  std::string plus_type;
  std::string minus_type;
};
[[nodiscard]] SetupClicks setup_clicks(const State& setup);

// The viewer state the battle screen implies: the view options and
// pause/speed follow the widgets; zoom is the slider's mapping around
// `base_zoom`. apply_battle_frame folds one frame of clicks into the
// previous state (pause/play are sticky, speed buttons are absolute).
struct BattleView {
  game::ViewOptions options;
  double zoom = 1.0;
  bool paused = false;
  double speed = 1.0;
};
[[nodiscard]] BattleView apply_battle_frame(const App& app,
                                            const BattleView& previous,
                                            double base_zoom);

}  // namespace render::ui
