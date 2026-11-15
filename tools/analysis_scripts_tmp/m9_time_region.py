#!/usr/bin/env python3
"""Runs mapgen for one region, reporting wall time and peak RSS."""
import resource
import subprocess
import sys
import time

region = sys.argv[1]
start = time.perf_counter()
result = subprocess.run(
    [sys.executable, "tools/mapgen/mapgen.py", "--region", region],
    capture_output=True, text=True)
elapsed = time.perf_counter() - start
peak_kb = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
print(f"{region}: {elapsed:.1f} s wall, peak RSS {peak_kb / 1024:.0f} MiB, "
      f"exit {result.returncode}")
if result.returncode != 0:
    print(result.stderr[-500:])
    sys.exit(1)
