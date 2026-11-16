# Milestone 11a — Navigation robustness

The M10 Part 2 pre-step measured that with `--swap-sides` every earth-map
battle stalled to the tick limit (Lebanon 0/0/40, Alps 0/0/20, Nile
0/0/20 at 1500 ticks). This milestone diagnoses the stall with numbers,
fixes it with one coherent design (`--nav v2`, default on; `--nav v1`
reproduces the M10 binary byte-for-byte), hardens mapgen (balanced bands,
march-axis ford checks, march-cost and both-orientation connectivity
metadata, `-v2` map variants), and adds the acceptance harness.

## Part A — the diagnosis (measured, not guessed)

**Correction of the M10 explanation.** M10 attributed the swapped-
orientation stall to terrain — "a stuck-handling bottleneck jam" in a
valley / against a river, with the fords "off both direct march axes".
That explanation was wrong. The armies never tried to cross anything:
the `--swap-sides` objective swap was a no-op, so each army walked to
the objective inside its **own** spawn band and parked — the M10 probe's
"Red packed into the x 816-831 valley, Blue jammed against the
x 250-290 river" were the parking positions around each faction's own
objective, not route jams — and the formation members that did press
against terrain were beelining to projected slots, not following the
flow field (which was valid and the bands fully reachable all along).
The swapped-orientation and orientation-effect numbers in
[docs/milestone-10.md](milestone-10.md) are marked invalid accordingly:
they measured a setup bug, not an orientation effect. The valid M11a
orientation numbers are in Part D below.

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

# Milestone 11b — Player orders

Army-level player commands through the commander system, per the
design: broad orders, delayed delivery, imperfect obedience, bounded
score terms, full determinism and replay.

## API and semantics

`issue_order(faction, squad_ids, type, x, y, formation)` with MoveTo,
AttackMove, Hold, Retreat (optionally to a point), Regroup and
ChangeFormation (docs/squads.md documents every rule). The CLI applies
timed orders from `--script orders.json` headlessly; `--orders off`
(default on) makes orders inert; with no order issued every battle is
byte-identical to the m11a binary (verified by export comparison, and
asserted by a test).

- **Delivery**: courier delay ceil(distance(HQ, acting commander) x
  order_delay_per_unit) from the faction's spawn-band HQ, fixed at
  issue time; + succession_delay_ticks through a successor;
  + leaderless_delay_ticks at leaderless_obedience strength when
  nobody can lead. Delivered into the squad's order slot (the
  commander AI stands down while it is active), propagated to members
  with the standard distance delay, inherited by successors, ended by
  replacement or collapse. Rate limit: max_per_tick calls per faction
  (a call may address many squads; "all" is one call).
- **Point orders** (MoveTo / AttackMove / Retreat-to-point): a
  squad-specific flow field (the same Dijkstra as the faction field,
  computed once at delivery) redirects the m11a member field-march,
  the commander's Advance and the stuck-recovery path; MoveTo adds a
  small keep-marching Hold bias while enemies are visible.
- **Obedience**: morale x discipline x cohesion (the spec's factors —
  the distance is spent on the courier delay; with the M6
  distance-to-commander factor in, measured mean obedience was 0.16
  and a hold order moved 12% of the army). The score bonus is
  weight x weight_scale (4.0) x (0.5 + 0.5 x obedience), clamped to
  [0, 1] per action — never an availability override. Routed units
  keep the forced Flee and never see the terms. Measured calibration
  on the corridor fixture: a delivered Hold flips 29% of an aggressive
  army to Hold (0% in the control), slows the march measurably;
  cautious squads hold solidly; the most aggressive, healthy units
  still refuse.
- **Replay**: `--record` writes `order` lines (replays gain
  `orders=on|off`; pre-m11b files parse as on), `--replay --verify`
  re-applies them; the state hash covers the queue, the squad
  player-order state and the squad field targets. kSimVersion 11.1.
  `order_issued` / `order_delivered` events; report and export
  (format 8, only when orders were issued) carry the counters.

## Tests

14 new tests in tests/test_orders.cpp (336 total, all green): wrong
faction / unknown squad / bad formation rejected with nothing
half-applied; the per-tick rate limit (per call, per faction); orders
off makes issue inert; the exact courier-delay formula per squad and
non-decreasing delivery with distance; commander death routing through
the successor (exact expected tick); Hold measurably slows the march;
MoveTo crosses a wall through the gap to the point; ChangeFormation
changes the shape; routed units ignore orders; double runs
byte-identical and grid == brute with orders; no-orders battles
byte-identical with the machinery on or off; the script parser
(valid + five malformed cases); the replay file round trip (orders +
legacy default); and the CLI end-to-end (script changes the battle,
the replay records `order 20 red all moveto 100 900`, --replay
--verify passes).

## Acceptance — flank order vs control

Scripted scenario (tools/analysis/order_acceptance.py): Red's outer
ten squads MoveTo to the north flank at tick 60, then AttackMove into
Blue's rear at tick 450; the control is the identical battle with no
orders. earth-lebanon-v2, 1500-tick limit, 100 seeds per arm:

| arm | red / blue / draw | mean end tick | mean Red deaths | mean Blue deaths | mean first contact |
| --- | --- | --- | --- | --- | --- |
| control (no orders) | 100 / 0 / 0 | 860.9 | 6.8 | 27.5 | 60.9 |
| flank order | 98 / 2 / 0 | 837.6 | 7.3 | 27.4 | 60.9 |

The order measurably changes the battle: 98 of the 100 flank battles
end 23.3 ticks faster on average (the flank arriving in Blue's rear
accelerates the collapse), at the cost of 2 battles flipping to Blue —
detaching a quarter of the army from the main line occasionally lets
the weakened center lose — and +0.5 mean Red deaths. On this baseline
Red already wins 100% of the control battles, so there is no headroom
for the maneuver to add wins; the numbers are reported as measured.
(The tool prints per-arm aggregates and the deltas:
tools/analysis/order_acceptance.py.)

## Determinism and performance

- Full suite 336/336 (Release); ASan+UBSan 336/336.
- With no orders issued the m11b binary reproduces the m11a binary's
  export byte-for-byte (asserted + verified against the m11a build).
- A delivered point order costs one Dijkstra (~160k cells) per squad
  per order, at most max_per_tick per faction per tick; orders change
  no per-tick steady-state cost (the member march replaces a separation
  radius query with a field lookup).
