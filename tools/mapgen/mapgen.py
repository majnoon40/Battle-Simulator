#!/usr/bin/env python3
"""M9 mapgen: real-Earth terrain maps for the battle sim.

Turns NOAA ETOPO 2022 elevation (15 arc-second GeoTIFF tiles) and
Natural Earth 10m rivers and lakes into an M8 terrain map
(data/maps/<region>.png, exact palette from data/terrain.json), a metadata
sidecar (data/maps/<region>.meta.json) and a battle scenario
(data/scenarios/earth-<region>.json). The simulation itself is unchanged.

    python tools/mapgen/mapgen.py --region lebanon
    python tools/mapgen/mapgen.py --bbox 10.6,46.5,12.6,47.4 --name alps \
        --preview

Classification rules (tools/mapgen/classify.json):
  elevation <= sea_level_m                    -> water (sea and lakes)
  elevation >= high_elev_m OR slope >= steep_slope_deg -> mountain
  elevation >= hills_elev_m                   -> hills
  otherwise                                   -> grassland
Natural Earth rivers rasterize to water at least one cell wide (dilated so
they can never leak), with fords stamped deterministically every
ford_spacing_cells river cells; a connectivity check then adds fords
(halving the spacing) until both spawn bands are connected. Natural Earth
lakes rasterize as filled water polygons. A synthetic forest mask
(grassland below forest_max_elev_m and forest_max_slope_deg) is available
but OFF by default and labeled "synthetic" in the metadata — these
datasets have no real forest or road data, and none is faked otherwise.

Scale: the bbox maps linearly onto the 400x400 grid and then onto the
1000x1000 abstract world (cell_size 2.5). The projection is plain
equirectangular in degrees — longitude is NOT shrunk by latitude — so a
cell's real size varies with cos(latitude); see the "scale" block in the
metadata. Unit speeds and sight radii stay abstract game units.

Deterministic: same inputs -> byte-identical PNG and JSON (no timestamps,
fixed iteration order, fixed numpy operations, sort_keys JSON).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
import zipfile
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).parent))
from shapefile import read_shapefile  # noqa: E402

TOOL_VERSION = "1.0.0"
REPO = Path(__file__).resolve().parents[2]
CACHE = Path(__file__).resolve().parent / "cache"
DEFAULT_GRID = 400
CELL_SIZE = 2.5          # world units per cell (400 cells -> 1000x1000)
SPAWN_BAND_FRACTION = 0.15
# ETOPO 2022 15 arc-second tiles: 1/240 degree per pixel, 3600x3600 per
# 15x15 degree tile. A tile named N{a}E{b} covers lat [a-15, a), lon
# [b, b+15), stored north-up (row 0 = latitude a).
ETOPO_PIXELS_PER_DEGREE = 240.0
ETOPO_TILE_PX = 3600
ETOPO_URL_DIR = ("https://www.ngdc.noaa.gov/mgg/global/relief/ETOPO2022/"
                 "data/15s/15s_surface_elev_gtif")
NE_URL_BASE = "https://naciscdn.org/naturalearth/10m/physical"
NE_FILES = {
    "rivers": ("ne_10m_rivers_lake_centerlines.zip",
               f"{NE_URL_BASE}/ne_10m_rivers_lake_centerlines.zip"),
    "lakes": ("ne_10m_lakes.zip", f"{NE_URL_BASE}/ne_10m_lakes.zip"),
}

REGIONS = {
    "lebanon": {
        "description": "Lebanon coast and Bekaa",
        "bbox": (35.0, 33.0, 36.7, 34.7),
    },
    "alps-innsbruck": {
        "description": "Alps around Innsbruck (Inn valley, Karwendel, "
                       "Tux Alps)",
        "bbox": (10.6, 46.5, 12.6, 47.4),
    },
    "nile-delta": {
        "description": "Nile delta, Alexandria to Damietta and Cairo",
        "bbox": (29.5, 29.9, 32.5, 31.7),
    },
}


# ---------------------------------------------------------------------------
# Inputs
# ---------------------------------------------------------------------------

def validate_bbox(bbox):
    lon_min, lat_min, lon_max, lat_max = (float(v) for v in bbox)
    if not (-180 <= lon_min < lon_max <= 180 and -90 <= lat_min < lat_max <= 90):
        raise ValueError(
            f"invalid bbox {bbox}: need -180 <= lon_min < lon_max <= 180 "
            "and -90 <= lat_min < lat_max <= 90")
    if lon_max - lon_min > 30 or lat_max - lat_min > 30:
        raise ValueError("bbox spans more than 30 degrees; keep regions small")
    return lon_min, lat_min, lon_max, lat_max


class EtopoMosaic:
    """Bilinear sampler over the cached ETOPO 2022 15s surface tiles."""

    def __init__(self, cache_dir=CACHE):
        self.cache = Path(cache_dir)
        self.tiles = {}

    def _tile_name(self, north, east):
        # NOAA names tiles with 2-digit latitudes (N00, N45, N90) and
        # 3-digit longitudes (E000, E030, W180).
        ns = f"N{abs(north):02d}" if north >= 0 else f"S{abs(north):02d}"
        ew = f"E{abs(east):03d}" if east >= 0 else f"W{abs(east):03d}"
        return f"ETOPO_2022_v1_15s_{ns}{ew}_surface.tif"

    def _load(self, north, east):
        key = (north, east)
        if key not in self.tiles:
            path = self.cache / self._tile_name(north, east)
            if not path.exists():
                raise FileNotFoundError(
                    f"missing ETOPO tile {path.name}; download it from "
                    f"{ETOPO_URL_DIR}/{path.name} into tools/mapgen/cache/")
            import tifffile
            self.tiles[key] = tifffile.imread(path).astype(np.float64)
        return self.tiles[key]

    def sample_grid(self, lons, lats):
        """Bilinear elevation at each (lon, lat); points on a tile seam
        always sample the eastern/southern tile, deterministically."""
        out = np.full(lons.shape, np.nan, dtype=np.float64)
        north = (np.floor(lats / 15.0) + 1.0) * 15.0
        east = np.floor(lons / 15.0) * 15.0
        for n in np.unique(north):
            for e in np.unique(east):
                mask = (north == n) & (east == e)
                if not mask.any():
                    continue
                data = self._load(int(n), int(e))
                fi = (n - lats[mask]) * ETOPO_PIXELS_PER_DEGREE - 0.5
                fj = (lons[mask] - e) * ETOPO_PIXELS_PER_DEGREE - 0.5
                i0 = np.clip(np.floor(fi).astype(np.int64), 0,
                             ETOPO_TILE_PX - 2)
                j0 = np.clip(np.floor(fj).astype(np.int64), 0,
                             ETOPO_TILE_PX - 2)
                ti = fi - np.floor(fi)
                tj = fj - np.floor(fj)
                tl = data[i0, j0]
                tr = data[i0, j0 + 1]
                bl = data[i0 + 1, j0]
                br = data[i0 + 1, j0 + 1]
                out[mask] = ((tl * (1 - tj) + tr * tj) * (1 - ti) +
                             (bl * (1 - tj) + br * tj) * ti)
        return out

    def used_tiles(self):
        return sorted(self.tiles)


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def extract_zip_once(zip_path, dest_dir):
    dest_dir.mkdir(parents=True, exist_ok=True)
    marker = dest_dir / ".extracted"
    if not marker.exists():
        with zipfile.ZipFile(zip_path) as zf:
            zf.extractall(dest_dir)
        marker.write_text("ok\n")
    return dest_dir


def load_natural_earth(cache_dir=CACHE):
    """Returns (river_polylines, lake_polygons): lists of parts, each part
    a list of (lon, lat). River/lake centerlines from
    ne_10m_rivers_lake_centerlines, lake polygons from ne_10m_lakes."""
    rivers, lakes = [], []
    for kind, (zip_name, _url) in NE_FILES.items():
        zip_path = Path(cache_dir) / zip_name
        if not zip_path.exists():
            raise FileNotFoundError(
                f"missing Natural Earth archive {zip_path}; download it "
                f"from {_url} into tools/mapgen/cache/")
        base = extract_zip_once(zip_path, Path(cache_dir) / zip_name[:-4])
        stem = base / zip_name[:-4]
        for shape, _attrs in read_shapefile(str(stem)):
            if kind == "rivers" and shape["type"] not in ("polyline",):
                continue
            if kind == "lakes" and shape["type"] not in ("polygon",):
                continue
            for part in shape["parts"]:
                if len(part) >= 2:
                    (rivers if kind == "rivers" else lakes).append(part)
    return rivers, lakes


# ---------------------------------------------------------------------------
# Grid, classification
# ---------------------------------------------------------------------------

def grid_axes(bbox, width, height):
    lon_min, lat_min, lon_max, lat_max = bbox
    dlon = (lon_max - lon_min) / width
    dlat = (lat_max - lat_min) / height
    lons = lon_min + (np.arange(width) + 0.5) * dlon
    lats = lat_max - (np.arange(height) + 0.5) * dlat  # row 0 = north
    return lons, lats


def slope_degrees(elev, bbox, width, height):
    """Central-difference slope in degrees, with the longitude spacing
    shrunk by cos(mid-latitude) to approximate true meters."""
    _, lat_min, _, lat_max = bbox
    dlon = (bbox[2] - bbox[0]) / width
    dlat = (lat_max - lat_min) / height
    mid_lat = math.radians((lat_min + lat_max) / 2.0)
    dx_m = 111320.0 * math.cos(mid_lat) * dlon
    dy_m = 110540.0 * dlat
    dzdx = np.gradient(elev, axis=1) / dx_m
    dzdy = np.gradient(elev, axis=0) / dy_m
    return np.degrees(np.arctan(np.hypot(dzdx, dzdy)))


def load_palette(terrain_json_path):
    """{terrain id: index} and {index: (r, g, b)} straight from the
    battle sim's terrain table, so the PNG always matches exactly."""
    table = json.loads(Path(terrain_json_path).read_text())
    index_of, colors = {}, {}
    for i, entry in enumerate(table["types"]):
        color = entry["color"].lstrip("#")
        index_of[entry["id"]] = i
        colors[i] = tuple(int(color[k:k + 2], 16) for k in (0, 2, 4))
    return table, index_of, colors


def classify(elev, slope, cfg, index_of, synthetic_forest=False):
    """Elevation/slope -> terrain index grid (see the module docstring)."""
    types = np.full(elev.shape, index_of["grassland"], dtype=np.uint8)
    land = elev > cfg["sea_level_m"]
    hills = land & (elev >= cfg["hills_elev_m"])
    mountain = land & ((elev >= cfg["high_elev_m"]) |
                       (slope >= cfg["steep_slope_deg"]))
    if synthetic_forest:
        forest = land & ~mountain & (elev <= cfg["forest_max_elev_m"]) & \
                 (slope <= cfg["forest_max_slope_deg"])
        types[forest] = index_of["forest"]
    types[hills & ~mountain] = index_of["hills"]
    types[mountain] = index_of["mountain"]
    types[~land] = index_of["water"]
    return types


# ---------------------------------------------------------------------------
# Rivers, lakes, fords, connectivity
# ---------------------------------------------------------------------------

def cell_of(lon, lat, bbox, width, height):
    lon_min, lat_min, lon_max, lat_max = bbox
    cx = int((lon - lon_min) / (lon_max - lon_min) * width)
    cy = int((lat_max - lat) / (lat_max - lat_min) * height)
    return cx, cy


def stamp_rivers(river_polylines, bbox, width, height, ford_spacing):
    """Rasterizes river polylines: returns (river_cells, ford_cells).
    Rivers are walked at half-cell steps so no cell is skipped, and are
    recorded in deterministic file order. Every ford_spacing-th stamped
    cell of each part becomes a ford."""
    river, fords = set(), set()
    if ford_spacing <= 0:
        ford_spacing = 10 ** 9  # fords disabled
    for part in river_polylines:
        stamped = 0
        for (x1, y1), (x2, y2) in zip(part, part[1:]):
            # Keep segments whose extent overlaps the bbox; a river
            # entering the region from outside must still be stamped
            # through its in-bounds part (in-bounds cells only).
            if (max(x1, x2) < bbox[0] or min(x1, x2) > bbox[2] or
                    max(y1, y2) < bbox[1] or min(y1, y2) > bbox[3]):
                continue
            steps = max(abs(x2 - x1) / (bbox[2] - bbox[0]) * width,
                        abs(y2 - y1) / (bbox[3] - bbox[1]) * height) * 2.0
            steps = max(1, int(math.ceil(steps)))
            for s in range(steps + 1):
                t = s / steps
                cx, cy = cell_of(x1 + (x2 - x1) * t, y1 + (y2 - y1) * t,
                                 bbox, width, height)
                if not (0 <= cx < width and 0 <= cy < height):
                    continue
                cell = (cy, cx)
                if cell not in river:
                    river.add(cell)
                if stamped > 0 and stamped % ford_spacing == 0:
                    fords.add(cell)
                stamped += 1
    return river, fords


def dilate(mask):
    """4-neighbor dilation of a boolean (H, W) array."""
    out = mask.copy()
    out[1:, :] |= mask[:-1, :]
    out[:-1, :] |= mask[1:, :]
    out[:, 1:] |= mask[:, :-1]
    out[:, :-1] |= mask[:, 1:]
    return out


def rasterize_lakes(lake_polygons, bbox, width, height):
    """Even-odd fill of lake polygons at cell centers (vectorized)."""
    lons, lats = grid_axes(bbox, width, height)
    water = np.zeros((height, width), dtype=bool)
    for poly in lake_polygons:
        minx = min(p[0] for p in poly)
        maxx = max(p[0] for p in poly)
        miny = min(p[1] for p in poly)
        maxy = max(p[1] for p in poly)
        if maxx < bbox[0] or minx > bbox[2] or maxy < bbox[1] or miny > bbox[3]:
            continue
        inside = np.zeros((height, width), dtype=bool)
        px, py = np.meshgrid(lons, lats)
        for (x1, y1), (x2, y2) in zip(poly, poly[1:] + [poly[0]]):
            if y1 == y2:
                continue
            straddle = (y1 > py) != (y2 > py)
            x_at = x1 + (py - y1) * (x2 - x1) / (y2 - y1)
            inside ^= straddle & (px < x_at)
        water |= inside
    return water


def apply_water_and_fords(types, river_cells, ford_cells, lake_water,
                          index_of):
    """Water layout: sea (already classified) | lakes | rivers, then ford
    patches (a ford cell plus its 4 neighbors where water) so every ford
    is a passable gap at least 3 cells wide across the river."""
    height, width = types.shape
    river = np.zeros((height, width), dtype=bool)
    for cy, cx in river_cells:
        river[cy, cx] = True
    river_dilated = dilate(river)
    water = (types == index_of["water"]) | lake_water | river | river_dilated
    types[water] = index_of["water"]
    for cy, cx in sorted(ford_cells):
        if not water[cy, cx]:
            continue  # a ford stamp on land (clipped river) stays land
        for ny, nx in ((cy, cx), (cy - 1, cx), (cy + 1, cx),
                       (cy, cx - 1), (cy, cx + 1)):
            if 0 <= ny < height and 0 <= nx < width and water[ny, nx]:
                types[ny, nx] = index_of["ford"]
    return types


def connected_components(passable):
    """Connectivity labeling with the SAME semantics as the battle sim's
    flow field: orthogonal steps always, diagonals only when at least one
    of the two shared orthogonal neighbors is also passable (no corner
    cutting through water). The generator's connectivity guarantee must
    match sim validation exactly or generated maps get rejected."""
    height, width = passable.shape
    labels = np.full((height, width), 0, dtype=np.int32)
    current = 0
    for sy in range(height):
        for sx in range(width):
            if not passable[sy, sx] or labels[sy, sx] != 0:
                continue
            current += 1
            stack = [(sy, sx)]
            labels[sy, sx] = current
            while stack:
                y, x = stack.pop()
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        if dy == 0 and dx == 0:
                            continue
                        ny, nx = y + dy, x + dx
                        if not (0 <= ny < height and 0 <= nx < width):
                            continue
                        if not passable[ny, nx] or labels[ny, nx] != 0:
                            continue
                        if dy != 0 and dx != 0:
                            # diagonal: no corner cutting, like the sim
                            if (not passable[y + dy, x] and
                                    not passable[y, x + dx]):
                                continue
                        labels[ny, nx] = current
                        stack.append((ny, nx))
    return labels, current


def band_passable_share(types, index_of, width, height, fraction):
    """(west, east) share of passable cells inside each spawn band."""
    band = max(1, int(math.ceil(fraction * width)))
    land = (types != index_of["water"]) & (types != index_of["obstacle"])
    west = land[:, :band]
    east = land[:, width - band:]
    return float(west.mean()), float(east.mean())


# Candidate spawn band fractions, tried in order: the generator widens the
# bands until both sit on enough passable land to deploy an army (the
# battle sim draws 64 positions per unit and then snaps to the nearest
# passable cell within 16 cells, so very sparse bands fail to spawn).
BAND_FRACTIONS = (0.15, 0.20, 0.25, 0.30, 0.35, 0.40)
MIN_BAND_PASSABLE_SHARE = 0.25


def bands_connectable(passable, width, height,
                      fraction=SPAWN_BAND_FRACTION):
    """True when both spawn bands touch the largest 8-connected land
    component. Isolated pockets (islets) do not count — the generator
    floods them to water right after, so the battle sim sees one clean
    landmass containing every spawn cell."""
    band = max(1, int(math.ceil(fraction * width)))
    labels, count = connected_components(passable)
    if count == 0:
        return False
    sizes = np.bincount(labels.ravel(), minlength=count + 1)
    largest = int(np.argmax(sizes[1:])) + 1
    west = labels[:, :band] == largest
    east = labels[:, width - band:] == largest
    return bool(west.any()) and bool(east.any())


def snap_to_passable(cx, cy, allowed, max_radius=None):
    """Nearest allowed cell center by a deterministic expanding-ring scan
    (row-major within each ring, matching the sim's snap). Returns
    (cx, cy) or None."""
    height, width = allowed.shape
    if max_radius is None:
        max_radius = max(height, width)
    if 0 <= cy < height and 0 <= cx < width and allowed[cy, cx]:
        return cx, cy
    for ring in range(1, max_radius + 1):
        for oy in range(-ring, ring + 1):
            for ox in range(-ring, ring + 1):
                if max(abs(ox), abs(oy)) != ring:
                    continue
                nx, ny = cx + ox, cy + oy
                if 0 <= ny < height and 0 <= nx < width and allowed[ny, nx]:
                    return nx, ny
    return None


# ---------------------------------------------------------------------------
# Generation pipeline
# ---------------------------------------------------------------------------

def generate_map(region, bbox, width, height, sample_elev, river_polylines,
                 lake_polygons, cfg, palette, synthetic_forest=False):
    """Pure generation: everything except file writing, so tests can call
    it with synthetic fixtures. Returns (types, meta) where meta carries
    every statistic the writers need."""
    table, index_of, colors = palette
    lon_min, lat_min, lon_max, lat_max = bbox
    lons, lats = grid_axes(bbox, width, height)
    lon_grid, lat_grid = np.meshgrid(lons, lats)
    elev = sample_elev(lon_grid, lat_grid)
    slope = slope_degrees(elev, bbox, width, height)
    types = classify(elev, slope, cfg, index_of, synthetic_forest)
    lake_water = rasterize_lakes(lake_polygons, bbox, width, height)

    # Choose the spawn band fraction first: the smallest fraction whose
    # bands are both at least MIN_BAND_PASSABLE_SHARE passable (the
    # default 0.15 stays when the terrain allows it).
    chosen_fraction = None
    for fraction in BAND_FRACTIONS:
        west, east = band_passable_share(types, index_of, width, height,
                                         fraction)
        if west >= MIN_BAND_PASSABLE_SHARE and east >= MIN_BAND_PASSABLE_SHARE:
            chosen_fraction = fraction
            break
    if chosen_fraction is None:
        raise RuntimeError(
            f"region '{region}': no spawn band fraction up to "
            f"{BAND_FRACTIONS[-1]} puts both bands on enough passable land "
            f"({MIN_BAND_PASSABLE_SHARE:.0%}); adjust the bbox")

    # Rivers + fords; halve the spacing until both bands connect.
    spacing = int(cfg["ford_spacing_cells"])
    rounds = []
    while True:
        river_cells, ford_cells = stamp_rivers(river_polylines, bbox, width,
                                               height, spacing)
        types = classify(elev, slope, cfg, index_of, synthetic_forest)
        types = apply_water_and_fords(types, river_cells, ford_cells,
                                      lake_water, index_of)
        passable = (types != index_of["water"]) & \
                   (types != index_of["obstacle"])
        ok = bands_connectable(passable, width, height, chosen_fraction)
        rounds.append({"ford_spacing_cells": spacing,
                       "ford_cells": len(ford_cells),
                       "connected": ok})
        if ok or spacing <= 1:
            break
        spacing = spacing // 2
    if not rounds[-1]["connected"]:
        raise RuntimeError(
            f"region '{region}': the two spawn bands cannot be connected "
            "even with fords every river cell — water (a lake or the sea) "
            "splits the map; adjust the bbox")

    # Isolated passable pockets (offshore islets, oxbows behind rivers)
    # would make the battle sim reject the map — every spawn-band cell
    # must be reachable from the objective. Flood everything outside the
    # main component to water and record how many cells that cost.
    labels, _ = connected_components(passable)
    band = max(1, int(math.ceil(chosen_fraction * width)))
    west = [(y, x) for y in range(height) for x in range(band)
            if passable[y, x]]
    main = labels[west[0][0], west[0][1]]
    isolated = passable & (labels != main)
    if isolated.any():
        types[isolated] = index_of["water"]
        passable = (types != index_of["water"]) & \
                   (types != index_of["obstacle"])
    flooded = int(isolated.sum())

    # Objectives: each band's center snapped to the main component.
    west = [(y, x) for y in range(height) for x in range(band)
            if passable[y, x]]
    main = labels[west[0][0], west[0][1]]
    in_main = passable & (labels == main)
    def snap_world(cx, cy):
        found = snap_to_passable(cx, cy, in_main)
        if found is None:
            raise RuntimeError(f"region '{region}': no passable cell for a "
                               "spawn-band objective")
        fx, fy = found
        return ((fx + 0.5) * CELL_SIZE, (fy + 0.5) * CELL_SIZE)
    red_obj = snap_world(width - band // 2 - 1, height // 2)   # east band
    blue_obj = snap_world(band // 2, height // 2)              # west band

    counts = {}
    for i, entry in enumerate(table["types"]):
        n = int(np.sum(types == i))
        if n:
            counts[entry["id"]] = n
    west_share, east_share = band_passable_share(types, index_of, width,
                                                 height, chosen_fraction)
    meta = {
        "region": region,
        "bbox": {"lon_min": lon_min, "lat_min": lat_min,
                 "lon_max": lon_max, "lat_max": lat_max},
        "grid": {"width": width, "height": height},
        "cell_size": CELL_SIZE,
        "spawn_band_fraction": chosen_fraction,
        "spawn_band_passable_share": {"west": round(west_share, 3),
                                      "east": round(east_share, 3)},
        "terrain_counts": counts,
        "objectives": {"red": red_obj, "blue": blue_obj},
        "fords": rounds[-1]["ford_cells"],
        "ford_rounds": rounds,
        "isolated_land_cells_flooded": flooded,
        "synthetic_forest": bool(synthetic_forest),
    }
    return types, meta


def hillshade(elev, bbox, width, height, path):
    """Preview image (azimuth 315, altitude 45); not used by the sim."""
    _, lat_min, _, lat_max = bbox
    dlon = (bbox[2] - bbox[0]) / width
    dlat = (lat_max - lat_min) / height
    mid_lat = math.radians((lat_min + lat_max) / 2.0)
    dzdx = np.gradient(elev, axis=1) / (111320.0 * math.cos(mid_lat) * dlon)
    dzdy = np.gradient(elev, axis=0) / (110540.0 * dlat)
    slope = np.arctan(np.hypot(dzdx, dzdy))
    aspect = np.arctan2(-dzdy, dzdx)
    az, alt = math.radians(315.0), math.radians(45.0)
    shade = (math.sin(alt) * np.sin(slope) +
             math.cos(alt) * np.cos(slope) * np.cos(az - aspect))
    image = np.clip(shade * 255.0, 0, 255).astype(np.uint8)
    Image.fromarray(image).save(path, optimize=False)


def write_outputs(types, meta, region, elev, bbox, width, height, palette,
                  out_root=REPO, preview=None, sources=None):
    table, index_of, colors = palette
    maps_dir = Path(out_root) / "data" / "maps"
    scen_dir = Path(out_root) / "data" / "scenarios"
    maps_dir.mkdir(parents=True, exist_ok=True)
    scen_dir.mkdir(parents=True, exist_ok=True)

    rgb = np.zeros((height, width, 3), dtype=np.uint8)
    for i, color in colors.items():
        rgb[types == i] = color
    png_path = maps_dir / f"{region}.png"
    Image.fromarray(rgb).save(png_path, optimize=False)

    meta_full = dict(meta)
    meta_full["map"] = png_path.name
    meta_full["scenario"] = f"earth-{region}.json"
    meta_full["world"] = {"width": width * CELL_SIZE,
                          "height": height * CELL_SIZE}
    meta_full["scale"] = {
        "projection": "equirectangular (degrees, longitude NOT shrunk)",
        "cell_meters_east_west": round(
            111320.0 * math.cos(math.radians((meta["bbox"]["lat_min"] +
                                              meta["bbox"]["lat_max"]) / 2)) *
            (meta["bbox"]["lon_max"] - meta["bbox"]["lon_min"]) / width, 1),
        "cell_meters_north_south": round(
            110540.0 * (meta["bbox"]["lat_max"] - meta["bbox"]["lat_min"]) /
            height, 1),
        "note": "world units are abstract game units, not meters",
    }
    meta_full["thresholds"] = json.loads(
        (Path(__file__).parent / "classify.json").read_text())
    meta_full["tool_version"] = TOOL_VERSION
    meta_full["sources"] = sources or {}
    meta_full["synthetic_forest"] = meta_full.get("synthetic_forest", False)
    if meta_full["synthetic_forest"]:
        meta_full["synthetic_forest_note"] = (
            "forest cells are a SYNTHETIC mask (low elevation, low slope), "
            "not real land-cover data")
    meta_path = maps_dir / f"{region}.meta.json"
    meta_path.write_text(json.dumps(meta_full, indent=2, sort_keys=True) +
                         "\n")

    scenario = {
        "battlefield": {"width": width * CELL_SIZE,
                        "height": height * CELL_SIZE},
        "spawn_band_fraction": meta["spawn_band_fraction"],
        "map": {"file": png_path.name, "cell_size": CELL_SIZE},
        "objectives": {
            "red": {"x": meta["objectives"]["red"][0],
                    "y": meta["objectives"]["red"][1]},
            "blue": {"x": meta["objectives"]["blue"][0],
                     "y": meta["objectives"]["blue"][1]},
        },
        "armies": {
            "red": {"commander": 40, "infantry": 550, "archer": 250,
                    "cavalry": 200},
            "blue": {"commander": 40, "infantry": 550, "archer": 250,
                     "cavalry": 200},
        },
        "squads": {"red": {"count": 40, "size": 25},
                   "blue": {"count": 40, "size": 25}},
    }
    scen_path = scen_dir / f"earth-{region}.json"
    scen_path.write_text(json.dumps(scenario, indent=2, sort_keys=True) + "\n")
    if preview:
        hillshade(elev, bbox, width, height, preview)
    return png_path, meta_path, scen_path


def build_sources(mosaic):
    sources = {
        "etopo": {
            "product": "NOAA NCEI ETOPO 2022, 15 arc-second surface "
                       "elevation (GeoTIFF tiles)",
            "version": "v1 (2022)",
            "url_dir": ETOPO_URL_DIR,
            "tiles": {},
        },
        "natural_earth": {"product": "Natural Earth 10m physical",
                          "license": "public domain",
                          "files": {}},
    }
    for north, east in mosaic.used_tiles():
        name = mosaic._tile_name(north, east)
        path = Path(CACHE) / name
        sources["etopo"]["tiles"][name] = {
            "url": f"{ETOPO_URL_DIR}/{name}",
            "sha256": sha256_of(path),
        }
    for kind, (zip_name, url) in NE_FILES.items():
        path = Path(CACHE) / zip_name
        sources["natural_earth"]["files"][zip_name] = {
            "url": url, "sha256": sha256_of(path),
        }
    return sources


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Generate battle terrain maps from real Earth data "
                    "(see docs/mapgen.md)")
    parser.add_argument("--region", choices=sorted(REGIONS),
                        help="a named region from REGIONS")
    parser.add_argument("--bbox",
                        help="lon_min,lat_min,lon_max,lat_max (degrees)")
    parser.add_argument("--name", help="region name (required with --bbox)")
    parser.add_argument("--grid", type=int, default=DEFAULT_GRID,
                        help="grid size in cells (default 400x400)")
    parser.add_argument("--preview", action="store_true",
                        help="also write a hillshaded PNG (sanity check)")
    parser.add_argument("--synthetic-forest", action="store_true",
                        help="deterministic synthetic forest on low "
                             "grassland, labeled 'synthetic' in the meta")
    args = parser.parse_args(argv)

    if args.region:
        region = args.region
        bbox = validate_bbox(REGIONS[region]["bbox"])
    elif args.bbox and args.name:
        region = args.name
        bbox = validate_bbox(tuple(float(v) for v in
                                   args.bbox.split(",")))
    else:
        parser.error("need --region, or --bbox and --name")

    cfg = json.loads(
        (Path(__file__).parent / "classify.json").read_text())
    palette = load_palette(REPO / "data" / "terrain.json")

    mosaic = EtopoMosaic()
    rivers, lakes = load_natural_earth()
    sample = mosaic.sample_grid
    types, meta = generate_map(region, bbox, args.grid, args.grid, sample,
                               rivers, lakes, cfg, palette,
                               synthetic_forest=args.synthetic_forest)
    sources = build_sources(mosaic)

    lons, lats = grid_axes(bbox, args.grid, args.grid)
    lon_grid, lat_grid = np.meshgrid(lons, lats)
    elev = sample(lon_grid, lat_grid)
    preview = (CACHE / f"preview-{region}.png") if args.preview else None
    png, meta_path, scen = write_outputs(
        types, meta, region, elev, bbox, args.grid, args.grid, palette,
        preview=preview, sources=sources)
    total = args.grid * args.grid
    share = {k: f"{100.0 * v / total:.1f}%" for k, v in
             meta["terrain_counts"].items()}
    print(f"{region}: {png} ({png.stat().st_size} bytes)")
    print(f"  terrain share: {share}")
    print(f"  fords: {meta['fords']} (rounds: "
          f"{[(r['ford_spacing_cells'], r['connected']) for r in meta['ford_rounds']]})")
    print(f"  isolated land flooded: {meta['isolated_land_cells_flooded']} cells")
    print(f"  spawn band fraction: {meta['spawn_band_fraction']} "
          f"(passable west {meta['spawn_band_passable_share']['west']:.0%} / "
          f"east {meta['spawn_band_passable_share']['east']:.0%})")
    print(f"  objectives: {meta['objectives']}")
    print(f"  meta: {meta_path}")
    print(f"  scenario: {scen}")
    if preview:
        print(f"  preview: {preview}")


if __name__ == "__main__":
    main()
