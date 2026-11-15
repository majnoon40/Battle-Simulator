#!/bin/bash
# Part 2 pre-step: Alps + Nile balance, 20 seeds/orientation, ticks 1500.
for r in alps-innsbruck nile-delta; do
  ./build/balance --scenario earth-$r --seeds 20 --max-ticks 1500 > logs/m10p2-balance-$r.log 2>&1
  echo BALANCE_${r}_DONE
done
echo M10P2_PRESTEP_DONE
