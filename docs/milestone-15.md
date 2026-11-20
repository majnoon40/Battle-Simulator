# Milestone 15 — The game (v0.1.0) and engine stability

Item index: `docs/PROGRESS.md`. This file carries the per-item Part A
(what shipped, the evidence, and the honest misses).

## S1 — Invariant checker (`--check-invariants[=N]`, default off)

Branch `m15-s1-invariants` (on top of main at `686e8e0`).

### What shipped

- **`Simulation::check_invariants()`** — a read-only validation of the
  state the simulation actually maintains, one violation record per
  finding (tick, unit id and index, invariant name, a detail string
  with the offending values):
  - ids non-zero and unique;
  - positions finite and (for alive units on a terrain map) not inside
    an impassable cell — bounds are deferred, see the finding below;
  - health finite, alive units in `(0, max_health]`, dead units at
    exactly 0 (what `apply_deaths` guarantees), `max_health > 0`;
  - morale in `[0, 100]` with a valid state, fatigue and experience
    in `[0, 1]`, facing finite;
  - dead units carry no target and no cooldown (`apply_deaths` zeroes
    the cooldown immediately; the target slot has a documented one-tick
    grace — see below);
  - no alive unit targets a same-faction unit, or a dead unit past the
    grace;
  - every unit's faction is one this battle fields;
  - squad membership consistent in both directions (unit → squad →
    member slot → unit, plus faction match), commanded squads have a
    living commander, cohesion in `[0, 1]`, finite centroid, contact
    count within the fixed maximum, member orders valid enum values;
  - the in-flight order queue references existing squads of the right
    faction, keeps `deliver_tick >= issue_tick >= 1`, stays sorted by
    `deliver_tick`, and point orders carry finite targets;
  - the spatial grid's contents equal the unit positions
    (`SpatialHashGrid::verify_contents`: every alive unit tracked in
    the bucket matching its position and faction with a current query
    shadow, no dead unit tracked, bucket totals = `tracked_count_` =
    alive count) — only in the modes that maintain the grid;
  - per-faction aggregates: estimates, HQ and objectives finite, deaths
    never above the initial head count.
- **`--check-invariants[=N]`** (CLI): off by default; bare means every
  tick, `=N` every N ticks (strict parse, `[1, 100000]`). On the first
  violation the CLI prints each violation (tick, unit, invariant,
  detail) with a full state dump of the offending unit plus the global
  counters, writes a replay of the run (`invariants-failure.replay.json`,
  or points at the `--record` file when recording), and exits **3**.
- **Freeze semantics**: the first violation makes `finished()` true —
  `run()`, the CLI loops, and the `ReplayDriver` all exit instead of
  ticking corrupt state further; `result()` honestly reports the
  battle as unfinished (`finished = false`, reason None). This is the
  one behavior difference a checker-on run can have, and only in a
  battle that is already corrupt.
- **Tests build with the checker on by default**:
  `Simulation::set_default_invariant_checks(30)` is armed by a static
  initializer in `tests/test_invariants.cpp`, so every Simulation
  constructed anywhere in the suite validates its state every 30 ticks
  and at termination. The production binary never calls the setter —
  the CLI default stays off.

### The one-tick grace (a finding from running the checker)

The first version of the target invariants fired on the very first
mid-battle death of `default.json`: a unit that dies during tick T
keeps its last target slot, and its killers keep targeting it, until
tick T+1's decide phase resets `target_index_` for everyone. That is
by design — `target_index_` is part of the hashed state, so clearing
it at death would change every replay hash and break byte-identity
with the previous release. The checker therefore snapshots the dead
set at the start of any tick whose end will run a check, and enforces
`dead-no-target` / `target-alive` only for units already dead when the
tick began (they have been through a decide reset). A stale target that
survives one full tick is still caught at the next check.

### The second finding: fleeing units leave the battlefield

Enforcing `position-bounds` froze a corridor battle at tick 264: Blue
units stood at y = 106.2 and 115.3 on a 200x100 field. The cause is in
`resolve_movement` — the Advance/Approach/Retreat move paths
(`move.x = unit.x + dx / dist * step`) never clamp to the battlefield;
only the retreat-with-enemies and formation-steering paths do. The
spatial grid has always tolerated out-of-world positions (documented
border-cell clamping), so nothing ever crashed — units simply flee past
the border and keep running. That is a real, long-standing behavior
bug, but fixing it changes battle bytes: it needs its own PR with a
flag, a `kSimVersion` bump and a re-pinned baseline sweep. The checker
therefore does NOT enforce bounds yet; the finding is queued as the
follow-up item, after which the check comes back strict.

The same investigation found the constructor accepting explicit
initial units placed outside the battlefield (three test fixtures had
units at x = 900 on 400-wide fields — the battles only worked because
of the grid's clamping). That is input validation, not behavior, and
is fixed here: the explicit-state constructor now rejects non-finite
or out-of-bounds positions, and the three fixtures place their units
inside the field.

### The evidence

- **25 new tests** (`InvariantsTest.*`): checker-on default, zero
  interval rejected, healthy battles (plain and squads) clean through
  termination, interval independence (the same battle at N = 1, 7 and
  30 produces the identical state hash — the checks only read state),
  one corrupt-and-catch test per invariant family via the
  `InvariantCorruption` test seam (ids, positions, health, morale,
  fatigue, experience, facing, faction, dead-unit targets, dead
  targets, same-faction targets, order queue shape and sorting, squad
  membership both ways, squad cohesion, dead commander holding
  command, grid desync), the mid-battle freeze (violation recorded at
  the right tick, world stops advancing, `run()` returns), and the
  state dump contents.
- **The whole existing suite is the byte-identity evidence**: all
  pre-existing tests (the pinned baselines, export formats, replay
  verification, determinism tests) run with the checker on by default
  and pass unchanged — the checker provably does not perturb a healthy
  battle, and the allocation-counting tests (steady-state ticks with
  the checker on) confirm a healthy check allocates nothing: the
  scratch vectors are pre-sized at construction, and the violation
  strings — including the grid's per-unit prefix — are built only when
  a violation actually fires.
- **CI job `invariants`**: every shipped scenario (19) at 300 ticks,
  seed 42, threads 1 with the checker at N = 10; 7 swapped
  orientations; 4 runs at threads 4; 2 runs with a scripted Lua
  general (default + three-way, self-play) — 32 runs, plus a smoke
  assertion that the bare flag exits 0 so a silent skip cannot pass.

### Honest misses

- The checker validates the post-tick state every N ticks, not every
  phase within a tick; a transiently corrupt mid-tick state that
  repairs itself by tick end is invisible to it.
- Flow fields, the timeline and event buffers are not covered (their
  own test suites pin them); the check covers units, squads, orders,
  the grid and the per-faction aggregates.
- `--check-invariants` with `--replay --verify` reports violations but
  the replay-verify verdict is skipped (the run exits 3 first).
- The failure replay carries the deterministic inputs (scenario, seed,
  flags, script orders, interventions); a scripted-general run needs
  the same `--general` flags supplied again — generals are not
  captured in replay files (the same limitation `--record` has).
