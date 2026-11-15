# Milestone 3 — Performance foundation (spatial hash)

## Scope

Replace the O(n) nearest-enemy scan with a spatial hash grid so thousands of
units run fast, with identical behavior: the same nearest-enemy choice and
the same lowest-id tie-break as M1/M2. The unit cap rises to 20,000; the
brute-force scan stays as a test oracle and a CLI baseline
(`--neighbor-mode brute|grid`, default grid). No AI, morale, squads or
terrain.

## Design decisions and assumptions

1. **Profile first.** At 5,000 units the brute scan is 98.4% of tick time
   (16.15 of 16.42 ms/tick, timer evidence) and 99.38% of gprof self time
   (`logs/m3-profile-before-5000.log`, `logs/m3-gprof-flat.log`). The
   optimization targets exactly that function.
2. **Uniform grid, no hashing.** The battlefield is partitioned into
   `ceil(width/cs) x ceil(height/cs)` square cells indexed directly
   (`col * rows + row`) — a perfect hash. No `unordered_map` anywhere in the
   grid: cells are addressed and iterated in fixed (row, col) order, and the
   simulation inserts/updates/removes in fixed unit-index order. Query
   results are additionally independent of intra-bucket order because the
   minimum over (squared distance, unit id) is unique.
3. **Exact nearest-enemy query.** Expanding Chebyshev rings of cells are
   scanned outward from the query's cell; every candidate is compared with
   the exact brute-force rule (strictly closer wins, exact ties to the lower
   unit id). A ring is skipped only when every point in it is strictly
   farther than the best candidate found so far (bound: any point in a
   ring-r cell is at squared distance >= ((r-1)*cs)^2), so an equal-distance
   lower-id unit can never be skipped. Out-of-world coordinates clamp to
   border cells, which preserves the bound. Dead units are re-checked at
   query time (defense in depth) so state mutated behind the simulation's
   back still yields the brute-force answer — M1's
   `DeathTest.DeadUnitsAreNotTargeted` depends on this.
4. **Occupancy bitmaps.** Each faction keeps a row-major and a column-major
   occupancy bitmap (one bit per cell). Ring scans test whole row/column
   segments with a few 64-bit word loads and only visit occupied cells; this
   is what keeps far-target queries (armies approaching across the field)
   cheap. Measured effect at 5,000 units: 388 -> ~600 ticks/s; at 20,000:
   17 -> 33 ticks/s.
5. **Grid lifecycle.** Built once after spawning (or adopting an explicit
   state) in unit-index order; then maintained incrementally: moved units
   are re-bucketed after the movement phase, dead units are removed at the
   moment of death. The grid stores unit indices only; the world vector
   remains the authoritative state.
6. **Unit cap 20,000.** `sim::kMaxUnits` was 500 (M1/M2 documented but M1
   never rejected larger values; M2 enforced it). Battles at or below 500
   units are bit-for-bit unchanged (all M1/M2 reference results re-verified
   in both neighbor modes). The two M2 cap tests were updated to the new
   limit (11,000+11,000 and 20,001 instead of 260+260 and 501) — the
   assertions are not weakened, they track the raised cap.
7. **Cell size** is configurable (`--grid-cell`, `SimulationConfig::
   grid_cell_size`), default 16.0 world units — the best of the swept sizes
   (8/16/32/64/128) at both 5,000 and 20,000 units after the bitmap change.
8. **Steady-state allocations.** Scratch buffers are sized at construction;
   grid buckets are reserved for ~64x the spawn-band density (the densest
   starting configuration). Verified by an operator-new-counting unit test
   (static-wall battle: 0 allocations in the measured window) and by the
   harness on real battles: 5,000 units -> 0 allocations after warm-up;
   20,000 units -> 52 capacity-doubling events over 550 ticks during front
   convergence (~0.1 per tick, amortized; buffers are reused, never churned
   per tick).
9. **Export unchanged.** The JSON export contains no neighbor-mode or grid
   fields, so grid-mode and brute-mode exports are byte-identical for the
   same seed (verified at 5,000 and 20,000 units).

## Verification results (2026-11-13)

- 125/125 GoogleTest tests pass (109 M1/M2 + 16 new), `ctest`, Release.
- Property test: grid == brute-force nearest enemy over 400 random states
  (~8,000 comparisons) with shuffled ids, exact-duplicate positions (ties),
  cell-boundary and world-edge coordinates, out-of-world overshoots, dead
  units, empty factions and single-unit armies; plus targeted edge cases,
  incremental-maintenance-vs-rebuild equivalence and radius-query checks.
- Regression: seeds 42 (Blue, 456, 0/64), 123 (Blue, 713, 0/6), 5 (draw,
  777, 0/0), 0 (Blue, 709, 0/26), 2^64-1 (Red, 633, 48/0) and mixed seed 42
  (Blue, 664, 0/22) reproduce exactly in BOTH neighbor modes; every seed's
  grid export is byte-identical to its brute export.
- Determinism: 5,000-unit double runs byte-identical in both modes; the
  CLI seed-42 500-unit double run and 5k/20k cross-mode exports are
  byte-identical (`cmp`).
- Benchmarks (same 2-vCPU sandbox, same Release build, seed 42, 600 ticks,
  `--ticks 600` recipe; the CLI prints ticks actually run, ticks/s and
  ms/tick):
  - 5,000 units: brute 59.6 ticks/s (16.78 ms/tick) vs grid 597-612
    ticks/s (1.63-1.67 ms/tick) — **10.3x faster, well above the 60
    ticks/s bar**.
  - 20,000 units (stretch): grid 33.1 ticks/s (30.2 ms/tick) vs brute
    1.9 ticks/s (513.6 ms/tick, 20-tick run) — 16.4x, but **below 60
    ticks/s**: the remaining cost is the approach-phase ring walk
    (gprof: `scan_cell` 43%, `segment_has_units` 27%, `nearest_enemy`
    19%). A hierarchical grid would be the next step; not attempted here.
- Cell-size sweep (grid, 600 ticks): 5,000 units — cs8 493, cs16 600-611,
  cs32 493, cs64 320, cs128 191 ticks/s; 20,000 units — cs16 33.1,
  cs32 31.6, cs64 21.3 ticks/s. Default 16.0.
- Purity: no `raylib`/`std::chrono`/`random_device`/`rand(`/`getrusage` and
  no file I/O in `sim/`; `unordered_set` appears only in input validation,
  never in results; exports contain no time/date/path/host/neighbor/grid
  values.

Full command logs live in `logs/m3-*.log`.
