#!/usr/bin/env python3
"""M13 item H(b) — RPS balance matrix for a unit-type table.

Runs the unit-matrix protocol (pure-type armies, 250 vs 250 + 10
commanders per side, squads on, the shipped scenario shape) with an
explicit --config unit table (data/units-balanced.json for the tuning
rounds; data/units.json reproduces the item-3 baseline). 100 paired
seeds x 2 orientations by default; --screen cuts to 20 seeds for quick
steering between measured rounds. Reports each matchup's win share for
type A (red = attacker type, blue = defender type, both orientations
merged) with Wilson 95% intervals, plus the RPS verdict: does every
type beat at least one and lose to at least one other?

Usage: python3 tools/analysis/m13_balance_rps.py \
           [--config data/units-balanced.json] [--screen] [--label r1]
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

REPO = Path(__file__).resolve().parents[2]
CLI = REPO / "build/battle_cli"

MATCHUPS = [("infantry", "archer"), ("archer", "cavalry"),
            ("cavalry", "infantry")]


def wilson(p_hat: float, n: int, z: float = 1.96) -> tuple[float, float]:
    if n == 0:
        return (0.0, 0.0)
    denom = 1 + z * z / n
    center = (p_hat + z * z / (2 * n)) / denom
    half = z * math.sqrt(p_hat * (1 - p_hat) / n + z * z / (4 * n * n)) / denom
    return (max(0.0, center - half), min(1.0, center + half))


def scenario_path(out_dir: Path, attacker: str, defender: str) -> Path:
    # Same shape as the item-3 unit matrix: squads on, 10 commanders per
    # side (squad-less pure-type battles are cascade-degenerate).
    scenario = {
        "battlefield": {"width": 1000.0, "height": 1000.0},
        "spawn_band_fraction": 0.15,
        "armies": {
            "red": {"commander": 10, attacker: 250},
            "blue": {"commander": 10, defender: 250},
        },
        "squads": {
            "red": {"count": 10, "size": 25},
            "blue": {"count": 10, "size": 25},
        },
    }
    path = out_dir / f"matrix_{attacker}_vs_{defender}.json"
    path.write_text(json.dumps(scenario, indent=2))
    return path


def run_one(config: Path, path: Path, seed: int, swap: bool):
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "b.json"
        cmd = [str(CLI), "--config", str(config), "--scenario", str(path),
               "--seed", str(seed), "--ticks", "3000",
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
    parser.add_argument("--config", default="data/units.json")
    parser.add_argument("--screen", action="store_true",
                        help="20 seeds instead of 100 (steering only)")
    parser.add_argument("--label", default="round",
                        help="measurement directory label")
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()

    config = (REPO / args.config).resolve()
    if not config.exists():
        sys.exit(f"no such config: {config}")
    seeds = list(range(1, 21)) if args.screen else list(range(1, 101))
    out_dir = REPO / "measurements" / "balance_rps" / args.label
    out_dir.mkdir(parents=True, exist_ok=True)

    results = {}
    for attacker, defender in MATCHUPS:
        path = scenario_path(out_dir, attacker, defender)
        rows = {}
        for swap in (False, True):
            with ThreadPoolExecutor(max_workers=args.jobs) as pool:
                rows[swap] = list(pool.map(
                    lambda s: run_one(config, path, s, swap), seeds))
            if any(r is None for r in rows[swap]):
                sys.exit(f"a battle failed for {attacker} vs {defender}")
        # Type A (attacker) is Red unswapped, Blue swapped; draws count
        # in the denominator (the item-3 matrix convention).
        a_wins = (sum(1 for r in rows[False] if r["win"] == "Red") +
                  sum(1 for r in rows[True] if r["win"] == "Blue"))
        total = len(rows[False]) + len(rows[True])
        draws = sum(1 for swap in (False, True)
                    for r in rows[swap] if r["win"] == "draw")
        ends = [r["end"] for swap in (False, True) for r in rows[swap]]
        share = a_wins / total if total else 0.0
        results[(attacker, defender)] = {
            "share": share, "wins": a_wins, "total": total, "draws": draws,
            "lo": wilson(share, total)[0], "hi": wilson(share, total)[1],
            "mean_end": sum(ends) / len(ends) if ends else 0.0,
        }
        r = results[(attacker, defender)]
        print(f"{attacker:9} vs {defender:9}: {share:.3f} "
              f"[{r['lo']:.3f}, {r['hi']:.3f}] ({a_wins}/{total}, "
              f"{draws} draws), mean end {r['mean_end']:.0f}", flush=True)

    # RPS verdict over the directed win shares.
    def beats(a: str, b: str) -> bool:
        for (x, y), r in results.items():
            if x == a and y == b:
                return r["share"] >= 0.5
        return False

    verdict = []
    for a, b in MATCHUPS:
        verdict.append(f"{a}{'>' if beats(a, b) else '<'}{b}")
    print("verdict: " + ", ".join(verdict))
    beats_any = {t: any(beats(t, o) for o in ("infantry", "archer", "cavalry")
                        if o != t)
                 for t in ("infantry", "archer", "cavalry")}
    loses_any = {t: any(beats(o, t) for o in ("infantry", "archer", "cavalry")
                         if o != t)
                 for t in ("infantry", "archer", "cavalry")}
    ok = all(beats_any.values()) and all(loses_any.values())
    print("RPS " + ("PASS" if ok else "FAIL") +
          " (each type beats >= 1: " +
          ", ".join(f"{t}={'y' if v else 'n'}" for t, v in beats_any.items())
          + "; loses to >= 1: " +
          ", ".join(f"{t}={'y' if v else 'n'}" for t, v in loses_any.items())
          + ")")

    summary = {"config": args.config, "seeds": len(seeds),
               "label": args.label,
               "matchups": {f"{a}_vs_{b}": r for (a, b), r in results.items()},
               "rps_pass": ok}
    (out_dir / "summary.json").write_text(json.dumps(summary, indent=2))
    print(f"summary: {out_dir / 'summary.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
