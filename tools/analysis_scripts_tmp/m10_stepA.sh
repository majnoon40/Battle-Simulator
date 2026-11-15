#!/bin/bash
# STEP A: pending Part 1 batches, on the frozen P1 binary.
export BATTLE_CLI=./build/battle_cli.p1
seq 1 100 | xargs -P2 -I{} scripts/m10_run_seed.sh m10-line {} on > logs/m10-line-100.log 2>&1
echo LINE100_DONE
seq 1 100 | xargs -P2 -I{} scripts/m10_run_seed.sh squads {} on > logs/m10-squads-100.log 2>&1
echo SQUADS100_DONE
for r in lebanon alps-innsbruck nile-delta; do
  ./build/balance --scenario earth-$r --seeds 40 > logs/m10-balance-$r.log 2>&1
  echo BALANCE_${r}_DONE
done
echo STEP_A_DONE
