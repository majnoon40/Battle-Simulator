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

## Item 1 — Scale: perception off the Unit array (branch m14-scale)

The M13 item 4 benchmark (this box, 2 vCPU, seed 42, 600 ticks,
--profile-phases) showed the tick at 20,000 units is 70.4 ms of which
perception is 63.7 ms (90%): pass A's radius queries randomly touch the
232-byte Unit structs per candidate, and the aggregation loops walk them
again. The item is a set of byte-identity-preserving throughput changes
to that path — no behavior change anywhere, by construction and by
measurement.

### What shipped

- **Query shadow** (`SpatialHashGrid::Shadow`, 24 bytes/unit: x, y, id,
  faction, alive): the grid maintains a per-unit copy of exactly the
  fields queries test, refreshed by build/insert/update/remove — every
  simulation mutation flows through those (movement through
  update_grid_after_movement, deaths through the same apply_deaths
  iteration that clears the flag, and update() refreshes before its
  same-cell early return). `radius_query` reads the shadow instead of
  the unit array; values are copies and the bucket iteration order is
  unchanged, so outputs are bit-identical. `nearest_enemy` still reads
  the unit array and keeps its documented behind-the-back liveness
  contract (test-pinned); `radius_query`'s liveness is the shadow, which
  in the simulation is always as current as the buckets (documented in
  spatial_hash.hpp).
- **O(1) remove**: a slot array (unit -> position in its bucket) makes
  remove() swap-remove through the tracked slot — the identical writes
  the std::find-based removal performed, without scanning the bucket.
- **Faction-scoped query** (`radius_query_faction`): four call sites
  only ever keep one faction of the both-faction scan (the avail-v2
  routed-ally probe, commander auras, the charge dense-line check,
  morale death-event spread); the variant iterates only that faction's
  buckets and returns exactly the subsequence the scan kept — proven by
  a property test, and the other faction's buckets are never touched.
- **Pass-A candidate shadow** (`Simulation::CandidateShadow`:
  health, max_health, routed — 24 bytes/unit, one sequential refresh at
  decide_targets start): the perception aggregation, the Help scan,
  target_routed and the tactical-memory update read shadows instead of
  Unit structs; every read is a copy in the same iteration order.
- **Fused termination sweeps**: update_termination's five full
  unit-array scans (per-faction alive x2, per-faction active x2,
  stalemate attackers) became one pass over integer counters.
- **Bigger query buffer reserve** (256 -> 2048/worker): sight-radius
  queries on converged 20k fronts exceed 256 candidates; the buffers
  used to grow during warm-up.

### Measured (same protocol as the M13 table: single runs, seed 42,
600 ticks, this 2-vCPU box, Release, --profile-phases means)

| scenario | threads | t/s before | t/s after | gain | ms/tick | perception ms |
| --- | --- | --- | --- | --- | --- | --- |
| perf-5000 | 1 | 79.8 | 85.2 | +7% | 12.5 -> 11.7 | 11.8 -> 10.9 |
| perf-5000 | 2 | 73.1 | 92.8 | +27% | 13.7 -> 10.8 | 12.6 -> 9.8 |
| perf-5000-squads | 1 | 59.2 | 63.6 | +7% | 16.9 -> 15.7 | 13.2 -> 12.3 |
| perf-5000-squads | 2 | 75.2 | 81.7 | +9% | 13.3 -> 12.2 | 9.4 -> 8.6 |
| perf-20000 | 1 | 15.9 | 22.6 | **+42%** | 63.0 -> 44.3 | 57.2 -> 39.5 |
| perf-20000 | 2 | 17.1 | 19.2 | +12% | 58.4 -> 52.2 | 51.8 -> 46.8 |
| perf-20000-squads | 2 | 5.8 | 7.7 | **+34%** | 172.9 -> 129.4 | 125.7 -> 94.9 |
| earth-lebanon | 2 | 238.1 | 235.5 | ~0 | 4.2 -> 4.3 | 3.3 -> 3.3 |

earth-lebanon is unchanged deliberately noted: at 2,080 units on a
terrain map the tick is flow-field/nav bound and perception is a small
share, so the item's target (the 5k/20k scale wall) is where the gains
are.

### Honest observations

- **Single-thread now beats two threads at 20k** (22.6 vs 19.2 t/s;
  before the change t2 won 17.1 vs 15.9). With the per-candidate cost
  down, the pool's dispatch/barrier overhead and two workers contending
  for the same shadow arrays cost more than the parallelism returns on
  2 cores. Output remains byte-identical for every thread count (pinned
  by the existing suite); a chunk-size/dispatch follow-up could recover
  the t2 gap. Not pursued here — the item is the byte-identical
  throughput change, and t1 is now the fastest configuration at 20k.
- perf-5000 t2 was already slower than t1 before the change on this box
  (73.1 vs 79.8); after it t2 wins clearly (92.8 vs 85.2). The 5k
  working set fits cache; 20k does not.

### Verification

- Byte-identity: 17 scenario exports (default, mixed, squads,
  demo-river, earth-lebanon swapped, balanced-demo, perf-5000,
  perf-20000, perf-20000-squads, combat-v1 pairs; thread counts 1/2/3/4)
  compared before/after — all 17 byte-identical, plus the full existing
  suite including the M1/M2 references, mixed double-runs, the
  5,000-unit both-modes byte-identity and replay verification.
- 487/487 tests (486 + the new
  `SpatialHashPropertyTest.RadiusQueryExactUnderIncrementalMaintenance`:
  300 random move/kill/revive steps through grid ops assert the
  shadow==units invariant, radius_query == brute-force oracle, and the
  faction query == the both-faction subsequence).
- TSan (Debug, -fsanitize=thread, CI's two selections):
  14/14 multithreading tests (incl. ScenarioExportsAreByteIdentical,
  980 s) and 33/33 sim/squad tests, 0 warnings.

## Item 2 — Multi-faction battles (branch m14-factions, PR #23)

### What shipped

- **Scenario layer**: `armies` accepts 2-8 factions — fixed indices
  (red/blue/green/yellow/purple/orange/cyan/magenta), contiguous from
  Red; the parser rejects unknown keys, holes in the index set, and
  per-faction data (squads, spawn patterns) for factions an army does
  not field. `write_scenario` round-trips a three-faction file. The
  faction limit `kMaxFactions = 8` is a single validated constant.
- **Internals generalized**: every per-faction array member (spawn
  patterns, squad plans, HQ/objective/estimate arrays, timeline rows,
  reports, player-order reports) sized `kMaxFactions`; the spatial
  hash grid's buckets are flattened to `cell * faction_count + faction`
  with faction-scoped queries; perception, the utility AI, combat
  targeting, morale spread and the flow of order delivery all use
  any-enemy / per-faction semantics instead of Red-vs-Blue pairs.
- **Sector spawn default**: without an explicit spawn pattern, faction
  *i* of *n* deploys around the point at angle `pi + 2*pi*i/n` on a
  circle around the field center (Red west; at n = 2 Blue is exactly
  east, byte-identical to the band default). Since item 4 the
  directions come from `kSectorDirections` (hex float literals —
  identical bits on every libm).
- **Termination and results**: per-faction elimination counting with a
  last-standing winner; the timeline, faction reports and the state
  hash cover every fielded faction (the hash's player-order report
  block is per-faction at n > 2 — a format change from the 2-faction
  hash, which is unchanged).
- **11 tests** (`MultiFaction*`): parse acceptance/rejection shapes +
  round-trip, config validation (count rules, unfielded-faction data),
  sector-spawn determinism per seed, three-faction nearest-enemy vs
  brute force, a three-faction battle running to termination with all
  factions accounted, and the two-faction-unchanged pin.

### Byte-identity, and the two CI rounds that proved it the hard way

Every two-faction battle is byte-identical to pre-item-2: the final
tree passed the full suite (498/498) and a 13-export sweep vs the
`2a4c1ee` baseline (default/mixed/squads/demo-river/balanced-demo/
perf-5000 with and without `--swap-sides`, earth-lebanon
`--swap-sides`). Getting there took two CI fix rounds, both real bugs
the local gcc-14 run survived:

- **Round 1 — the formation axes**: moving the axes computation out of
  `assign_squads` left 2-faction *squad* battles with zero
  forward/lateral axes — formations collapsed toward the origin and
  every squad-dependent output diverged (six CI test failures plus the
  earth-lebanon export). `assign_squads` computes them again after its
  early return, at every faction count: a 2-faction battle computes
  them exactly when a squad plan runs and leaves them zero otherwise —
  the timing is byte-identity, the state hash covers these arrays.
- **Round 2 — the gcc-13 crash**: `update_morale`'s `routed_now` /
  `active_now` locals were still `kFactionCount`(2)-sized stack arrays;
  a Green unit's index-2 write is a stack-buffer-overflow that ASan
  catches, gcc-13 crashed on, and gcc-14 silently absorbed — the
  reminder that a green local run is evidence, not proof. Two 2-bounded
  loops went with it (faction estimates, the hash's player-order
  block) and `M_PI` became a `kPi` literal (MSVC does not define it).

### Honest misses

- The setup editor and the in-game faction colors stay two-faction;
  multi-faction battles are CLI/scenario-file only.
- `--swap-sides` at n > 2 is rejected (the mirror of 3+ factions is
  not well-defined).
- Cross-platform n>2 byte-identity arrived with item 4's direction
  table; before that the sector math used runtime `cos`/`sin`.
## Item 4 — Cross-platform report

Branch `m14-crossplat` (on top of item 2). The question: does the same
seed produce the same battle on every platform we ship CI for, and what
exactly is proven versus assumed?

### The matrix and what each job proves

| job | platform | compiler | runs |
| --- | --- | --- | --- |
| Build and test (headless) | ubuntu-24.04 | gcc | full suite + same-seed-twice + grid==brute |
| macOS build and test | macos-latest | clang (Xcode SDK) | **full suite**, including the committed M7/M13 export baselines |
| Windows build (compile + smoke) | windows-latest | MSVC | compile + this item's export pins (below) |
| ASan+UBSan / TSan / fuzzing / mapgen / window build | ubuntu-24.04 | gcc | memory model, fuzz corpus, tooling |

The strongest cross-platform evidence has always been the macOS job: it
runs `NoMapPinsTest.M7BaselinesReproduceByteForByte` and the M13
baseline suites — committed reference exports are re-produced
byte-for-byte by a different compiler, standard library and OS. The
Windows job was compile+smoke only (10 ticks, exit code): nothing about
its outputs was verified. This item closes that gap.

### The arithmetic inventory (why it should hold)

The simulation's entire math is IEEE-754-exact operations —
`std::clamp` (74 sites), `std::sqrt` (27), `min`/`max`, `floor`,
`ceil`, `fabs`, `round` — all of which IEEE 754 defines as exact or
correctly rounded, so every conforming platform produces identical
bits. The RNG is SplitMix64 integer arithmetic. The state hash and the
JSON export encode little-endian canonical bytes (m13 audit finding 5),
never host order. The one remaining libm dependency:

- `std::atan2` — three sites: a unit's facing at spawn
  (simulation.cpp, toward the objective), facing after each move
  (last_move), and the charge-angle comparison in combat. A libm that
  differs in the last ulp could in principle flip a facing/charge
  boundary and diverge a battle. Every platform we test (glibc, Xcode
  libm, MSVC UCRT) implements atan2 as a correctly-rounded-ish
  polynomial; the empirical answer below is the actual check.

### M14 additions this item ships

- **`kSectorDirections`** (factions.hpp): the multi-faction sector
  spawn directions as hex float literals — `[n][i] = (cos, sin)` of
  `pi + 2*pi*i/n`. Item 2 computed these with runtime `cos`/`sin`, whose
  last-ulp differences between libms would have shifted every
  multi-faction spawn position; the literals are the single source of
  truth, identical bits everywhere. (Two-faction battles never consult
  the table — west/east bands — so their outputs are untouched.)
- **Windows export pins** in the CI smoke job: `default.json` and the
  new `three-way.json` (the shipped 3-faction example), 300 ticks,
  seed 42, `--export-json` piped to `md5sum`, compared against the
  values produced by the Linux build. Green means MSVC reproduces the
  Linux export byte-for-byte — the first direct Windows byte-identity
  evidence in the repo.

### The empirical result

Both pins were produced by the Linux build of this branch's tree
(default `908322e6c9fc9a9c5923b0b748f8655f`, three-way
`95579bb578efe756ec7c215548fe2092`) and re-run identical; the CI job
compares the Windows builds against them on every push. Note the
three-way pin intentionally differs from the value in the branch's
first commit: that earlier pin was produced before the item-2
`update_morale` stack-overflow fix (m14 item 2, PR #23 round 2) — the
pre-fix bytes were the bug's corruption, deterministic on one compiler
but not a battle anyone wanted pinned. If a pin ever fails, that is a
real cross-platform finding to file, not a flaky test to retry.

### Verifying locally

```
./build/battle_cli --scenario data/scenarios/default.json --seed 42   --ticks 300 --export-json out.json
md5sum out.json   # == the pin in .github/workflows/ci.yml
```

### Honest limits

- `atan2` remains libm-dependent (three sites, listed above). Platforms
  outside the CI matrix are unverified; a conforming-but-different
  libm could diverge a battle through a facing boundary.
- The window build compiles on all three platforms but runs nowhere in
  CI (no display); its determinism is inherited from the shared core,
  not independently exercised.
- The Windows pins cover two scenarios at 300 ticks; the macOS full
  suite (with the committed baselines) remains the broader check.

## Item 3 — Scripted AI generals (branch m14-lua)

### What shipped

- **Lua 5.4.7 embedded** via FetchContent (pinned tag, built as C from
  the upstream sources, `lua_static`). The general sandbox registers
  only `_G` (base), `math`, `string` and `table`: no `io`, `os`,
  `package`, `debug` or `coroutine` — a script cannot reach the file
  system, the process or the loader (they compile but are never
  registered; pinned by `LuaGeneralTest.SandboxHasNoIOOrOS`).
- **`cli::LuaGeneral`** (cli/lua_general.hpp/.cpp): one instance per
  faction, each with its own state. The script must define
  `decide(g)`; every `--general-interval` ticks (default 30, before
  ticks N, 2N, ...) the CLI snapshots the battle and calls it.
- **The snapshot `g`** (docs/generals.md has the full table): tick,
  faction, own and enemy squads (`{id, x, y, members, commander,
  formation}`), and the living-enemy aggregate `{count, centroid_x,
  centroid_y}`. God view — the general sees the whole board;
  fog-of-war snapshots are future work.
- **Orders** return as `{squad = id | "all", order = "moveto" |
  "attackmove" | "hold" | "retreat" | "regroup" | "formation",
  x, y, formation}` and flow through the **existing m11b
  `issue_order` pipeline** — the same rate limits, courier delays and
  rejection warnings as order scripts and replays. Malformed returns
  fail the run naming the order index.
- **CLI**: `--general [faction=]script.lua` (repeatable; a bare path
  gives every faction its own instance of the same script — self-play),
  `--general-interval N`. Needs `--orders on`; rejected together with
  `--record`/`--replay`/render modes/`--tactic-scan`/`--script` (a
  replayed battle re-runs its generals live).
- **Determinism**: no `--general` flags -> the code path never runs
  (byte-identity pinned by the whole existing suite, 498/498 + the
  13-export sweep of items 1-2). With a general: same script + seed +
  scenario -> same orders -> same battle, per-tick state hashes
  compared in `LuaGeneralTest.ScriptedBattleIsDeterministic`. Scripts
  using `math.sin/cos/atan` inherit the libm caveat of item 4.
- 7 new tests (`LuaGeneralTest.*`): load errors, the sandbox, the
  snapshot contents, order-parsing shapes, malformed-order rejections,
  end-to-end determinism, delivery counters moving. docs/generals.md
  documents the API, sandbox and determinism contract.

### Honest misses

- Fog-of-war: the snapshot is the full board. The M4 perception layer
  would need a per-faction visibility aggregate exposed per squad;
  deferred (the god view is explicitly documented, and no script can
  see anything the export does not contain).
- `--record`/`--replay` with generals: rejected rather than recorded —
  a replay would need the script itself embedded in the replay file to
  re-run identically; deferred with the flag guard.
- The window does not host generals (CLI only); the setup editor stays
  2-faction (item 2's documented limitation).
