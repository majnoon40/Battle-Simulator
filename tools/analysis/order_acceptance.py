#!/usr/bin/env python3
"""m11b acceptance: does a scripted flank order change the battle?

Runs two arms over N seeds of one scenario — the control (no orders)
and an identical battle where one side's outer squads execute a flank
march (MoveTo to a flank point, then AttackMove into the enemy rear)
— and reports the difference in outcome, casualties and end tick.

Usage:
  python3 tools/analysis/order_acceptance.py \
      --scenario data/scenarios/earth-lebanon-v2.json \
      --seeds 100 --ticks 1500 --json out.json
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# The flank maneuver: a quarter of Red's squads (the last 10 of 40 —
# squad ids are deterministic: Red first, then Blue) march to the north
# flank before contact, then swing into Blue's rear while the main
# bodies engage.
FLANK_SCRIPT = [
    {"tick": 60, "faction": "red",
     "squads": [30, 31, 32, 33, 34, 35, 36, 37, 38, 39],
     "order": "moveto", "x": 500.0, "y": 150.0},
    {"tick": 450, "faction": "red",
     "squads": [30, 31, 32, 33, 34, 35, 36, 37, 38, 39],
     "order": "attackmove", "x": 850.0, "y": 500.0},
]


def run_one(battle_cli: str, scenario: str, seed: int, ticks: int,
            script_path: str | None) -> dict | None:
    with tempfile.TemporaryDirectory() as tmp:
        report = Path(tmp) / "battle.md"
        cmd = [battle_cli, "--scenario", scenario, "--seed", str(seed),
               "--ticks", str(ticks), "--report", str(report)]
        if script_path:
            cmd += ["--script", script_path]
        done = subprocess.run(cmd, cwd=REPO, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL)
        if done.returncode != 0 or not report.exists():
            return None
        data = json.loads(report.with_suffix(".json").read_text())
        red = next(f for f in data["factions"] if f["name"] == "Red")
        blue = next(f for f in data["factions"] if f["name"] == "Blue")
        return {
            "seed": seed,
            "winner": data["outcome"]["winner"],
            "ticks": data["outcome"]["ticks"],
            "reason": data["outcome"]["reason"],
            "red_deaths": red["deaths"],
            "blue_deaths": blue["deaths"],
            "first_contact": data["outcome"].get("first_contact_tick", 0),
        }


def aggregate(rows: list[dict]) -> dict:
    n = len(rows)
    return {
        "battles": n,
        "red_wins": sum(1 for r in rows if r["winner"] == "Red"),
        "blue_wins": sum(1 for r in rows if r["winner"] == "Blue"),
        "draws": sum(1 for r in rows if r["winner"] == "draw"),
        "mean_end_tick": round(sum(r["ticks"] for r in rows) / n, 1),
        "mean_red_deaths": round(sum(r["red_deaths"] for r in rows) / n, 1),
        "mean_blue_deaths": round(
            sum(r["blue_deaths"] for r in rows) / n, 1),
        "mean_first_contact": round(
            sum(r["first_contact"] for r in rows) / n, 1),
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--scenario", required=True)
    ap.add_argument("--seeds", type=int, default=100)
    ap.add_argument("--ticks", type=int, default=1500)
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--battle-cli", default="build/battle_cli")
    ap.add_argument("--json")
    args = ap.parse_args()
    battle_cli = str(REPO / args.battle_cli)

    script_path = "/tmp/m11b_flank_script.json"
    Path(script_path).write_text(json.dumps(FLANK_SCRIPT, indent=1))

    jobs = []
    for seed in range(1, args.seeds + 1):
        jobs.append(("control", seed, None))
        jobs.append(("flank", seed, script_path))
    results = {"control": [], "flank": []}
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [(arm, pool.submit(run_one, battle_cli, args.scenario,
                                     seed, args.ticks, script))
                   for arm, seed, script in jobs]
        for arm, future in futures:
            row = future.result()
            if row is not None:
                results[arm].append(row)

    summary = {arm: aggregate(rows) for arm, rows in results.items()}
    for arm in ("control", "flank"):
        s = summary[arm]
        print(f"{arm:8} red {s['red_wins']:3} / blue {s['blue_wins']:3} / "
              f"draw {s['draws']:3} | mean end {s['mean_end_tick']:7.1f} | "
              f"Red deaths {s['mean_red_deaths']:6.1f} | "
              f"Blue deaths {s['mean_blue_deaths']:6.1f} | "
              f"contact {s['mean_first_contact']:6.1f}")
    control, flank = summary["control"], summary["flank"]
    print(f"delta   red wins {flank['red_wins'] - control['red_wins']:+d}, "
          f"blue wins {flank['blue_wins'] - control['blue_wins']:+d} | "
          f"end tick {flank['mean_end_tick'] - control['mean_end_tick']:+.1f} | "
          f"Red deaths {flank['mean_red_deaths'] - control['mean_red_deaths']:+.1f} | "
          f"Blue deaths {flank['mean_blue_deaths'] - control['mean_blue_deaths']:+.1f}")
    if args.json:
        Path(args.json).write_text(json.dumps(summary, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
