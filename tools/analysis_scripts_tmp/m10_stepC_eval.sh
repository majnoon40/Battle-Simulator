#!/bin/bash
# STEP C evaluation with the contact-seek build (default flags).
seq 1 100 | xargs -P2 -I{} scripts/m10_run_seed.sh default {} on > logs/m10-default-seek.log 2>&1
echo DEFAULT_SEEK_DONE
seq 1 100 | xargs -P2 -I{} scripts/m10_run_seed.sh viewer-demo {} on > logs/m10-viewer-seek.log 2>&1
echo VIEWER_SEEK_DONE
echo STEP_C_EVAL_DONE
