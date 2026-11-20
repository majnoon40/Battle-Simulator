# PLAYTEST — 5-minute checks per PR

After each PR merges, the fastest way to confirm it works on your
machine. Run from the repository root with a fresh build (see the
README for build commands); every check is headless — no window needed.

## m15 S1 — invariant checker (PR #26)

1. **Happy path stays silent (2 min).** Run a shipped battle with the
   checker on every tick:

   ```
   ./build/battle_cli --scenario default --seed 42 --ticks 1000 --check-invariants
   ```

   What should happen: the normal report prints, no `INVARIANT FAILURE`
   lines, exit code 0 (`echo $?`).

2. **The interval form works (30 s).** Same battle, checking every 10
   ticks:

   ```
   ./build/battle_cli --scenario squads --seed 7 --ticks 500 --check-invariants=10
   ```

   What should happen: again silence and exit 0 (the battle itself is
   byte-identical with the checker on — see the state-hash test).

3. **A big scenario with terrain and many units (1 min).**

   ```
   ./build/battle_cli --scenario earth-lebanon --seed 42 --ticks 300 --swap-sides --check-invariants=10
   ```

   What should happen: silence, exit 0, normal report.

4. **Bad flag values are rejected (30 s).**

   ```
   ./build/battle_cli --check-invariants=0
   ./build/battle_cli --check-invariants=abc
   ```

   What should happen: a clear error message naming the expected range
   `[1, 100000]`, exit code 1.

5. **Threads do not matter (1 min).**

   ```
   ./build/battle_cli --scenario perf-5000 --seed 42 --ticks 300 --threads 4 --check-invariants=25
   ```

   What should happen: silence, exit 0.

What a failure looks like: `INVARIANT FAILURE at tick N: <invariant> —
<detail>` followed by a state dump and `replay for reproduction:
'invariants-failure.replay.json'`, exit code 3. That file replays with
`--replay invariants-failure.replay.json` — please attach it to a bug
report.

## m15 S2 — differential and metamorphic tests

1. **Replay a shipped scenario (1 min).** Record, then verify the
   re-run hash-by-hash:

   ```
   ./build/battle_cli --scenario earth-lebanon --seed 42 --ticks 300 --record /tmp/s2.replay
   ./build/battle_cli --replay /tmp/s2.replay --verify
   ```

   What should happen: the second command ends with `replay verified`
   and exit code 0.

2. **Grid == brute on your machine (1 min).** The spatial grid and
   the O(n^2) scan must produce the same battle:

   ```
   ./build/battle_cli --scenario three-way --seed 42 --ticks 300 --neighbor-mode grid --export-json g.json
   ./build/battle_cli --scenario three-way --seed 42 --ticks 300 --neighbor-mode brute --export-json b.json
   diff g.json b.json
   ```

   What should happen: `diff` prints nothing.

3. **Flag-off reaches the previous release (2 min).** Every
   switchable feature off, inert modes however they like:

   ```
   ./build/battle_cli --scenario default --seed 42 --variance off --ai-avail v1 --end v1 --combat v1 --orders off --neighbor-mode brute --threads 4 --export-json era.json
   diff era.json tests/baselines/m8/default-s42.json
   ```

   What should happen: `diff` prints nothing (the M7-era bytes).

4. **Side-swap is fair in aggregate, not per seed (1 min).**

   ```
   ./build/battle_cli --scenario default --seed 1 --ticks 2000 --swap-sides --report /tmp/sw.md
   ```

   What should happen: a normal report — and a *different* outcome
   than the plain seed-1 run (that is the documented finding:
   `--swap-sides` translates the spawn bands; it is not a mirror, see
   docs/milestone-15.md S2). Over many seeds the wins split roughly
   evenly — the CI job pins seeds 1-50 both orientations at
   Red 49 / Blue 50 / draw 1.
