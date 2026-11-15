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
| L / I / O / G / H    | toggle squad links / sight radius / formation slots / grid / HUD |
| T                    | toggle the terrain layer (M8 maps) |

The terrain layer (M8 maps) is drawn first, in the terrain palette
colors, and can be toggled with T; see docs/terrain.md. The sim runs at
60 ticks/s at 1x.

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
