#pragma once

// m11d — the sandbox screens on the m11c UI framework: a main menu, a
// scenario picker and the battle controls (the m11c demo screen),
// plus the screen-level App reducer that routes events to the active
// screen and handles navigation clicks. Everything stays pure data +
// pure functions: the game window and the --ui-script harness feed
// the same events and draw the same DrawLists.

#include <string>
#include <vector>

#include "game/view.hpp"
#include "render/ui/ui.hpp"

namespace render::ui {

// The screens.
enum class Screen : std::uint8_t { Menu, Scenario, Battle };

// Stable widget ids across the screens.
inline constexpr const char* kIdNewBattle = "new-battle";
inline constexpr const char* kIdScenario = "scenario";
inline constexpr const char* kIdQuit = "quit";
inline constexpr const char* kIdScenarioList = "scenario-list";
inline constexpr const char* kIdStart = "start";
inline constexpr const char* kIdBack = "back";

// The whole sandbox UI: the active screen and its three states.
struct App {
  Screen screen = Screen::Menu;
  State menu;
  State scenario;
  State battle;
  bool quit = false;            // the Quit button was clicked
  std::string chosen_scenario;  // the scenario Start picked ("" = none)
  int width = 0;
  int height = 0;

  [[nodiscard]] const State& active() const noexcept {
    switch (screen) {
      case Screen::Menu:
        return menu;
      case Screen::Scenario:
        return scenario;
      case Screen::Battle:
        break;
    }
    return battle;
  }
  [[nodiscard]] State& active() noexcept {
    return const_cast<State&>(static_cast<const App*>(this)->active());
  }
};

// Builds the app: the menu screen active, the scenario list filled
// with `scenarios` (in the given order), the battle screen from
// demo_state. All three states are laid out for width x height.
[[nodiscard]] App sandbox_app(int width, int height,
                              const std::vector<std::string>& scenarios);

// The screen-level reducer: routes the event to the active screen's
// state (through the pure ui::reduce), then applies navigation clicks
// — menu: New battle / Scenario / Quit; scenario: Start (with the
// selected entry) / Back; battle: Back returns to the menu. Pure.
[[nodiscard]] App reduce_app(App app, const Event& event,
                             const std::vector<std::string>& scenarios);

// Maps the battle screen's widget values onto the viewer's options:
// the hud / terrain / squad-links / grid checkboxes, and the filter
// select (All units keeps every shape; a type filter is a viewer
// feature for M12 — the mapping records the choice only). The zoom
// slider value is exposed separately for the camera.
[[nodiscard]] game::ViewOptions options_from(const State& battle);
[[nodiscard]] double zoom_from(const State& battle);

}  // namespace render::ui
