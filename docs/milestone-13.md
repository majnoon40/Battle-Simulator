# Milestone 13 — combat depth, deterministic threads, UI completion, fuzzing, packaging

Working notes, one section per item, evidence first. Updated as items land.

## Item 0 — state check (report only)

### PRs and main

| PR | Title | State |
| --- | --- | --- |
| #4 | m11c — UI framework (widgets, layout, pure reducer, --ui-script harness) | merged |
| #5 | m11d — sandbox screens (menu, scenario picker, battle controls) | merged |
| #6 | m12 audit — ordered armies fight again; flat-map point orders; collapse ends orders; parser hardening | merged |

main is at the merge of #6 (sim version 11.2), all CI jobs green.

### The 11d spec versus what exists

| Feature | Status |
| --- | --- |
| UI framework: widgets, layout, pure reducer, DrawList drawing | done (m11c) |
| Menu / scenario picker / battle-control screens, App reducer | done (m11d) |
| `--ui-script` harness with `screen`/`scenarios` | done (m11d) |
| Window wiring: raylib input -> events -> reducer -> DrawList, screen switching, scenario reload | done in this milestone (item 1; the m11d patch was lost to a sandbox reset and re-derived) |
| Army editor with drag-to-place spawn | not done (item 5c) |
| Minimap (terrain thumbnail, unit dots, click to jump) | not done (item 5a) |
| Order UI (right-click MoveTo, AttackMove modifier, Hold/Retreat/Regroup hotkeys, order markers) | not done (item 5a) |
| End screen (result, curves, save replay/report, rematch) | not done (item 5b) |
| Replay viewer (list, scrub, verify) | not done (item 5b) |
| Settings (resolution, fullscreen, vsync, UI scale, colorblind palette) | not done (item 5c) |
| Scenario save/load through data-file validation | not done (item 5c) |

### Orders study re-run (final code, main 1993a90)

Five arms — none / AttackMove (all squads, enemy centroid at t150) /
flank (squads 0-2 north flank at t60, then AttackMove into the rear at
t400) / hold (all squads at t100) / retreat (all squads at t150) — 200
paired seeds x 2 orientations (the ordered faction is Red in both, so
side-averaging removes the spawn-band asymmetry), squads scenario, 1500
ticks (script: `tools/analysis/m13_orders_study.py`; raw rows:
`download/m13/orders_study_200seeds.json`).

| arm | win share [95% CI] | mean end tick | kill ratio |
| --- | --- | --- | --- |
| none | 0.487 [0.439, 0.536] | 937.5 | 1.589 |
| attackmove | 0.560 [0.511, 0.608] | 913.9 | 2.141 |
| flank | 0.568 [0.519, 0.615] | 1015.3 | 1.712 |
| hold | 0.003 [0.000, 0.014] | 1462.8 | 0.046 |
| retreat | 0.003 [0.000, 0.014] | 1173.0 | 0.072 |

Paired per-seed deltas vs control: attackmove kill ratio +0.55
[+0.29, +0.82] (significant, favorable); flank +0.12 [-0.07, +0.31]
(win share significant, kill-ratio delta not); hold -1.54, retreat
-1.52 (both strongly unfavorable; the arms also stretch battles +525 /
+236 mean ticks). After the m12 audit the march orders are healthy
orders; the losing arms are battle dynamics (see the hold diagnosis)
plus the termination gap they expose.

### Hold diagnosis (why the kill ratio is ~0.05, with evidence)

Probe: `tools/analysis/m13_hold_probe.cpp` replicates the hold arm (squads
scenario, all Red squads Hold at t100) and measures per tick, per unit,
whether an enemy is inside attack range and what the unit chose.

- **The order does not suppress fighting.** When a holding unit has an
  enemy inside attack range it chooses Attack 81.5% of engaged
  unit-ticks (seed 7; 86.3% seed 23); the rest is the cooldown dip
  (Attack briefly unavailable after each swing). The m12 fix holds.
- **The kill ratio comes from a rout cascade, not from refusing to
  fight.** Timeline (seed 7): first contact t~250; by t450 93/228 alive
  Red units are routed; by t550 193/216. The stationary blob's perimeter
  is focus-fired — melee targeting picks the nearest enemy, and a
  standing target stays the nearest for many attackers at once — so
  deaths concentrate in a small area, ally-death morale loss compounds
  (the blob keeps its dead nearby), and the rout wave rolls inward.
  Routed units cannot attack (forced Flee) and are run down: final 56
  Red dead vs 1 Blue (kill ratio 0.018 this seed; 0.042 seed 23; the
  study average is ~0.05).
- **Exposure is one-sided.** Standing (non-routed) Red accumulates only
  1071 engaged-unit-ticks across the whole battle (seed 7) while Blue's
  attackers accumulate 54,137: after the perimeter routs, the next
  standing units are engaged and routed within ticks, while every Blue
  attacker cycles through the front.
- For comparison, in the control run the exchange is a real fight: Red
  attack-when-engaged 32.1% vs Blue 35.2% (both cooldown-limited), kill
  ratio 1.38, and the loser's mass rout happens at the end, not in the
  first 200 ticks of contact.

Conclusion: "Hold on open ground loses" is battle dynamics (a
stationary, concentrated force is focus-fired and routed), not an order
bug. Items 2 (termination: a mass-routed faction ends the battle) and 3
(facing/flank damage: surrounding gets rewarded, but a disciplined line
with flanks secure fares better) both act on this evidence.

## Item 1 — window wiring (PR #7)

The m11d parked patch was lost to a sandbox reset; the wiring was
re-derived and, this time, the window-level policy is pure and tested
instead of living only in the unverified raylib file:

- `render/ui/shell.{hpp,cpp}` — the window policy: scenario listing
  (sorted .json stems), pointer-over-UI gating (`ui_covers`), arrow-key
  ownership (`ui_arrow_ownership`), momentary battle-button parsing
  (`battle_clicks`), the zoom slider mapping (`zoom_from_slider` /
  `slider_from_zoom`, log-symmetric around the fit zoom, exact inverses)
  and the per-frame viewer-state fold (`apply_battle_frame`).
- `game/src/main.cpp` — the wiring: raylib input -> `ui::Event` ->
  `reduce_app`; Quit exits; the picker's Start reloads the battle from
  `data/scenarios/<name>.json`; battle-screen buttons/checkboxes/slider
  fold into pause/speed/options/camera zoom; the wheel follows back
  into the slider; unit picking and wheel zoom are gated off UI panels;
  menus draw over a plain background exactly like the `--ui-script`
  harness. Compile-verified against a raylib-signature stub locally and
  by the CI game-build job; the human checklist is in docs/viewer.md.
- `tests/test_shell.cpp` — 8 tests (363 total): scenario listing, UI
  coverage on menu and battle screens, arrow ownership per screen and
  focus, click parsing, zoom round-trips, the viewer-state fold through
  real reducer clicks, and a menu -> picker -> battle session.

Known gaps (documented in docs/viewer.md, planned for 5a): no
battle->menu navigation in the App reducer yet; a window resize rebuilds
the screens without preserving widget values.

## Item 2 — availability and termination (PR #8, branch m13-avail-end)

`--ai-avail v1|v2` and `--end v1|v2`, both default v2; v1 reproduces
milestone-12 byte-for-byte (committed baselines in
`data/baselines/m13_avail_v1/`, pinned by tests/test_avail_end.cpp; the
M7 no-map pins in test_terrain.cpp now also run with the v1 flags).
Sim version 13.0; replay setups record and re-apply both flags.

- **Availability v2**: the M10 regroup term no longer clamps an
  unavailable Advance (enemy visible) to a pseudo-available 0.0 — the
  m12 audit's root cause; a delivered MoveTo keeps the march available
  explicitly (documented "marches past enemies", melee self-defense
  still wins); Help is capped per ally per tick
  (`help.max_helpers_per_ally`, deterministic in unit index order) and
  never targets routed allies farther than
  `help.routed_help_enemy_distance` from any living enemy; a contact
  seeker that reaches the faction estimate and sees nothing drops it
  (v1 keeps milling at the stale point — pinned by test).
- **Termination v2**: (a) a faction's active units below
  `max(routed_threshold_min_units, routed_threshold_fraction x initial)`
  for `routed_threshold_ticks` consecutive ticks ends the battle
  (ROUTED_THRESHOLD, winner = the other faction); (b) the stalemate
  rule — after first contact, when the last `stalemate_ticks` ticks
  average fewer than `stalemate_max_engagements` attackers (alive
  units, both factions combined, choosing Attack), the battle ends
  (STALEMATE, winner by alive count). Values in data/morale.json's
  `termination` block (optional; spec defaults 5% / 5 / 60 / 600 / 5).

### The acceptance, with the diagnosis trail

- Swapped Nile (earth-nile-delta, 20 seeds, 3000 ticks): main ends 0/20
  before the limit; the threshold rule alone reaches 11/20 (55%) — the
  remainder are the M12-documented rout-lock standoffs (85-90% routed,
  56-698 active survivors, 0-3 attackers per tick with sporadic bursts).
  A streak rule failed at 75% (bursts reset it); the windowed-average
  rule reaches **20/20 (100%)**: 9 routed_threshold + 11 stalemate.
- Default 250v250 (10 seeds): v1 mean end 881.5 (8 faction_routed /
  2 tick_limit) vs v2 632.7 (2 routed / 2 routed_threshold /
  6 stalemate). Viewer-demo: 345.0 vs 330.1 (8 routed / 2
  routed_threshold). Not worse — battles end earlier, none drag.

### Tests (376 total; 14 new for this item)

Regroup availability at the score_actions level; the Help cap (2 of 5
helpers pass, v1 uncapped); routed-ally exclusion; estimate decay on
arrival (v1 keeps the stale estimate); threshold termination (winner
and both-below draw, v1 runs on); stalemate termination (quiet window,
winner by alive count, v1 runs to the tick limit); morale termination
block parse + defaults + range validation; CLI flag parsing + usage;
replay flag round-trip + legacy defaults + bad-value rejection; five
scenario baselines + an orders battle reproduce main byte-for-byte
under v1; v1- and v2-flagged records verify on the swapped Nile map.
Two M5-era fixtures pin `end_v2 = false` (they test the v1 rout rules
with armies below the v2 floor); the M7 no-map pins run with v1 flags.

### Item 2 review — termination v2, one 100-seed look back

The rule, exactly: the STALEMATE check records the combined attacker
count (alive units of both factions whose chosen action this tick is
Attack) every tick after the battle's first enemy sighting, and ends
the battle as STALEMATE when the last `stalemate_ticks` (600) real
ticks — the window only fills after first contact and is only judged
once full — sum to less than `stalemate_max_engagements` (5) x the
window length, i.e. an *average* below 5 attackers per tick. The winner
is decided by alive count exactly like the tick limit (equal survivors
is a draw). All values live in the optional `termination` block of
`data/morale.json` (`stalemate_ticks`, `stalemate_max_engagements`).
Both v2 reasons are named in exports (`termination.reason`), reports
and re-run by replay verification; the flags ride in the replay setup.
Full semantics: docs/morale.md "Termination v2".

Termination reasons, 100 seeds per row, v1 = `--ai-avail v1 --end v1`
(the 13.0 rules) vs v2 = defaults:

| Scenario | v1 reasons (n=100) | v2 reasons (n=100) | mean end v1 -> v2 |
| --- | --- | --- | --- |
| default 250v250 | 81 faction_routed, 19 tick_limit | 38 faction_routed, 15 routed_threshold, 47 stalemate | 1061.6 -> 571.6 |
| viewer-demo | 99 faction_routed, 1 tick_limit | 80 faction_routed, 19 routed_threshold, 1 stalemate | 379.8 -> 341.4 |
| earth-nile-delta | 55 faction_routed, 45 tick_limit | 100 routed_threshold | 1788.4 -> 359.2 |
| earth-nile-delta +swap | 98 tick_limit, 2 faction_routed | 81 stalemate, 17 routed_threshold, 2 tick_limit | 2953.4 -> 1450.8 |

False-positive check — how often the rule fires while kills were still
happening: for every v2 row that ended STALEMATE, the deaths in the
last 200 ticks before the end (from the CSV timeline): default 47
stalemates with a *median of 2* deaths per 200 ticks (min 0, max 18);
nile+swap 81 stalemates with median 5 (min 0, max 26); viewer-demo's
single stalemate had 1. By any threshold that separates a trickle from
a fight (the same battles run hundreds to thousands of deaths per 200
ticks while alive), the rule fired **only in trickle endgames** — the
zero-false-positive reading of "kill rate above a small threshold"
holds for any threshold at or above ~1 death per 10 ticks, an order of
magnitude above everything observed (max 26 per 200 ticks = 1.3 per 10
ticks, in a 2000-unit battle). The rule is engagement-based, not
kill-based, by design: a handful of attackers picking off stragglers is
a dead battle. No tightening needed; the parameters stay as shipped.

Paired orders-study win-share differences (same seeds, both
orientations, 400 pairs per arm, `--combat v1` = the 13.0 combat the
review covers): **AttackMove +0.080 [+0.033, +0.127]** (63 arm-wins /
31 control-wins / 306 ties) and **flank +0.058 [+0.007, +0.108]** (65 /
42 / 293). Both CIs exclude zero: both orders are significantly
beneficial, confirming the item 0 finding under the paired design.

## Item 3 — combat depth (branch m13-combat)

`--combat v2|v1` (default v2; kSimVersion 13.1; v1 is the 13.0 combat
byte-for-byte — baselines in `data/baselines/m13_combat_v1/`,
cross-checked against the m13_avail_v1 set; combat depth never runs in
legacy mode). Six mechanics, every value in the validated
`data/combat.json`; no new RNG — v2 is as deterministic as v1 (same
seed, same bytes; grid == brute and replay verification unchanged).
Semantics and values: docs/ai.md "Combat depth".

- **Facing** — heading = last movement direction (spawned toward the
  objective). Arcs at the target: frontal x1, flank (60-120 degrees from
  the target's front) x1.25, rear x1.5 + 2 morale per rear hit.
- **Charge** — type speed >= 4.0 that ran >= 20 units toward the target
  over the last 10 ticks: x1.5, then the run window resets; a target on
  Hold, in the stance, or in a dense line (3+ allies within 3) takes the
  bonus x0.5. Coincident pairs (approach ends on the target) project the
  run on the summed window displacement.
- **Fatigue** [0,1] — 1/500 per full-speed tick, 1/40 per swing, 1/300
  recovery per idle tick; at 1.0: speed x0.7, damage and accuracy x0.85.
- **Experience** [0,1] — 0.05/kill, 1/2000/attack; up to +0.10 accuracy
  and 0.10 morale-loss resist.
- **Commander aura** — same-faction units inside a commander's
  leadership_radius: +5% damage, -25% morale losses.
- **Defensive stance** — a Hold-ordered squad (or a unit choosing Hold)
  with cohesion > 0.6: incoming x0.8, +0.20 morale-loss resist, the
  charge reduction; combined resist (experience + stance + aura) caps at
  0.9. Keying on the order, not the momentary action, was the round-1
  mechanism fix: holders flip to Attack when engaged (81-86% of engaged
  unit-ticks), so an action-keyed stance absorbed 3 hits in a probe
  battle where the order-keyed one absorbs 350-980.

Exports: format 9 adds per-unit `facing`/`fatigue`/`experience` and
per-faction `flank_hits`/`rear_hits`/`charge_hits`/`stance_hits_taken`;
the text report gains a "Combat depth" section; the replay setup carries
`combat=v1|v2`. Tests: 389 total (+13: arc math at exact angles,
rear-only-from-behind, charge trigger/reset/Hold-reduction, fatigue
bounds + speed factor + idle recovery, exact experience gains, aura
radius, order-keyed stance, format-9 shape, determinism, grid==brute,
replay flag round-trip, v1 baselines, config parser). Era tests pin
`--combat v1` (format-ladder rungs, M4/M6/M7/13.0 reproductions); the
DeathFixture (M6 succession) pins v1 because the order-keyed stance
legitimately changes v2 battle damage.

### The 5000-unit benchmark (honest miss on this box)

`--units 5000 --seed 42` (2500 v 2500 infantry, defaults): **49.0-59.7
t/s across seeds/runs** (median ~55) under combat v2, and **50.5 t/s
under `--combat v1`** on the same seed — the combat-depth overhead is
~3%, and the shortfall below the 60 t/s target is this 2-vCPU sandbox's
baseline for the utility-AI decide phase + morale pass (the same battle
runs 119.7 t/s with `--morale off`, 541.7 t/s in legacy AI mode; squads
on/off changes nothing). The 60 t/s acceptance is not met on this
machine by 13.0 either; item 4 (`--threads`) is the performance item
and re-measures this battle.

### The orders study under combat v2 (acceptance, final code)

200 paired seeds x both orientations (400 battles per arm), squads
scenario, orders at tick 100, under the final code (order-keyed stance):

| arm | win share [Wilson 95%] | paired diff vs control | kill ratio | mean end |
| --- | --- | --- | --- | --- |
| control | 0.448 [0.400, 0.496] | — | 1.368 | 659.5 |
| attackmove | 0.485 [0.436, 0.534] | **+0.037 [-0.013, +0.088]** (60/45/295) | 1.809 | 624.5 |
| flank | 0.507 [0.459, 0.556] | **+0.060 [+0.009, +0.111]** (67/43/290) | 1.535 | 696.3 |
| hold | 0.000 [0.000, 0.010] | -0.448 [-0.496, -0.399] (0/179/221) | 0.074 | 890.4 |
| retreat | 0.000 [0.000, 0.010] | -0.448 (0/179/221 by construction of the control arm) | 0.029 | 593.8 |

Acceptance reading: **no arm above 0.70** (max 0.507) — pass;
**AttackMove and flank at or above control** — pass on the paired point
estimates (+0.037 and +0.060, 60v45 and 67v43 paired wins); flank's CI
excludes zero, attackmove's now touches it (under 13.0 combat the same
study measured +0.080 [+0.033, +0.127] — the order-keyed stance also
buffs AI-holding defenders in every arm, which erodes attackmove's
edge); **hold at or above 0.25** — miss, see below. Note the stance
change legitimately altered every arm's battles (only ~80/400 end ticks
match the pre-fix runs — AI-issued Hold orders now carry the stance too;
chaos amplifies), which is why the final numbers replace the round-1
table wholesale.

### The hold arm: three tuning rounds, and the structural evidence

The acceptance target (hold win share >= 0.25, "a viable choice")
could not be met, and the evidence says it is structural, not a value
problem. Round 1 also fixed the mechanism: an action-keyed stance
absorbed 3 of thousands of hits in a probe battle (holders flip to
Attack when engaged); keying on the Hold order made the stance real
(350-980 absorbed hits per battle) — but the holder still loses. The
enemy never commits: in a probe battle the unordered side's action
totals were ~88k search and ~56k approach ticks against ~2k attack
ticks — a hovering, sniping ring. The blob grinds down 30-94 dead
against 0-17, stops routing wholesale (the stance's morale resist
works: 150-220 holders still active), the battle ends STALEMATE at
~800-1100 ticks, and every win condition (rout, stalemate alive-count,
tick limit) favors the side that keeps its distance.

| round | stance damage / morale_resist | probe result (10 seeds) |
| --- | --- | --- |
| 1 (spec defaults) | 0.8 / 0.2 | 0/10 wins, holder 63.3 dead vs 6.7 |
| 2 | 0.55 / 0.4 | 0/10 wins, holder 94.3 dead vs 14.0 |
| 3 | 0.65 / 0.3 | 0/10 wins, holder 75.7 dead vs 10.3 |

The trend is monotone and diagnostic: a stronger stance *prolongs* the
grind and raises both sides' losses at a constant ~7:1 ratio — the
holder cannot out-kill an enemy that chooses when to engage, and a
defensive-only bonus (the prompt's spec: incoming damage, morale
resistance, charge reduction — no offense) cannot flip an alive-count
decision the disengaged enemy wins by default. Values reverted to the
prompt's spec (0.8 / 0.2 / 0.6) since no setting moved the metric.
What would plausibly fix it is design, not data: pursuit orders that
punish hovering (the M6 pursuit anchor exists), or attacker fatigue
biting harder on the sniping ring.

### The unit matrix (pure-type armies, squads on)

Pure-type armies (250 + 10 commanders per side, squads 10x25, the
shipped scenario shape; 100 paired seeds x both orientations; type A is
red unswapped and blue swapped — `--swap-sides` swaps the compositions
with the bands, so the corrected attribution covers each type from both
spawn bands):

| matchup | type A win share |
| --- | --- |
| infantry vs archer | **0.995** (199/200) |
| archer vs cavalry | **0.000** (0/200) |
| cavalry vs infantry | **1.000** (200/200) |

The mirror control (identical infantry armies, squads on) is symmetric:
10/20 and 9/20 across orientations — no side bias. The acceptance
criteria ("no type wins everything, each type beats >= 1") **fail on the
shipped type stats**: cavalry wins everything, archer beats nobody. This
predates combat depth — archers lose 0/20 to both melee types under
`--combat v1` as well; the cause is `data/units.json`: archers (speed
1.6, damage 9) are strictly outrun by infantry (2.0) and cavalry (4.5),
so they cannot kite and lose every duel; cavalry additionally lands
charges (x1.5) under v2, widening a gap that was already 100/0. Fixing
the triangle is a units.json balance pass (archer speed at or above
infantry, or enough ranged damage to break a charge before contact) —
deliberately not done here: unit stats feed every byte-identity
baseline and every measured number in the milestone docs, so a stat
change is a milestone-scale re-baselining, not an item-3 patch.

## Item 3b — adversarial audit: 13 findings, all fixed

A post-merge adversarial audit of the non-combat code produced 13
findings; all are fixed on this branch, each with regression tests.
No sim behavior changed for any valid input: the fixes only reject
inputs that were never valid but previously slipped through as garbage
values, and the state-hash change is byte-identical on every supported
platform (proof below).

| # | finding | fix | tests |
| --- | --- | --- | --- |
| 1 | CLI numeric options `--trace-interval`, `--csv-interval`, `--seek`, `--intervene` parsed with raw `strtol`/`strtoull`: `25junk` accepted, `-1` wrapped to a huge value, overflow silently ignored | all four go through the same strict digits-only parser as `--seed`/`--units` (full consumption, errno, per-flag range; errors name the flag and echo the value) | `TraceIntervalRejectsMalformedValues`, `CsvIntervalRejectsMalformedValues`, `SeekRejectsMalformedAndNegativeValues`, `InterveneRejectsMalformedValues` |
| 2 | replay `order` x/y accepted `inf`/`nan` (std::stod parses them) and `0x10` (hex float) | decimal-only grammar + full-consumption + isfinite in `parse_double`; the retreat branch now errors on present-but-invalid points instead of silently dropping them | `NonFiniteOrderPointsAreRejected` (11 non-finite spellings x both coordinates + hex + trailing junk) |
| 3 | JSON order-script squad ids truncated through unchecked `get<uint32_t>` (4294967296 -> 0) | read as uint64, range-checked against UINT32_MAX before the cast | `ScriptParserSquadIdsFitUint32` (0, max, max+1, much larger, negative, fractional, string) |
| 4 | scenario squad-plan `count`/`size` truncated the same way | uint32-max check before the cast, consistent with the army-count parser; `map.cell_size` now also requires finite | `SquadPlanCountAndSizeFitUint32` |
| 5 | state hash serialized in native byte order | canonical encoding: little-endian integers (by shifts) and IEEE-754 double bit patterns — documented in docs/reports.md; **proved byte-identical** by building the pre-change `state_hash.cpp` from git history against the same library and comparing digests (tick 0 and tick 25 of the pinned test battle match), so every recorded replay still verifies | `StateHashKnownAnswers` (two pinned digests) |
| 6 | `validate_unit_stats` passed +inf on health/speed/damage/range and never checked accuracy/damage_variance/cooldown_jitter/leadership_radius | every floating-point statistic must be finite with its documented range, mirroring the unit-type file parser (the public `validate_config` is safe independently of JSON) | `NonFiniteUnitStatsAreRejected` (9 fields x {+inf, -inf, NaN} + range cases) |
| 7 | `parse_replay`/`parse_order_script` mutated the caller's output line by line — a failed parse left partial state in a reused object | both parse into temporaries and commit atomically; contracts documented in `cli/replay.hpp` (replacement for replays, append + stable tick-merge for scripts, output untouched on failure) | `FailedParseLeavesReplayDataUnchanged`, `ScriptParserCommitsAtomically` |
| 8 | duplicate unreachable `--combat` branch in `parse_args` | removed (the combined `--ai-avail/--end/--combat` handler is the live one) | `CombatFlagParsesThroughCombinedBranch` |
| 9 | TickAccumulator converted the accumulated double to int before any range check (UB for huge/inf values), no constructor validation | cap comparison happens in double space before the conversion (and reproduces the old `limited` semantics exactly at the boundary), non-finite deltas add nothing, a non-finite accumulation drops the backlog instead of pinning every later frame, constructor parameters fall back to the documented defaults | 6 accumulator tests (huge delta, non-finite deltas, overflow reset, constructor fallbacks, boundary semantics) |
| 10 | battlefield width/height accepted +inf | finite + strictly positive | `NonFiniteBattlefieldDimensionsAreRejected` |
| 11 | terrain `cell_size` accepted +inf; `move_cost`/`sight_modifier`/`defense_modifier` not finite-checked | cell_size finite + positive in `build_terrain_grid` (the public entry that takes a raw double); the type-table fields reject non-finite values | `NonFiniteCellSizeIsRejected` (NaN, +inf, -inf, 0, negative) |
| 12 | CSV / report Markdown / report JSON / events / trace writes never checked the stream after writing — a full disk exited 0 with a truncated file | every output flushes and checks (events/trace after the run, exports after writing, `write_replay` flush+close); failures exit 1 with the path; failure injection via `/dev/full` (writes fail at flush — exactly the case the old code missed) | `OutputFailureTest.WriteFailuresFailTheRun` (6 output kinds) + `WritablePathsStillSucceed` |
| 13 | PNG writer computed `int stride = width * 3` (overflow UB for extreme widths) and ignored `fclose` (buffered writes fail only at close) | dimension cap `width <= INT_MAX/3` before any stride math, buffer-size check, fwrite AND fclose checked | `InvalidImagesAreRejected`, `WriteFailuresAreReported` (incl. `/dev/full`) |

Reliability extras from the focused non-combat review (each with a
regression test):

- `--render-frames` with a 23-digit tick: `std::stoull` threw
  `out_of_range` out of `main` and aborted the CLI; now a clean exit-1
  error (`RenderFramesOverflowIsRejected`).
- UI-script integer fields were read with unchecked `get<int>` —
  `{"width": 1e300}` was undefined behavior, `2.5` silently truncated;
  now every integer goes through a range-checked reader
  (`UiScriptNumbersAreRangeChecked`).
- A compact PNG map claiming 100000x100000 pixels made stb allocate
  the ~30 GB decode before the 4096x4096 cap ran; the IHDR is now
  pre-checked before decoding (`OversizedPngIsRefusedBeforeDecoding`).
- `data/morale.json` termination count fields
  (`routed_threshold_min_units`, `stalemate_max_engagements`)
  truncated above UINT32_MAX through an unchecked cast —
  4294967296 silently became 0; now rejected
  (`TerminationBlockParsesAndDefaults` extension).
- `Rng::next_uint32(0)` divided by zero (no callers, defensive guard).
- The `--intervene` re-parse in `main.cpp` used raw `strtoul` on the
  canonicalized strings; now strict (defense in depth).
- `--record` without an explicit `--scenario` wrote an empty scenario
  line (the raw flag value instead of the resolved path), so every
  default-scenario recording failed `--verify` with "no scenario
  line"; the resolved path is recorded now
  (`RecordWithoutScenarioFlagVerifies`; both v2 and v1 combat
  recordings round-trip record -> verify).

Compatibility: the acceptance studies, byte-identity baselines and
every recorded replay are untouched — the full pre-audit suite still
passes plus the new regression tests, the shipped scenario and data
files load unchanged, and recorded-replay verification round-trips
(the known-answer digests pin it). The only visible changes are
rejections of inputs that were never valid.
