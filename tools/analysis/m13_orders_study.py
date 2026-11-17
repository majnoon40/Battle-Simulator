#!/usr/bin/env python3
"""M13 orders study — do orders matter? (Item 0.2 / Item 3 acceptance)

Balanced open-field battle (data/scenarios/squads.json: 250v250 + 10
commanders per side, 10 squads of 25 on a 1000x1000 field), paired seeds,
five arms — the ordered faction is Red in both orientations so the
side-averaged win rate averages away the spawn-band asymmetry:

  none       — control, no orders
  attackmove — all squads AttackMove to the enemy centroid at tick 150
               (probed from control runs per orientation)
  flank      — squads 0-2 MoveTo the north flank at tick 60, then
               AttackMove into the enemy rear at tick 400
  hold       — all squads Hold at tick 100
  retreat    — all squads Retreat (toward the own side) at tick 150

Per arm, per orientation, N seeds; every arm runs the same seeds. Reports
win share (Wilson 95% CI), mean end tick, mean per-battle kill ratio
(enemy deaths / own deaths) of the ordered side, plus paired per-seed
deltas vs the control. --swap-sides gives the second orientation.
"""
from __future__ import annotations

import argparse
import json
import math
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent / "m13"


def wilson(p_hat: float, n: int, z: float = 1.96) -> tuple[float, float]:
    if n == 0:
        return (0.0, 0.0)
    denom = 1 + z * z / n
    center = (p_hat + z * z / (2 * n)) / denom
    half = z * math.sqrt(p_hat * (1 - p_hat) / n + z * z / (4 * n * n)) / denom
    return (max(0.0, center - half), min(1.0, center + half))


def mean_ci(values: list[float]) -> tuple[float, float, float]:
    n = len(values)
    if n == 0:
        return (0.0, 0.0, 0.0)
    mean = sum(values) / n
    if n < 2:
        return (mean, mean, mean)
    variance = sum((v - mean) ** 2 for v in values) / (n - 1)
    se = math.sqrt(variance / n)
    return (mean, mean - 1.96 * se, mean + 1.96 * se)


def run_one(seed: int, ticks: int, script: list[dict] | None, swap: bool,
            scenario: str, battle_cli: str,
            extra: list[str] | None = None) -> dict | None:
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "battle.json"
        cmd = [battle_cli, "--scenario", scenario, "--seed", str(seed),
               "--ticks", str(ticks), "--orders", "on",
               "--export-json", str(out)]
        if extra:
            cmd += extra
        if script:
            script_path = Path(tmp) / "orders.json"
            script_path.write_text(json.dumps(script))
            cmd += ["--script", str(script_path)]
        if swap:
            cmd += ["--swap-sides"]
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              cwd=str(REPO))
        if proc.returncode != 0 or not out.exists():
            return None
        data = json.loads(out.read_text())
        red_dead = sum(1 for u in data["units"]
                       if u["faction"] == "Red" and not u["alive"])
        blue_dead = sum(1 for u in data["units"]
                        if u["faction"] == "Blue" and not u["alive"])
        winner = data["termination"]["winner"]
        # Ordered side is Red in both orientations; winner is "Red"/"Blue"
        # (to_string(Faction)) or "draw".
        return {"win": 1.0 if winner == "Red" else 0.0,
                "draw": 1.0 if winner == "draw" else 0.0,
                "end": float(data["ticks_elapsed"]),
                "kills": float(blue_dead),
                "losses": float(red_dead)}


def probe_centroid(battle_cli: str, scenario: str, seeds: list[int],
                   swap: bool) -> tuple[float, float]:
    """Enemy (Blue) centroid at tick 150, averaged over probe seeds."""
    xs: list[float] = []
    ys: list[float] = []
    for seed in seeds:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "battle.json"
            cmd = [battle_cli, "--scenario", scenario, "--seed", str(seed),
                   "--ticks", "150", "--export-json", str(out)]
            if swap:
                cmd += ["--swap-sides"]
            subprocess.run(cmd, capture_output=True, text=True, cwd=str(REPO))
            data = json.loads(out.read_text())
            blue = [u for u in data["units"]
                    if u["faction"] == "Blue" and u["alive"]]
            if blue:
                xs.append(sum(u["x"] for u in blue) / len(blue))
                ys.append(sum(u["y"] for u in blue) / len(blue))
    return (sum(xs) / len(xs) if xs else 500.0,
            sum(ys) / len(ys) if ys else 500.0)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--scenario", default="data/scenarios/squads.json")
    ap.add_argument("--seeds", type=int, default=200)
    ap.add_argument("--ticks", type=int, default=1500)
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--battle-cli", default="build/battle_cli")
    ap.add_argument("--json", help="write raw per-seed rows here")
    ap.add_argument("--cli-extra", default="",
                    help="extra battle_cli flags for every run")
    ap.add_argument("--arms", default="none,attackmove,flank,hold,retreat",
                    help="comma-separated arms to run")
    args = ap.parse_args()

    battle_cli = str(REPO / args.battle_cli)
    seeds = list(range(1, args.seeds + 1))
    probe_seeds = seeds[:6]

    centroids = {}
    for swap in (False, True):
        centroids[swap] = probe_centroid(battle_cli, args.scenario,
                                         probe_seeds, swap)
        print(f"enemy centroid @150 (swap={swap}): "
              f"({centroids[swap][0]:.1f}, {centroids[swap][1]:.1f})",
              flush=True)

    def flank_script(swap: bool) -> list[dict]:
        cx, cy = centroids[swap]
        # Enemy rear: push 150 units beyond the enemy centroid along the
        # attack axis (Blue sits right normally, left when swapped).
        rear_x = cx + (150.0 if not swap else -150.0)
        return [
            {"tick": 60, "faction": "red", "squads": [0, 1, 2],
             "order": "moveto", "x": 500.0, "y": 120.0},
            {"tick": 400, "faction": "red", "squads": [0, 1, 2],
             "order": "attackmove", "x": rear_x, "y": cy},
        ]

    def attackmove_script(swap: bool) -> list[dict]:
        cx, cy = centroids[swap]
        return [{"tick": 150, "faction": "red", "squads": "all",
                 "order": "attackmove", "x": cx, "y": cy}]

    all_arms: dict[str, callable] = {
        "none": lambda swap: None,
        "attackmove": attackmove_script,
        "flank": flank_script,
        "hold": lambda swap: [
            {"tick": 100, "faction": "red", "squads": "all",
             "order": "hold"}],
        "retreat": lambda swap: [
            {"tick": 150, "faction": "red", "squads": "all",
             "order": "retreat"}],
    }

    arms = {name: all_arms[name] for name in
            [a.strip() for a in args.arms.split(",")]}
    extra = args.cli_extra.split() if args.cli_extra else None
    rows: list[dict] = []
    for arm, script_for in arms.items():
        for swap in (False, True):
            script = script_for(swap)
            label = arm + ("+swap" if swap else "")
            with ThreadPoolExecutor(max_workers=args.jobs) as pool:
                results = list(pool.map(
                    lambda s: run_one(s, args.ticks, script, swap,
                                      args.scenario, battle_cli, extra),
                    seeds))
            ok_seeds = [seeds[i] for i, r in enumerate(results)
                        if r is not None]
            ok = [r for r in results if r is not None]
            failed = len(results) - len(ok)
            if failed:
                print(f"{label}: WARNING {failed} runs failed", flush=True)
            wins = sum(r["win"] for r in ok)
            draws = int(sum(r["draw"] for r in ok))
            kills = sum(r["kills"] for r in ok)
            losses = sum(r["losses"] for r in ok)
            ratios = [r["kills"] / max(r["losses"], 1.0) for r in ok]
            ends = [r["end"] for r in ok]
            m, lo, hi = mean_ci(ratios)
            e, elo, ehi = mean_ci(ends)
            n = len(ok)
            print(f"{label:<16} red {int(wins):3d} / blue "
                  f"{n - int(wins) - draws:3d} / draw {draws:3d} | "
                  f"end {e:7.1f} | kill ratio {m:6.3f} | "
                  f"kills {int(kills)} losses {int(losses)}", flush=True)
            for seed, r in zip(ok_seeds, ok):
                rows.append({"arm": arm, "swap": swap, "seed": seed, **r})
        sys.stdout.flush()

    # Side-averaged summary per arm.
    print("\nSide-averaged (ordered faction = Red in both orientations):",
          flush=True)
    for arm in arms:
        sub = [r for r in rows if r["arm"] == arm]
        n = len(sub)
        p = sum(r["win"] for r in sub) / n if n else 0.0
        lo, hi = wilson(p, n)
        ratios = [r["kills"] / max(r["losses"], 1.0) for r in sub]
        m, _, _ = mean_ci(ratios)
        ends = [r["end"] for r in sub]
        e, _, _ = mean_ci(ends)
        print(f"  {arm:<12} win share {p:5.3f} [{lo:5.3f}, {hi:5.3f}] | "
              f"end tick {e:7.1f} | kill ratio {m:6.3f}", flush=True)

    # Paired deltas vs control (same seeds and orientation).
    print("\nPaired deltas vs none (mean, 95% CI):", flush=True)
    control = {(r["swap"], r["seed"]): r for r in rows if r["arm"] == "none"}
    for arm in [a for a in arms if a != "none"]:
        if arm == "none":
            continue
        sub = [r for r in rows if r["arm"] == arm]
        d_ends: list[float] = []
        d_ratios: list[float] = []
        for r in sub:
            c = control.get((r["swap"], r["seed"]))
            if c is None:
                continue
            d_ends.append(r["end"] - c["end"])
            d_ratios.append(r["kills"] / max(r["losses"], 1.0) -
                            c["kills"] / max(c["losses"], 1.0))
        me, elo, ehi = mean_ci(d_ends)
        mr, rlo, rhi = mean_ci(d_ratios)
        print(f"  {arm}:ticks   {me:+9.3f}  [{elo:+10.3f}, {ehi:+10.3f}]",
              flush=True)
        print(f"  {arm}:kills   {mr:+9.3f}  [{rlo:+10.3f}, {rhi:+10.3f}]",
              flush=True)

    if args.json:
        Path(args.json).write_text(json.dumps(rows))
        print(f"\nraw rows: {args.json}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
