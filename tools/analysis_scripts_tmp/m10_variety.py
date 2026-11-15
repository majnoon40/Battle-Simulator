#!/usr/bin/env python3
"""M10 STEP B variety metrics from a m10_run_seed.sh log: pre-limit
share, rout end-tick quartiles, winner split, distinct end ticks."""
import re
import statistics
import sys
from collections import Counter


def quartiles(vals):
    vals = sorted(vals)
    n = len(vals)

    def q(p):
        idx = p * (n - 1)
        lo = int(idx)
        frac = idx - lo
        return vals[lo] + (vals[min(lo + 1, n - 1)] - vals[lo]) * frac

    return vals[0], q(0.25), q(0.5), q(0.75), vals[-1]


def analyze(name, path):
    rows = []
    for line in open(path):
        m = re.match(
            r'\S+ seed (\d+) Ticks elapsed:\s+(\d+).*?Termination reason:'
            r'\s+(\S+);Winner:\s+(\S+);Total deaths:\s+(\d+)', line)
        if m:
            rows.append(m.groups())
    if not rows:
        print(f"{name}: no rows in {path}")
        return
    ends = [int(r[1]) for r in rows]
    pre = [r for r in rows if r[2] != 'tick_limit']
    rout = [int(r[1]) for r in pre if r[2] == 'faction_routed']
    print(f"=== {name} (n={len(rows)}) ===")
    print(f"  winners: {dict(Counter(r[3] for r in rows))}")
    print(f"  reasons: {dict(Counter(r[2] for r in rows))}")
    print(f"  share ending before the tick limit: {len(pre)/len(rows)*100:.0f}%")
    if rout:
        q = quartiles(rout)
        print(f"  rout end-tick quartiles (n={len(rout)}): "
              f"min {q[0]:.0f} / 25% {q[1]:.0f} / median {q[2]:.0f} / "
              f"75% {q[3]:.0f} / max {q[4]:.0f}")
    print(f"  distinct end-tick values: {len(set(ends))}/{len(ends)}")
    print(f"  deaths mean {statistics.mean(int(r[4]) for r in rows):.0f}")
    print()


if __name__ == '__main__':
    analyze(sys.argv[1], sys.argv[2])
