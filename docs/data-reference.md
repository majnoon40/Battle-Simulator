# Data reference — unit types, scenarios, and army budgets

Every data file the simulator loads, the exact field contracts, and the
validation rules behind them. All parsers reject unknown fields and
invalid values with descriptive errors and never silently default a
present field; only the fields marked optional may be absent.

## Unit type tables (`--config`, default `data/units.json`)

A JSON array of type objects. Two tables ship:

- `data/units.json` — the legacy M1-M13 stats (never edited; every
  measured number in the milestone docs is pinned to it).
- `data/units-balanced.json` — the m13 item H(b) balanced stats with
  `cost` values (see "Balance and budgets" below).

| field | type | rules |
| --- | --- | --- |
| `id` | string | non-empty, unique in the table |
| `health` | number | > 0 |
| `damage` | number | > 0 |
| `speed` | number | > 0 (world units per tick) |
| `range` | number | > 0; attack range, strictly below `sight_radius` |
| `cooldown` | integer | >= 1 ticks between attacks |
| `sight_radius` | number | > `range` (a unit always sees what it can hit) |
| `leadership_radius` | number, optional | > 0; absent = cannot command |
| `accuracy` | number, optional | [0, 1], default 1.0 |
| `damage_variance` | number, optional | [0, 1], default 0.0 |
| `cooldown_jitter` | number, optional | [0, 1], default 0.0 |
| `cost` | number, optional | > 0, default 1.0 — the army-building value (item H(b)) |

## Scenarios (`--scenario`, default `data/scenarios/default.json`)

```json
{
  "battlefield": {"width": 1000.0, "height": 1000.0},
  "spawn_band_fraction": 0.15,
  "armies": {"red": ..., "blue": ...},
  "objectives": {"red": {"x": 900.0, "y": 500.0}, "blue": {...}},
  "squads": {"red": {"count": 10, "size": 25}, "blue": {...}},
  "map": {"file": "maps/lebanon.png", "cell_size": 2.0},
  "spawn_pattern": "band"
}
```

`battlefield`, `spawn_band_fraction` (in (0, 0.5]) and `armies` are
required; `objectives`, `squads`, `map` and the spawn patterns
(band / line / columns / echelon / cluster) are optional. A `map`
overrides the battlefield size with map size x `cell_size`.

### Army forms

An army is either **count-form** or **budget-form** — never both.

Count-form (the classic shape, all shipped scenarios except the
balanced demo):

```json
{"infantry": 250, "archer": [10, 30]}
```

Each entry is `type: count` (>= 1) or `type: [min, max]` for a per-seed
draw (counter RNG channel 4; validation uses the maximum).

Budget-form (item H(b)) — build the army from a points budget:

```json
{"budget": 300, "mix": {"commander": 1, "infantry": 5, "archer": 2, "cavalry": 2}}
```

- `budget` > 0 points; `mix` maps type ids to integer weights >= 1.
- The points split proportionally to the weights; a unit of a type
  costs that type's `cost`.
- The fill is deterministic (never seed-dependent): counts start at
  the floor of each share, every mix type fields at least one unit,
  and leftover points go one unit at a time to the largest affordable
  fractional remainder (ties to the lower mix index).
- A budget that cannot field one unit of every mix type, or a fill
  above the 20,000-unit cap, is rejected.

## Balance and budgets (m13 item H(b))

`data/units-balanced.json` retunes the three troop types for a
rock-paper-scissors tendency (measurement protocol: 100 paired seeds x
2 orientations per tuning round, `tools/analysis/m13_balance_rps.py`),
with `cost` expressing army-building value: infantry 1.0, archer 1.4,
cavalry 2.4, commander 3.0. The legacy `data/units.json` is untouched.

The balanced demo — two different 300-point armies, fair by cost:

```
battle_cli --config data/units-balanced.json \
           --scenario data/scenarios/balanced-demo.json
```

Both sides spend the same budget on different mixes (red is
infantry-archer weighted, blue cavalry weighted), so the fill
re-equalizes from the current costs. The demo fields no squads:
budget armies are size-elastic (the fill follows the costs) while
squad plans pin exact soldier counts, so the two forms do not
compose.

## Counters (m14 item 0)

`data/combat.json` carries an optional `counters` object: a
`multiplier_cap` (default 3.0, validated in [1, 8]) and a `list` of
`{"attacker": type, "defender": type, "multiplier": m}` entries
(validated in [0.25, 4]; unlisted pairs fight at x1; duplicate pairs are
rejected; the type names are cross-checked against the unit-type table
when the battle is built). Under `--combat v2` the attacker's damage
into the listed defender type is multiplied by `m`, and the product of
all attacker-side multipliers (charge x facing x fatigue x aura x
counter) is capped at `multiplier_cap` — the shipped 4.0 never binds
without counters (the pre-counters maximum was 1.5 x 1.5 x 1.05 =
2.3625), so every pre-counters v2 battle is bit-identical. `--combat
v1` ignores counters entirely.

The shipped table (tuned against `data/units-balanced.json`, equal
600-point 19:1 body:commander armies, squad-less, 100 paired seeds x 2
orientations per round, `tools/analysis/m14_counters_rps.py`):

| attacker | defender | multiplier |
| --- | --- | --- |
| cavalry | archer | 0.90 |
| archer | cavalry | 1.15 |
| archer | infantry | 2.15 |
| infantry | archer | 0.95 |
| infantry | cavalry | 0.50 |
| cavalry | infantry | 2.25 |

Measured win shares (round 3 of 3): cavalry > archer 0.640
[0.571, 0.703], archer > infantry 0.695 [0.628, 0.755], infantry >
cavalry 0.585 [0.516, 0.651] — every Wilson lower bound above 0.5, no
matchup above 0.70. The baseline without counters was 0.800 / 0.070 /
1.000: the multipliers pull in both directions, not only up.
