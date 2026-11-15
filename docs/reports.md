# Battle reports, timelines and event logs (M10 Part 2a)

The CLI records what happened in a battle without changing it: the same
battle with and without any of the flags below is byte-identical
(cmp-verified in the test suite).

## Event log — `--events out.jsonl`

One JSON object per line, appended in deterministic order (phase, then
unit/squad index). Every event carries its 1-based tick.

| type | fields | fired when |
| --- | --- | --- |
| `death` | `unit`, `killer` | the attack that brings a unit from positive health to <= 0 |
| `rout` | `unit` | a unit's morale drops below the breaking value |
| `recovery` | `unit` | a routed unit meets the recovery hysteresis |
| `commander_death` | `commander`, `squad` | a squad's acting commander dies |
| `succession` | `successor`, `squad` | a new commander takes over (lowest unit id eligible) |
| `order_change` | `squad`, `order` | a commander issues a different order |
| `flank_start` / `help_start` | `unit` | the first tick of a Flank/Help decision run |
| `pursuit_start` | `unit` | a unit starts chasing a newly targeted routed enemy |
| `seek_start` | `unit` | contact seeking escalates the unit's contact to the faction estimate |
| `squad_collapse` | `squad` | a squad's last alive member dies |

Off by default; `config.events` is the library hook (`sim/events.hpp`).

## Timeline — `--export-csv out.csv` (`--csv-interval K`, default 10)

One row every K ticks plus the battle's final tick: `tick`, then per
faction `alive`, `routed` (alive but routing), `dead` (cumulative),
`avg_morale` (over non-routed alive units; empty when none), and
`engagements` (units whose current action is Attack), then alive counts
per unit type per faction (`red_infantry_alive`, ...). Sampling only
reads state.

## Report — `--report out.md` (writes `out.md` + `out.json`)

Generated from recorded state only (result, world, squads, timeline,
events) — no sentence without a measured value behind it. Sections:
outcome; casualties by unit type; morale timeline (first/min/last
sample); routing (totals, first rout, largest 20-tick rout wave);
squad performance ranked by kills (kills, losses, final cohesion,
orders issued, commander status); commander events; turning points:

- **Kill ratio** — the first tick where one side's kills in the last 50
  ticks exceed twice the other side's, with at least 3 kills in the
  window (the floor keeps a single early kill from tripping the rule).
- **First commander death** — first `commander_death` event.
- **First mass rout** — the first tick where a 20-tick window contains
  at least 10% of a faction's initial strength in routs.

## Chart — `--report-png out.png`

Casualty (dead per faction) and average-morale curves over the sampled
timeline, drawn with the M7 software rasterizer (`render/`: DrawList ->
rasterize -> PNG; the built-in 5x7 bitmap font labels axes and legends).
Deterministic: the same battle produces the same PNG bytes. Needs the
timeline (implies sampling at `--csv-interval`).

## Replay — `--record out.replay`, `--replay in.replay [--verify] [--seek T]`

`--record` runs the battle tick-by-tick and writes the full setup
(sim version, scenario, mod, seed, tick limit, the variance /
contact-seek / neighbor / squads / swap-sides flags), any
`--intervene kill:UNIT:TICK` interventions, and a canonical state hash
every 25 ticks (plus the final one). The hash is FNV-1a 128 over a
field-by-field serialization of every piece of state that can influence
the battle's future (tick, RNG state, all per-unit fields, perceptions,
squad state, faction counters, estimates, stuck bookkeeping) —
`sim/src/state_hash.cpp`.

`--replay FILE --verify` re-runs the battle from the replay's recorded
setup, re-applies its interventions at the recorded ticks, and compares
every checkpoint hash; on mismatch it prints the first mismatching tick
and exits 2. A replay recorded by a different sim version is refused
with a clear error (exit 1). Without `--verify`, the re-run simply
replays the battle (the normal report/export flags apply).

`--seek T` re-runs from tick 0 and stops at T — the cost is a full
re-simulation, O(T): there are no mid-battle snapshots, only hashes.
Combined with `--export-json` it produces the state at exactly T.

Interventions: `--intervene kill:UNIT:TICK` kills one unit by id
before tick TICK's phases run (dead by the end of TICK; recorded as a
death event with killer 0 when events are on).
