#!/bin/bash
# wait for acceptance1 to finish, then the remaining batches (forget 1200)
while ! grep -q ALL_DONE logs/m10-acceptance1-progress.log 2>/dev/null; do sleep 20; done
seq 1 100 | xargs -P2 -I{} scripts/m10_run_seed.sh squads {} on > logs/m10-squads-on.log 2>&1
echo SQUADS1200_DONE
seq 1 100 | xargs -P2 -I{} scripts/m10_run_seed.sh m10-line {} on > logs/m10-line-on.log 2>&1
echo LINE_DONE
echo ALL2_DONE
