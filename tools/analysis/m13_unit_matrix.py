#!/usr/bin/env python3
"""M13 item 3 acceptance — the unit matrix.

Pure-type armies (250 vs 250 on the open 1000x1000 field, no squads):
infantry vs archer, archer vs cavalry, cavalry vs infantry — 100 paired
seeds x 2 orientations (--swap-sides mirrors the setup, so the same seed
runs both spawn orientations). Reports each side's win share; the
acceptance target is that no type wins everything and each type beats at
least one other.
"""
from __future__ import annotations

import json
import math
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path("/home/z/my-project/m13")
CLI = REPO / "build/battle_cli"
OUT = Path("/home/z/my-project/m13/measurements/unit_matrix")
OUT.mkdir(parents=True, exist_ok=True)

MATCHUPS = [("infantry", "archer"), ("archer", "cavalry"),
            ("cavalry", "infantry")]
SEEDS = list(range(1, 101))


def wilson(p_hat: float, n: int, z: float = 1.96) -> tuple[float, float]:
    if n == 0:
        return (0.0, 0.0)
    denom = 1 + z * z / n
    center = (p_hat + z * z / (2 * n)) / denom
    half = z * math.sqrt(p_hat * (1 - p_hat) / n + z * z / (4 * n * n)) / denom
    return (max(0.0, center - half), min(1.0, center + half))


def scenario_path(attacker: str, defender: str) -> Path:
    # The shipped scenarios run squads (a commander per squad): the
    # squad-less variant produces cascade-degenerate battles where the
    # spawn side decides everything (measured 100/0 in both orientations
    # of a mirror matchup's neighborhood). Same shape as
    # data/scenarios/squads.json.
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
    path = OUT / f"matrix_{attacker}_vs_{defender}.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(scenario, indent=2))
    return path


def run_one(path: Path, seed: int, swap: bool):
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "b.json"
        cmd = [str(CLI), "--scenario", str(path), "--seed", str(seed),
               "--ticks", "3000", "--export-json", str(out)]
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
    results: dict[tuple[str, str], dict] = {}
    for attacker, defender in MATCHUPS:
        path = scenario_path(attacker, defender)
        for swap in (False, True):
            label = f"{attacker} vs {defender}" + (" +swap" if swap else "")
            with ThreadPoolExecutor(max_workers=2) as pool:
                rows = list(pool.map(
                    lambda s: run_one(path, s, swap), SEEDS))
            ok = [r for r in rows if r is not None]
            wins = sum(1 for r in ok if r["win"] == "Red")
            draws = sum(1 for r in ok if r["win"] == "draw")
            n = len(ok)
            p = wins / n if n else 0.0
            lo, hi = wilson(p, n)
            print(f"{label:24s} red({attacker}) {wins:3d} / blue({defender})"
                  f" {n - wins - draws:3d} / draw {draws:3d} | "
                  f"{attacker} win share {p:.3f} [{lo:.3f}, {hi:.3f}] | "
                  f"mean end {sum(r['end'] for r in ok) / max(n, 1):7.1f}",
                  flush=True)
            results[(attacker, defender, swap)] = ok
    (OUT / "rows.json").write_text(json.dumps(
        {f"{k[0]}_vs_{k[1]}{'_swap' if k[2] else ''}": v
         for k, v in results.items()}))

    print("\nType A's true win share across both orientations "
          "(--swap-sides swaps the compositions: type A is red "
          "unswapped, blue swapped):", flush=True)
    for attacker, defender in MATCHUPS:
        normal = results[(attacker, defender, False)]
        swapped = results[(attacker, defender, True)]
        a_wins = (sum(1 for r in normal if r["win"] == "Red") +
                  sum(1 for r in swapped if r["win"] == "Blue"))
        n = len(normal) + len(swapped)
        p = a_wins / n if n else 0.0
        lo, hi = wilson(p, n)
        print(f"  {attacker:9s} vs {defender:9s}: {p:.3f} "
              f"[{lo:.3f}, {hi:.3f}]  ({a_wins}/{n})", flush=True)
    print("DONE", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
