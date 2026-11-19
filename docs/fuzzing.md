# Fuzzing (m13 item 6)

The parsers and the simulation are fuzzed with libFuzzer. Every harness
is a plain function over raw bytes (`fuzz/harnesses.cpp`): the clang
builds (`BUILD_FUZZING=ON`) wrap them in `LLVMFuzzerTestOneInput`
targets, and the normal test suite runs every harness over the
committed seed corpus (`tests/test_fuzz_harnesses.cpp`) — so the
ASan/UBSan CI job exercises the same code paths.

## The harnesses

| target | input surface |
| --- | --- |
| `fuzz_unit_types` | unit type tables (`--config`) |
| `fuzz_scenario` | scenarios (`--scenario`) |
| `fuzz_personalities` / `fuzz_morale` / `fuzz_tactics` / `fuzz_formations` / `fuzz_commanders` / `fuzz_combat` | the data configs (also reachable through `--mod`) |
| `fuzz_terrain_grid` | JSON terrain grids (maps) |
| `fuzz_settings` | the window's settings.json |
| `fuzz_replay` | replay files (written to a scratch file: exactly how untrusted input reaches `parse_replay`) |
| `fuzz_order_script` | `--script` order files |
| `fuzz_simulation` | a valid scenario -> a bounded simulation (at most 1,000 units, 10 ticks) — catches crashes and UB in the state machine, not just the parsers |

The contract: a harness never crashes, hangs or corrupts memory on any
input. Parse rejections are expected — the parsers throw, the harness
catches; a crash, a sanitizer report or a timeout is the finding.

## Running

```
# clang + libFuzzer build
CC=clang CXX=clang++ cmake -S . -B build-fuzz -DCMAKE_BUILD_TYPE=Release \
     -DBUILD_FUZZING=ON
cmake --build build-fuzz -j
./build-fuzz/fuzz_scenario -max_total_time=60 fuzz/corpus/scenario
```

Without clang, the corpus test still runs in the normal suite
(`ctest -R FuzzHarness`).

## CI

- every push: the `Fuzzing` job runs each target for 60 seconds over
  the committed corpus;
- nightly (`.github/workflows/fuzz-nightly.yml`, 03:00 UTC):
  10 minutes per target; crash files upload as artifacts named
  `fuzz-crashes`.

## The corpus

`fuzz/corpus/<target>/` holds the seeds: the shipped data files (the
parsers' known-good shapes) plus shared edge cases (empty, truncated,
deeply nested, huge numbers, NUL bytes, unicode). A finding becomes a
seed: minimize the crash with `-minimize_crash=1`, add the reduced
input to the corpus, fix, and re-run. The corpus is committed so every
run starts from everything the project has already learned.
