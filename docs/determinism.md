# Determinism

The same initial configuration and seed must produce byte-identical exported
results. This document records every mechanism that guarantees it and how it
is verified.

## Random number generation

- `sim::Rng` is a SplitMix64 generator: plain, fixed-width 64-bit integer
  arithmetic with a published reference implementation, so the sequence is
  identical on every platform and compiler.
- Every `Simulation` owns its own `Rng`, seeded explicitly from
  `SimulationConfig::seed`. There is no global random state, no
  `std::rand`, and no `std::uniform_*_distribution` (those are not
  portable across standard libraries).
- `next_double()` maps 53 random bits to `[0, 1)`; `next_uint32(bound)` uses
  rejection sampling so there is no modulo bias.
- The RNG is consumed in a fixed order: all Red spawns, then all Blue spawns.
  Since M2, composition-mode spawns follow the same rule — Red's types in
  scenario file order, then Blue's — and every unit draws `x` then `y`
  exactly like M1, so an infantry-only composition reproduces the M1 stream
  bit for bit. Scenario files are parsed with `nlohmann::ordered_json`, so
  the composition order a file shows is the spawn order every run.

## Simulation logic

- Fixed timestep: one `tick()` equals one simulation step; nothing depends
  on wall-clock time. The CLI measures execution time only for reporting,
  never as simulation input.
- No multithreading: a single thread updates the world.
- Update order is fixed (see `docs/architecture.md`): vector order in spawn
  sequence, id-order tie-breaks, decision phases separated from resolution
  phases.
- Stable identifiers: unit ids are assigned once at spawn, are never reused,
  and dead units are never removed from the world vector.
- No unordered iteration influences results. (`std::unordered_set` is used
  only to *validate* explicit initial state and unit type tables — it never
  produces output.)

## Spatial index (M3)

- The spatial hash grid is a direct-indexed uniform grid (cell
  `col * rows + row`): no hashing, no `unordered_map`, no iteration order to
  depend on. Cells are scanned in fixed (row, column) order.
- The grid is built in unit-index order and maintained in fixed orders:
  moved units are re-bucketed after movement in index order, dead units are
  removed at death in index order.
- Query results do not depend on the intra-bucket layout at all: the
  nearest-enemy minimum over (squared distance, unit id) is unique because
  ids are unique, and ring pruning only skips candidates that are strictly
  farther than the best found so far — equal-distance candidates are never
  skipped, so the lowest-id tie-break is preserved.
- Grid mode and brute mode produce byte-identical exports for the same seed
  (verified in tests and CLI runs); the export contains no grid or mode
  fields.

## Utility AI (M4)

- Personalities are derived from a SplitMix64 hash of (seed, unit id) —
  plain 64-bit integer arithmetic, never the spawn RNG stream — so spawn
  positions are identical in legacy and utility modes, and the same
  (seed, id, table) yields the same personality on every platform.
- Utility-mode perception always uses the spatial grid (radius queries), so
  `--neighbor-mode` cannot change utility results; utility grid and brute
  runs are verified byte-identical. The one order-sensitive quantity (the
  visible-enemy centroid) therefore has a single canonical enumeration per
  run, and double runs reproduce it exactly.
- Action scores are fixed linear blends of clamped inputs; choices depend
  only on (perception, personality, cooldown, previous action), all
  deterministic. The stickiness bonus and the minimum commit time introduce
  no timing or platform dependence.
- Utility-mode double runs are byte-identical (test and CLI `cmp`); the v3
  export adds personality, last_action and per-faction action_counts with
  the same fixed-precision serialization rules as every other field.

## Morale and routing (M5)

- The morale update is a pure function of deterministic inputs: the
  tick-start perception snapshot, this tick's damage/kills/death events and
  the personality factors. Event effects accumulate as integer counts
  (order-independent); the one-tick delay on rout contagion makes the
  spread independent of unit index order.
- Morale-off utility battles are bit-identical to their M4 runs — verified
  byte-for-byte against an M4-era export — and legacy mode ignores morale
  entirely, so the M1-M3 references are unaffected.
- Break/recover transitions use fixed thresholds and counters (hysteresis:
  break below 12, recover above 35 after 120 safe ticks); no timing,
  platform or iteration-order dependence.
- Morale-on double runs and grid==brute runs are byte-identical (tests and
  CLI `cmp`); the v4 export adds morale/state/routed and routing statistics
  with the same fixed-precision serialization rules.

## Floating point

- Every GCC/Clang build applies `-fno-fast-math -ffp-contract=off`
  (root `CMakeLists.txt`), so no FMA contraction and no relaxed IEEE
  assumptions.
- All determinism comparisons use the same Release build configuration;
  compiler flags are never changed between regression runs.

## Serialization (`sim::export_state_json`)

- Locale-independent: nlohmann::json serializes doubles with its own
  deterministic shortest round-trip algorithm (never `printf`/locale).
- Fixed precision: every exported float is quantized to 6 decimal places
  (`round(v * 1e6) / 1e6`) before serialization.
- Negative zero is normalized to `+0.0`.
- NaN and infinity are rejected with an exception — they must never appear
  in a valid exported state.
- Stable field ordering: nlohmann objects serialize keys in a fixed
  (alphabetical) order.
- Stable entity ordering: units are exported in ascending id order.

## Verification

- `tests/test_rng.cpp` — same seed ⇒ same sequence; different seed ⇒
  different sequence; bounded, unbiased draws.
- `tests/test_simulation.cpp` — two full runs with identical configuration
  produce identical per-unit state (id, position, health, alive) and
  identical results.
- `tests/test_json_export.cpp` — byte-identical exports for identical runs,
  stable ordering, no `nan`/`inf`/`-0.0`, plain dot-decimal number tokens.
- `tests/test_type_battle.cpp` (M2) — the M1 regression (seeds 42 and 123),
  composition-path/legacy-path bit-for-bit equality, and byte-identical
  double runs with mixed unit types.
- `tests/test_spatial_hash.cpp` (M3) — grid == brute-force nearest enemy
  over thousands of random states (ties, boundaries, edges, dead units,
  empty factions), incremental-maintenance equivalence, every M1/M2
  reference result in both neighbor modes with byte-identical cross-mode
  exports, and byte-identical 5,000-unit double runs in both modes.
- Acceptance runs: the same CLI invocation executed twice with
  `--export-json` must produce a clean `diff` (see `logs/m1-diff.log`,
  `logs/m2-det-*.log` and `logs/m3-*`). Verified for seeds 42, 123, 0 and
  18446744073709551615, for the mixed-type scenario, and for 5,000- and
  20,000-unit battles in both neighbor modes.
