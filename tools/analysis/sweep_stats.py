#!/usr/bin/env python3
"""Statistics for the M2 pre-check 200-seed sweep (report only).

Reads logs/precheck-sweep-200.txt and computes:
- the sweep table (A / B / D win counts and shares)
- exact two-sided binomial p-values vs a fair 50/50 split
- per-seed consistency checks: A-vs-B winner agreement, D-vs-A winner flip,
  tick-count differences between sweeps
"""
import math
import re
from collections import Counter

PATH = "logs/precheck-sweep-200.txt"
LINE = re.compile(
    r"^seed (\d+)\s+A=([RBD])\((\d+)\)\s+B=([RBD])\((\d+)\)\s+D=([RBD])\((\d+)\)$"
)

rows = []
with open(PATH) as handle:
    for line in handle:
        match = LINE.match(line.strip())
        if match:
            rows.append(match.groups())

assert len(rows) == 200, f"expected 200 seed lines, got {len(rows)}"


def binom_two_sided(k, n, p=0.5):
    """Exact two-sided binomial test P(|X - n/2| >= |k - n/2|)."""
    if n == 0:
        return 1.0
    tail = sum(math.comb(n, i) * p**i * (1 - p) ** (n - i) for i in range(k, n + 1))
    return min(1.0, 2 * min(tail, 1.0 - tail + math.comb(n, k) * p**k * (1 - p) ** (n - k)))


print(f"parsed seeds: {len(rows)} (1..{rows[-1][0]})")
print()

for name, col in (("A normal spawn", 1), ("B sides swapped", 3), ("D positions swapped", 5)):
    counts = Counter(row[col] for row in rows)
    red, blue, draw = counts["R"], counts["B"], counts["D"]
    decisive = red + blue
    p_value = binom_two_sided(red, decisive)
    print(f"{name:20s} Red={red:3d}  Blue={blue:3d}  Draw={draw}  "
          f"Red share of decisive={red}/{decisive}={red/decisive:.3f}  "
          f"binomial p={p_value:.3f}")

print()

# Per-seed consistency between sweeps.
same_winner_ab = sum(1 for row in rows if row[1] == row[3])
flip_winner_ad = sum(
    1
    for row in rows
    if (row[1], row[5]) in {("R", "B"), ("B", "R"), ("D", "D")}
)
ticks_equal_ab = sum(1 for row in rows if row[2] == row[4])
ticks_equal_ad = sum(1 for row in rows if row[2] == row[6])
ticks_diff_ab = [int(row[4]) - int(row[2]) for row in rows if row[2] != row[4]]
draws = [row[0] for row in rows if row[1] == "D"]

print(f"A vs B same winner:        {same_winner_ab}/200")
print(f"D is exact flip of A:      {flip_winner_ad}/200")
print(f"A vs B same tick count:    {ticks_equal_ab}/200")
if ticks_diff_ab:
    print(f"A vs B tick differences:   n={len(ticks_diff_ab)}, "
          f"range {min(ticks_diff_ab)}..{max(ticks_diff_ab)}")
print(f"A vs D same tick count:    {ticks_equal_ad}/200")
print(f"draw seeds:                {draws}")
