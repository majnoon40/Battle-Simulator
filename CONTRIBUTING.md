# Contributing

Thanks for considering a contribution. This engine is deliberately
deterministic — almost every rule below exists to protect that property.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

The map generator has its own (offline, no-download) test suite; run it
when you touch `tools/mapgen/`:

```sh
python3 -m unittest discover -s tools/mapgen
```

CI runs the headless build + tests, the game build (compile only), the
mapgen tests and an ASan/UBSan pass on every pull request.

## Determinism rules

The core guarantee: **the same seed produces byte-identical output**. Two
runs of the same battle must produce identical JSON exports; the spatial
hash and brute-force neighbor modes must agree exactly. Before opening a
PR, check your change against both:

```sh
./build/battle_cli --scenario default --seed 42 --export-json a.json
./build/battle_cli --scenario default --seed 42 --export-json b.json
diff a.json b.json
./build/battle_cli --scenario default --seed 42 --neighbor-mode grid --export-json g.json
./build/battle_cli --scenario default --seed 42 --neighbor-mode brute --export-json c.json
diff g.json c.json
```

Rules of thumb when writing engine code:

- The only source of randomness is the seeded `sim::Rng`. Never call
  `rand()`, read the clock, the environment, or anything platform- or
  run-order-dependent from simulation code.
- Iterate containers in a fixed order (unit index, member index); never
  iterate a hash map whose order can vary.
- Floating-point: keep `-fno-fast-math -ffp-contract=off` (already set for
  GCC/Clang); avoid `fast-math` intrinsics or reordered math in the sim.
- Simulation output must not depend on neighbor mode, grid cell size
  (beyond documented resolution limits), or whether events/reports are
  being recorded — those features are passive observers by design.

## Every bug fix needs a test

When you fix a bug, add a test that fails without the fix and passes with
it. This project was largely developed and verified through its test
suite, so an untested fix is an unverifiable one. The same goes for new
features: pin the important behaviors (outputs, edge cases, error
messages) in `tests/` before or with the change.

## Data-driven behavior changes

Unit types, personalities, morale, tactics, terrain and scenarios are JSON
under `data/`. If you change a shipped data file, check whether a test
baseline needs regeneration (baselines under `tests/baselines/` pin exact
exports) and say so in the PR.

## Commits and PRs

- Describe *what* and *why*; the detailed engineering history lives in
  `docs/milestone-*.md`, so a PR that updates the relevant doc is a good
  PR.
- Keep the public interface in mind: CLI flags and export formats are
  compatibility surfaces (the export has a `format_version`); change them
  deliberately and document it.
- By contributing, you agree your contributions are licensed under the
  project's [MIT license](LICENSE).
