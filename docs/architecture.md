# Architecture

## Build targets

| Target       | Type         | Responsibility                                        |
| ------------ | ------------ | ----------------------------------------------------- |
| `sim`        | static lib   | World state, units, RNG, decisions, combat, statistics, JSON serialization |
| `battle_cli` | executable   | Argument parsing, runs the battle, prints the report, writes files |
| `battle_tests` | executable | GoogleTest suite (unit, integration, determinism, regression) |
| `battle_game` | executable  | Optional raylib application (future milestone, `BUILD_GAME`) |

The `sim` library never depends on raylib, rendering, terminal output or
file I/O. It receives configuration and initial state and produces updated
state (`World`) and measurable results (`BattleResult`); JSON is serialized
to a `std::string` by `sim::export_state_json` and written to disk only by
the CLI. Since M2, gameplay data is data-driven: the CLI reads
`data/units.json` (unit type table) and a scenario file, hands the *text*
to `sim::parse_unit_types` / `sim::parse_scenario` (which also accept any
`std::istream`), and passes the resulting table plus the composition to the
simulation through `SimulationConfig`. `battle_game`/`render` are
intentionally empty; when their milestone is authorized, raylib is fetched
and linked only into the graphical application while the rest keeps
building headless.

## sim modules

| File                  | Contents                                                    |
| --------------------- | ----------------------------------------------------------- |
| `factions.hpp/.cpp`   | `Faction` enum, stable faction identifiers                   |
| `unit.hpp`            | `UnitStats` (data-driven combat stats incl. sight radius),   |
|                       | `Personality`, `Action`, `Unit` state                        |
| `unit_types.hpp/.cpp` | `UnitType`/`UnitTypeTable`, strict JSON loading (M2, M4      |
|                       | sight radius)                                                |
| `personalities.hpp/.cpp` | `PersonalityTable`, strict JSON loading, the hash-based   |
|                       | `personality_for(seed, id, table)` (M4)                      |
| `scenario.hpp/.cpp`   | `Scenario` (world, band, composition, objectives), JSON      |
|                       | loading, `build_composition` (M2, M4 objectives)             |
| `rng.hpp/.cpp`        | Seeded SplitMix64 generator; no global random state          |
| `config.hpp/.cpp`     | `SimulationConfig` (legacy + composition modes), validation, |
|                       | army split, `kMaxUnits` cap, `NeighborMode` (M3),            |
|                       | `AiMode`/objectives/stickiness (M4)                          |
| `world.hpp/.cpp`      | `World`: units + battlefield + tick counter                   |
| `results.hpp/.cpp`    | `TerminationReason`, `FactionReport` (incl. action counts),  |
|                       | `TypeReport`, `BattleResult`                                 |
| `neighbors.hpp/.cpp`  | `nearest_enemy_index` — the brute-force O(n) scan, kept as   |
|                       | the test oracle and CLI baseline (M3)                        |
| `spatial_hash.hpp/.cpp` | `SpatialHashGrid` — uniform grid, exact ring queries,      |
|                       | occupancy bitmaps, incremental insert/update/remove (M3)     |
| `ai.hpp/.cpp`         | Utility AI (M4): `Perception`, `ScoreInputs`, the documented |
|                       | `score_actions` formulas, `choose_action` tie-break          |
| `morale.hpp/.cpp`     | Morale (M5): `MoraleConfig`, strict JSON loading,           |
|                       | `morale_delta`, state thresholds, score modifiers            |
| `squads.hpp/.cpp`     | Squads (M6): formation/commander configs (strict JSON),  |
|                       | slot generation, order rules, obedience, Squad state,     |
|                       | SquadStats                                                 |
| `simulation.hpp/.cpp` | The fixed-timestep tick pipeline, composition spawn (M2),    |
|                       | spatial-index maintenance (M3), utility decide/move/combat   |
|                       | and AI debug capture (M4), morale/routing/recovery and      |
|                       | FACTION_ROUTED termination (M5)                              |
| `json_export.hpp/.cpp`| Deterministic JSON serialization of the current state (v4:   |
|                       | morale/state/routed and routing statistics; v3 without       |
|                       | morale)                                                      |

## Fixed tick pipeline

`Simulation::tick()` runs the same ordered phases every tick. The mapping to
the project-wide pipeline is:

| # | Project phase                    | Implementation                                |
| - | -------------------------------- | --------------------------------------------- |
| 0 | Update spatial structures        | M3/M4: grid built once at spawn, then moved   |
|   |                                  | units re-bucketed after movement and dead     |
|   |                                  | units removed at death (fixed index order)    |
| 1 | Calculate perception             | legacy: nearest living enemy via the spatial  |
|   |                                  | hash grid (or the brute oracle). utility (M4):|
|   |                                  | grid radius query within sight_radius —       |
|   |                                  | nearest visible enemy, counts, centroid       |
| 2 | Evaluate AI decisions            | legacy: target choice. utility (M4): the five |
|   |                                  | documented action scores with stickiness and  |
|   |                                  | a minimum commit time (docs/ai.md)            |
| 3 | Resolve movement                 | `resolve_movement` — snapshot, then commit    |
|   |                                  | (utility: move per chosen action)             |
| 4 | Resolve combat                   | `resolve_combat` — decide, then resolve       |
|   |                                  | (utility: only the Attack action engages)     |
| 5 | Apply damage and deaths          | damage in `resolve_combat`, `apply_deaths`    |
|   |                                  | (deaths recorded as morale events, M5)        |
| 6 | Update morale                    | `update_morale` (M5): bounded loss/gain       |
|   |                                  | deltas, break/recover with hysteresis,        |
|   |                                  | rout-contagion events, routing statistics     |
| 7 | Update squads and leadership     | M6: `update_squads` — squad metrics, command, |
|   |                                  | succession, orders and propagation            |
| 8 | Process events                   | (future milestone)                            |
| 9 | Update statistics                | `update_termination` (reports, survivors,     |
|   |                                  | active counts, action counts, routing)        |
| 10| Commit simulation state          | `update_termination` (tick counter, flags;    |
|   |                                  | elimination, FACTION_ROUTED, tick limit)      |

Timers (`resolve_cooldowns`) run first so that `attack_cooldown_ticks == K`
means exactly K ticks between two attacks.

## Decision / resolution separation

To prevent update-order advantages, every phase that writes state is preceded
by a read-only decision pass over a consistent snapshot:

- **Targets** are selected for all units before anything moves.
- **Movement** computes every destination from the tick-start positions and
  commits them afterwards — a unit's move never depends on whether its
  target already moved this tick.
- **Combat** first records every attack (attacker alive, target alive, in
  range after movement, cooldown ready), then applies all damage at once.
  Two units can therefore kill each other in the same tick, and focus fire
  can overshoot (health is clamped to zero on death).

Iteration is always over the `World::units` vector in spawn order (which is
ascending id order for spawned armies); nearest-enemy ties break to the lower
id. No unordered container affects simulation results.

## Termination rules

- A faction with no surviving units ends the battle
  (`faction_eliminated`); the other faction wins. Mutual annihilation is a
  draw.
- Reaching `max_ticks` ends the battle (`tick_limit`); the faction with more
  survivors wins, an even split is a draw.

## Per-tick allocations

Scratch buffers (`target_index_`, `pending_moves_`, `pending_attacks_`) are
members that are reused every tick; the simulation loop performs no heap
allocations after construction (a `.assign()`/`.clear()` on an
already-reserved vector does not allocate).
