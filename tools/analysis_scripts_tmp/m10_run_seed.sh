#!/bin/bash
# usage: m10_run_seed.sh <scenario> <seed> <variance>
${BATTLE_CLI:-./build/battle_cli} --scenario "$1" --seed "$2" --variance "$3" 2>/dev/null |
  grep -E "Ticks elapsed|Termination reason|Winner|Total deaths" |
  sed 's/  */ /g' | tr '\n' ';' | sed "s/^/$1 seed $2 /"
echo
