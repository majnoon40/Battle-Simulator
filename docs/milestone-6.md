# Milestone 6 — Squads, formations, commanders

## Scope

Squads with cohesion, formations, commander orders with imperfect
obedience, and commander death/succession. No terrain. `--squads on|off`
(default on; utility mode only), `--formations`/`--commanders` config
flags, `--squad-debug`, export format 5. Full design: `docs/squads.md`.

## Design decisions and assumptions

1. **Squad = `size` soldiers + 1 commander** (the commander is a member and
   leader; member count `size + 1`). The scenario `squads` section is
   validated against the composition exactly: `count` commander-type units
   and `count * size` soldiers, or the battle refuses to run. The squad
   size default of 25 is the only default (documented, spec-sanctioned).
2. **Assignment is spatially contiguous in a deterministic order**: per
   faction, soldiers and commanders are sorted by (y, x, id) and sliced.
   Every unit of a planned faction lands in exactly one squad.
3. **Discipline** is the fourth personality draw, appended after the M4
   stream, so bravery/caution/aggression are bit-identical to M4/M5. It
   scales order obedience and is exported only in format 5.
4. **Orders are bounded score terms** (obedience * weight, weights capped
   at 1.0, scores clamped to [0, 1], only available actions boosted) —
   never overrides. Obedience falls with distance, rises with discipline
   and squad cohesion, and is scaled by morale state.
5. **Steering**: members with Hold/Advance steer to slots (weighted
   separation/cohesion/alignment/slot seeking); the acting commander
   anchors the formation and never steers. The near-slot pull weights are
   small by design: a combined pull gain above speed/gain produces a
   saturated two-tick oscillation instead of convergence (found and fixed
   during verification).
6. **Succession** happens exactly `succession_delay_ticks` after command
   loss (death or rout — routed commanders lose command permanently); the
   successor is the lowest-id alive, non-routed member. Commander death
   shocks members inside the leadership radius (personality-scaled morale
   loss, immediate).
7. **Reproduction**: `--squads off` battles are bit-identical to M5
   (proven against the M5 binary's own export using the M5-era units.json);
   scenarios without squad plans are no-ops with squads on; legacy ignores
   squads; morale-off and legacy references still pass.

## Verification results (2026-11-14)

- **229/229 GoogleTest tests pass** (191 M1-M5 + 38 new M6: config
  parsers and defaults-vs-shipped-file equality, scenario squad plans,
  commander type validation, plan-vs-composition validation, assignment
  coverage/contiguity/determinism, slot non-overlap for all shapes and
  counts 1..100, separation in a crowded squad, order-rule priority,
  obedience monotonicity + exact formula, order score bonuses,
  propagation delay/range/decay, commander death shock (exact differential
  measurement), succession at the exact documented tick, deposed-commander
  accounting, routed commanders lose command, hold-vs-advance behavioral
  effect, squads-off/no-plan byte-identical exports, squads-on double-run
  determinism, format-5 export shape, CLI flags).
- CLI-level reproduction: default scenario `--squads off` with the M5-era
  units.json is byte-identical to the stored M5 export; squads-on double
  runs byte-identical; grid == brute unchanged (legacy, existing tests).

## Acceptance (honest numbers)

- **Cohesion (standard battle, seed 42, final state)**: mean distance of
  squad members to their squad centroid **87.0 with squads on vs 147.6
  with squads off** (ratio 0.59) — squads keep members 41% closer. The
  squads-on battle itself ends by faction rout at tick 774; the off battle
  stalls to the 3000-tick limit.
- **Commander-kill (100 seeds, all Blue commanders slayed after a fixed
  tick vs control)**: the effect depends on when the blow lands.
  - Tick 250 (mid-engagement): Blue routs **+23.5 per battle** (157.5 vs
    134.0), battles end no earlier, outcomes flip in 27/100, Red wins rise
    48 -> 63 and Blue falls 51 -> 37 — losing commanders in the fight
    costs Blue the field.
  - Tick 50/150 (before contact): the result inverts — Blue wins 81-84 of
    100 (succession completes before contact). Ablation with succession
    disabled: Blue wins 45 of 100 (leaderless squads are the worst of the
    three configurations). So command matters (leaderless 45 < original
    51 < successor-commanded 81): successors perceive at the members'
    40-unit sight, while original commanders react at 60 and their early
    pre-contact retreat/regroup orders soften their squads' engagement.
- **Equal armies (100 seeds, 260 v 260, squads on, tick limit 3000)**:
  **18/100 (18%) end before the tick limit** — the 80% target is MISSED
  (M5 baseline: 0%). All 18 end by faction rout. Cause (measured, not
  tuned away): in every stalled battle, 100% of surviving active units
  have no enemy inside sight at tick 3000 (mean distance to the nearest
  enemy 323-849 on a 1000-unit field). The two remnants regroup into
  their squads (squad cohesion 0.75) but the M4 objective system marches
  each army to the *opposing* spawn band — they swap sides without
  re-establishing contact, then sit at their objectives while morale
  regenerates. Squads fixed cohesion; contact loss remains an M4-era
  objective/search-gap that M6's scope (no pursuit or search behavior)
  does not cover.
- **Performance (2-vCPU sandbox, Release)**: 4,992 units with 192 squads:
  **92.0 ticks/s (10.87 ms/tick)** — above the 60 bar (the battle ends by
  faction rout at tick 297); no-squads 5,000-unit baseline 117.6 t/s, so
  the squad machinery costs ~22% (steering separation queries + the squad
  phase). gprof (squads battle): `radius_query` 64.3%,
  `decide_unit_action` 17.8%, `steer_in_formation` 6.1%,
  `update_squad_metrics` 1.9%. 19,968 units with 768 squads:
  **7.8 ticks/s (128.3 ms/tick)**, reported (the M5 20k no-squads number
  was 23.4 t/s; squads roughly halve it at that scale).
- **--squad-debug before/after commander death** (seed 42, squad 12,
  Blue): before — commander unit 261 alive, order regroup, cohesion 0.44,
  members in formation (two far members' orders already decayed to none
  beyond the radius); after — "command lost at tick 283; was unit 275"
  (261 died, 275 succeeded, 275 fell), members routed, order none,
  cohesion 0.20. Captures in `logs/m6-squad-debug-and-tests.log`.

Full command logs live in `logs/m6-*.log`.
