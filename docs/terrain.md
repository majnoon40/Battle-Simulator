# Terrain, navigation and mods (M8)

Terrain affects movement, perception and combat; maps are data (PNG or
JSON); mods replace data files wholesale. The simulation itself gained no
new behavior on the open field: every M1-M7 reference reproduces
byte-for-byte when a scenario has no `map`.

## data/terrain.json

One type per entry; `color` is the exact palette for map files.

| field | meaning |
| --- | --- |
| `id` | type name, used by JSON map grids |
| `color` | `#RRGGBB`; map pixels must match exactly (duplicates rejected) |
| `move_cost` | **speed multiplier**: a unit crossing the cell moves at `speed x move_cost` (road 1.3 = faster, forest 0.6 = slower); ignored when `passable` is false |
| `passable` | false = units can never stand on it (water, obstacle) |
| `sight_modifier` | multiplies an observer's sight radius while it stands in this cell |
| `defense_modifier` | multiplies the damage a unit takes while it stands in this cell |

The shipped types: grassland (all 1.0), forest (0.6 / sight 0.65 /
defense 0.85), hills (0.5 / 1.15 / 0.75), water (impassable), road (1.3 /
1.0 / 1.05), mountain (0.15 / 1.25 / 0.7), obstacle (impassable) and
ford (0.35 / 1.0 / 1.1 — a slow but passable river crossing).

## Maps

A scenario names a map; the battlefield size becomes
`map size x cell_size`:

```json
"map": {"file": "demo-river.png", "cell_size": 5.0}
```

- **PNG** (`data/maps/*.png`): every pixel must be an exact RGB match of a
  terrain type color; an unknown color is an error naming the pixel. PNG
  decoding lives in cli/ (`cli/map_load.cpp`, pinned stb_image) and
  game/, never in sim/ — the simulation only receives a `TerrainGrid`.
- **JSON grid**: `{"id": "...", "grid": [["grassland", "water", ...], ...]}`
  — rows of type ids, same validation (unknown types, ragged rows).

Load-time validation (readable errors, nothing silently defaulted):
battlefield/map size mismatch, spawn bands with no passable cells, spawn
band cells unreachable from the faction objective, objectives on
impassable ground, units starting inside impassable terrain, duplicate
terrain ids or colors, bad sizes.

## Simulation rules (all in sim/terrain.cpp, one place)

- **Movement**: each pending step is scaled by the speed multiplier of the
  cell it enters, then wall-slides: the full step, the x axis alone, the
  y axis alone, else the unit stays. No unit ever ends a tick inside an
  impassable cell (tested over 1,000-tick random battles).
- **Navigation**: one cached flow field per faction — a Dijkstra cost
  field from the faction objective over `1/move_cost` edge weights, no
  corner cutting — computed at battle start and recomputed whenever the
  objective moves to a different cell. The **Advance** action follows the
  field when a map is present (straight line otherwise). Approach and
  Retreat keep steering plus wall sliding.
- **Stuck detection**: a goal-seeking unit (Approach, Advance, formation
  steering) that actually moves less than `kStuckEpsilon = 0.05` world
  units while intending more, for `N = 20` consecutive ticks, follows its
  faction's flow field for `M = 60` ticks, then resumes normal steering.
  Retreating/fleeing units never follow the field (it points at the enemy
  objective). The constants live in `Simulation` (simulation.hpp).
- **Perception**: an observer's sight radius is multiplied by the sight
  modifier of its own cell, clamped by `terrain_sight_max_scale` (default
  2.0) so radius queries stay valid.
- **Combat**: damage is multiplied by the defense modifier of the
  target's cell.
- **Formation slots**: a slot that lands on an impassable cell is nudged
  to the nearest passable cell center by a deterministic expanding-ring
  scan (`Simulation::snap_to_passable`).

## Export v6 (only when a map is present)

`format_version` becomes 6 and adds `config.map` (`id`, `width`,
`height`, `cell_size`) and `statistics.terrain_kills` (kills by the
terrain type of the victim's cell, zero entries omitted) plus the stuck
statistics. Map-less exports keep the exact format-3/4/5 bytes.

## Mods

`--mod <dir>` (a path, or a name under `data/mods/`): the directory may
contain `units.json`, `terrain.json`, `personalities.json`,
`morale.json`, `formations.json`, `commanders.json`, a `scenarios/` and a
`maps/` subdirectory. Files replace their built-in counterparts
**wholesale** — no partial merging. The whole mod is validated first:
any invalid file rejects the entire mod with readable errors before
anything loads. Explicit `--config`/`--morale-config`/... paths override
the mod. A `--scenario` argument that is not an existing path is resolved
as a name against the mod's `scenarios/` first, then `data/scenarios/`;
map files resolve the same way (`mod/maps/` then `data/maps/`).

The example mod, `data/mods/example/`, raises infantry health to 120 and
adds the `swamp` terrain type, with a JSON-grid `swampy` map and the
`swamp-war` scenario:

```
./build/battle_cli --mod data/mods/example --scenario swamp-war --seed 7
```

## Rendering

The DrawList gains a terrain layer (run-length merged cell rows in the
palette colors — a 200x200 map collapses to a few hundred rects), drawn
by both backends; toggle it with **T** in the window or
`ViewOptions::show_terrain`. `--render-frame`/`--render-frames` show it
automatically.

## Navigation v2 (m11a)

`--nav v2` (the default) fixes the two root causes of the M10
swapped-orientation stalls, measured on the earth maps. `--nav v1`
reproduces the M10 binary byte-for-byte (verified by replay hash
checkpoints against the M10 build on default, demo-river and
earth-lebanon swapped runs) and exists as the regression gate; a replay
file records its generation (`nav=v1|v2`, files from before m11a parse
as v1).

### The diagnosis (numbers)

With `--swap-sides` every earth battle stalled to the tick limit
(Lebanon 0/0/40, Alps 0/0/20, Nile 0/0/20 at 1500 ticks; M10):

1. **The explicit-objective swap was a no-op.** The CLI's swap loop
   iterated over both factions and swapped the objectives twice — the
   Red iteration swapped them, the Blue iteration swapped them right
   back (the same loop also un-swapped the spawn patterns). With every
   earth scenario setting explicit objectives, a swapped battle sent
   each army to the objective inside its **own** spawn band: it walked
   home and parked. Measured on Lebanon seed 1: all 40 Red commanders
   ended standing on Red's own objective cell (823.75, 501.25), all 40
   Blue commanders on Blue's (253.75, 563.75); zero contact, zero
   deaths, 1040 survivors per side at the tick limit.
2. **Formation members never navigate.** `steer_in_formation` moves a
   member toward its geometric slot (plus cohesion, alignment and
   separation) — the flow field is only followed by units whose action
   resolves through the Advance branch, i.e. commanders and squadless
   units. In terrain corridors the projected slots bunch onto the few
   passable cells and the beeline jams the squad against water or
   valley walls: the trace (swapped Lebanon, broken swap, seed 1)
   showed Blue milling in a 31-unit coastal strip at a mean 1.6 units
   per tick with **zero** net displacement for 1100 ticks, and Red
   parked behind mountain-crawling commanders at 0.036 units per tick
   with 81% of the army moving less than the stuck epsilon. Members'
   moves were not goal-seeking, so the stuck accounting could not even
   see them.
3. **The static geometry is fine.** Every spawn band is fully
   reachable (zero unreached cells on all three maps), but paths funnel
   through 1–3-cell gates: Lebanon west→east sends 74.6% of band cells
   through two adjacent ford cells, east→west 50.7% through one; the
   Alps funnel 100% through a three-cell mountain valley. Path costs
   (flow-field time at speed 1): Lebanon 536–807, Alps ~1791,
   Nile ~388–400 ticks.

### The fix (one coherent design: field-first formation march)

- **The swap happens exactly once** (`cli::apply_side_swap`, extracted
  from the CLI for testability): compositions, squad plans, explicit
  objectives and spawn patterns each swap once. A swapped battle now
  marches each army across the map. On Lebanon seed 1 the same battle
  that stalled for 1500 ticks ends at tick 827 (faction rout).
- **Members far from their slot march with the army.** A member whose
  action is Advance and whose projected slot is farther than
  `kSlotMarchDist` (6 world units) follows the faction flow field —
  terrain-aware, the same field the commander uses — instead of
  beelining to the slot. Near the slot the shape steering takes over
  again, so formations re-form on open ground and columns through
  corridors. The march is goal-seeking, so stuck accounting sees
  members (the m11a acceptance metric is honest).
- **No congestion term.** The stall was steering, not routing: with
  members on the field, squads pass the measured gates at terrain speed
  and the acceptance numbers hold without dynamic field costs (which
  would cost performance and risk determinism). The stuck-recovery
  mechanism is unchanged.

The flag only matters with a terrain map (or `--swap-sides` on a
scenario with explicit objectives); without a map, v1 and v2 are
byte-identical (asserted by a test).

### mapgen `--v2`

`python3 tools/mapgen/mapgen.py --region <r> --v2` writes
`<region>-v2.png`, `<region>-v2.meta.json` and
`earth-<region>-v2.json` (the M9 files stay untouched and regenerate
byte-identically):

- **Balanced spawn bands**: the smallest band fraction whose sides'
  passable shares AND mean move costs agree within 10% (recorded in
  `balanced_band_pick`; `null` when no fraction qualifies — Lebanon's
  coastal strip vs. its mountains is honestly unbalanceable at every
  width, and the meta says so).
- **Axis fords**: a ford patch on every river segment crossed by a
  march axis that has no ford within 12 cells (`axis_ford_cells`,
  `axis_river_crossings` — sea on the axis is not a crossing). On the
  shipped regions the axes cross no unserved river (Lebanon and the
  Alps cross none; the Nile's six crossings all sit within 12 cells of
  an existing ford), so the shipped v2 maps stamp none — the check
  runs on every regeneration.
- **March cost and connectivity**: `march` records the median
  flow-field cost from each band to the objective it marches on, the
  ratio between the two marches, and whether every passable band cell
  reaches both objectives — in BOTH orientations (`connectivity`).

### Verification summary

- `--nav v1` reproduces the M10 binary byte-for-byte (replay hash
  checkpoints equal on default/demo-river/earth-lebanon swapped).
- v2 double runs are byte-identical; grid == brute; the full C++
  suite and the mapgen suite pass (numbers in the milestone report).
- Lebanon swapped under v2 runs 5.5x faster than under v1 (3.8 s vs
  20.9 s median for 1500 ticks: the v1 stall battles churn all units
  through separation queries for the full limit).
- The acceptance sweep (first contact, pre-limit rate, stuck share,
  side-averaged win rate per map and orientation) lives in
  `tools/analysis/nav_acceptance.py`; the per-battle numbers come from
  the report's `first_contact_tick`, `units_stuck_over_100` and
  `longest_stuck_stretch` outcome fields.
