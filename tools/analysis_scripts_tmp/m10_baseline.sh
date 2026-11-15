#!/bin/bash
for job in viewer-demo default; do
  seq 1 100 | xargs -P2 -I{} scripts/m10_run_seed.sh $job {} off
done
echo DONE
