#!/usr/bin/env python3
"""m11a acceptance sweep: contact time, pre-limit rate, stuck units,
win split per map and orientation.

Runs battle_cli per (scenario, orientation, seed), parses the report's
JSON twin and aggregates the m11a acceptance numbers:

  * first contact before tick 1200 in at least 90% of battles
  * at least 60% end before the tick limit
  * at most 1% of units stuck more than 100 ticks
  * side-averaged win rate (Lebanon target 35-65%)

Usage:
  python3 tools/analysis/nav_acceptance.py \
      --scenario data/scenarios/earth-lebanon-v2.json \
      [--scenario ...] [--seeds 40] [--ticks 1500] [--jobs 3] \
      [--json out.json]

Battles are independent processes, so parallel workers are safe (each
battle is deterministic on its own).
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
CONTACT_TARGET_TICK = 1200


def run_one(battle_cli: str, scenario: str, seed: int, ticks: int,
            swap: bool) -> dict | None:
    with tempfile.TemporaryDirectory() as tmp:
        report = Path(tmp) / "battle.md"
        cmd = [battle_cli, "--scenario", scenario, "--seed", str(seed),
               "--ticks", str(ticks), "--report", str(report)]
        if swap:
            cmd.append("--swap-sides")
        done = subprocess.run(cmd, cwd=REPO, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL)
        if done.returncode != 0 or not report.exists():
            return None
        data = json.loads(
            report.with_suffix(".json").read_text())
        outcome = data["outcome"]
        total_units = sum(f["initial"] for f in data["factions"])
        return {
            "seed": seed,
            "ticks": outcome["ticks"],
            "reason": outcome["reason"],
            "winner": outcome["winner"],
            "first_contact_tick": outcome.get("first_contact_tick", 0),
            "units_stuck_over_100": outcome.get("units_stuck_over_100", 0),
            "longest_stuck_stretch":
                outcome.get("longest_stuck_stretch", 0),
            "total_units": total_units,
        }


def aggregate(rows: list[dict], ticks: int) -> dict:
    n = len(rows)
    decided = [r for r in rows if r["winner"] != "draw"]
    red_wins = sum(1 for r in rows if r["winner"] == "Red")
    blue_wins = sum(1 for r in rows if r["winner"] == "Blue")
    pre_limit = [r for r in rows if r["reason"] != "tick_limit"]
    contact_ok = [r for r in rows
                  if 0 < r["first_contact_tick"] <= CONTACT_TARGET_TICK]
    no_contact = [r for r in rows if r["first_contact_tick"] == 0]
    stuck_units = sum(r["units_stuck_over_100"] for r in rows)
    total_units = sum(r["total_units"] for r in rows)
    out = {
        "battles": n,
        "red_wins": red_wins,
        "blue_wins": blue_wins,
        "draws": n - len(decided),
        "red_share": round(red_wins / n, 3) if n else None,
        "pre_limit": len(pre_limit),
        "pre_limit_share": round(len(pre_limit) / n, 3) if n else None,
        "mean_end_tick": round(sum(r["ticks"] for r in rows) / n, 1)
        if n else None,
        "contact_by_target": len(contact_ok),
        "contact_by_target_share": round(len(contact_ok) / n, 3)
        if n else None,
        "no_contact": len(no_contact),
        "mean_first_contact_tick": round(
            sum(r["first_contact_tick"] for r in rows
                if r["first_contact_tick"] > 0) /
            max(1, len([r for r in rows if r["first_contact_tick"] > 0])),
            1),
        # fight duration after first contact: end tick minus the first
        # sighting, per battle (median = the typical fight)
        "fight_after_contact_median": None,
        "fight_after_contact_mean": None,
        "units_stuck_over_100": stuck_units,
        "stuck_share_of_units": round(stuck_units / total_units, 5)
        if total_units else None,
        "longest_stuck_stretch": max(
            (r["longest_stuck_stretch"] for r in rows), default=0),
    }
    fights = sorted(r["ticks"] - r["first_contact_tick"] for r in rows
                    if r["first_contact_tick"] > 0)
    if fights:
        out["fight_after_contact_median"] = fights[len(fights) // 2]
        out["fight_after_contact_mean"] = round(
            sum(fights) / len(fights), 1)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--scenario", action="append", required=True)
    ap.add_argument("--seeds", type=int, default=40)
    ap.add_argument("--ticks", type=int, default=1500)
    ap.add_argument("--jobs", type=int, default=3)
    ap.add_argument("--battle-cli", default="build/battle_cli")
    ap.add_argument("--orientation", choices=("both", "normal", "swapped"),
                    default="both",
                    help="which orientations to run (default both)")
    ap.add_argument("--json")
    args = ap.parse_args()
    battle_cli = str(REPO / args.battle_cli)

    results = {}
    jobs = []
    for scenario in args.scenario:
        for swap in (False, True):
            if args.orientation == "normal" and swap:
                continue
            if args.orientation == "swapped" and not swap:
                continue
            for seed in range(1, args.seeds + 1):
                jobs.append((scenario, swap, seed))
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {(scenario, swap, seed):
                   pool.submit(run_one, battle_cli, scenario, seed,
                               args.ticks, swap)
                   for scenario, swap, seed in jobs}
        for scenario, swap, seed in jobs:
            row = futures[(scenario, swap, seed)].result()
            key = f"{Path(scenario).stem}{'+swap' if swap else ''}"
            results.setdefault(key, []).append(row)

    summary = {}
    for key, rows in results.items():
        summary[key] = aggregate(rows, args.ticks)
        s = summary[key]
        print(f"{key:38} red {s['red_wins']:2} / blue {s['blue_wins']:2} / "
              f"draw {s['draws']:2} | pre-limit {s['pre_limit_share']:5.1%} "
              f"(target >= 60%) | contact<=1200 "
              f"{s['contact_by_target_share']:5.1%} (target >= 90%) | "
              f"mean end {s['mean_end_tick']:6.1f} | fight after contact "
              f"median {s['fight_after_contact_median']} "
              f"mean {s['fight_after_contact_mean']} | stuck "
              f"{s['stuck_share_of_units']:6.2%} (target <= 1%) | "
              f"longest stuck {s['longest_stuck_stretch']}")

    # side-averaged win rate per scenario (normal + swapped)
    for scenario in args.scenario:
        stem = Path(scenario).stem
        normal = summary.get(stem)
        swapped = summary.get(stem + "+swap")
        if normal and swapped:
            avg = (normal["red_share"] + swapped["red_share"]) / 2
            print(f"{stem:38} side-averaged Red win share: {avg:.1%} "
                  f"(Lebanon target 35-65%)")
    if args.json:
        Path(args.json).write_text(json.dumps(summary, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
