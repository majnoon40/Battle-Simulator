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
