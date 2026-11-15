# Milestone 11a — Navigation robustness

The M10 Part 2 pre-step measured that with `--swap-sides` every earth-map
battle stalled to the tick limit (Lebanon 0/0/40, Alps 0/0/20, Nile
0/0/20 at 1500 ticks). This milestone diagnoses the stall with numbers,
fixes it with one coherent design (`--nav v2`, default on; `--nav v1`
reproduces the M10 binary byte-for-byte), hardens mapgen (balanced bands,
march-axis ford checks, march-cost and both-orientation connectivity
metadata, `-v2` map variants), and adds the acceptance harness.

## Part A — the diagnosis (measured, not guessed)

Static analysis (tools/analysis/nav_diagnose.py, committed): every spawn
band is fully reachable — zero unreached cells on all three maps in both
orientations — so the M10 "BFS says reachable" observation was correct;
routing was never the problem. But the flow-field paths funnel hard:
Lebanon west→east sends 74.6% of all band cells' shortest paths through
two adjacent ford cells ([164,342]+[165,341], the Litani line),
east→west 50.7% through a single cell; the Alps funnels 100% through a
three-cell mountain valley. Median flow-field march costs (time at speed
1): Lebanon 536–807, Alps ~1791, Nile ~388–400 ticks.

Runtime traces (the new `--trace`/`--trace-interval` CLI flags: per-faction
centroid, mean move, still fraction, Advance fraction, x histogram, pure
observation — a traced battle is bit-identical) found the two real causes:

1. **The explicit-objective swap was a no-op.** The CLI's `--swap-sides`
   block looped over both factions and swapped the objectives twice —
   the second swap undid the first (the same loop shape also un-swapped
   the spawn patterns). Every earth scenario sets explicit objectives,
   so a swapped battle sent each army to the objective inside its own
   spawn band: it walked home and parked. Swapped Lebanon seed 1: all 40
   Red commanders ended on Red's own objective cell (823.75, 501.25),
   all 40 Blue commanders on Blue's (253.75, 563.75); zero contact,
   zero deaths, 1040 survivors per side, 1500-tick draw.
2. **Formation members never navigate terrain.** `steer_in_formation`
   moves a member toward its geometric slot (slot seeking + cohesion +
   alignment + separation); only commanders and squadless units follow
   the flow field. In corridors the projected slots bunch onto the few
   passable cells and the beeline jams: Blue milled in a 31-unit
   coastal strip at a mean 1.6 units/tick with zero net displacement for
   1100 ticks (separation vs. blocked slot-seeking), Red parked behind
   mountain-crawling commanders at 0.036 units/tick with 81% of the army
   below the stuck epsilon. Members' moves were not goal-seeking, so the
   stuck accounting could not even see them — `units_stuck_over_100`
   stayed 0 while 2080 units froze.

A third observation closed the case: the M10 stuck-recovery mechanism
(20 stuck ticks → follow the field for 60 ticks) is a no-op for Advance
units, which already follow the field.

## Part B — the fix: field-first formation march (`--nav v2`)

One coherent design, documented in docs/terrain.md:

- `cli::apply_side_swap` (extracted from the CLI, unit-tested) swaps
  compositions, squad plans, explicit objectives and spawn patterns —
  each exactly once.
- A squad member whose action is Advance and whose projected slot is
  farther than `kSlotMarchDist` (6 world units) follows the faction flow
  field — the same terrain-aware field the commander uses — instead of
  beelining to the slot. Near the slot, shape steering takes over, so
  formations re-form on open ground and column through corridors. The
  march is goal-seeking, so the stuck accounting finally sees members.
- No congestion term in the field: the stall was steering, not routing
  (bands are fully reachable; with members on the field, squads pass the
  measured gates at terrain speed). Keeping the field static preserves
  determinism and costs nothing.

`--nav v1` gates every change above; a replay file records its
generation (`nav=v1|v2`; pre-m11a files parse as v1, so they replay
under the exact behavior they were recorded with). kSimVersion is bumped
to 11.0.

First measured effect (swapped Lebanon, seed 1, 1500 ticks): 1500-tick
draw → Blue wins by faction rout at tick 827.

## Part C — mapgen `--v2`

`mapgen.py --region <r> --v2` writes `<region>-v2.png`,
`<region>-v2.meta.json`, `earth-<region>-v2.json`:

- **Balanced spawn bands**: the smallest fraction whose sides' passable
  shares and mean move costs agree within 10%. Alps and Nile pick 0.15
  (balanced); Lebanon records `null` — its coastal strip vs. mountains
  is unbalanceable at every band width, and the meta says so instead of
  pretending.
- **March-axis fords**: a ford patch on every river segment crossed by a
  march axis that has no ford within 12 cells. On the shipped regions
  the axes cross no unserved river — Lebanon and the Alps axes cross no
  river at all (the Litani runs south of both axes; the Alps axis runs
  along the Inn valley), and the Nile's six axis crossings all sit
  within 12 cells of an existing ford — so no new fords are stamped; the
  check runs on every regeneration and the counts are in the meta.
- **March cost + connectivity**: median flow-field cost per march, the
  ratio between them, and whether every passable band cell reaches both
  objectives in both orientations (`connectivity` — true on all six
  v2 configs). The port was cross-validated against the independent
  static diagnosis (identical medians).

Both regenerations are byte-identical: the v1 files regenerate to the
shipped M9 bytes (verified for all three regions), and the v2 files
regenerate to themselves.

## Part D — verification

- **v1 byte-identity**: `--nav v1` replay hash checkpoints equal the
  main-branch (M10) binary's on default (no map), demo-river (map, no
  squads) and earth-lebanon swapped (map + squads + explicit
  objectives), 14/19/24 checkpoints.
- **Determinism**: v2 double runs byte-identical (exports, `cmp`);
  grid == brute byte-identical; the full M7 byte-pin baselines pass
  unchanged (no-map battles are identical under v1 and v2 — asserted by
  a test).
- **Performance**: Lebanon swapped, 1500 ticks, median of 3: v1 20.9 s,
  v2 3.8 s (5.5x faster — the v1 stall battles churn every unit through
  separation queries for the full limit; v2 resolves). No-map battles
  are code-identical under both flags.
- **Reports**: `first_contact_tick`, `units_stuck_over_100` and
  `longest_stuck_stretch` in the markdown and JSON-twin outcomes; a
  `first_contact` event (once per battle, the seer's id) in `--events`.

### Acceptance sweep (tools/analysis/nav_acceptance.py; 1500-tick limit)

v2 maps, 40 seeds per orientation:

| map + orientation | red/blue/draw | pre-limit | contact ≤ 1200 | mean end | stuck >100 ticks |
| --- | --- | --- | --- | --- | --- |
| lebanon-v2 | 40/0/0 | **85.0%** | 100% | 813 | 0.00% |
| lebanon-v2 swap | 6/34/0 | 50.0% | 100% | 1283 | 0.00% |
| alps-innsbruck-v2 | 38/2/0 | **65.0%** | 100% | 1354 | 0.00% |
| alps-innsbruck-v2 swap | 2/37/1 | 57.5% | 100% | 1276 | 0.00% |
| nile-delta-v2 | 36/4/0 | **67.5%** | 100% | 991 | 0.00% |
| nile-delta-v2 swap | 15/25/0 | 12.5% | 100% | 1456 | 0.00% |

Side-averaged Red win shares: Lebanon 57.5%, Alps 50.0%, Nile 63.7%
(target 35–65%: met on all three). Longest stuck stretch anywhere: 20
ticks (wall-slide epsilon scale), far below the 100-tick threshold.

**Honest misses:** the ≥60% pre-limit target holds on all three normal
orientations (Alps improved from M10's 5%) but not on the swapped ones
(50% / 57.5% / 12.5%). Swapped battles now genuinely happen — 100%
contact, decisive winners, zero stuck — but both armies must cross the
whole map before meeting, and the meeting battles outlast the limit on
Nile in particular. That is combat duration, not navigation: pushing it
would mean tuning casualty or morale rules, which is outside m11a
(scope: navigation) and would touch every reference. Reported as
measured; a candidate for a later milestone.

Old (M9) maps under v2, 20 seeds per orientation, for comparison:

| map + orientation | red/blue/draw | pre-limit | contact ≤ 1200 | mean end | stuck |
| --- | --- | --- | --- | --- | --- |
| lebanon (M9 map) | 20/0/0 | 80.0% | 100% | 859 | 0.00% |
| lebanon (M9 map) swap | 3/17/0 | 55.0% | 100% | 1263 | 0.00% |
| alps-innsbruck (M9 map) | 19/1/0 | 60.0% | 100% | 1367 | 0.00% |
| alps-innsbruck (M9 map) swap | 2/18/0 | 40.0% | 100% | 1348 | 0.00% |
| nile-delta (M9 map) | 19/1/0 | 70.0% | 100% | 970 | 0.00% |
| nile-delta (M9 map) swap | 5/15/0 | 10.0% | 100% | 1455 | 0.00% |

(M10 measured the same orientations at 0/0/N with zero contact — the
stall this milestone fixes.)

## Tests

13 new C++ tests (322 total, all green): `--nav` parsing and defaults,
`--trace` parsing and bounds, `apply_side_swap` (objectives swap once,
compositions/plans/patterns swap, defaults untouched), first-contact
tick + event (close/far/no-map), the no-map v1==v2 byte identity, the
corridor member-march regression (v2 carries the army through a walled
gap; v1 jams or dies), the CLI-level swap regression (earth-lebanon
swapped: v1 stalls with no contact, v2 makes contact), and the replay
nav flag (round trip, pre-m11a default v1, bad value rejected). mapgen:
4 new test classes (axis fords stamp/skip/sea, Dijkstra cost monotone
and sealed-wall infinity, v2 pipeline meta, v2 determinism + v1 meta
byte-compat), 26 Python tests total.
