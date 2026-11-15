# game/ — the viewer (M7)

- `core/` — `game_core`, the pure raylib-free library: camera math
  (`camera.hpp`), the fixed-timestep accumulator (`accumulator.hpp`),
  picking (`picking.hpp`), HUD/inspector/squad text, colors and shapes
  (`theme.hpp`), view options (`view.hpp`) and the ascii map (`ascii.hpp`).
  Always built; covered by the test suite.
- `src/main.cpp` — `battle_game`, the raylib window, built only with
  `-DBUILD_GAME=ON` (raylib 5.5, fetched and pinned in CMakeLists.txt).
  Needs the X11/GL dev libraries listed in docs/viewer.md; it was NOT
  compiled or run in the headless sandbox where M7 was developed
  (missing libxrandr & friends), so treat it as unverified-by-agent code
  whose logic lives in the tested game_core/render libraries.

The window draws the same DrawList the headless PNG renderer rasterizes;
see docs/viewer.md for build/run instructions and controls.
