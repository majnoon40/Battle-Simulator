#!/bin/bash
for region in lebanon alps-innsbruck nile-delta; do
  for s in $(seq 1 20); do
    ./build/battle_cli --scenario earth-$region --seed $s 2>/dev/null |
      grep -E "Ticks elapsed|Termination reason|Winner|Total deaths|Longest stuck|Units stuck" |
      sed 's/  */ /g' | tr '\n' ';' | sed "s/^/$region seed $s /"
    echo
  done
done
