# Milestone 14 — Scale, multi-faction battles, scripted AI generals

Item index: `docs/PROGRESS.md`. This file carries the per-item Part A
(what shipped, the evidence, and the honest misses).

## Item 0 — Counters and a viable defense

Branch `m14-counters` (on top of m13-packaging). One validated data
layer plus three measured tuning rounds; no unit stat changed.

### What shipped

- `data/combat.json` gains an optional `counters` object:
  `multiplier_cap` (default 3.0, validated in [1, 8]) and a `list` of
  `{attacker, defender, multiplier}` entries (validated in [0.25, 4];
  unlisted pairs x1; duplicates rejected; empty ids rejected; unknown
  fields rejected — the house parser rules).
- `sim::parse_combat` parses the section; the type names are
  cross-checked against the battle's unit-type table when the
  `Simulation` is built: an unknown name throws `invalid_argument`
  naming the entry and listing the known types (the parser only sees
  the combat file — the two meet at build time).
- Combat v2 applies the matrix: the attacker's damage into the listed
  defender type is multiplied by the entry, then the product of all
  attacker-side multipliers (charge x facing x fatigue x aura x
  counter) is capped at `multiplier_cap`. Defender-side reductions
  (stance, terrain) stay outside the capped product. The matrix is a
  flat attacker-major array; when no counters are configured it is
  empty and the attack loop skips the layer (`counter_types_ == 0`).
- `--combat v1` ignores counters entirely: v1 byte-identity is pinned
  by an exact-damage test (a x2 counter changes nothing under v1).
- 5 new tests (`CombatCountersTest.*`): parse defaults/values/10
  rejection shapes, the paired-damage scaling (the listed direction
  doubles exactly; the unlisted return direction is unchanged), the
  cap clamping (a x4 counter under cap 2.0 yields exactly x2), v1
  immunity, and the unknown-type build error. The shipped-file test
  additionally verifies every shipped counter name resolves in BOTH
  shipped unit tables (a typo would break every CLI battle at load
  time). 486/486 total.

### The cap never changed a pre-counters battle

The maximum attacker-side product without counters is charge x rear x
aura = 1.5 x 1.5 x 1.05 = 2.3625. The shipped cap is 4.0 — it first
binds only when counters stack onto a rear charge (e.g. cavalry into
infantry rears: 1.5 x 1.5 x 2.25 = 5.06 -> 4.0), which is exactly the
guard the cap exists for. Tested: 2.3625 < 4.0, and every pre-counters
v2 battle is bit-identical (the full existing suite, including the
v1-era baselines and the type-matrix tests, passes unchanged).

### The acceptance: 3 tuning rounds, all reported

Protocol (`tools/analysis/m14_counters_rps.py`): pure-type equal-budget
armies on `data/units-balanced.json` — budget form, 600 points per
side, 19:1 body:commander mix (cavalry ~237 units, archers ~407,
infantry ~570, 10 commanders each side), squad-less (budget armies are
size-elastic; squad plans pin counts), 1000 x 1000 open field, 3000
tick limit, 100 paired seeds x 2 orientations per matchup, Wilson 95%
intervals, draws in the denominator. The script measures a candidate
`combat.json` through `--mod` (an overlay dir), so the shipped file is
never touched mid-tuning.

The knobs are per-direction — each matchup has two (the attacker's
counter into the defender, and the return direction), and the
multiplier range allows values below 1.

| round | cav>arch | arch>inf | inf>cav | verdict |
| --- | --- | --- | --- | --- |
| r0 baseline (no counters) | 0.800 | 0.070 | 1.000 | the budget protocol is far from even: cavalry already dominate archers, archers lose badly, infantry always win |
| r1 (0.80/1.30, 2.60/0.85, 0.75/1.40) | 0.435 | 0.825 | 0.990 | cav/arch and arch/inf overshot in opposite directions; inf/cav barely moved — the matchup is rigid |
| r2 (0.90/1.15, 2.15/0.95, 0.45/3.30) | 0.640 | 0.695 | 0.430 | two matchups landed in window; inf/cav overshot down |
| r3 (0.90/1.15, 2.15/0.95, 0.50/2.25) | **0.640** | **0.695** | **0.585** | **PASS** |

Final shipped table: cavalry->archer 0.90, archer->cavalry 1.15,
archer->infantry 2.15, infantry->archer 0.95, infantry->cavalry 0.50,
cavalry->infantry 2.25, cap 4.0.

Round-3 details (the acceptance numbers, reproduced exactly by the
shipped file):

- cavalry > archer: 0.640 [0.571, 0.703] (128/200), mean end tick 233
- archer > infantry: 0.695 [0.628, 0.755] (139/200), mean end 406
- infantry > cavalry: 0.585 [0.516, 0.651] (117/200), mean end 483

Every Wilson lower bound is above 0.5 and no share exceeds 0.70. Zero
draws in every round-3 matchup.

Notes from the tuning: the inf-vs-cav matchup needed by far the
strongest knobs (a 4.5x cavalry-favorable ratio) — equal-budget
infantry out-pool cavalry ~3.5:1 in raw melee math (170 hp x 570 vs
115 hp x 237 at better DPS), so the counters compensate for a
structural stat asymmetry, which is exactly what a counter layer is
for. The r1->r2->r3 response was interpolated in log-knob space and
r3 landed within the window on the first final-round try.

### The optional stance item: deferred, with reasoning

"Defensive stance scaled by terrain defense and local density" was
optional ("if budget allows"). Deferred: the M13 item H(a) evidence
says Hold fails structurally (holders never pursue disengaging
attackers — 0/20 holder wins, 100% of holder deaths at melee
distance), and a damage-factor rescale does not address that
mechanism, so the 0.35 win-share target on a hill map is unlikely to
be reachable by stance scaling alone. Doing it properly wants the
item-3 fix candidates (pursue-on-hold variants), which is engine work
beyond this item's "tune counters only" scope. Reported per the item's
"report otherwise" clause.
