# Battlefield Simulator

A deterministic, headless battle engine: two AI-driven armies fight on
procedural or real-world terrain, and every run with the same seed produces
byte-identical results. Written in C++20, with no required system
dependencies beyond a compiler and CMake — the third-party libraries
(nlohmann/json, stb, GoogleTest, and raylib for the optional window) are
fetched at configure time, pinned to exact versions. An optional raylib
window exists for watching battles; the engine itself, including rendering,
runs fully headless.

The project was developed milestone-by-milestone (M1–M10); each milestone
has a design report under `docs/`.

## Features (through milestone M13)

- **Deterministic simulation** — a seeded SplitMix64 RNG is the only source
  of randomness; the same seed reproduces the same battle byte-for-byte,
  and the spatial-hash neighbor mode produces identical output to the
  brute-force mode (`grid == brute`, tested).
- **Deterministic multithreading (M13)** — `--threads N` runs the
  perception and decision phases on a persistent worker pool in fixed
  chunks; output is byte-identical for every thread count (verified
  1/2/3/4/8 across scenarios; ThreadSanitizer-clean; measured 1.85x on
  2 cores at 20,000 units), `--profile-phases` prints per-phase timing.
- **Utility AI with morale** — units score actions (attack, approach,
  advance, retreat, hold) from health, local strength, distance, cooldown
  and personality (bravery / caution / aggression); morale states, routing,
  contagion and recovery (M4–M5).
- **Squads, commanders, orders** — formations, leadership radius, command
  succession after a commander dies, commander orders (attack / regroup /
  retreat), squad intel sharing and collapse detection (M6).
- **Terrain from maps** — PNG maps with a per-terrain palette: movement
  costs, line of sight, defense modifiers, impassable water with fords,
  flow-field pathing, stuck detection and recovery (M8).
- **Battle reports, replays and events (M10)** — `--report out.md`
  (outcome, casualties, morale timeline, squads, turning points),
  `--record` / `--replay --verify` with canonical state hashes every
  25 ticks, `--events` JSONL and `--export-csv` timelines.
- **Performance (M13 item P)** — the whole regression chain from the
  initial release is measured in `docs/milestone-13.md`; the routed-ally
  Help probe is memoized per tick: 5,000 units run at 82 t/s with 2
  threads on 2 cores (byte-identical, verified by replay hashes).
- **Unit balance data (M13 item H)** — optional `cost` field on unit
  types and budget-form scenario armies (`{"budget": 300, "mix": ...}`,
  deterministic largest-remainder fill); the tuned
  `data/units-balanced.json` makes every matchup competitive; the
  legacy `data/units.json` is untouched (`docs/data-reference.md`).
- **The sandbox window (M13 items 1, 5a-5c)** — the raylib window runs
  the same deterministic battle with the full UI: scenario picker,
  battle controls, minimap, multi-squad selection and player orders
  (right-click / hotkeys, the same courier-delay path as `--script`),
  order markers, an end screen with casualty curves and save
  replay/report, a replay viewer with verify + scrub, an army editor
  with drag-to-place spawn anchors, and settings (resolution,
  fullscreen, vsync, UI scale, colorblind palette) —
  `docs/viewer.md`.
- **Fuzzing (M13 item 6)** — 13 libFuzzer harnesses over every parser
  plus a bounded simulation harness; a 60s-per-push CI job, a nightly
  10-minute run and a committed seed corpus (`docs/fuzzing.md`).
- **Navigation v2** — `--nav v2` (default): the `--swap-sides`
  explicit-objective swap actually happens, and formation members far
  from their slot march along the flow field instead of beelining into
  rivers and valley walls; `--nav v1` reproduces the M10 binary
  byte-for-byte, and a replay records which generation ran (M11a, see
  [docs/terrain.md](docs/terrain.md)).
- **Earth scenarios** — maps generated from real geodata (NOAA ETOPO 2022
  elevation, Natural Earth rivers and lakes): Lebanon, Alps (Innsbruck),
  Nile Delta, with reproducible generation and checksums (M9, see
  [docs/DATA_SOURCES.md](docs/DATA_SOURCES.md)); `-v2` variants with
  balanced spawn bands, march-axis ford checks, and march-cost /
  both-orientation connectivity analysis in the meta (M11a).
- **Tactical depth** — per-unit combat variance, tactical memory, squad
  intel, Search / Help / Flank actions, bounded pursuit of routed enemies,
  contact seeking, spawn patterns; `--variance off` reproduces every
  M1–M9 reference byte-for-byte (M10).
- **Events, timelines, reports** — JSONL event logs (deaths with killer,
  routs, recoveries, command changes, first contact, ...), CSV timelines,
  markdown + JSON battle reports with turning points and the first-contact
  tick, and report charts rendered with the built-in software rasterizer
  (M10.2a, M11a).
- **Player orders** — `--script orders.json` issues timed army-level
  commands (MoveTo / AttackMove / Hold / Retreat / Regroup /
  ChangeFormation) through the commander system: courier delay from the
  faction HQ, succession and leaderless surcharges, obedience from
  morale, discipline and cohesion, bounded score terms — never
  overrides; recorded into replays and re-applied by `--replay
  --verify` (M11b, see [docs/squads.md](docs/squads.md)).
- **UI framework** — a retained-mode widget set in pure data (buttons,
  checkboxes, sliders, selects, panels), a deterministic layout engine
  (hug/fixed/grow, cross-axis stretch), a theme, focus + keyboard
  navigation, a pure reducer over UI events and a screen-space draw
  layer over the battle view; the sandbox screens (main menu, scenario
  picker, battle controls) run on it; `--ui-script` drives everything
  headlessly and writes PNG snapshots (M11c/d, see
  [docs/milestone-11.md](docs/milestone-11.md)).
- **Replay** — record a battle to a file (setup, interventions, orders,
  state hashes every 25 ticks) and verify a re-run against it; `--seek`
  re-runs to any tick (M10.2b, M11b).
- **Headless rendering** — the same drawing pipeline feeds a PNG renderer
  and an ASCII map; the optional raylib window (`BUILD_GAME=ON`) reuses it
  (M7).

## Build and test (headless)

Needs: a C++20 compiler (GCC 14 tested), CMake ≥ 3.16, git, and internet
access at configure time (FetchContent pulls the pinned dependencies).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

This builds the `sim` library, the `battle_cli` headless tool and the
`battle_tests` suite (349 tests). No windowing or OpenGL libraries are
required. Floating-point determinism flags (`-fno-fast-math
-ffp-contract=off`) are applied automatically for GCC/Clang; use the same
Release configuration for any determinism comparison.

## The optional window (BUILD_GAME=ON)

`battle_game` is the raylib viewer. raylib (pinned to 5.5) is fetched at
configure time; its bundled GLFW needs the windowing/OpenGL headers below.

**Debian / Ubuntu**

```sh
sudo apt install libx11-dev libxcursor-dev libxrandr-dev \
                 libxinerama-dev libxi-dev libgl-dev libasound2-dev
```

**Fedora** (same libraries, RPM names)

```sh
sudo dnf install libX11-devel libXcursor-devel libXrandr-devel \
                 libXinerama-devel libXi-devel mesa-libGL-devel alsa-lib-devel
```

**Windows** (MinGW-w64 or MSVC): no extra system libraries — GLFW uses
Win32 and OpenGL via the SDK. From an "x64 Native Tools" or MSYS2 UCRT64
shell:

```sh
cmake -S . -B build -DBUILD_GAME=ON && cmake --build build
```

**macOS**: install the Xcode command-line tools
(`xcode-select --install`), then the same `cmake -S . -B build
-DBUILD_GAME=ON && cmake --build build`.

**Fedora / Bazzite via Distrobox** — on immutable distros, build inside a
Fedora container with the headers installed once:

```sh
distrobox create --name bf-build --image fedora:40
distrobox enter bf-build
sudo dnf install -y gcc-c++ cmake git \
     libX11-devel libXcursor-devel libXrandr-devel libXinerama-devel \
     libXi-devel mesa-libGL-devel alsa-lib-devel
cmake -S . -B build -DBUILD_GAME=ON && cmake --build build -j"$(nproc)"
```

More detail (including runtime controls) in
[docs/viewer.md](docs/viewer.md).

## Run a battle

```sh
# 250v250 on the open field, full report on stdout
./build/battle_cli --scenario default --seed 42

# JSON state export (the determinism guarantee: same seed, same bytes)
./build/battle_cli --scenario default --seed 42 --export-json out.json

# Event log, CSV timeline, markdown + JSON report, and a chart PNG
./build/battle_cli --scenario default --seed 42 \
    --events events.jsonl --export-csv timeline.csv \
    --report report.md --report-png report.png

# Record a battle and verify a replay of it
./build/battle_cli --scenario default --seed 42 --record run.replay
./build/battle_cli --replay run.replay --verify

# Headless renders and an ASCII map
./build/battle_cli --scenario demo-river --seed 42 \
    --render-frames 100,200,300 --out-prefix frame
./build/battle_cli --scenario demo-river --seed 42 --ascii 250

# Benchmarks (the report prints ticks/s and ms/tick)
./build/battle_cli --scenario perf-5000 --ticks 600
./build/battle_cli --scenario perf-20000 --ticks 600

# Deterministic multithreading: perception + decision on a worker
# pool; output is byte-identical for every thread count (m13 item 4)
./build/battle_cli --scenario perf-5000 --ticks 600 --threads 4
./build/battle_cli --scenario perf-5000 --ticks 600 --threads 0  # = hw cores

# Per-phase timing (perception / decision / resolution / commit; the
# battle itself is unchanged by profiling)
./build/battle_cli --scenario perf-5000 --ticks 600 --profile-phases
```

Shipped scenarios (see `data/scenarios/`): `default`, `mixed`,
`viewer-demo`, `squads`, `demo-river`, `m10-line`, the earth maps
`earth-lebanon`, `earth-alps-innsbruck`, `earth-nile-delta`, and the
`perf-*` sizes (5,000 / 20,000 units) used for benchmarks. Battles are
data-driven: unit types, personalities, morale, tactics and scenarios are
JSON files under `data/` (`data/README.md` documents the formats), so
battles change without recompiling.

## Maps and mods

- Terrain maps are palette-exact PNGs; each color is a terrain type with
  movement cost, sight and defense modifiers, and passability
  (`data/terrain.json`). See [docs/terrain.md](docs/terrain.md).
- The `tools/mapgen/` Python package generates maps from real elevation
  and hydrology data (needs `numpy` and `pillow`; tests run offline):

  ```sh
  python3 -m unittest discover -s tools/mapgen
  ```

  See [docs/mapgen.md](docs/mapgen.md).
- Mods override data files (unit types, terrain, scenarios) from a single
  directory: `--mod data/mods/example`. See `data/mods/example/`.

## Data source credits

The earth scenarios are built from public geodata — NOAA NCEI ETOPO 2022
elevation and Natural Earth rivers/lakes — with exact versions, checksums
and download URLs listed in
[docs/DATA_SOURCES.md](docs/DATA_SOURCES.md).

## Status — read this first

- **The raylib window (`battle_game`) is unverified.** It has never been
  run by the maintainers, and per `docs/viewer.md` it has never even been
  compiled in the development environment (which lacked the X11/GL
  headers); the CI `game-build` job is the first place it gets compiled.
  Treat it as a prototype. Everything else — the engine, the CLI, the
  headless PNG/ASCII rendering, events, reports and replay — is exercised
  by the test suite.
- **This project was developed with AI coding agents**, milestone by
  milestone, and verified by its test suite (determinism, byte-identical
  baselines, per-feature behavior tests) rather than by traditional
  manual review. Bugs are expected; the determinism guarantees make them
  reproducible.
- Known rough edges are documented in the milestone reports. The M10
  swapped-orientation stall on the earth maps was root-caused and fixed
  in M11a (a no-op objective swap plus formation members that never
  navigated terrain; `--nav v1` still reproduces the old behavior
  byte-for-byte — see [docs/terrain.md](docs/terrain.md) and
  [docs/milestone-11.md](docs/milestone-11.md)).

## Known issues

Measured on main (M13 item 0 state check; full method and tables in
[docs/milestone-13.md](docs/milestone-13.md)):

- **Hold on open ground still loses (measured again under combat depth,
  m13 item 3).** The m13 defensive stance (a Hold-ordered squad with
  cohesion > 0.6 takes x0.8 incoming damage, +20% morale-loss resistance
  and the charge reduction, `--combat v2`) now genuinely engages — 350-980
  hits absorbed per probe battle (an action-keyed variant absorbed 3 of
  thousands; the stance now follows the Hold *order*) — and the holder
  stops routing wholesale: battles end as stalemates at ~800-1100 ticks
  with 150-220 holders alive. But the holder still loses every one: the
  unordered enemy never commits (action counts in a probe: ~88k search
  and ~56k approach ticks vs ~2k attack ticks — a hovering, sniping
  ring), so the blob grinds down 30-94 dead against 0-17 while every win
  condition (rout, stalemate alive-count, tick limit) favors the side
  that keeps its distance. Three data-tuning rounds over the stance
  values (0.8/0.2, 0.55/0.4, 0.65/0.3 damage/resist) all produced 0/10
  with a constant ~7:1 loss ratio — stronger stances only prolong the
  grind. AttackMove (0.580 win share, paired +0.080 vs control) and
  flank (0.528, +0.058) remain significantly beneficial; retreat stays a
  losing order (0.000). Full evidence: docs/milestone-13.md item 3.
- **A mass-routed side does not end the battle.** The routed-faction end
  condition waits for a full route; battles where one side is >90% routed
  but ~20 units stay active run to the tick limit (the study's hold arm
  averaged 1463 of 1500 ticks). The M13 termination v2 work
  (`--end v2`) ends a battle when a faction's active units stay below
  max(5, 5% of initial) for 60 ticks.
- **The M10 regroup term lifts an unavailable Advance to 0.0**
  (`clamp(-1 + 0, 0, 1)` while enemies are visible). Harmless without
  orders (0.0 never wins a fair score) and documented in the m12 audit;
  `--ai-avail v2` removes it (unavailable actions stay excluded from
  selection).
- **The raylib window is compile-verified only** (CI `game-build`); it has
  never been run by the maintainers. The UI screens' window wiring is new
  (M13 item 1) — the hand checklist is in
  [docs/viewer.md](docs/viewer.md). Known window gaps: no battle->menu
  navigation yet, and a window resize resets the UI widget values.

## Documentation

| File | Contents |
| --- | --- |
| [docs/architecture.md](docs/architecture.md) | Modules and the tick pipeline |
| [docs/ai.md](docs/ai.md) | The utility AI, morale, M10 tactics |
| [docs/determinism.md](docs/determinism.md) | The determinism rules |
| [docs/terrain.md](docs/terrain.md) | Maps, movement, line of sight |
| [docs/mapgen.md](docs/mapgen.md) | Generating maps from geodata |
| [docs/squads.md](docs/squads.md) | Squads, commanders, formations |
| [docs/morale.md](docs/morale.md) | Morale, routing, recovery |
| [docs/reports.md](docs/reports.md) | Events, timelines, reports, replay |
| [docs/viewer.md](docs/viewer.md) | The window and headless rendering |
| [docs/DATA_SOURCES.md](docs/DATA_SOURCES.md) | Geodata credits and checksums |
| `docs/milestone-*.md` | Per-milestone design reports |

## License

MIT — see [LICENSE](LICENSE). Third-party licenses (nlohmann/json,
GoogleTest, stb, raylib) are reproduced in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Packaging (M13 item 7)

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cpack --config build/CPackConfig.cmake   # TGZ + ZIP with bin/, data/, docs/
```

The archive name carries the sim version (`battle-simulator-13.1-...`).
`cmake --install` lays out the same tree. Pushing a `v*` tag runs the
Release workflow: archives built on Linux, macOS and Windows, attached
to the GitHub release. CI builds and tests on all three platforms
(Windows: compile + smoke — see `.github/workflows/ci.yml`).
