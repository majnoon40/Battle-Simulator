#!/bin/bash
# Part 1 acceptance batches (variance ON, search-first semantics).
seq 1 100 | xargs -P2 -I{} scripts/m10_run_seed.sh squads {} on > logs/m10-squads-on.log 2>&1
echo SQUADS_DONE
seq 1 100 | xargs -P2 -I{} scripts/m10_run_seed.sh default {} on > logs/m10-default-on.log 2>&1
echo DEFAULT_DONE
seq 1 100 | xargs -P2 -I{} scripts/m10_run_seed.sh viewer-demo {} on > logs/m10-viewer-on.log 2>&1
echo VIEWER_DONE
seq 1 100 | xargs -P2 -I{} bash -c './build/battle_cli --scenario default --variance on --swap-sides --seed {} 2>/dev/null | grep -E "Ticks elapsed|Termination reason|Winner|Total deaths" | sed "s/  */ /g" | tr "\n" ";" | sed "s/^/default-swap seed {} /"; echo' > logs/m10-default-swap.log 2>&1
echo SWAP_DONE
for r in lebanon alps-innsbruck nile-delta; do
  ./build/balance --scenario earth-$r --seeds 40 > logs/m10-balance-$r.log 2>&1
  echo BALANCE_${r}_DONE
done
echo ALL_DONE
