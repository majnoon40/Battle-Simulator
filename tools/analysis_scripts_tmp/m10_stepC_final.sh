#!/bin/bash
# Final STEP C evaluation chain (final build, default flags).
seq 1 100 | xargs -P2 -I{} scripts/m10_run_seed.sh default {} on > logs/m10-default-seek2.log 2>&1
echo DEFAULT_FINAL_DONE
seq 1 100 | xargs -P2 -I{} scripts/m10_run_seed.sh viewer-demo {} on > logs/m10-viewer-final.log 2>&1
echo VIEWER_FINAL_DONE
seq 1 100 | xargs -P2 -I{} scripts/m10_run_seed.sh m10-line {} on > logs/m10-line-final.log 2>&1
echo LINE_FINAL_DONE
for r in lebanon alps-innsbruck nile-delta; do
  ./build/balance --scenario earth-$r --seeds 40 > logs/m10-balance-$r-after.log 2>&1
  echo BALANCE_AFTER_${r}_DONE
done
echo STEP_C_FINAL_DONE
