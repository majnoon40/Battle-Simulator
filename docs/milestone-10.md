# Milestone 10 — Tactical depth, variety, battle reports and replay

Part 1 (tactical depth and variety) and Part 2 (battle reports and
replay). `--variance off` (and every M10 flag default-off) reproduces
every M1-M9 reference byte-for-byte — the pinned baselines in
`tests/baselines/m8` still match, bit for bit.

# Part 1 — Tactical depth and variety

## What was built

**Counter-based randomness (order-independent).** All battle-time
randomness comes from the pure function `tactics_roll(seed, unit_id,
tick, channel)` — SplitMix64 mixing, no shared stream — so no result
can depend on iteration order and shuffling the unit vector cannot
change any roll. Channels (docs/ai.md): 0 combat hit, 1 combat damage,
2 cooldown jitter, 3 decision interval, 4 composition range draw,
5 spawn pattern jitter. The M1-M9 spawn stream is untouched (and the
M1-M9 battle had no battle-time randomness at all, which is why the
off-path is byte-exact by construction). Tested: order independence
with shuffled units, uniformity over 100k draws, per-channel distinct
streams.

**Combat variance (per unit type, data/units.json, validated).**
`accuracy` (hit chance; a miss still costs the cooldown),
`damage_variance` and `cooldown_jitter` (both spread by
`1 + v*(2*roll-1)`, cooldown at least 1 tick). Defaults absent =
deterministic M1-M9 combat. Shipped values: infantry 0.85/0.15/0.10,
archer 0.80/0.20/0.10, cavalry 0.90/0.10/0.15, commander 0.85/0.15/0.10.
`--variance on|off` (default on, utility mode only). Tested: accuracy
0.5 lands 40-60% of attacks over 800 draws; variance 0.3 keeps every
hit in [0.7, 1.3]x base with the mean within 8%.

**Decision intervals.** Each unit re-scores every 1-5 ticks (drawn once
at spawn from channel 3); between decisions the committed action
persists like the M4 commit time, which the interval replaces. Units
react with a personality-staggered delay. Tested: intervals in [1, 5],
all five values drawn.

**Memory and squad intel (data/tactics.json, validated).** A unit
remembers the position of the nearest enemy it last saw;
`memory.forget_ticks` (shipped 1200 — measured: remnants of
equal-army battles outlive shorter memories and the battle re-stalls)
bounds the memory. Squad members inside the acting commander's
leadership radius publish contacts into a shared list (freshest 8,
newest first); readers honor `squad_intel_delay_ticks` (12). Memory
first, Advance after forgetting (the spec's fallback semantics — with
a fresh contact, Advance is not available). Tested: the exact delay
boundary, forgetting at the exact tick, sharing only in reach.

**New actions** (scores in [0, 1], weights sum to 1.0, ties by the
fixed enum order; formulas in docs/ai.md):
- **Search** — move to the freshest own/squad contact;
  `0.55 + 0.25*(1-d/sight) + 0.20*(1-age/forget)`.
- **Help** — toward the neediest visible ally (health < 0.5 or damaged
  last tick) within 40, local strength share >= 0.8;
  `0.50 + 0.30*(1-ally_health) + 0.20*(1-d/radius)`.
- **Flank** — squad frontally engaged (a member saw an enemy last
  tick), local ratio (allies+1)/(enemies+1) >= 1.25, member under the
  per-squad cap (first 4 soldiers; commander never); moves to a point
  25 units perpendicular to the squad-enemy line (side by member
  parity); `0.65 + 0.20*strength`.
- **Pursuit** — a bounded bonus on Approach against the routed nearest
  enemy (`0.25`, x2 for speed >= 4.0), bounded by 240 consecutive
  ticks and 300 units from the pursuit anchor; past either bound the
  pursuer disengages (the routed enemy no longer drives Approach, and
  its memory is suppressed), and switching targets resets. Tested:
  the counter caps at max_ticks and the disengage path.
- **Regroup** — Advance gains `0.20*(1-cohesion)` when cohesion < 0.35
  with no enemy visible (Advance's movement already seeks formation
  slots).

**Spawn variety (scenario opt-in).** `"spawn_pattern"`: band (default,
M1-M9 behavior and RNG stream), line, columns, echelon, cluster —
deterministic placement inside the band (cluster jitter from channel
5). Army counts may be `[min, max]` ranges drawn per seed (channel 4).
Existing scenarios are untouched. Tested: every pattern stays inside
its band, patterns differ, ranges draw in-range and reproduce per
seed. New scenario `data/scenarios/m10-line.json` (250v250 infantry,
line) demonstrates it.

**Side swapping and balance tooling.** `--swap-sides` mirrors the
setup (bands, compositions, explicit objectives, squad plans, spawn
patterns). `tools/analysis/balance.cpp` runs N seeds in both
orientations and reports per-orientation splits, the side-averaged win
rate and the orientation effect.

## Acceptance (real numbers, logs/m10-*.log)

Baselines first (current code, `--variance off`, byte-identical to M9
by test and by cmp against tests/baselines/m8):

| scenario | winners | pre-limit | end tick mean/sd/CV | deaths |
| --- | --- | --- | --- | --- |
| viewer-demo 100 seeds | Red 41 / Blue 58 / draw 1 | 83% | 779.6 / 1005.4 / 1.290 | 87 |
| default 250v250 100 seeds | Red 47 / Blue 53 | 0% | 3000.0 / 0.0 / 0.000 | 289 |
| earth-* (M9, 20 seeds) | Lebanon Blue 19/1, Alps Blue 15/5, Nile Red 20/0 | — | — | — |

With M10 (`--variance on`, defaults + forget 1200):

| scenario | winners | pre-limit | end tick mean/sd/CV | deaths |
| --- | --- | --- | --- | --- |
| m10-line 250v250 (line pattern), 100 seeds | Red 50 / Blue 48 / draw 2 | **94%** (all faction_routed) | rout quartiles 297/303/306/312/549 | 68 |
| default 250v250 (band spawn), 100 seeds, Part 1 | Red 43 / Blue 54 / draw 3 | 2% | 2946.3 / 375.9 / 0.128 | 129 |
| **default 250v250, 100 seeds, + contact seek (final)** | Red 46 / Blue 54 | **81%** (81 faction_routed) | rout quartiles 316/323/333/746/2527 | 61 |
| squads 260v260 (M6 setup), 100 seeds | Red 44 / Blue 53 / draw 3 | 21% (M6 baseline 18%) | rout quartiles 326/338/354/366/2183 | 64 |
| viewer-demo, 100 seeds, Part 1 | Red 44 / Blue 56 | 90% | 609.1 / 814.1 / 1.337 | 75 |
| viewer-demo, 100 seeds, + contact seek (final) | Red 48 / Blue 51 / draw 1 | **99%** | rout quartiles 317/328/335/342/1180 | 35 |

Side-averaged open symmetric map (default + swapped, 200 seeds, Part 1):
Red 47.0% — inside the 40-60% target; orientation effect 8 points.
With contact seek the default scenario ends 81% before the limit (81
faction_routed, quartiles above), meeting the >=80% target without
changing the scenario.

**The >=80% target on 250v250: MET on the default band-spawn scenario
(81%) with the M10-continuation contact-seek fix, and on the line
deployment (94%).** The Part 1 diagnosis (20 seeds): first contact is
reliable (tick 167-173), but by tick 400 the armies have passed
through each other (0% of units have an enemy within the 40-unit
sight; centroids 450+ apart) and by tick 600 they stand on their own
last-sighting memories searching in place (68k Search decisions, no
movement) until the memories expire (~tick 1500), then creep to the
swapped objectives and sit. The fix (docs/ai.md): a faction-level
enemy estimate (mean of members' last-seen nearest-enemy positions,
refreshed on any sighting); a unit contact-less for idle_ticks = 100
escalates its freshest contact to that estimate, so both sides
converge back on the contact zone and re-engage. Deterministic, no
randomness; --contact-seek off (default on) is byte-identical to the
Part 1 binary (cmp-verified), --variance off to M9.

**--ai-debug examples** (live captures from viewer-demo seed 42 via the
new `--tactic-scan <action>` flag, saved under download/m10/): a unit
that searches (unit 1505, tick 227, no visible enemy, fresh contact,
score 0.770), one that flanks (unit 907, tick 204, enemy at 26.4,
flank rights, score 0.797) and one that helps an ally (unit 338, tick
221, ally in need, score 0.822).

**Determinism and performance:** double runs with variance on are
byte-identical (cmp); grid == brute (byte-identical exports);
`--variance off` reproduces the M9 pin byte-for-byte, and
`--contact-seek off` the Part 1 binary (both cmp-verified). Throughput
with the final code on idle CPU: 2,080 units 252 t/s, 5,000 units
76.4 t/s (>= 65; no optimization needed), 20,000 units 22.7 t/s.

**Earth balance (Lebanon, 40 seeds per orientation):** before (Part 1
binary) Red 18.8% side-averaged, 5% of normal-orientation battles end
pre-limit; after (contact seek) Red 21.2% side-averaged (not worse),
80% pre-limit, mean end tick 2931 -> 1264. The swapped orientation
stalls in both runs (a pre-M10 Lebanon deployment asymmetry). The Alps
and Nile before/after runs were cut off; re-run with
`BATTLE_CLI=./build/battle_cli.p1 ./build/balance --scenario earth-<r>
--seeds 40` and without the override for the final binary.

## Tests

22 new tests in tests/test_tactics.cpp (RNG order independence and
uniformity, decision intervals, config validation, score bounds over
10k random inputs, shuffled-unit order independence, byte-identical
double runs with variance on, variance-off self-consistency and
on-vs-off difference, accuracy and damage statistics, memory decay at
the exact tick, no-memory-never-searches, Help fires/does not fire,
Flank fires under the cap (never above), pursuit counter bounds,
metamorphic far-unit score invariance, spawn patterns in-band and
distinct, composition ranges in-range and per-seed reproducible,
pattern/range validation, squad intel delay boundary). Full suite:
289/289 (all 267 M1-M9 tests unchanged and green).

## Part 2 pre-step — Lebanon asymmetry, Alps/Nile balance, housekeeping

- scripts/ moved into tools/analysis/ (cross-references updated);
  download/, logs/ and the old accidentally-committed `core` dump are
  untracked + gitignored (artifacts stay on disk).
- Lebanon asymmetry (report only, no code change): the scenario's
  armies are IDENTICAL (250 archer / 200 cavalry / 40 commander / 550
  infantry per side; band fraction 0.35 both; objectives ~equidistant),
  so the asymmetry is geometric. Final binary, 40 seeds/orientation:
  normal Red 17 / Blue 23 / 0 draws (80% decided pre-limit, mean end
  tick 1264; Blue wins 72% of decided battles); swapped 0/0/40 — every
  battle a tick-limit draw. Winner at the limit = more alive units
  (even = draw); full rout or annihilation decide earlier. Cause
  analysis (lebanon.png, 400x400 cells): the west band is 73% water /
  ~25% passable (narrow coastal plain), the east band 96% hills +
  mountain (2% grassland); Blue's objective sits on grassland, Red's on
  a mountain cell; the interior fords (Litani) sit at x 342-465,
  y 700-965 — off both direct march axes — and the coastal river at
  x ~250-290 has no fords at all. In swapped orientation both armies
  make ZERO net progress for ~2900 ticks (3.1M pure Advance decisions
  each, zero approach/attack/search; probe seed 1: Red packed into the
  x 816-831 valley, Blue jammed against the x 250-290 river; BFS
  reachability is True from both spawn clusters) — a stuck-handling
  bottleneck jam, not unreachable objectives. Alps shows the same
  swapped pattern (0/0/20 at tick 1500). M11 candidates: per-orientation
  band deployability validation, fords on every river segment crossing
  a march axis, no-progress detection.
- Alps-Innsbruck balance, 20 seeds/orientation, --ticks 1500:
  normal Red 3 / Blue 16 / none 1 (5% pre-limit, mean end 1493);
  swapped Red 0 / Blue 0 / none 20; side-averaged Red 7.5%.
- Nile-Delta balance, 20 seeds/orientation, --ticks 1500: normal
  Red 15 / Blue 5 / none 0 (45% pre-limit, mean end tick 1183); swapped
  Red 0 / Blue 0 / none 20; side-averaged Red 37.5%. The swapped-
  orientation total stall is thus universal across all three earth maps
  (Lebanon 0/0/40, Alps 0/0/20, Nile 0/0/20); which side wins normal
  orientation depends on the map (Lebanon/Alps favor the east band,
  Nile the west).
- Events overhead re-measured on an idle machine (perf-5000, 300 ticks,
  alternating off/on): 12.47 vs 12.42 t/s = ~0.3% (within noise);
  517 KB of JSONL per 300 ticks.

- `--events out.jsonl`: deaths (with killer), routs, recoveries,
  commander deaths, successions, order changes, flank/help/pursuit/seek
  starts, squad collapses. A recorder changes nothing: exports with and
  without it are byte-identical (tested), event streams are
  deterministic (tested).
- `--export-csv out.csv` (`--csv-interval`, default 10): alive/routed/
  dead/avg-morale/engagements per faction plus per-type alive counts.
- `--report out.md` + `.json`: outcome, casualties by type, morale
  timeline, routing statistics, squad performance (kills, losses,
  cohesion, orders), commander events, and turning points from measured
  rules (first 50-tick 2:1 kill window, first commander death, first
  10%-in-20-ticks rout wave). A test maps every template number to a
  recorded field.
- `--report-png out.png`: casualty + morale curves via the software
  rasterizer.
- 300/300 tests (8 new). Samples under download/m10p2/ (not committed).
- Events overhead at 5,000 units (perf-5000, 300 ticks, measured under
  background load so only the on/off ratio is meaningful): 9.13 t/s off
  vs 9.06 t/s on, ~0.7%, within run-to-run noise; 517 KB of JSONL.


## Part 2b — replay (docs/reports.md)

- `--record out.replay`: setup (sim version, scenario, mod, seed, tick
  limit, variance/contact-seek/neighbor/squads/swap flags),
  `--intervene kill:UNIT:TICK` interventions, and a canonical state
  hash (FNV-1a 128, field-by-field serialization of all future-
  relevant state) every 25 ticks + the final one.
- `--replay in.replay --verify`: re-runs from the recorded setup,
  re-applies interventions, compares every checkpoint; prints the first
  mismatching tick on divergence (exit 2). A different sim version is
  refused with a clear error (exit 1). `--seek T` re-runs from the
  start to T (cost O(T), no snapshots) and combines with --export-json.
- Verified by hand: default / demo-river / earth-lebanon record+verify
  (13/32/32 checkpoints), one flipped hash digit -> "first mismatching
  tick: 25", version edit -> refusal, intervention recorded and
  reproduced (the battle measurably changes: 51 -> 53 Red deaths).
- 309/309 tests (9 new: identical re-run hashes, divergence detection,
  file round-trip, malformed-file rejection, 3-scenario CLI record+
  verify, flipped-byte mismatch, version refusal, intervention
  reproduction, seek stop).
- Untracked the accidentally committed `core` dump file (kept on disk,
  gitignored, like download/ and logs/).
