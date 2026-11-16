#!/usr/bin/env python3
"""Static navigation diagnosis for the earth maps (m11a).

Rebuilds, in Python, exactly the flow field the engine computes
(sim/src/flow_field.cpp): a Dijkstra from the faction objective cell
where entering a cell costs 1 / move_cost (times sqrt(2) for
diagonals, no corner cutting), and reports, per map and orientation:

  * band deployability: passable share and mean move_cost of each
    spawn band, and the passable-area cost distribution from the band
    to the objective the faction actually uses in that orientation
    (min / median / p90 / max, in cost units and in cell steps);
  * unreached pockets: passable band cells with no path to the
    objective (islands) -- units spawning there fall back to the
    straight-line Advance and jam against water;
  * chokepoint funnel: for every passable band cell, the descent path
    to the objective is walked and each cell on it counted; the top
    cells by traffic share are the chokepoints (the fraction of band
    cells whose path passes through them bounds the fraction of units
    that can pile up there);
  * march axes: the straight line from each band center to the
    objective, which river cells it crosses, and the nearest ford
    (by cost) to each crossing.

Usage:
  python3 tools/analysis/nav_diagnose.py --repo-root . \
      [--map lebanon|alps-innsbruck|nile-delta] [--json OUT.json]
"""
from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
from pathlib import Path

from PIL import Image

# kFlowNeighbors order must match sim/include/sim/flow_field.hpp:
# ties in the descent break to the lowest index.
NEIGHBORS = [(1, 0), (1, -1), (0, -1), (-1, -1),
             (-1, 0), (-1, 1), (0, 1), (1, 1)]
UNREACHED = 1e30


def load_types(repo: Path) -> dict:
    table = json.loads((repo / "data" / "terrain.json").read_text())["types"]
    by_color = {}
    for i, t in enumerate(table):
        rgb = tuple(int(t["color"][k:k + 2], 16) for k in (1, 3, 5))
        by_color[rgb] = i
    return {
        "by_color": by_color,
        "types": table,
    }


def load_grid(repo: Path, map_name: str):
    """PNG -> (cells[row][col] = type index, width, height)."""
    table = load_types(repo)
    img = Image.open(repo / "data" / "maps" / f"{map_name}.png").convert("RGB")
    width, height = img.size
    cells = []
    unknown = set()
    for y in range(height):
        row = []
        for x in range(width):
            rgb = img.getpixel((x, y))
            if rgb not in table["by_color"]:
                unknown.add(rgb)
                row.append(0)
            else:
                row.append(table["by_color"][rgb])
        cells.append(row)
    if unknown:
        raise SystemExit(f"unknown pixel colors in {map_name}.png: {unknown}")
    return cells, width, height, table


def dijkstra(cells, width, height, table, target_cx, target_cy):
    """Mirror of compute_flow_field: cost from every cell TO the target."""
    types = table["types"]

    def speed(cx, cy):
        t = types[cells[cy][cx]]
        return t["move_cost"] if t["passable"] else 0.0

    cost = [UNREACHED] * (width * height)
    if not (0 <= target_cx < width and 0 <= target_cy < height):
        return cost, False
    if speed(target_cx, target_cy) <= 0.0:
        return cost, False
    import heapq
    start = target_cy * width + target_cx
    cost[start] = 0.0
    open_heap = [(0.0, start)]
    while open_heap:
        c, idx = heapq.heappop(open_heap)
        if c > cost[idx]:
            continue
        cx, cy = idx % width, idx // width
        for n, (dx, dy) in enumerate(NEIGHBORS):
            nx, ny = cx + dx, cy + dy
            if not (0 <= nx < width and 0 <= ny < height):
                continue
            sp = speed(nx, ny)
            if sp <= 0.0:
                continue
            if n % 2 == 1:  # diagonal: no corner cutting
                if speed(nx, cy) <= 0.0 and speed(cx, ny) <= 0.0:
                    continue
            w = (1.0 / sp) * (math.sqrt(2.0) if n % 2 == 1 else 1.0)
            nxt = c + w
            j = ny * width + nx
            if nxt < cost[j]:
                cost[j] = nxt
                heapq.heappush(open_heap, (nxt, j))
    return cost, True


def step_direction(cells, width, height, table, cost, cx, cy):
    """The C++ descent: lowest-cost neighbor, ties to the lowest index."""
    types = table["types"]
    best, best_dir = UNREACHED, -1
    for n, (dx, dy) in enumerate(NEIGHBORS):
        nx, ny = cx + dx, cy + dy
        if not (0 <= nx < width and 0 <= ny < height):
            continue
        if cost[ny * width + nx] >= UNREACHED:
            continue
        t = types[cells[ny][nx]]
        if not t["passable"]:
            continue
        c = cost[ny * width + nx]
        if c < best:
            best, best_dir = c, n
    return best_dir


def walk_path(cells, width, height, table, cost, cx, cy):
    """Descent path from (cx, cy) to the target, as cell indices."""
    path = []
    seen = set()
    while True:
        idx = cy * width + cx
        if idx in seen or len(path) > 4 * width * height:
            break  # defensive: a valid field never cycles
        seen.add(idx)
        d = step_direction(cells, width, height, table, cost, cx, cy)
        if d < 0:
            break
        path.append(idx)
        cx += NEIGHBORS[d][0]
        cy += NEIGHBORS[d][1]
    return path


def next_step_field(cells, width, height, table, cost, target_idx):
    """next_step[idx] = the descent step's target index, or -1.

    One pass over all cells; the result is a forest rooted at the
    target (every reached cell's chosen neighbor has strictly lower
    cost, so no cycles are possible). The target itself has no step,
    exactly like the C++ field (kNoDir on the target cell).
    """
    types = table["types"]
    next_step = [-1] * (width * height)
    for cy in range(height):
        row = cells[cy]
        for cx in range(width):
            idx = cy * width + cx
            if cost[idx] >= UNREACHED or idx == target_idx:
                continue
            best, best_n = UNREACHED, -1
            for n, (dx, dy) in enumerate(NEIGHBORS):
                nx, ny = cx + dx, cy + dy
                if not (0 <= nx < width and 0 <= ny < height):
                    continue
                j = ny * width + nx
                c = cost[j]
                if c >= best:
                    continue
                if c >= UNREACHED or not types[cells[ny][nx]]["passable"]:
                    continue
                best, best_n = c, j
            next_step[idx] = best_n
    return next_step


def funnel_traffic(next_step, cost, width, height, sources):
    """Traffic per cell: how many source paths pass through it.

    Sources carry 1 each; processing ALL reached cells in strictly
    decreasing cost order pushes each cell's accumulated traffic to
    its descent step (a valid topological order -- every step strictly
    decreases cost, so a cell is complete before it is pushed).
    """
    traffic = {}
    for idx in sources:
        traffic[idx] = traffic.get(idx, 0) + 1
    order = sorted((i for i in range(width * height) if cost[i] < UNREACHED),
                   key=lambda i: -cost[i])
    for idx in order:
        t = traffic.get(idx, 0)
        if t <= 0:
            continue
        nxt = next_step[idx]
        if nxt >= 0:
            traffic[nxt] = traffic.get(nxt, 0) + t
    return traffic


def band_cells(width, height, west: bool, fraction: float, world_w: float,
               cell_size: float):
    band_w = world_w * fraction
    x0 = 0.0 if west else world_w - band_w
    x1 = band_w if west else world_w
    c0, c1 = int(x0 / cell_size), int(math.ceil(x1 / cell_size))
    for cx in range(max(0, c0), min(width, c1)):
        for cy in range(height):
            yield cx, cy


def line_cells(x0, y0, x1, y1, cell_size, width, height):
    """Bresenham-ish sample of the segment in cell coordinates."""
    steps = int(max(abs(x1 - x0), abs(y1 - y0)) / cell_size) + 1
    for s in range(steps + 1):
        t = s / steps
        cx = int((x0 + t * (x1 - x0)) / cell_size)
        cy = int((y0 + t * (y1 - y0)) / cell_size)
        if 0 <= cx < width and 0 <= cy < height:
            yield cx, cy


def diagnose_map(repo: Path, map_name: str, swap: bool,
                 cache: dict | None = None) -> dict:
    scenario_name = {
        "lebanon": "earth-lebanon.json",
        "alps-innsbruck": "earth-alps-innsbruck.json",
        "nile-delta": "earth-nile-delta.json",
    }[map_name]
    scenario = json.loads(
        (repo / "data" / "scenarios" / scenario_name).read_text())
    if cache is None:
        cache = {}
    if "grid" not in cache:
        cells, width, height, table = load_grid(repo, map_name)
        cache["grid"] = (cells, width, height, table)
        cache["fords"] = [
            (cx, cy) for cy in range(height) for cx in range(width)
            if table["types"][cells[cy][cx]]["id"] == "ford"]
    cells, width, height, table = cache["grid"]
    types = table["types"]
    fords = cache["fords"]
    cell_size = scenario["map"]["cell_size"]
    world_w = scenario["battlefield"]["width"]
    fraction = scenario["spawn_band_fraction"]

    # Orientation semantics (cli/src/main.cpp --swap-sides): armies and
    # explicit objectives both mirror, so in swapped orientation Red
    # spawns in the EAST band and uses the point listed for blue.
    red_obj = scenario["objectives"]["red" if not swap else "blue"]
    blue_obj = scenario["objectives"]["blue" if not swap else "red"]
    red_west = not swap

    out = {"map": map_name, "swap": swap, "grid": [width, height],
           "factions": {}}
    for name, obj, west in (("red", red_obj, red_west),
                            ("blue", blue_obj, not red_west)):
        objx, objy = obj["x"], obj["y"]
        tcx, tcy = int(objx / cell_size), int(objy / cell_size)
        key = (tcx, tcy)
        if cache is not None and key in cache.get("fields", {}):
            cost, target_ok, next_step = cache["fields"][key]
        else:
            cost, target_ok = dijkstra(cells, width, height, table,
                                       tcx, tcy)
            next_step = next_step_field(
                cells, width, height, table, cost,
                tcy * width + tcx if target_ok else -1)
            cache.setdefault("fields", {})[key] = (cost, target_ok,
                                                   next_step)

        band = list(band_cells(width, height, west, fraction, world_w,
                               cell_size))
        passable = [(cx, cy) for cx, cy in band
                    if types[cells[cy][cx]]["passable"]]
        costs = [cost[cy * width + cx] for cx, cy in passable]
        reached = [c for c in costs if c < UNREACHED]
        unreached = len(costs) - len(reached)
        move_costs = [types[cells[cy][cx]]["move_cost"]
                      for cx, cy in passable]

        # chokepoint funnel + path lengths over reached band cells
        sources = [cy * width + cx for cx, cy in passable
                   if cost[cy * width + cx] < UNREACHED]
        traffic = funnel_traffic(next_step, cost, width, height, sources)
        # path length for every reached cell: 1 + its descent step's
        # length (ascending cost order; the target itself is 0)
        lengths = {}
        for idx in sorted((i for i in range(width * height)
                           if cost[i] < UNREACHED),
                          key=lambda i: cost[i]):
            nxt = next_step[idx]
            lengths[idx] = 0 if nxt < 0 else 1 + lengths[nxt]
        cell_steps = [lengths[i] for i in sources]
        total = max(1, len(sources))
        top = sorted(traffic.items(), key=lambda kv: -kv[1])[:8]
        top_cells = [{
            "cell": [idx % width, idx // width],
            "type": types[cells[idx // width][idx % width]]["id"],
            "share": round(v / total, 3),
        } for idx, v in top]

        # march axis: band center -> objective, river crossings and
        # nearest ford by cell distance
        band_w = world_w * fraction
        bx = (band_w / 2.0) if west else (world_w - band_w / 2.0)
        by = scenario["battlefield"]["height"] / 2.0
        crossings = []
        for cx, cy in line_cells(bx, by, objx, objy, cell_size, width,
                                 height):
            if types[cells[cy][cx]]["id"] in ("water", "river"):
                crossings.append((cx, cy))

        def fmt_cost(c):
            return round(c, 1)

        fac = {
            "band": "west" if west else "east",
            "objective": [objx, objy],
            "objective_cell": [tcx, tcy],
            "objective_type": types[cells[tcy][tcx]]["id"],
            "target_reachable": target_ok,
            "band_passable_share": round(len(passable) / max(1, len(band)),
                                         3),
            "band_passable_cells": len(passable),
            "band_mean_move_cost": round(statistics.fmean(move_costs), 3)
            if move_costs else None,
            "path_cost": {
                "min": fmt_cost(min(reached)) if reached else None,
                "median": fmt_cost(statistics.median(reached))
                if reached else None,
                "p90": fmt_cost(statistics.quantiles(reached, n=10)[8])
                if len(reached) >= 10 else None,
                "max": fmt_cost(max(reached)) if reached else None,
            },
            "path_steps_median": int(statistics.median(cell_steps))
            if cell_steps else None,
            "unreached_band_cells": unreached,
            "unreached_share_of_passable": round(
                unreached / max(1, len(passable)), 4),
            "chokepoints": top_cells,
            "march_axis_river_cells": len(crossings),
            "fords_total": len(fords),
        }
        # nearest ford to each crossing cluster (first/last crossing)
        if crossings and fords:
            def nearest_ford(c):
                return min(math.hypot(f[0] - c[0], f[1] - c[1])
                           for f in fords)
            fac["nearest_ford_cells"] = round(nearest_ford(crossings[0]), 1)
            fac["nearest_ford_cells_last"] = round(
                nearest_ford(crossings[-1]), 1)
        out["factions"][name] = fac
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo-root", default=".")
    ap.add_argument("--map", action="append",
                    choices=["lebanon", "alps-innsbruck", "nile-delta"])
    ap.add_argument("--json")
    args = ap.parse_args()
    repo = Path(args.repo_root)
    maps = args.map or ["lebanon", "alps-innsbruck", "nile-delta"]
    results = []
    for m in maps:
        cache: dict = {}
        for swap in (False, True):
            r = diagnose_map(repo, m, swap, cache)
            results.append(r)
            for name, fac in r["factions"].items():
                print(f"{m:14} {'swapped' if swap else 'normal  ':8} "
                      f"{name:4} band={fac['band']:5} "
                      f"obj_cell={fac['objective_cell']} "
                      f"type={fac['objective_type']:9} "
                      f"reach={fac['target_reachable']} "
                      f"passable={fac['band_passable_share']:.2f} "
                      f"cost med={fac['path_cost']['median']} "
                      f"max={fac['path_cost']['max']} "
                      f"unreached={fac['unreached_band_cells']} "
                      f"({fac['unreached_share_of_passable']:.1%}) "
                      f"axis_river={fac['march_axis_river_cells']} "
                      f"fords={fac['fords_total']}")
                for cp in fac["chokepoints"][:3]:
                    print(f"{'':45} choke {cp['type']:9} "
                          f"cell={cp['cell']} share={cp['share']:.1%}")
    if args.json:
        Path(args.json).write_text(json.dumps(results, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
