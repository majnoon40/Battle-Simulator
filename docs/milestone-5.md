# Milestone 5 — Morale and routing

## Scope

Individual morale, fear, routing and recovery so armies can collapse before
the last soldier dies and battles actually end. No commanders, squads or
terrain (leader effects are M6). `--morale on|off` (default on; utility mode
only), `--morale-config` selects the configuration file.

## Design decisions and assumptions

1. **Morale applies to utility mode only.** Legacy mode ignores it
   completely (it must keep reproducing M1-M3), and `--morale off` makes
   utility battles bit-identical to their M4 runs — proven at the strongest
   level: a morale-off export of seed 42 is byte-identical to the M4-era
   export captured before M5 started (`output/m4-utility-a.json`).
2. **Everything data-driven** (`data/morale.json`): starting morale and its
   bravery scaling, the five state thresholds, all loss/gain rates, the
   recovery threshold and safe time, the AI score modifiers, per-tick
   change bounds and the rout timeout. Validation rejects missing fields,
   unordered thresholds and any recovery threshold not strictly above the
   breaking value (the hysteresis requirement).
3. **Personalities drive morale**: starting morale = 40 + 40*bravery; every
   loss is scaled by `clamp(1 + 0.5*caution - 0.5*bravery, 0.2, 2)`. Same
   (seed, id) hash as M4 — the spawn stream stays untouched.
4. **Two rout-contagion channels beyond the spec's minimum loss list**
   (documented in docs/morale.md): a one-tick-delayed morale hit for ally
   breaks within sight, and a per-tick drain from currently-routed allies
   in sight. These were added after four pure-rate tuning rounds failed to
   make army-wide collapses possible: without them a local collapse stays
   local. The delay and count-based accumulation keep them deterministic.
5. **Fleeing** is full speed (routed units run), away from the visible
   enemy centroid toward the unit's own side; cornered units go for the
   nearest world edge. Recovered units rejoin as Shaken-or-better only
   after 120 safe ticks above morale 35 (hysteresis over the break at 12).
6. **FACTION_ROUTED** ends a battle after 30 consecutive ticks with no
   active units (all dead or routed); mutual collapse is a draw. Routed
   units count as survivors but not active.
7. **Export v4** (morale on) adds per-unit morale/state/routed and
   per-faction routing statistics plus active counts; morale-off and legacy
   exports keep the exact format-3 shape.
8. **M4 test edits (spec-mandated)**: the M4 utility tests in test_ai.cpp
   pin `morale_enabled = false` (they verify M4 semantics, which `--morale
   off` must reproduce exactly); assertions unchanged. 29 new M5 cases live
   in test_morale.cpp; 6 new CLI cases cover --morale/--morale-config.

## Verification results (2026-11-14)

- **190/190 GoogleTest tests pass** (161 M1-M4 + 29 new M5).
- Bounds: morale deltas within the per-tick caps and finite over 2,000
  random inputs; a 10,000-step clamped random sequence stays in [0, 100].
  Every loss/gain source moves the delta in its documented direction
  (isolation, outnumbering, damage, ally deaths, ally routs, routed-ally
  presence, kills, superiority, allies present, safe time); bravery shields
  and caution amplifies.
- Routing: a broken unit never attacks (in-range enemy untouched); a routed
  unit flees at full speed toward its own side; recovery happens exactly at
  tick 120 (not 119), never while threatened, and the threshold is isolated
  with a custom slow-gain config; routed-state changes measure **0.061 per
  unit per 100 ticks** (bound 3); FACTION_ROUTED fires after the timeout
  and never while an active unit remains; explicit initial states carry
  their morale and routed values (tests construct broken and recovering
  units).
- Reproduction: morale off == M4 byte-for-byte (anchor export cmp; values
  also hardcoded: seed 42 → Blue, 3000 ticks, 83/114 survivors, exact
  action totals); legacy + morale on == M1 references (seed 42: Blue, 456,
  0/64); morale-on double runs and grid==brute runs byte-identical.
- Purity: no new I/O in sim/; exports carry no timestamps/paths.

## Acceptance (honest numbers)

- **3:1 mismatch (375 vs 125, seeds 1..100)**: smaller army routed before
  annihilation in **97/100**; Red wins 100/100; termination 97 routed, 3
  tick-limit; avg 393 ticks (min 291); avg 401 survivors, 99 deaths; Blue
  avg 32.6 routed at the end; Red routs 40 total (never collapses).
- **Equal armies (250 vs 250, seeds 1..100)**: winners Red 47 / Blue 53;
  termination **0 eliminated / 0 routed / 100 tick-limit — the ≥80% target
  is MISSED (0%)**. Routs 10,396 (Red) / 9,826 (Blue) vs 337/374
  recoveries; peak simultaneous routed ~99 of 250; avg 211 survivors.
  Cause (documented, not tuned away): the M4 objective system dissolves
  armies into sparse skirmish clouds; the rout funnel melts the army but a
  resilient active remnant of locally-winning brave units never fully
  collapses inside the 3000-tick default limit. 10,000-tick probes reach
  25-51 actives — near-misses, not collapses. The mechanisms that would
  fix this (cohesive fronts, router pursuit, command) are M6 scope.
  Baseline comparison: M4 (morale off) also ended 100% at the tick limit,
  so morale strictly improved endgame dynamics (rout funnel, mismatch
  resolution) without reaching the equal-army target.
- **--ai-debug**: unit 46 breaks at tick 212 (health 4/100, morale 10.8
  broken/routed, 37 enemies visible, all scores -1, action flee); unit 498
  recovers at tick 437 (morale 39.5 worried, no enemies visible, advance
  0.74) — `logs/m5-ai-debug-break.log`, `logs/m5-ai-debug-recover.log`.
- **Performance (2-vCPU sandbox, Release, morale on)**: 5,000 units
  **107.4 ticks/s (9.31 ms/tick)** — above the 60 bar (the perf battle
  itself ends by faction rout at tick 272); gprof: `radius_query` 90.5%,
  `decide_unit_action` 9.5%, `morale_delta`/`state_from_morale` negligible.
  Timer harness (static walls): 140.8 t/s at 5k, 30.4 t/s at 20k, zero
  steady-state allocations in both. 20,000 units CLI: **23.4 ticks/s
  (42.7 ms/tick)** vs 26.6 in M4 — the morale phase costs ~13% at that
  scale, mostly the extra per-unit bookkeeping on the same perception
  scan.

Full command logs live in `logs/m5-*.log`.
