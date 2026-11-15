# Milestone 4 — Perception and utility AI

## Scope

Replace "everyone always knows the nearest enemy" with limited perception
and explainable utility AI: units sense only enemies and allies within their
sight radius, score five actions with personality-weighted formulas, and act
on the best. `--ai-mode legacy|utility` (default utility); legacy reproduces
every M1-M3 reference exactly. No morale, squads, commanders or terrain.

## Design decisions and assumptions

1. **Personality from a hash, not the spawn stream.** `personality_for(seed,
   unit_id, table)` mixes (seed, id) with a SplitMix64 finalizer and draws
   archetype (weighted) then the three traits from the archetype's ranges.
   Spawn positions are bit-identical in legacy and utility modes (tested);
   personalities are identical across runs, modes and platforms.
2. **Utility perception always uses the spatial grid** (radius queries), so
   `--neighbor-mode` cannot change utility results (verified
   byte-identical). The enemy centroid — the one order-dependent
   quantity — therefore has a single canonical enumeration in every run.
3. **Exactness inside sight.** The nearest visible enemy uses the M1 rule
   (minimum squared distance, ties to the lower unit id); the sight radius
   is inclusive (an enemy exactly at `sight_radius` is seen).
4. **Documented formulas** (docs/ai.md): five linear score blends whose
   weights sum to exactly 1.0 per action, so scores stay in [0, 1] by
   construction. Inputs: health fraction, local strength ratio
   `(allies+1)/(allies+enemies+1)`, distance fraction, cooldown readiness,
   personality traits.
5. **Anti-flip-flop by two mechanisms**: the configurable stickiness bonus
   (default +0.05) and a three-tick minimum commit time after a switch
   (kept while the action stays available). Measured 2.08 switches per unit
   per 100 ticks (bound: 8, documented in docs/ai.md). Development note:
   the first implementation measured 14.6 — the commit time was added after
   profiling showed attack/hold cycling with the cooldown readiness term.
6. **Retreat is a fighting withdrawal at half speed**: full-speed retreat
   kites equal-speed pursuers forever (first 100-battle run: every battle
   stalled to the 3000-tick limit with 0.85% attack stances). Direction:
   away from the visible enemy centroid with the horizontal component
   forced toward the unit's own side, clamped to the battlefield.
7. **Objectives** default to the center of the opposing spawn band and are
   overridable per faction in scenario files (validated on-battlefield).
8. **Export v3** adds per-unit personality and last_action and per-faction
   cumulative action_counts; legacy battles export "none"/zeros. No paths,
   timestamps or mode flags in exports.
9. **M1-M3 test files were edited** (spec-mandated): every old
   Simulation-constructing test pins `ai_mode = Legacy` (the M4 default is
   utility); unit-type fixtures gained the now-required `sight_radius`
   field; export tests assert format v3. The M1/M2/M3 assertions
   themselves are unchanged — no test was weakened.
10. **`.env` untracked** (pre-step): tracked since the initial commit, one
    variable (`DATABASE_URL`), removed from the index in a new commit;
    `.gitignore` already listed it.

## Verification results (2026-11-14)

- 161/161 GoogleTest tests pass (125 M1-M3 + 36 new M4 cases), `ctest`,
  Release; 20 consecutive ctest runs at the pre-step: 20/20 green.
- Property tests: scores in [0, 1] and availability rules over 1,000 random
  input sets; retreat strictly rises as health falls; attack strictly falls
  as local strength falls; metamorphic invariance (a far enemy moving
  farther changes nothing).
- Sight boundary: an enemy exactly at sight radius is seen, one at
  +0.001 is not; allies/enemies counted separately; dead enemies never
  seen; personality assignment deterministic and spawn-independent.
- Legacy regression (CLI): seeds 42 (Blue, 456, 0/64), 123 (Blue, 713,
  0/6), 5 (draw, 777, 0/0), 0 (Blue, 709, 0/26), 2^64−1 (Red, 633, 48/0),
  mixed seed 42 (Blue, 664, 0/22), default run — all unchanged.
- Determinism: utility double runs byte-identical (test + CLI `cmp`);
  utility grid == utility brute byte-identical (the neighbor mode cannot
  affect utility battles).
- Archers hold at their own range (measured, mixed scenario seed 42):
  average decide-time distance to target while attacking — archer 5.02,
  cavalry 0.62, infantry 0.91 (archer range 8, melee range 2).
- AI debug: cautious unit 1 at tick 250 (64/100 health, 15 enemies, 9
  allies) retreats with score 0.651; brave unit 3 at first contact (tick
  252, enemy at 39.95) approaches with score 0.878 vs retreat 0.130
  (`logs/m4-ai-debug-*.log`).
- Performance (2-vCPU sandbox, Release, seed 42, 600 ticks): 5,000 units
  utility **208.2 ticks/s (4.80 ms/tick)** — above the 60 ticks/s bar;
  gprof: `radius_query` 86% of tick time, `decide_unit_action` 13%.
  20,000 units utility **26.6 ticks/s (37.5 ms/tick)** — the M3 miss did
  NOT shrink (legacy grid: 33.1 ticks/s); perception's radius queries add
  ~4.5 ms/tick at that scale. Steady-state allocations: 2 events in 550
  ticks at 5k, 3 at 20k (bucket high-water growth only).
- 100 seeded utility battles (500 infantry, seeds 1..100): Red 44, Blue 55,
  draws 1; all reach the 3000-tick limit (wars of attrition — wounded
  cautious units disengage and re-engage; see docs/ai.md); average 210
  survivors, 290 deaths per battle; per-faction action totals: Red attack
  710k, approach 1.44M, advance 31.7M, retreat 622k, hold 618k; Blue attack
  713k, approach 1.47M, advance 32.2M, retreat 591k, hold 637k.
- Purity: no raylib/chrono/random_device/rand/getrusage and no file I/O in
  `sim/`; exports contain no time/date/path/host values.

Full command logs live in `logs/m4-*.log`.
