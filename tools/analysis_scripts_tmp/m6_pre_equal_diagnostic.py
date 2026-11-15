#!/usr/bin/env python3
"""M6 pre-step 3: equal-army diagnostic, no code changes.

Runs the default scenario (250 infantry vs 250 infantry) for 10 seeds to the
3000-tick limit, exports each final state, and reports per faction:
  - active / routed / dead counts
  - mean and median distance from each active unit to its nearest alive enemy
  - share of active units with no enemy inside their sight radius
All computed from the exported JSON (the simulation itself is untouched).
"""
import json
import statistics
import subprocess
from pathlib import Path

ROOT = Path("/home/z/my-project")
CLI = ROOT / "build" / "battle_cli"
OUT = ROOT / "output"
SEEDS = range(1, 11)

# aggregates: faction -> list of per-seed values
agg = {
    f: {"active": [], "routed": [], "dead": [], "mean_d": [], "median_d": [],
        "no_enemy_share": []}
    for f in ("Red", "Blue")
}

for seed in SEEDS:
    export = OUT / f"m6-pre-equal-{seed}.json"
    subprocess.run(
        [str(CLI), "--seed", str(seed), "--ticks", "3000",
         "--morale", "on", "--export-json", str(export)],
        cwd=ROOT, check=True, capture_output=True)
    state = json.loads(export.read_text())
    units = state["units"]
    # Sight radius per unit type, from the exported unit type table.
    sight = {t["id"]: t["sight_radius"]
             for t in state["config"]["unit_types"]}
    alive = {f: [u for u in units if u["alive"] and u["faction"] == f]
             for f in ("Red", "Blue")}
    for f in ("Red", "Blue"):
        other = "Blue" if f == "Red" else "Red"
        enemies = alive[other]
        active = [u for u in alive[f] if not u["routed"]]
        routed = [u for u in alive[f] if u["routed"]]
        dead = sum(1 for u in units if not u["alive"] and u["faction"] == f)
        dists = []
        no_enemy = 0
        for u in active:
            best = min(((u["x"] - e["x"]) ** 2 + (u["y"] - e["y"]) ** 2) ** 0.5
                       for e in enemies) if enemies else float("inf")
            dists.append(best)
            if best > sight[u["type"]]:
                no_enemy += 1
        agg[f]["active"].append(len(active))
        agg[f]["routed"].append(len(routed))
        agg[f]["dead"].append(dead)
        agg[f]["mean_d"].append(statistics.mean(dists) if dists else 0.0)
        agg[f]["median_d"].append(statistics.median(dists) if dists else 0.0)
        agg[f]["no_enemy_share"].append(
            no_enemy / len(active) if active else 0.0)

print("Equal-army diagnostic: default scenario (250 v 250), seeds 1..10, "
      "tick limit 3000, morale on, no code changes")
print(f"{'':6} {'active':>16} {'routed':>16} {'dead':>16} "
      f"{'mean d':>16} {'median d':>16} {'no-enemy%':>16}")
for f in ("Red", "Blue"):
    a = agg[f]
    def col(key, fmt="{:.1f}"):
        vals = a[key]
        avg = statistics.mean(vals)
        return f"{fmt.format(avg)} [{min(vals):.0f}-{max(vals):.0f}]"
    print(f"{f:6} {col('active'):>16} {col('routed'):>16} {col('dead'):>16} "
          f"{col('mean_d'):>16} {col('median_d'):>16} "
          f"{col('no_enemy_share', '{:.1%}'):>16}")
