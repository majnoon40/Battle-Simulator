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
follow-up item, after which the check comes back strict. (Shipped —
see "The flee clamp" below; the check is strict again under the
default-on flag.)

The same investigation found the constructor accepting explicit
initial units placed outside the battlefield (three test fixtures had
units at x = 900 on 400-wide fields — the battles only worked because
of the grid's clamping). That is input validation, not behavior, and
is fixed here: the explicit-state constructor now rejects non-finite
or out-of-bounds positions, and the three fixtures place their units
inside the field.

### The evidence

- **29 new tests** (`InvariantsTest.*`; the later flee-clamp PR added
  2 more, 31 in the file today): checker-on default, zero
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

## S2 — Differential and metamorphic tests

### What shipped

Six `DifferentialTest` cases in `tests/test_differential.cpp` — every
one runs the real binary end to end and compares runs against each
other, never against a golden file — plus a `differential` CI job
that extends the sweeps to every shipped scenario:

- **`SideSwapAggregateFairnessOnSymmetricMap`** — `default.json` is
  mirror-symmetric (open field, identical 250-infantry armies), so a
  faction advantage would skew the win count. Seeds 1-5, both
  orientations, 10 deterministic battles: Red 6 / Blue 4 / draw 0,
  pinned exactly. The counts are exact pins of deterministic battles:
  a battle-behavior change (always behind a flag) shifts them and
  forces a conscious re-measure.
- **`SideSwapTranslatesSpawnBandsNotMirrors`** — pins what
  `--swap-sides` actually does to spawns: each unit's band-relative
  offset maps to the same offset in the opposite band (a translation,
  measured max offset delta 4.000 = two speed-2.0 steps after one
  tick), keeping the army's layout. A mirror mapping would put the
  delta at |2*offset − 150| for most units, far outside the 4.5
  slack — the test fails if the semantics ever change to a mirror.
- **`ReplayRecordVerifyOnRepresentativeScenarios`** — record then
  verify (hash-by-hash re-run) on open field, squads, three factions,
  terrain, an earth map `--swap-sides`, and perf-5000 at a reduced
  budget; default and perf-5000 also verify under `--threads 4`.
  The CI job sweeps **all 19 shipped scenarios** plus swapped default
  and earth-lebanon (measured locally: 19/19 verified, 12 checkpoints
  at 300 ticks).
- **`GridVsBruteAgreeOnShippedScenarios`** — grid and brute neighbor
  modes export byte-identical JSON on default, mixed, squads,
  three-way (faction-scoped multi-faction queries) and demo-river
  (terrain); the CI job adds m10-line, balanced-demo and viewer-demo
  (measured: 8/8 agree).
- **`EraFlagsComposeWithInertModesToBaselines`** — the M7-era recipe
  (`--variance off --ai-avail v1 --end v1 --combat v1`) reproduces
  the committed baselines; the output-neutral modes (`--orders off`,
  `--neighbor-mode brute`, `--threads 4`) compose with it to the same
  bytes. Turning every switchable feature off reaches the previous
  release no matter how the inert modes are set.
- **`ScriptOrdersMatchReplayInterventionsExactly`** — an 8-order
  script (both factions, all six order types, all/specific squads,
  with/without points) is recorded into the replay as exactly the
  same intervention lines — count, order, fields — and the replay
  verifies. `CliAppliesScriptAndRecordsIt` checked one line's
  presence; this pins the full bijection.

### The finding: side-swap is not a mirror, and per-seed symmetry does not hold

The S2 checklist asked for "side-swap symmetry on a symmetric map:
the winner flips with the same end tick." Measured on `default.json`
(seeds 1-100, both orientations, 2000-tick cap): **it does not hold**
— winner flips 60/100 (what two independent even battles give), same
end tick **0/100**, mirrored casualties **0/100**. Example (seed 1):
Blue wins in 324 ticks (`faction_routed`) plain, Blue wins in 822
ticks (`stalemate`) swapped — a different battle, not a mirrored one.

Root cause, measured at the spawn (seed 1, tick 1): the swapped run
is neither the mirror of the plain run (mirror positions overlap
0/250) nor the same draws (0/250) —

1. The spawn stream allocates draws by faction index and maps them by
   **translation** into the flipped band (`x = band_lo + d*span` with
   `band_lo` flipped): the army keeps its exact relative layout on
   the other side; it is not reflected.
2. The default objective, in contrast, mirrors properly (band centers
   75 vs 925), so a unit's |dx| toward its objective differs between
   the runs and the normalized y-step diverges from tick 1 (y differs
   for 250/250 units after one tick).
3. Combat is *not* the culprit: attacks are collected from the
   tick-start state into `pending_attacks_` and applied afterwards,
   so a unit that dies mid-loop still lands its own queued attack —
   there is no first-mover faction advantage.

What a true per-seed fix would need: reflected in-band mapping +
cross-faction draw allocation (the army playing the opponent's mirror
role must consume the opponent's draws) + id-order-neutral
tie-breaking and event ordering (ties break by unit id; event
processing iterates by index — both favor whoever holds the low
indices). Every battle changes bytes → flag + `kSimVersion` bump +
full re-pin, the same shape as the queued flee-clamp change. Not done
here: `--swap-sides` as shipped is an orientation-flip deployment
tool (the M13 tuning used both orientations to cancel map asymmetry,
which works regardless of per-seed symmetry), and whether true
per-seed symmetry is wanted is a game-design decision.

### What does hold (and is now pinned)

- **Aggregate fairness on the symmetric map**: the 200-battle
  measurement gives Red 93 / Blue 103 / draw 4 — share 0.465, 95% CI
  [0.396, 0.534] — no evidence of a faction advantage. The CI job
  pins seeds 1-50 both orientations at Red 49 / Blue 50 / draw 1
  (~5 min single-threaded; 50 seeds, not 100, to halve the cost).
- **Replay record/verify: every shipped scenario** (19/19 measured;
  CI sweeps all, gtest keeps six representatives).
- **grid == brute** on every scenario small enough for the O(n^2)
  scan (8 measured: 5 in the gtest at battle length, 7 in CI).
- **flag-off == previous release**: the era recipe composes with
  every inert mode to the committed M7 baselines, byte-for-byte.
- **script orders == recorded interventions**, exactly (the 8-order
  bijection), and the scripted battle's replay verifies.

### Honest misses

- The perf scenarios (5k/20k units) are excluded from grid==brute —
  brute is O(n^2); they run the grid exclusively (the brute path is
  covered at smaller scale here and by the m13 synthetic tests).
- The fairness pins are exact counts of deterministic battles, so a
  knife-edge seed flip on a different libm (the cross-platform
  atan2 exposure from the m14 report) would shift a count by one and
  fail loudly — if that ever fires on a platform change rather than
  a code change, re-measure and re-pin.
- The spawn-semantics test pins tick-1 state (the CLI refuses
  `--ticks 0`); the 4.5 slack is two movement steps, tight enough to
  distinguish translation from a mirror but not to detect a partial
  reflection of a few units.

## The flee clamp (`--flee-clamp`, default on) — the S1 finding, fixed

The S1 checker found routed/flanking units leaving the battlefield (the
Advance/Approach/Retreat move family never clamped; only
retreat-with-enemies, formation steering and Flee did). The fix ships
behind `--flee-clamp on|off` (default **on**): a single clamp at the
move-apply choke point — the one loop every collected pending move
passes through (utility and legacy collection, terrain and plain
commits alike) — so one site covers all ten unclamped assignment
paths. `std::clamp` is a no-op for in-bounds destinations, so only
battles where a unit actually leaves the field change bytes.
`kSimVersion` bumps 13.1 → 13.2 once: old replays are refused cleanly
by version (this also closes the m14 gap — the item-0 counters change
shipped while the version stayed 13.1, so same-version pre-counters
replays failed at checkpoint verify instead of a clean refusal). The
checker's `position-bounds` check is strict again when the clamp is on
(and stays silent with it off, where running past the border is the
documented 13.1 behavior).

### The evidence

- **Repro**: a 200x100 edge battle (objectives at y=85 pull the armies
  to the top border; flank points get computed beside enemies close to
  it) — 13.1 leaves one Blue unit 18.4 units outside the field, alive,
  permanently (`tests/test_flee_clamp.cpp` pins both directions: the
  clamped battle stays inside at every tick; the unclamped battle ends
  with a unit outside).
- **OFF reaches the previous release**: 26 battle pairs (every shipped
  scenario at seed 42, plus default/squads at seeds 1, 7, 123 to 2000
  ticks and a 1000-tick earth-lebanon run) — `--flee-clamp off`
  exports are byte-identical to the independently built 13.1 binary in
  every pair. The edge battle's 13.1 export is committed as
  `tests/baselines/flee-clamp/edge-s1.json` and pinned by
  `FleeClampTest.OffReproducesThe131ReferenceBytes`.
- **What ON actually changes**: 2 of those 26 battles — the two
  earth-alps-innsbruck maps at seed 42 (the 96%-hills terrain pushes
  units out via the unclamped paths). No other shipped battle has a
  unit leave the field before termination ends it.
- **Re-pinned baselines: none.** No committed baseline pins an alps
  battle byte-for-byte, and the whole 546-test suite passes unchanged
  with the clamp on by default (540 pre-existing + 6 new: four
  FleeClampTest cases, two InvariantsTest bounds cases). The era
  recipes (M7 baselines, combat v1) need no `--flee-clamp off`: their
  battles never leave the field, ON reproduces the committed bytes.
- The flag rides in replays (`flee_clamp=on|off` in the flags line) and
  a hand-written 13.1 replay is refused by version — pinned by
  `FleeClampTest.ReplayCarriesTheFlagAndVerifies`.

### Honest misses

- The clamp holds routed units *at* the border (they pile up along the
  edge instead of dispersing past it); whether cornered units should
  fight, surrender or be removed is a game-design question, not a
  bounds question.
- The strict `position-bounds` check trusts the clamp completely; a
  future move path added outside the pending-move pipeline would not
  be covered by the choke point (it would, however, be caught by this
  check the moment a unit crosses).

## `--swap-sides mirror` (test-only) — the S2 follow-up, honestly short of its acceptance

The S2 finding queued this mode: a `--swap-sides mirror` value that
produces the mirror battle on a symmetric scenario. The design that
shipped: **Blue spawns first** (taking the unit index and id space the
plain run gave Red — so every index-ordered pass aligns: spawn draws,
`personality_for(seed, id)` personalities, combat collection, morale,
events, and id-based tie-breaking), and each band's x draws map
**reflected** through the counterpart's plain band
(`x = width - (role_lo + d * role_span)`, the role expressions written
exactly as the plain run computes them), so Blue stands at
mirror(plain Red) and Red at mirror(plain Blue). Bands stay default —
objectives, HQ and formation axes align with the roles automatically.
The CLI swaps compositions/squad plans (and mirrors explicit
objectives) like translate; `swap_sides=mirror` rides in replays.

### What is pinned, and what is not (measured, seeds 1-100)

- **Spawn mirror: bitwise, 100/100.** For every unit id k, the
  mirrored unit stands at `width - x` (memcmp-level), same y, same
  type, same personality.
- **Outcome mirror: 25/100.** Winner flips, same end tick, same
  reason, casualties mirror exactly on 25 seeds (pinned on eight of
  them). The other 75 diverge at a **single knife-edge late in the
  battle** — one unit routes or dies one tick apart (first divergence
  ticks 238-340; 21 of the 49 outcome failures differ by exactly one
  end tick), after which outcomes drift by a tick or a few
  casualties. Mirrored alive-counts match every tick before the
  divergence point.
- **Root cause: floating-point mirroring, not a logical asymmetry.**
  Movement arithmetic is not associative — the mirrored unit computes
  `(width - x) - t` while the ideal mirror is
  `width - (x + t)`, a last-ulp difference — and libm `atan2` is not
  antisymmetric, so the facing angles the combat arcs and rear-morale
  rules classify against differ by ulps from the ideal mirror. A
  battle runs within ~1 ulp of its ideal mirror until a morale or arc
  threshold cuts a knife edge; everything after is chaos amplification.
- **The 100/100 acceptance is not reachable inside the time-box.**
  Fixing it means restructuring the core float math into
  mirror-invariant forms (movement, facing, arcs, morale) — dozens of
  sites, high regression risk, and it would change default bytes.
  That is explicitly out of scope; the S2 aggregate-fairness pins
  remain the practical guarantee.

The mode stays opt-in and test-only: default battles and every
existing baseline are untouched (the full suite passes on the stacked
branch). `tests/test_swap_mirror.cpp` pins the bitwise spawn mirror
(10 seeds), the id-space swap, the outcome mirror on the eight
measured passing seeds, and the CLI/replay round-trip.

## S5 — The performance gate: the 100k scale point, the t2 fix, and a CI floor

Three pieces, one contract: **the ladder reaches 100,000 units, the
known thread regression is fixed byte-identically, and CI now measures
the ladder on every push against explicit floors.**

### The scale point: `perf-100000.json` and the cap raise

`kMaxUnits` rises 20,000 → 100,000 (M3 had raised it 500 → 20,000).
The new `perf-100000.json` keeps the spawn-band density of
`perf-20000.json` constant (5600 = 2500·√5 rounded up, so 2.5× the
units per side at the same units-per-area): the ladder measures scale,
not crowding. The cap raise is validation-only — every existing
scenario was already under 20,000, so no existing battle changes (the
35-pair byte-identity matrix below includes every shipped scenario).

### The fix: occupancy-skip in the radius queries (byte-identical)

m14 item 1 left an honest observation: at 20k units two threads ran
*slower* than one (19.2 vs 22.6 t/s) — the worker pool paid
bucket-header cache misses both workers contended for, and the
parallelism could not pay for them. The fix shipped here:
`radius_query` and `radius_query_faction` now skip any (cell, faction)
bucket whose occupancy bitmap bit is clear. The bitmap is maintained
exactly in lockstep with bucket emptiness (`insert_into_cell` sets the
bit on empty → non-empty, `remove` clears it on empty, `build` resets
both), so an empty bucket appends nothing either way — the appended
sequence is unchanged **by construction**, and the 35-pair cross-binary
byte-identity matrix confirms it (every shipped scenario at seed 42,
plus the perf ladder at threads 1/2/3/4, plus output-neutral modes).

The mechanism is exactly the m14 observation's mirror: the bit test
lives in an L1-resident ~3 KB bitmap, while the skipped bucket-header
load was a random access into a 1.2 MB (20k) or 5.9 MB (100k) bucket
array — the memory two workers evict from each other's caches.

**Measured (same protocol as the m14 table: single runs, seed 42, 600
ticks, this 2-vCPU box, Release, before = main 2abaf7b):**

| scenario | threads | t/s before | t/s after | gain |
| --- | --- | --- | --- | --- |
| perf-5000 | 1 | 81.8 | 80.6 | ~0 (noise) |
| perf-5000 | 2 | 75.4 | 88.0 | **+17%** |
| perf-20000 | 1 | 21.4 | 21.9 | ~0 (noise) |
| perf-20000 | 2 | 17.9 | 22.3 | **+25%** |
| perf-20000-squads | 1 | 5.50 | 5.56 | +1% |
| perf-20000-squads | 2 | 7.79 | 7.96 | +2% |
| perf-100000 | 1 | — (new) | 2.39 | — |
| perf-100000 | 2 | — (new) | 2.84 | — |

The t2 regression is gone: two threads beat one again at 20k (22.3 vs
21.9) and at 100k (2.84 vs 2.39). t1 is unchanged within noise, as the
mechanism predicts — a single worker's cache pressure is what the skip
does not target. At 100k, perception is 390 of 418 ms/tick (93%) and
peak memory is 262 MiB.

### The gate: the CI perf job

A new `Performance (M15 S5)` job runs the ladder (perf-5000,
perf-20000, perf-20000-squads, perf-100000; seed 42, 600 ticks) at
threads 1 and 4 on every push, gates each scenario on the **best** of
the two thread counts (the pool sometimes loses to t1 on small cores —
the same m14 observation), and writes the measured table into the job
summary. Floors are set well under the first measured medians to
absorb runner-to-runner variance; they are the honest gate — a number
the maintainers can defend, not the fastest box ever seen.

### The honest misses

- **The m14-era targets (20k ≥ 60 t/s, 100k ≥ 10 t/s) are not met on
  this 2-vCPU sandbox** (21.9 and 2.39 t/s). They were never measured
  on CI hardware before this PR; the gate job is what makes the CI
  numbers visible and enforced. The floors committed here are derived
  from the first CI measurements on the PR, not from the targets.
- **Perception is still 89-93% of every scale tick.** The next
  byte-identical win would need layout work (SoA shadows, bucket
  prefetch) or an algorithmic change (hierarchical grid) — the skip
  here is the one that could not change bytes.
- **Squads barely move** (+2% at t2): the squad/cohesion share of the
  squads scenario is outside the perception path the skip targets.

### Verification

- **551/551 tests** (550 + the new occupancy lockstep test; the two
  cap-pinning tests and the budget-fill rejection moved to the new
  cap, and the checker-on default now validates the occupancy
  invariant every 30 ticks of every test battle — the S1 checker's
  `verify_contents` gained the bitmap⟺bucket check in both
  directions).
- **35/35 byte-identical** cross-binary pairs (before = main 2abaf7b
  independently built; every shipped scenario at seed 42 × 300 ticks,
  swapped orientations on default and earth-lebanon, the perf ladder
  at threads 1/2/3/4, `--orders off` and `--ai-avail v1` runs).
- **The new scale point works end-to-end**: the invariant checker at
  100k costs ~2% (2.39 → 2.34 t/s at `--check-invariants=10`); a
  60-tick 100k replay records to 352 bytes and verifies (3
  checkpoints) — the differential job's sweep runs it at 60 ticks.
