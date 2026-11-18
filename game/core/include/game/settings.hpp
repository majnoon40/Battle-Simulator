#pragma once

// m13 item 5c — the window settings: resolution, fullscreen, vsync,
// UI scale and the colorblind palette, persisted as settings.json in
// the working directory. Pure data + parse/write; the window applies
// them to raylib and the UI theme, the defaults reproduce today's
// window exactly.

#include <string>

namespace game {

struct Settings {
  int width = 1280;
  int height = 720;
  bool fullscreen = false;
  bool vsync = false;
  double ui_scale = 1.0;  // [0.75, 2.0]
  bool colorblind = false;
};

// Parses settings JSON. Unknown fields are rejected (the same
// never-silently-default rule every data file follows); a missing file
// is the caller's business (defaults apply). On failure returns false
// and fills `error`; `out` is left unchanged.
[[nodiscard]] bool parse_settings(const std::string& text, Settings& out,
                                  std::string& error);

// Serializes the settings (round-trips through parse_settings).
[[nodiscard]] std::string write_settings(const Settings& settings);

// The colorblind palette toggle (m13 item 5c): swaps the faction
// colors to blue/orange (deuteranopia-safe) in game::faction_color /
// routed_color. A process-global viewer concern: colors never feed
// simulation state, so determinism is untouched; the flag itself is
// part of Settings (persisted), not of any battle.
void set_colorblind_palette(bool on);
[[nodiscard]] bool colorblind_palette() noexcept;

}  // namespace game
