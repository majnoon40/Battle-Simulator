# data/ — gameplay configuration (M2/M4/M5)

All gameplay numbers live here, loaded at runtime; nothing needs recompiling
to rebalance a battle.

## units.json — unit type table

A JSON array of unit type objects. Every field is required and must be a
strictly positive number (`cooldown` a whole number of ticks ≥ 1); unknown
fields, duplicate ids, zero/negative values and empty ids are rejected with a
descriptive error.

| Field          | Meaning                                            |
| -------------- | -------------------------------------------------- |
| `id`           | stable type identifier used by scenarios           |
| `health`       | health at spawn                                    |
| `damage`       | health removed per attack                          |
| `speed`        | world units moved per tick                         |
| `range`        | maximum attack distance (inclusive)                |
| `cooldown`     | ticks between two attacks                          |
| `sight_radius` | perception radius (M4, inclusive); must strictly   |
|                | exceed `range`                                     |

`infantry` equals the M1 default stats (health 100, damage 12, speed 2.0,
range 2.0, cooldown 5, sight radius 40) so default runs reproduce M1 results
exactly. Shipped sight radii: infantry 40, archer 60, cavalry 50. The sight
radius only affects utility-mode battles (`--ai-mode legacy` ignores it).

## personalities.json — personality archetypes (M4)

```json
{
  "archetypes": [
    {"id": "brave",
     "traits": {"bravery": [0.75, 1.0], "caution": [0.0, 0.25],
                "aggression": [0.5, 1.0]},
     "spawn_weight": 1.0}
  ]
}
```

- `id` — non-empty, unique archetype name.
- `traits.bravery/caution/aggression` — `[min, max]` ranges with
  0 ≤ min ≤ max ≤ 1; a unit draws each trait uniformly from its range.
- `spawn_weight` — relative probability of the archetype (≥ 0; the sum must
  be positive; 0-weight archetypes never spawn).

Each unit's archetype and traits are drawn from a hash of (seed, unit id) —
never from the spawn RNG — so spawn positions are identical in every AI
mode. The file is validated like the other data files; unknown fields are
rejected. `--personalities <file>` loads an alternative table.

## morale.json — morale and routing (M5)

Every field is required (no silent defaults); unknown fields and invalid
values are rejected. The full rule set lives in `docs/morale.md`.

| Section | Fields | Meaning |
| ------- | ------ | ------- |
| `start` | `base`, `bravery_scale` | starting morale = base + bravery_scale × bravery |
| `states` | `confident_min`, `steady_min`, `worried_min`, `shaken_min` | state thresholds; strictly ordered; below `shaken_min` a unit breaks |
| `loss` | `ally_death`, `ally_rout`, `routed_ally_presence_per_tick`, `outnumbered_per_tick`, `damage_per_health_fraction`, `isolation_per_tick`, `caution_amplifier`, `bravery_shield`, `min_scale` | loss rates (personality-scaled); rout contagion |
| `gain` | `kill`, `superiority_per_tick`, `allies_present_per_tick`, `safe_per_tick` | gain rates |
| `thresholds` | `outnumbered_below`, `superiority_above` | strength-ratio thresholds (ratio = (allies+1)/(allies+enemies+1)) |
| `recovery` | `morale_threshold`, `safe_ticks` | recovery requires morale strictly above the threshold (which must exceed `shaken_min` — the hysteresis) after the safe time |
| `ai` | `confident_bonus`, `worried_penalty`, `shaken_penalty` | score modifiers per state |
| — | `max_loss_per_tick`, `max_gain_per_tick` | per-tick bounds on the total morale change |
| — | `rout_timeout_ticks` | a faction with no active units for this many consecutive ticks loses |

`--morale on|off` toggles the system (default on; utility mode only —
legacy mode ignores it); `--morale-config <file>` loads an alternative
configuration.

## scenarios/*.json — battle scenarios

```json
{
  "battlefield": {"width": 1000.0, "height": 1000.0},
  "spawn_band_fraction": 0.15,
  "armies": {"red": {"infantry": 250}, "blue": {"infantry": 250}},
  "objectives": {"red": {"x": 500.0, "y": 500.0}}
}
```

- `battlefield.width/height` — positive field size.
- `spawn_band_fraction` — each faction spawns in a side band of
  `width * fraction`; must be in (0, 0.5].
- `armies.red/blue` — non-empty objects of `type id -> count`; counts are
  whole numbers ≥ 1, referenced types must exist in units.json, and the
  scenario total may not exceed the 100,000-unit cap (M15 S5; it was
  20,000 from M3 through M14).
- `objectives.red/blue` — optional per-faction objective points (M4): where
  utility units advance when no enemy is visible. Both coordinates are
  required, must be ≥ 0 and lie on the battlefield, and each faction may be
  omitted (default: the center of the opposing spawn band). Ignored by the
  legacy AI.

Spawn order: the Red army spawns first, then Blue; within an army, types
spawn in file order. Composition order only affects which id gets which
type — every unit draws its position from the seeded RNG stream exactly like
M1.

- `default.json` — M1 geometry and armies (250 infantry per faction).
- `mixed.json` — 100 infantry, 100 archers, 50 cavalry per faction.
- `perf-5000.json` — 1000×1000 field, 2,500 infantry per faction
  (benchmarks).
- `perf-20000.json` — 2500×2500 field, 10,000 infantry per faction
  (benchmarks).
- `perf-100000.json` — 5600×5600 field, 50,000 infantry per faction (M15
  S5 benchmarks; 5600 = 2500·√5 rounded, so the spawn-band density matches
  perf-20000 and the ladder measures scale, not crowding).

File reading happens in the CLI only (`battle_cli --config`, `--scenario`,
`--personalities`); the `sim` library parses text or streams and never
touches the filesystem.
