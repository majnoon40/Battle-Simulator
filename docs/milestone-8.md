# Milestone 8 — Terrain, navigation and mods

## Scope

Terrain that affects movement, perception and combat; data-driven maps
(PNG with exact palette colors, or JSON grids); flow-field navigation
with stuck recovery; whole-file mods. Every terrain rule lives in one
module (`sim/terrain.cpp` — the TerrainGrid query methods); PNG decoding
lives in cli/ (and game/), sim/ receives only a TerrainGrid. No map means
no terrain rule fires at all and every M1-M7 reference reproduces
byte-for-byte (pinned in tests by the M7 export baselines under
`tests/baselines/m8/`). Design doc: `docs/terrain.md`.

## Implementation summary

- **sim/terrain**: `TerrainTypeTable` (id, palette color, move_cost as a
  speed multiplier, passable, sight_modifier, defense_modifier),
  `TerrainGrid` (O(1) cell lookup; out of bounds is impassable),
  `parse_terrain_types` and `parse_terrain_grid_json` with readable
  rejections (duplicate ids/colors, bad values, ragged grids, unknown
  types), `build_terrain_grid` size/index validation.
- **sim/flow_field**: cached Dijkstra cost field per faction from the
  objective over 1/move_cost edges, no corner cutting; the preferred step
  per cell is the cheapest neighbor (acyclic by the Dijkstra parent
  argument; deterministic fixed-order ties). Computed at start and
  recomputed when an objective moves to a different cell.
- **Simulation integration**: wall-sliding move commit scaled by the
  entered cell's multiplier (full step, x-only, y-only, stay — no unit
  ever ends inside impassable terrain); Advance follows the flow field
  with a map; stuck detection (epsilon 0.05 world units, N = 20 ticks,
  M = 60 flow-follow ticks, goal-seekers only); sight radius scaled by
  the observer's cell (capped at 2.0x); damage scaled by the target
  cell's defense; formation slots nudged off impassable cells
  (deterministic ring scan); kills counted by the victim's terrain;
  constructor validation (size match, spawn bands passable and reachable,
  objectives on passable ground); spawn positions resampled off
  impassable cells.
- **Export v6** (map battles only): `config.map` {id, width, height,
  cell_size} and `statistics.terrain_kills` + stuck statistics. Format
  stays 3/4/5 without a map.
- **CLI**: `cli/map_load.cpp` (PNG via the pinned stb_image — exact RGB
  match, errors name the pixel — or JSON grids), `cli/mods.cpp`
  (whole-mod validation, overlay resolution), `--mod <dir>`, scenario
  names resolve through mod `scenarios/`, map files through mod `maps/`;
  the battle report prints a terrain section (map, stuck stats, kills by
  terrain).
- **render/**: terrain layer (run-length merged cell rows — a 200x200 map
  is a few hundred rects), drawn by both backends, `ViewOptions::
  show_terrain` toggle, window key **T**. The window's sim rate is now
  60 ticks/s at 1x.
- **Data**: `data/terrain.json` (grassland, forest, hills, water, road,
  mountain, obstacle, ford), `data/maps/demo-river.png` (200x200 cells,
  cell_size 5, generated deterministically by
  `scripts/gen_demo_river.py`: wavy river, road bridge at y=500, ford at
  y=750, forest patches, hills, mountains), scenarios `demo-river`
  (2,080 mixed units, 80 squads) and `demo-river-perf` (5,000 units, 200
  squads), and the example mod `data/mods/example/` (infantry health
  120, swamp terrain type, swampy JSON map, swamp-war scenario).

## Notable bug found by the acceptance probes

The first flow-field direction rule (cost + enter-cost) double-counted
slow cells and could produce two-cell direction cycles near the ford —
a unit froze against the river bank for 1,000+ ticks. Fixed to the
cheapest-neighbor rule; the frozen case now crosses via the ford
(scripts/m8_river_probe.cpp demonstrates it).

## Verification (2026-11-14)

**266/266 GoogleTest tests pass** (244 M1-M7 + 22 new M8): terrain table
parsing/validation; grid lookup, bounds and every rule; build validation;
flow field around a wall (descent reaches the target, never enters
impassable cells), sealed pockets, impassable targets; forest slows a
crossing (24 vs 40 units in 20 ticks); impassable blocks and wall
sliding; **the invariant: over 1,000-tick random battles (3 seeds, 400
units, messy maps) no unit ever stands inside an impassable cell**;
forest hides an observer (visible_enemies 1 vs 0 at distance 30);
hills absorb damage (9 vs 12 per hit); load-time rejections (no passable
spawn cells, unreachable objective, explicit unit on impassable ground,
size mismatch); formation slots always on passable ground (30-tick
squad battle next to water); Advance crossing a wall through a gap (flow
field + stuck recovery); PNG map loading round-trip, unknown-color and
ragged-grid errors, the shipped demo map loads with a road bridge; mod
loading (example mod changes infantry to 120 hp and loads the swamp map
without recompiling, invalid mod rejected wholesale with the file named,
partial mod works, missing mod/map rejected); **no-map pins: the M7
export baselines for seeds 42, 123, 5, 0, 2^64-1 (default), mixed s42
and viewer-demo s42 reproduce byte-for-byte**; terrain double-run
determinism and grid == brute with terrain; DrawList terrain layer
counts, determinism and toggle.

## Acceptance results

**Controlled terrain effects** (see `logs/m8-river-probe.log` and the
tests): river crossing on demo-river — a unit starting on the bridge row
crosses in 126 ticks; a unit starting beside the ford crosses in 89 ticks
(through the ford); midway starts detour to the bridge (150 ticks) and
even a ford-row start 250 units west prefers the bridge detour (197
ticks) — the flow field trades the slow ford (0.35x) against the longer
bridge route. Forest vs grassland speed: 20 ticks at speed 2 covers ~40
units on grassland vs ~24 in forest. Hill defense: a hit deals 12 on
grassland vs 9 on hills (0.75x).

**100 seeds on demo-river** (`logs/m8-100seeds.log`): 38 battles end by
faction_routed (end tick mean 464, sd 143, range 378–1272), 62 grind to
the 3,000-tick limit — the river funnel makes battles long; deaths mean
162 (10–938). Blue wins 75 vs Red 25: the demo map is deliberately
scenic, not mirror-symmetric (the sine-shaped river and the
hills/mountain placement affect the two approach corridors differently),
and that asymmetry favors Blue's side; a symmetric map would remove the
skew. **Stuck statistics: the longest stretch any unit made less than
epsilon progress is 20 ticks (the detection threshold itself — recovery
always engaged); across all 100 battles x 2,080 units, zero units were
ever stuck for more than 100 ticks.**

**Performance** (Release, 2 vCPU): the demo map at 5,000 units
(demo-river-perf) runs at **66.6 t/s (15.0 ms/tick)** — above the 60 t/s
requirement; the 2,080-unit demo-river scenario runs at 274 t/s (3.6
ms/tick) vs 305 t/s for the same battle without terrain. gprof
(`logs/m8-gprof-flat.log`): radius_query 76%, decide_unit_action 18%,
steer_in_formation 2%; **all terrain code combined is ~1.3%**
(effective_sight 0.62%, commit_move_terrain 0.41%, flow fields 0.2%,
computed twice — once per faction — at battle start). The 5k slowdown
versus open-field battles is the battle's shape (units mass at the
bridge, making radius queries denser), not terrain bookkeeping.

**PNG renders** of demo-river (seed 42) at ticks 0, 150, 300:
`download/m8-frames/demo-river-0000*.png` (each ~52-55 ms to render),
verified programmatically: terrain layer present (grass ~30-35%, river
1%, forest 2%, hills 1%, the road bridge visible at ticks 0/150 and
covered by the bridge melee at tick 300 — 557 Red vs 121 Blue unit
pixels, Red pressing that seed).

## Notes

- The viewer window remains UNVERIFIED (no display / raylib deps here,
  unchanged since M7); the new terrain layer and the T toggle are part
  of the same DrawList both backends consume and are tested headlessly.
- STOPPED after M8. M9 was not started.
