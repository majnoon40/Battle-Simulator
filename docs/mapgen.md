# M9 mapgen — real-Earth battle maps (docs/mapgen.md)

`tools/mapgen/mapgen.py` turns real elevation (NOAA ETOPO 2022) and
rivers/lakes (Natural Earth) into M8 terrain maps, metadata and battle
scenarios. The simulation is unchanged — the tool only writes
`data/maps/<region>.png`, `data/maps/<region>.meta.json` and
`data/scenarios/earth-<region>.json`. Data sources, versions, licenses,
citations and pinned SHA-256 checksums: `docs/DATA_SOURCES.md`.

## Setup

Python 3 with numpy, Pillow, tifffile and imagecodecs (no GIS stack).
Put the pinned raw files in `tools/mapgen/cache/` (gitignored) —
see `docs/DATA_SOURCES.md` for exact URLs and checksums, or:

```sh
cd tools/mapgen/cache
BASE=https://www.ngdc.noaa.gov/mgg/global/relief/ETOPO2022/data/15s/15s_surface_elev_gtif
for t in N30E015 N30E030 N45E015 N45E030 N60E000; do
  curl -O "$BASE/ETOPO_2022_v1_15s_${t}_surface.tif"
done
curl -O https://naciscdn.org/naturalearth/10m/physical/ne_10m_rivers_lake_centerlines.zip
curl -O https://naciscdn.org/naturalearth/10m/physical/ne_10m_lakes.zip
```

## Commands

```sh
# a named region (see REGIONS in mapgen.py)
python3 tools/mapgen/mapgen.py --region lebanon --preview

# an ad-hoc region
python3 tools/mapgen/mapgen.py --bbox 10.6,46.5,12.6,47.4 --name my-alps

# options
#   --grid 400           grid size in cells (default 400x400)
#   --preview            also write a hillshaded PNG to cache/preview-<region>.png
#   --synthetic-forest   deterministic forest mask on low grassland
#                        (OFF by default; labeled "synthetic" in the meta)

# run a battle on it
./build/battle_cli --scenario earth-lebanon --seed 42

# python tests (synthetic fixture, no downloads needed)
python3 tools/mapgen/test_mapgen.py
```

## How it works

1. **Elevation**: the bbox is resampled onto the 400x400 grid with
   bilinear interpolation from the ETOPO 15 arc-second tiles (tile
   `N{a}E{b}` covers lat [a−15, a], lon [b, b+15], north-up, 1/240° per
   pixel).
2. **Classification** (`tools/mapgen/classify.json`): elevation ≤
   `sea_level_m` (0) → water; elevation ≥ `high_elev_m` (1200) **or**
   slope ≥ `steep_slope_deg` (14°) → mountain; elevation ≥
   `hills_elev_m` (400) → hills; otherwise grassland. Slope is the
   central-difference gradient in meters (the longitude spacing shrunk
   by cos of the bbox's mid-latitude).
3. **Lakes**: Natural Earth polygons filled at cell centers
   (even-odd rule) → water.
4. **Rivers**: Natural Earth polylines walked at half-cell steps (never
   skipping a cell), stamped as water and then 4-neighbor dilated so the
   channel is at least one cell wide everywhere and can never leak at a
   corner. Every `ford_spacing_cells`-th stamped cell (default 60)
   becomes a ford, widened to a plus-shaped passable patch.
5. **Connectivity**: passable cells (everything except water/obstacle)
   must connect both spawn bands with the sim's exact semantics —
   8-connected but **no corner cutting**, like the flow field. If the
   fords do not suffice, the generator halves the spacing (60 → 30 → …
   → 1) and re-stamps, reporting each round. Passable pockets outside
   the main landmass (islets, oxbow islands) would make the sim reject
   the map (unreachable spawn cells), so they are flooded to water and
   the count recorded (`isolated_land_cells_flooded`).
6. **Spawn bands**: the smallest fraction in 15%, 20%, …, 40% whose
   west and east bands are both ≥ 25% passable (the default 15% stays
   when the terrain allows it; the Lebanon coast needs 35%). Objectives
   are the band centers snapped to the nearest passable cell of the
   main component (the sim's exact ring-scan order), then written as
   explicit scenario objectives.
7. **Outputs**: the PNG uses the exact palette from
   `data/terrain.json` (an unknown color would be a load error), the
   meta records bbox/grid/scale/sources/checksums/thresholds/counts, and
   the scenario is 2,080 mixed units in 80 squads per side. Everything
   is deterministic: same inputs → byte-identical PNG and JSON
   (verified by deleting and regenerating).

## Scale

The bbox maps linearly onto the 400x400 grid and onto the 1000x1000
abstract world (`cell_size` 2.5). The projection is plain
equirectangular in degrees — longitude is **not** shrunk by latitude —
so a cell's real size varies with cos(latitude); each meta.json lists
`cell_meters_east_west` and `cell_meters_north_south`. Unit speeds and
sight radii stay abstract game units; nothing claims meters or
kilometers per hour. (For Lebanon at ~34°N: a cell is about 342 m
east-west and 425 m north-south; the battlefield is roughly 137 km
east-west.)

## Adding a region

Add an entry to `REGIONS` in `tools/mapgen/mapgen.py`:

```python
"my-region": {
    "description": "why this region is interesting",
    "bbox": (lon_min, lat_min, lon_max, lat_max),
},
```

then run `python3 tools/mapgen/mapgen.py --region my-region --preview`
and check `tools/mapgen/cache/preview-<region>.png` (hillshade for
sanity). If any ETOPO tile is missing the tool names it; download it
into the cache (see `docs/DATA_SOURCES.md`). Commit only the three
generated files (PNG, meta, scenario) — never the cache.

## Changing the classification thresholds

Edit `tools/mapgen/classify.json` (documented keys: `sea_level_m`,
`hills_elev_m`, `high_elev_m`, `steep_slope_deg`, `ford_spacing_cells`,
`forest_max_elev_m`, `forest_max_slope_deg`) and regenerate; the
thresholds used are copied into each map's meta.json. Stricter
mountains: raise `steep_slope_deg` or lower `high_elev_m`. More/less
crossings: lower/raise `ford_spacing_cells`.

## Generated regions (committed)

| region | bbox | character |
| --- | --- | --- |
| `lebanon` | 35.0, 33.0, 36.7, 34.7 | coastal plain, Mount Lebanon ridge, Bekaa valley, Litani river; the thin coast forces a 35% spawn band |
| `alps-innsbruck` | 10.6, 46.5, 12.6, 47.4 | 91% mountain, the Inn valley as the main corridor, 22 fords |
| `nile-delta` | 29.5, 29.9, 32.5, 31.7 | flat delta grassland cut by the Rosetta and Damietta branches, sea to the north |
