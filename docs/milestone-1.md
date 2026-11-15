# Milestone 1 — Core simulation loop

## Scope

Smallest complete headless battle: two armies of individual units with
health, positions, nearest-enemy targeting, movement, attacks with range and
cooldown, death handling, battle termination, statistics and a deterministic
JSON final-state export via a headless CLI.

Deliberately **not** implemented (future milestones): spatial hash
(nearest-enemy is an O(n) scan — fine for the 500-unit M1 limit), PNG/ASCII
exports, CSV statistics, replays, JSON configuration loading, morale,
squads, leadership, raylib application.

## Design decisions and assumptions

1. **`--units` is the total unit count**, split between the two factions
   (500 ⇒ 250 Red + 250 Blue; odd counts give Red the extra unit). This
   matches the M1 restriction "only 500 units".
2. **Winner rules.** Faction elimination: the surviving faction wins;
   mutual annihilation (both factions lose their last unit in the same
   tick — possible because attacks resolve simultaneously) is a draw.
   Tick limit: the faction with more survivors wins; an even split is a
   draw. Winner `"draw"` is exported when there is no winner.
3. **M1 default unit stats** (in `SimulationConfig`, values not hardcoded
   in gameplay code): health 100, speed 2.0/tick, damage 12, attack range
   2.0 (inclusive), cooldown 5 ticks. Battlefield 1000×1000, each faction
   spawns in a 15% side band, y uniform over the field.
4. **`attack_cooldown_ticks == K`** means exactly K ticks between two
   attacks (cooldowns decrement at tick start before combat).
5. **Simultaneous combat.** All attacks of a tick are decided from one
   consistent state and then applied together: mutual kills are possible
   and focus-fire overkill is clamped to 0 on death.
6. **Initial-state constructor.** Besides seed-based spawning, `Simulation`
   accepts an explicit unit list — this is the "simulation receives
   configuration and initial state" seam, used by the tests and reserved
   for future replay loading.
7. **Timing is reporting only.** `std::chrono::steady_clock` and
   `getrusage` are used in the CLI for the performance report; the
   simulation itself never reads the clock.

## Verification results (2026-11-13)

- 69/69 GoogleTest tests pass (`ctest`, Release build).
- 500-unit acceptance battles: seed 42 → Blue wins at tick 456; seed 123 →
  Blue wins at tick 713; seed 0 → Blue at 709; seed 18446744073709551615 →
  Red at 633; seed 5 → full mutual annihilation (500 deaths, draw).
- Determinism: `diff -u` of repeated `--export-json` runs is empty for
  seeds 42, 123, 0 and 18446744073709551615 (85,488-byte exports for
  seed 42).
- Throughput at 500 units: ~5,900–10,100 ticks/s (~0.10–0.17 ms/tick) on
  the CI sandbox (Intel Xeon, GCC 14.2, Release, 2 cores).
- Win balance across 20 seeds: 14 Red, 5 Blue, 1 draw — both factions win
  under identical stats; outcomes are emergent, not rule-based.

Full command logs live in `logs/`; the acceptance exports live in
`output/`.
