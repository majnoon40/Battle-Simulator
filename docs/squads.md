# Squads, formations and commanders (M6)

Squads give armies structure: soldiers assemble into formations around a
commander, the commander issues orders from the squad's state, and orders
influence — never override — the utility AI. `--squads on|off` (default on;
utility mode only — legacy ignores it to reproduce M1-M3, and scenarios
without a `squads` section field no squads even when enabled). With squads
off, utility battles are bit-identical to their M5 runs.

## Data

| File                   | Contents                                                     |
| ---------------------- | ------------------------------------------------------------ |
| `data/units.json`      | `commander` type: own stats plus `leadership_radius` (120). A type with `leadership_radius > 0` is a commander type; the field is optional (absent = cannot command) but must be positive when present. |
| `data/formations.json` | The three formation shapes (`line`, `column`, `loose`) with per-formation `spacing`, the `default_formation` squads use, and the steering weights (`separation`, `cohesion`, `alignment`, `slot_seeking`, `separation_radius`, `cohesion_reference`). |
| `data/commanders.json` | Order rules and thresholds, order score weights, propagation and decay timings, succession delay, the commander-death morale shock, and the obedience model. |
| Scenario `squads`     | Per faction `{count, size}` (size optional, documented default 25). A faction with a plan must field exactly `count` commander-type units and `count * size` soldiers — validated, no silent defaults. |

## Squad assignment

At construction, per faction with a plan: soldiers and commanders are
sorted by `(y, x, id)` — spatially contiguous groups in a deterministic
order (spawn-order ids break exact ties) — then sliced into `count` squads
of `size` soldiers plus one commander each. Every unit ends up in exactly
one squad (or none, for factions without a plan). Members are stored
commander-first, then soldiers by unit index; a member's slot index is its
position in that order.

## Formations and steering

Slot offsets come from the shape, member count and spacing:
`line` is one lateral rank filled center-out (the commander holds the
center); `column` is a single file behind the commander; `loose` is a
square-ish block, rows front to back, center-out per row. Slots never
overlap (pairwise distance >= spacing, tested).

A squad member whose action is **Hold or Advance** steers instead of using
the plain move: the steering vector is the weighted sum of slot seeking,
cohesion (toward the squad's last computed centroid), alignment (the
squad's mean last move) and separation (pushed away from any unit strictly
inside `separation_radius`, any faction), and the member moves
`min(speed, |steering|)` along it, clamped to the battlefield. All reads
come from the tick-start snapshot. The acting commander never steers — it
anchors the formation and moves by the normal utility rules. A member
drops formation whenever its AI picks Attack, Approach, Retreat or Flee
(each drop counted once) and returns afterwards. A leaderless squad keeps
its shape around its last centroid.

The near-slot pull weights are deliberately small (slot seeking 0.35 +
cohesion 0.1): the proportional move rule is only stable while the pull
gain stays below `speed / gain`, otherwise members overshoot their slots
into a saturated two-tick oscillation (measured and fixed during M6;
formations now converge).

Formation axes: `forward` points from the faction's spawn band center
toward its objective (straight across by default), `lateral` is the
perpendicular. Slots are anchored on the commander's position each tick.

## Orders

The commander re-evaluates its order at most once per
`order_min_duration_ticks` (hysteresis) from the squad state:
`retreat` (cohesion < `retreat_cohesion_below` OR the commander's local
strength ratio < `retreat_strength_below` OR mean member morale <
`retreat_morale_below`), `regroup` (cohesion < `regroup_cohesion_below`),
`hold` (enemies visible to the commander AND strength >
`hold_strength_above`), otherwise `advance`. Local strength is the
commander's own sight-limited ratio `(allies+1)/(allies+enemies+1)`; squad
morale is the member mean, or a neutral 50 with morale off so only cohesion
and strength drive orders.

Propagation: a member inside the `leadership_radius` adopts the commander's
current order once `tick - issue_tick >= ceil(distance *
order_delay_per_unit)` (the latest order always wins); outside the radius
the last received order decays to none after `order_decay_ticks`
consecutive out-of-range ticks.

The order is a **bounded score term, never an override**: obedience *
order weight added to the actions the order favors — advance -> Advance +
Approach, hold -> Hold + Attack, regroup -> Hold, retreat -> Retreat —
only where the action is available, then clamped to [0, 1].

Obedience = `discipline * clamp(1 - distance/leadership_radius, 0, 1) *
morale_factor[state] * (cohesion_floor + (1-cohesion_floor) *
squad_cohesion)`, clamped to [0, 1]. It falls with distance and rises with
discipline and squad cohesion; Confident obeys more, Shaken less, Broken
never scores. `discipline` is the fourth personality draw (after bravery,
caution and aggression from the same hash stream, so M4/M5 personalities
are bit-identical).

## Commander death, rout and succession

An acting commander's death (in `apply_deaths`) costs the squad the
command: members inside the leadership radius take
`commander_death_morale_loss`, scaled by the standard personality loss
factor, immediately (a member shocked below the breaking value routs the
same tick via the normal morale machinery); all member orders drop to none
and the squad drifts until succession. A routed commander loses command
permanently (no shock, no death count for the already-deposed). Exactly
`succession_delay_ticks` after the loss, the lowest-unit-id alive,
non-routed member takes command (the recovered former commander is
eligible like anyone else); if nobody qualifies the squad stays leaderless
and retries every tick. The squad keeps its original `leadership_radius`.

## Export, CLI and debug

Format 5 (squads active) adds per-unit `squad` (id or -1), `formation`,
`order`, `cohesion` and the personality `discipline`; per-faction
`squads` statistics (squads, commander deaths, successions, order counts,
drops from formation); and `leadership_radius` in the unit type table.
Formats 3-4 are byte-identical to M5/M4, so `--squads off` reproduces M5
exactly (verified against the M5 binary's own export with the M5-era
units.json).

CLI: `--squads on|off`, `--formations <file>`, `--commanders <file>`,
`--squad-debug <id>` (end-of-run squad state: members, commander, order,
cohesion), and `--ai-debug` additionally prints the order, the obedience
weight and the applied order terms.

## Determinism

Assignment, metrics, order selection and propagation iterate fixed member
order; the only random stream is the spawn RNG (unchanged by M6).
Squads-on double runs are byte-identical (tested), and grid == brute
stays a legacy-mode property (squads never touch targeting).
