# The viewer (M7)

A window where you WATCH battles — no new simulation behavior; the sim and
AI are unchanged. The same pure drawing code drives two backends:

```
sim snapshot + camera + view options
              |
        render/  build_battle_draw_list()   (pure; no raylib)
              |
         DrawList (circles, triangles, lines, rects, text)
          /                    \
  game/ battle_game      render/ rasterize() + encode_png()
  (raylib window)        (software rasterizer -> PNG, stb_image_write)
```

Everything the automated tests cover — camera math, the fixed-timestep
accumulator, picking, the DrawList contents, the rasterized PNG bytes, HUD
text and colors — is exactly what the window draws.

## Headless use (no display needed; the default build)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_GAME=OFF
cmake --build build -j2

# One frame (camera fit to the world, 1280x720):
./build/battle_cli --scenario data/scenarios/viewer-demo.json --seed 42 \
    --render-frame 100 --out frame.png

# Several ticks, one PNG each (files: dir/name-0000250.png ...):
./build/battle_cli --scenario data/scenarios/viewer-demo.json --seed 42 \
    --render-frames 0,100,250,500,1000 --out-prefix out/frames

# A compact terminal map:
./build/battle_cli --scenario data/scenarios/viewer-demo.json --ascii 200
```

In render/ascii modes the simulation stops right after the last target
tick; the battle report reflects that state. All CLI battle flags
(`--seed`, `--scenario`, `--morale`, `--squads`, ...) apply. A 20,000-unit
frame renders in about 46 ms.

## The window (battle_game)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_GAME=ON
cmake --build build -j2
./build/battle_game            # data/scenarios/viewer-demo.json, seed 42
./build/battle_game --scenario data/scenarios/squads.json --seed 7 --morale off
```

`battle_game` takes the same arguments as `battle_cli`. raylib 5.5 is
fetched at configure time and pinned to release tag `5.5`
(commit `c1ab645ca298a2801097931d1079b10ff7eb9df8`); the default
`BUILD_GAME=OFF` build never fetches it.

Controls:

| Key / mouse          | Action                                              |
| -------------------- | --------------------------------------------------- |
| Space                | pause / resume                                      |
| + / -                | speed: 1, 2, 4, 8, 16x                              |
| `.`                  | step one tick (while paused)                        |
| WASD / arrows        | pan                                                 |
| right-drag           | pan                                                 |
| mouse wheel          | zoom about the cursor (0.05–32 px per world unit)   |
| F                    | fit the whole battlefield                           |
| R                    | restart from tick 0 with the same seed              |
| N                    | new random seed                                     |
| left-click           | select a unit (inspector) or a squad badge (panel)  |
| left-drag            | rectangle-select every Red squad touched (m13 5a)  |
| Shift + click/drag   | add to the squad selection (toggle on click)       |
| right-click          | MoveTo the world point for the selected squads     |
| Shift + right-click  | AttackMove the world point (m13 5a)                |
| 1 / 2 / 3            | Hold / Retreat / Regroup the selected squads (5a)  |
| M                    | toggle the minimap (m13 5a)                        |
| L / I / O / G / H    | toggle squad links / sight radius / formation slots / grid / HUD |
| T                    | toggle the terrain layer (M8 maps) |

The terrain layer (M8 maps) is drawn first, in the terrain palette
colors, and can be toggled with T; see docs/terrain.md. The sim runs at
60 ticks/s at 1x.

m13 item 5a adds the battle-view command layer: the minimap (bottom-
left; terrain thumbnail when a map is loaded, unit dots, camera
viewport outline — a click jumps the camera there), the multi-squad
selection (rings on every member of the selected squads), and order
markers (a line and glyph from each ordered squad to its target: M
MoveTo, A AttackMove, H Hold, R Retreat, G Regroup). Orders route
through the same courier-delay delivery as `--script` (m11b), so a
click takes effect a few ticks later — the marker appears when the
squad receives it. Right-drag still pans: a right press that moves
more than 6 px is a pan, a stationary release is an order. The pure
policy (selection rules, intent mapping, minimap geometry) lives in
game/battle_input.hpp and is headless-tested (test_battle_input.cpp);
the window only feeds mouse/key facts and renders.

Rendering: units are drawn per faction color (Red vivid red, Blue vivid
blue; routed units desaturated; dead units not drawn); shape by type
(infantry circle, archer triangle, cavalry and commander rectangles,
anything else falls back to a circle); commanders get a gold outline; the
selected unit gets a white ring. Below 1 px per world unit every unit is a
single pixel, so 20,000-unit battles stay readable. The sim runs on a
fixed-timestep accumulator (60 ticks/s at 1x, capped at 256 ticks per
frame); when the cap bites the HUD shows "sim limited" instead of
freezing. Clicking a unit arms the AI debug capture on it, so the
inspector shows live utility scores and order/obedience.

## Required dev libraries per OS (for BUILD_GAME=ON)

raylib bundles GLFW, which needs the windowing/OpenGL headers. The default
headless build (`BUILD_GAME=OFF`) needs NONE of these.

**Linux** (Debian/Ubuntu package names; Fedora: `libX11-devel libXcursor-devel
libXrandr-devel libXinerama-devel libXi-devel mesa-libGL-devel`):

```
sudo apt install libx11-dev libxcursor-dev libxrandr-dev \
                 libxinerama-dev libxi-dev libgl-dev
```

(On older distros `libgl-dev` is provided by `libgl1-mesa-dev` or
`mesa-common-dev`.) A C++20 compiler, CMake >= 3.16, git and internet
access at configure time (FetchContent pulls nlohmann/json v3.11.3, stb
`9600522`, GoogleTest v1.14.0 for tests, and raylib 5.5 for the game) are
needed everywhere.

**Windows** (MinGW-w64 or MSVC): no extra system libraries — GLFW uses
Win32 and OpenGL via the SDK. From a "x64 Native Tools" or MSYS2 UCRT64
shell: `cmake -S . -B build -DBUILD_GAME=ON && cmake --build build`.

**macOS**: install the Xcode command-line tools
(`xcode-select --install`); GLFW uses Cocoa and the system OpenGL
framework. Then `cmake -S . -B build -DBUILD_GAME=ON && cmake --build build`.

## What was verified without a display

The pure pipeline is fully tested (camera, accumulator, picking, DrawList
counts/styling/determinism, byte-identical PNG encoding, HUD/inspector
text, ascii map; 244/244 tests) and the rendered PNGs/ascii maps were
checked programmatically (faction pixel counts, routed styling, frame
differences, 20k timing). **The raylib window itself is UNVERIFIED**: this
sandbox lacks the X11/GL dev headers (raylib's configure fails with
"RandR headers not found; install libxrandr development package"), so
`game/src/main.cpp` was never compiled or run. See the M7 report for the
exact list of things to check by hand.

## The sandbox screens in the window (M13 item 1)

The window now runs the m11c/m11d UI screens. The wiring is thin and one
directional: raylib input becomes `ui::Event`s, the pure reducers
(`ui::reduce`, `reduce_app`) own every state change, and the window renders
the DrawList the reducers' state produces. The window-level policy lives
in `render/ui/shell.cpp` (pure, tested headlessly — `tests/test_shell.cpp`):

- the pointer over drawn UI panels belongs to the UI (no unit picking or
  wheel zoom through panels); right-drag always pans the map;
- arrow keys pan the map on the battle screen unless the focused widget
  consumes them (a slider takes Left/Right, an open select takes Up/Down);
  on menu screens they belong to the UI;
- the battle screen's Pause/Play/speed buttons and view checkboxes fold
  into the viewer state every frame; the zoom slider maps symmetric in
  log space around the fit zoom (x16 at the right end, /16 at the left)
  and the wheel zoom follows back into the slider (exact inverse);
- the scenario picker's Start reloads the battle (`data/scenarios/<name>
  .json`); the app starts on the menu and the CLI `--scenario` battle
  renders as the backdrop until a scenario is picked.

Known window gaps (planned for the M13 5a battle view): the battle screen
has no Back button yet (the m11d App reducer has no battle->menu
navigation; Escape closes selects only), and a window resize rebuilds the
screens without preserving widget values.

### What only a human can verify

`game/src/main.cpp` is compiled by the CI `game-build` job but was never
run in the development sandbox (no X11/GL). When you first run the window
(`cmake -S . -B build -DBUILD_GAME=ON && cmake --build build --target
battle_game && ./build/battle_game`), check:

1. The window opens 1280x720 with the AUTOBATTLE menu box centered on the
   plain background; text is legible at the default theme size.
2. Mouse hover highlights buttons/checkboxes; Tab moves a visible focus
   ring; Enter activates the focused button; the select dropdown opens
   downward and highlights the selected row.
3. Menu navigation: New battle and Scenarios both open the picker; Start
   reloads the battle (the HUD's scenario name changes) and Back returns
   to the menu; Quit closes the window.
4. Battle screen: the control panel sits on the left, the status bar at
   the bottom; Pause/Play freeze and resume the sim, the speed buttons set
   1x/2x/4x (cross-check with +/- and the HUD speed); the terrain, grid,
   squad-links and HUD checkboxes toggle the same things as the T/G/L/H
   hotkeys.
5. Zoom: the slider zooms about the screen center (x16 at the right end);
   wheel zoom over the map follows back into the slider position; wheel
   over a UI panel does not zoom; the wheel direction feels natural
   (scroll up = zoom in — the raylib sign mapping was written against the
   docs, not verified).
6. Input gating: left-click over the map picks units (inspector), over
   the panels it does not; right-drag pans everywhere; WASD pans, arrows
   pan until a slider (Left/Right) or an open select (Up/Down) has focus.
7. The battle keeps running at 60 ticks/s at 1x with the UI drawn (the
   UI adds only a handful of rects/text per frame); resizing the window
   relayouts the screens.
