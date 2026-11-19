#!/usr/bin/env python3
"""M14 item 0 — counter balance: rock-paper-scissors on equal budgets.

Pure-type equal-budget armies (data/units-balanced.json, budget form,
19:1 body:commander mix, squad-less — budget armies are size-elastic),
100 paired seeds x 2 orientations per matchup. Reports each directed
matchup's win share with Wilson 95% intervals.

The M14 acceptance: cavalry beats archers, archers beat infantry and
infantry beats cavalry, each with a CI lower bound above 0.5, and no
matchup above 0.70. Only data/combat.json's counters are tuned (the
unit stats are frozen); pass --combat <file> to measure a candidate
without touching the shipped file (it runs through --mod).

Usage: python3 tools/analysis/m14_counters_rps.py \
           [--combat data/combat.json] [--screen] [--label r1] [--jobs 2]
"""
from __future__ import annotations

import argparse
import json
import math
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CLI = REPO / "build/battle_cli"
UNITS = REPO / "data/units-balanced.json"

# A beats B is the acceptance direction (the counter list multiplies
# A's damage into B).
MATCHUPS = [("cavalry", "archer"), ("archer", "infantry"),
            ("infantry", "cavalry")]

BUDGET = 600  # per side; 19:1 body:commander split of the points


def wilson(p_hat: float, n: int, z: float = 1.96) -> tuple[float, float]:
    if n == 0:
        return (0.0, 0.0)
    denom = 1 + z * z / n
    center = (p_hat + z * z / (2 * n)) / denom
    half = z * math.sqrt(p_hat * (1 - p_hat) / n + z * z / (4 * n * n)) / denom
    return (max(0.0, center - half), min(1.0, center + half))


def scenario_path(out_dir: Path, winner: str, loser: str) -> Path:
    scenario = {
        "battlefield": {"width": 1000.0, "height": 1000.0},
        "spawn_band_fraction": 0.15,
        "armies": {
            "red": {"budget": BUDGET, "mix": {winner: 19, "commander": 1}},
            "blue": {"budget": BUDGET, "mix": {loser: 19, "commander": 1}},
        },
    }
    path = out_dir / f"budget_{winner}_vs_{loser}.json"
    path.write_text(json.dumps(scenario, indent=2))
    return path


def run_one(scenario: Path, mod_dir: Path, seed: int, swap: bool):
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "b.json"
        cmd = [str(CLI), "--config", str(UNITS), "--scenario", str(scenario),
               "--seed", str(seed), "--ticks", "3000",
               "--mod", str(mod_dir),
               "--export-json", str(out)]
        if swap:
            cmd.append("--swap-sides")
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              cwd=str(REPO))
        if proc.returncode != 0 or not out.exists():
            return None
        data = json.loads(out.read_text())
        return {"win": data["termination"]["winner"],
                "end": data["ticks_elapsed"],
                "reason": data["termination"]["reason"]}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--combat", default="data/combat.json",
                        help="combat.json to measure (copied into a --mod "
                             "overlay; the shipped file is not modified)")
    parser.add_argument("--screen", action="store_true",
                        help="20 seeds instead of 100 (steering only)")
    parser.add_argument("--label", default="round",
                        help="measurement directory label")
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()

    combat = (REPO / args.combat).resolve()
    if not combat.exists():
        sys.exit(f"no such combat file: {combat}")
    if not CLI.exists():
        sys.exit(f"battle_cli not built: {CLI}")

    # The overlay dir only needs the combat file; everything else falls
    # through to the shipped data.
    mod_dir = REPO / "measurements" / "counters" / args.label / "mod"
    if mod_dir.exists():
        shutil.rmtree(mod_dir)
    mod_dir.mkdir(parents=True)
    shutil.copy(combat, mod_dir / "combat.json")

    seeds = list(range(1, 21)) if args.screen else list(range(1, 101))
    out_dir = mod_dir.parent
    results = {}
    for winner, loser in MATCHUPS:
        path = scenario_path(out_dir, winner, loser)
        rows = {}
        for swap in (False, True):
            with ThreadPoolExecutor(max_workers=args.jobs) as pool:
                rows[swap] = list(pool.map(
                    lambda s: run_one(path, mod_dir, s, swap), seeds))
            if any(r is None for r in rows[swap]):
                sys.exit(f"a battle failed for {winner} vs {loser}")
        # The winner type is Red unswapped, Blue swapped; draws count in
        # the denominator (the established matrix convention).
        wins = (sum(1 for r in rows[False] if r["win"] == "Red") +
                sum(1 for r in rows[True] if r["win"] == "Blue"))
        total = len(rows[False]) + len(rows[True])
        draws = sum(1 for swap in (False, True)
                    for r in rows[swap] if r["win"] == "draw")
        ends = [r["end"] for swap in (False, True) for r in rows[swap]]
        reasons: dict[str, int] = {}
        for swap in (False, True):
            for r in rows[swap]:
                reasons[r["reason"]] = reasons.get(r["reason"], 0) + 1
        share = wins / total if total else 0.0
        lo, hi = wilson(share, total)
        results[(winner, loser)] = {
            "share": share, "wins": wins, "total": total, "draws": draws,
            "lo": lo, "hi": hi,
            "mean_end": sum(ends) / len(ends) if ends else 0.0,
            "reasons": reasons,
        }
        r = results[(winner, loser)]
        print(f"{winner:9} vs {loser:9}: {share:.3f} "
              f"[{r['lo']:.3f}, {r['hi']:.3f}] ({wins}/{total}, "
              f"{draws} draws), mean end {r['mean_end']:.0f}, "
              f"reasons {reasons}", flush=True)

    # The M14 acceptance: every CI lower bound above 0.5 and no share
    # above 0.70.
    ok = True
    for (winner, loser), r in results.items():
        if not (r["lo"] > 0.5 and r["share"] <= 0.70):
            ok = False
    verdict = ", ".join(
        f"{w}>{l} {r['share']:.3f}" for (w, l), r in results.items())
    print("acceptance (lo > 0.5 each, share <= 0.70): " +
          ("PASS " if ok else "FAIL ") + verdict)

    summary = {"combat": args.combat, "seeds": len(seeds),
               "label": args.label, "budget": BUDGET,
               "matchups": {f"{w}_vs_{l}": r for (w, l), r in results.items()},
               "acceptance_pass": ok}
    (out_dir / "summary.json").write_text(json.dumps(summary, indent=2))
    print(f"summary: {out_dir / 'summary.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
