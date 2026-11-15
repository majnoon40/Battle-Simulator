# Morale and routing (M5)

Individual morale, fear, routing and recovery, so armies can collapse before
the last soldier dies. Applies to utility mode only: `--ai-mode legacy`
ignores morale entirely (it keeps reproducing the M1-M3 references), and
`--morale off` (default on) makes utility battles bit-identical to their M4
runs. All rules run from deterministic inputs; every number lives in
`data/morale.json` (validated like the other configs, no silent defaults).

## The morale value and its states

Morale is a per-unit value in [0, 100], clamped and never NaN. Spawned units
start at `start.base + start.bravery_scale * bravery` (brave units start
higher). The state derives from the value via the thresholds:

| State | Morale range (default) |
| --- | --- |
| Confident | >= 75 |
| Steady | 50 - 74.99 |
| Worried | 30 - 49.99 |
| Shaken | 12 - 29.99 |
| Broken | < 12 |

## Per-tick update (phase 5, after deaths)

For each alive unit, one bounded change per tick:
`morale = clamp(morale + delta, 0, 100)` with
`delta in [-max_loss_per_tick, +max_gain_per_tick]` (10 / 2.5 by default —
morale falls faster than it recovers, and bursts spread over several ticks).

All loss amounts are scaled by the personality factor
`clamp(1 + caution_amplifier*caution - bravery_shield*bravery, min_scale, 2)`
— bravery shields, caution amplifies.

Losses (from the tick-start perception snapshot plus this tick's events):

| Source | Effect (default) |
| --- | --- |
| Nearby ally deaths | -10.0 per ally death within the unit's sight radius (event-based, same tick; one grid query per death) |
| Ally routs (contagion) | -3.0 per ally break within sight (one-tick delay; keeps the spread independent of unit order) |
| Routed allies nearby | -0.3 per tick per currently-routed ally within sight — routers streaming past keep draining morale |
| Locally outnumbered | -0.4 per tick while the strength ratio (allies+1)/(allies+enemies+1) is below 0.4 |
| Damage taken | -25.0 per fraction of max health lost this tick |
| Isolation | -0.6 per tick with enemies visible and no allies visible |

Gains (unscaled):

| Source | Effect (default) |
| --- | --- |
| Kills | +4.0 per kill credited this tick |
| Local superiority | +0.15 per tick while the strength ratio is above 0.75 and enemies are visible |
| Allies present | +0.05 per tick with allies and enemies visible |
| Safe time | +0.35 per tick with no enemy in sight |

The rout-contagion channels (ally routs, routed-ally presence) are the
mechanism that lets a local collapse spread army-wide: a breaking front
line pushes panic rearward through the ranks.

## Breaking, routing and recovery (hysteresis)

- A unit whose morale falls below `shaken_min` (12) **breaks**: the `routed`
  flag is set (recorded in the per-faction routing statistics) and it flees.
  A unit already below the breaking value at decision time flees
  immediately, on the same tick.
- **Routed units** are alive but not active. They do not score actions:
  they cannot Attack, Approach or Advance; they never attack. They flee at
  full (normal) speed away from the visible enemy centroid and toward their
  own side — with no enemies visible, straight toward their own side. A
  unit pressed against both relevant battlefield boundaries is cornered and
  flees toward the nearest world edge.
- **Recovery** requires BOTH: at least `safe_ticks` (120) consecutive ticks
  with no enemy in sight AND morale strictly above `morale_threshold` (35).
  The threshold lies strictly above the breaking value (12) — that gap is
  the hysteresis that prevents break/recover flip-flopping, enforced by
  config validation. A recovered unit rejoins as Shaken or better.
- Measured flip-flop bound (standard battle, seed 42): **0.061 routed-state
  changes per unit per 100 ticks**; the documented bound is 3.

## Termination: FACTION_ROUTED

A faction with **no active units** (all dead or routed) for
`rout_timeout_ticks` (30) consecutive ticks ends the battle: the other
faction wins (a mutual collapse is a draw). Elimination (all dead) and the
tick limit are unchanged; a routed unit counts as a survivor but not as an
active fighter.

## Utility AI interaction

Action scores are modified by the state before the choice:
Confident adds `confident_bonus` (0.05) to attack/approach and subtracts it
from retreat; Worried and Shaken subtract their penalty (0.10 / 0.25) from
attack/approach and add it to retreat; Steady changes nothing; Broken never
scores — it flees.

## Debug and export

`--ai-debug` prints the unit's morale, state and routed flag next to the
scores. The JSON export (format 4, morale on) adds per-unit `morale`,
`state` and `routed`, and per-faction `routing` statistics (`first_rout_tick`
(0 = never), `peak_routed`, `total_routs`, `recoveries`) plus `active`
counts. With morale off (and in legacy mode) the export keeps the exact
format-3 shape — morale-off exports are byte-identical to the M4 runs of
the same battle.

## Measured behavior (honest numbers)

- 3:1 mismatch (375 vs 125, seeds 1..100): the smaller army routs before it
  is annihilated in **97 of 100** battles; the bigger side never routs;
  battles end around tick 393 on average.
- Equal armies (250 vs 250, seeds 1..100): **0 of 100** battles end by
  FACTION_ROUTED — the ≥80% target is MISSED. Peak simultaneous routed is
  ~99 of 250 and the rout funnel is real (routs 10,396 vs 337 recoveries —
  routed units die or stay out), but the M4 objective system dissolves the
  armies into sparse skirmish clouds; a resilient active remnant (~200
  units, mostly brave and locally winning) never fully collapses within the
  3000-tick default limit. 10,000-tick probes show near-misses (25-51
  actives remaining). The natural fixes — cohesive fronts, pursuit of
  routers, command structure — are M6+ scope; the target was not tuned
  away.
