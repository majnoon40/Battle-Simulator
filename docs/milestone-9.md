# Milestone 9 — Real-Earth map generator

## Scope

An offline tool (`tools/mapgen/`, Python 3 + numpy + Pillow + tifffile +
imagecodecs, no GIS stack) that turns real elevation and river/lake data
into M8 terrain maps plus scenarios. **The sim is not changed** — no
file under sim/ or render/ was touched; all 267 GoogleTest tests (M1–M8,
including the no-map export pins and the new EarthMaps load test) pass
unchanged.

Data: NOAA ETOPO 2022 15 arc-second surface elevation (GeoTIFF tiles)
and Natural Earth 10m rivers + lakes, pinned with SHA-256 in
`docs/DATA_SOURCES.md` (licenses and citations included; raw downloads
gitignored under `tools/mapgen/cache/`). Roads and forests are not in
these datasets and are not faked; the optional `--synthetic-forest`
mask is OFF by default and labeled "synthetic" in the metadata.

## Implementation

- `tools/mapgen/shapefile.py` — a minimal .shp/.dbf reader (standard
  library only, records in file order).
- `tools/mapgen/mapgen.py` — the generator:
  - **Input**: `--region <name>` (from `REGIONS`) or `--bbox lon_min,
    lat_min, lon_max, lat_max --name ...`; `--grid` (default 400x400),
    `--preview` (hillshade), `--synthetic-forest`.
  - **Elevation**: bilinear resampling from the ETOPO tiles (tile
    `N{a}E{b}` covers lat [a−15, a], lon [b, b+15], north-up, 1/240°/px
    — verified against landmarks on three tiles).
  - **Classification** (`classify.json`): ≤ 0 m water; ≥ 1200 m or
    slope ≥ 14° mountain (slope in meters, longitude shrunk by
    cos(mid-lat)); ≥ 400 m hills; else grassland.
  - **Rivers**: Natural Earth polylines walked at half-cell steps,
    stamped water and 4-neighbor dilated (≥ 1 cell wide, cannot leak at
    corners); fords every `ford_spacing_cells` (60) stamped cells,
    widened to plus-shaped patches; a connectivity check (the sim's
    exact no-corner-cut semantics) halves the spacing until both spawn
    bands connect, reporting every round. **Lakes**: even-odd polygon
    fill. Isolated passable pockets (islets) that would fail the sim's
    spawn validation are flooded to water and counted.
  - **Spawn bands**: the smallest fraction of 15…40% whose west and
    east bands are ≥ 25% passable; objectives are band centers snapped
    to the main component (the sim's ring-scan order) and written as
    explicit scenario objectives. Scenario: 2,080 mixed units, 80
    squads per side.
  - **Outputs**: `data/maps/<region>.png` (exact terrain.json palette,
    all under 1 MB), `<region>.meta.json` (bbox, grid, cell size,
    sources with per-file SHA-256, thresholds, tool version, terrain
    counts, scale), `data/scenarios/earth-<region>.json`.
  - **Deterministic**: same inputs → byte-identical PNG and JSON
    (verified by delete-and-regenerate checksums).
- **Scale**: equirectangular degrees (longitude NOT shrunk), cell size
  in meters varies with cos(lat) and is recorded per map; unit speeds
  and sight stay abstract game units (docs/mapgen.md has the worked
  Lebanon numbers).

## Regions (committed)

| region | bbox | terrain share | notes |
| --- | --- | --- | --- |
| lebanon | 35.0, 33.0, 36.7, 34.7 | hills 38%, water 30.5%, mountain 22.2%, grassland 9.2% | coastal plain, Mount Lebanon ridge, Bekaa; thin coast → 35% spawn band, 5 fords on the Litani |
| alps-innsbruck | 10.6, 46.5, 12.6, 47.4 | mountain 91.1%, hills 7.3%, water 1.3% | Inn valley corridor, 19 fords, 312 islet cells flooded |
| nile-delta | 29.5, 29.9, 32.5, 31.7 | grassland 68.4%, water 30.6%, hills 0.8% | Rosetta + Damietta branches, 50 fords, 2,137 lagoon-islet cells flooded |

## Verification (2026-11-15)

**Python tests: 17/17** (`tools/mapgen/test_mapgen.py`, synthetic 64x64
DEM + hand-made river, no downloads): classification thresholds,
synthetic forest off by default, river rasterizing and width, ford
spacing and disabling, river sealing + repair connects the bands,
corner-cutting is not connectivity (the sim's rule), snap-to-passable,
lake polygon fill, byte-identical determinism of the full pipeline,
palette matches terrain.json exactly, meta completeness, scenario shape
(2,080 units, squads, explicit objectives), bbox validation, the
shapefile reader (against a hand-packed fixture), ETOPO tile naming.

**C++ tests: 267/267** — the M1–M8 suite plus `EarthMaps` (each
generated map + scenario loads in battle_cli and runs a tick, exporting
format 6). No sim/render/CLI code changed for M9.

## Acceptance results

20 seeds per region (logs/m9-20seeds.log), 2,080 units, default
3,000-tick limit:

| region | winners | termination | end tick mean | deaths mean | stuck > 100 |
| --- | --- | --- | --- | --- | --- |
| lebanon | Blue 19 / Red 1 | 19 tick_limit, 1 routed | 2,949 | 290 | 0 |
| alps-innsbruck | Blue 15 / Red 5 | 12 tick_limit, 8 routed | 2,698 | 104 | 0 |
| nile-delta | Red 20 / Blue 0 | 3 tick_limit, 17 routed | 1,258 | 187 | 0 |

These are real-geography maps, not balanced arenas: in Lebanon Blue
holds the wide Bekaa while Red must deploy on the thin coastal plain
and cross the ridge; in the Nile delta Red's desert flank meets Blue's
branch-cut east side and the battles resolve fast on open ground; the
Alps grind slowly at 91% mountain. The longest stuck stretch anywhere
is 20 ticks (the detection threshold itself — recovery always engaged);
zero units were ever stuck for more than 100 ticks.

- **Renders** at ticks 0/150/300 for each region (seed 42):
  `download/m9-frames/<region>-0000*.png`.
- **Regeneration**: outputs deleted and regenerated — byte-identical
  SHA-256 for every PNG, meta and scenario.
- **Cost**: generation 2.0 s (Lebanon) / 2.4 s (Alps) / 3.3 s (Nile,
  3 tiles); peak RSS 281 / 283 / 610 MiB.
- **Mirror control (pre-step)**: demo-river flipped horizontally with
  the same (symmetric) factions: winners flip from Blue 75/Red 25 to
  Red 80/Blue 20 — the M8 skew was terrain-side, not spawn, objective
  or code asymmetry; the residual ~5 points is consistent with Red's
  deterministic first position in the per-tick unit order.

## Notes

- The sandbox was recycled once mid-milestone (the gitignored build
  directory and a background job were lost; source, cache and logs
  survived) — rebuilt, no source was affected.
- STOPPED after M9. M10 was not started.
