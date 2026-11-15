# autobattle

A deterministic 2D autonomous battle simulation written in C++20. Two or
more armies fight on a 2D battlefield where every soldier decides and acts on
its own; the player observes battles rather than micromanaging units.

Current state: **Milestone 10 — tactical depth, variety, battle reports
and replay** (headless, up to 20,000 units, sight-limited perception,
personalities, morale with routing and recovery, squads with formations,
commander orders and succession, a GUI viewer with headless PNG/ascii
rendering, data-driven terrain maps with flow-field navigation,
whole-file mods, an offline mapgen that turns NOAA ETOPO 2022 elevation
plus Natural Earth rivers and lakes into battle maps — Lebanon, the Alps
and the Nile delta are committed — and, with `--variance on` (default):
per-type combat accuracy and damage variance from an order-independent
counter RNG, personality-staggered decision intervals, personal memory
with squad intel sharing, and the Search / Help / Flank actions with
bounded pursuit and regroup; `--variance off` reproduces every M1-M9
reference byte-for-byte). See `docs/ai.md` for the AI and the M10
tactics, `docs/mapgen.md` for the generator and `docs/DATA_SOURCES.md`
for the pinned data, `docs/terrain.md` for terrain, maps and mods,
`docs/viewer.md` for the viewer (build, run, controls, required dev
libraries per OS), `docs/squads.md` for the squad design,
`docs/morale.md` for the morale design, `docs/milestone-10.md` for scope
and results, and `docs/milestone-1.md` through `docs/milestone-9.md` for
the earlier baselines.

## Priorities

Emergent behavior, deterministic simulation, performance, explainable AI,
modular architecture, testable behavior, data-driven configuration.

## Layout

```
project/
├── CMakeLists.txt      Root build; pins dependencies, FP flags, BUILD_GAME
├── sim/                Static simulation library (no I/O, no rendering)
├── render/             Reserved for the future renderer (not part of M1)
├── game/               Reserved for the optional raylib app (not part of M1)
├── sim/terrain          Terrain types, grid, rules; flow-field navigation
├── tools/mapgen         Offline real-Earth map generator (ETOPO + Natural Earth)
├── tools/analysis       balance.cpp, profiling and experiment tools
├── render/             Pure DrawList + software rasterizer + PNG writer
├── game/               game_core (pure viewer logic) + battle_game window
├── cli/                battle_cli headless executable
├── tests/              GoogleTest suite
├── data/               JSON gameplay configuration (unit types, scenarios)
├── docs/               Architecture and determinism notes
├── tools/              Analysis scripts (faction-balance sweeps)
├── logs/               Complete command logs from verification runs
└── output/             Generated artifacts (JSON exports)
```

## Requirements

- C++20 compiler (GCC 14 tested)
- CMake ≥ 3.16
- Internet access at configure time (dependencies are fetched via
  FetchContent and pinned to exact Git tags):

| Dependency   | Version | Used by            |
| ------------ | ------- | ------------------ |
| nlohmann/json | v3.11.3 | sim (JSON export)  |
| GoogleTest   | v1.14.0 | tests              |

raylib is *not* fetched in M1: building `sim`, `battle_cli` and `tests`
requires no raylib, X11, OpenGL or display libraries (`-DBUILD_GAME=OFF` is
the default; the graphical application arrives in a later milestone).

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_GAME=OFF
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

Floating-point determinism flags (`-fno-fast-math -ffp-contract=off`) are
applied to every GCC/Clang build. Use the same Release configuration for any
determinism comparison.

## Usage

```bash
./build/battle_cli --help
./build/battle_cli --seed 42 --units 500 --ticks 3000 --export-json output/final.json
./build/battle_cli --seed 42 --scenario data/scenarios/mixed.json
# Benchmark recipe (--ticks 600, large armies; report prints ticks actually
# run, ticks/s and ms/tick):
./build/battle_cli --scenario data/scenarios/perf-5000.json --ticks 600
./build/battle_cli --scenario data/scenarios/perf-20000.json --ticks 600
```

Battles are data-driven: unit types come from `data/units.json` (infantry,
archer, cavalry — field reference in `data/README.md`) and the battlefield,
spawn bands and per-faction army composition from a scenario file
(`data/scenarios/default.json` by default, `mixed.json` for a 100/100/50
army, `perf-5000.json`/`perf-20000.json` for benchmarks). `--config`/
`--scenario` load other files; invalid data (missing fields, zero/negative
values, duplicate ids, unknown types, missing files, malformed JSON,
over-cap totals) fails with a descriptive error and nothing is silently
defaulted. Editing the JSON changes battles without recompiling.

`--units N` keeps the M1 army split: an infantry-only split of N units (Red
receives the extra unit when odd; 2 ≤ N ≤ 20,000); it cannot be combined
with `--scenario`. Nearest-enemy search for legacy targeting uses the M3
spatial hash grid by default; `--neighbor-mode brute` selects the M1/M2 scan
(identical results, kept as a test oracle and baseline), and `--grid-cell`
tunes the grid cell size (default 16.0). The AI is the M4 utility model by
default: units see only within their `sight_radius`, carry hash-derived
personalities (`data/personalities.json`) and score five actions
(`docs/ai.md`); `--ai-mode legacy` reproduces every M1-M3 reference result,
`--personalities` loads another archetype table, and `--ai-debug <unit id>`
(+ `--ai-debug-tick N`) prints one unit's perception, morale, action scores
and choice. Morale and routing (M5) are on by default (`docs/morale.md`,
configured by `data/morale.json`): units break, flee and can recover, and a
faction with no active units for 30 ticks ends the battle by FACTION_ROUTED;
`--morale off` makes utility battles bit-identical to their M4 runs. The
battle ends when a faction has no surviving units, when it has no active
units for the rout timeout, or at the tick limit. The CLI prints winner,
survivors, active counts, routing statistics, per-type statistics,
termination reason, timing, throughput and peak memory, and optionally
writes a deterministic JSON final-state export (format v4 with morale on:
per-unit morale/state/routed and per-faction routing statistics; v3
otherwise).

## Documentation

- `docs/architecture.md` — modules and the fixed tick pipeline
- `docs/determinism.md` — how reproducibility is guaranteed and verified
- `docs/ai.md` — the utility AI: perception, personalities, score formulas
- `docs/morale.md` — morale, routing, recovery and FACTION_ROUTED
- `docs/milestone-1.md` — M1 scope, decisions and verification results
- `docs/milestone-2.md` — M2 scope, decisions and verification results
- `docs/milestone-3.md` — M3 scope, decisions and verification results
- `docs/milestone-4.md` — M4 scope, decisions and verification results
- `docs/milestone-5.md` — M5 scope, decisions and verification results
- `data/README.md` — unit type and scenario file formats
