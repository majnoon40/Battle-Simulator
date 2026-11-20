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
