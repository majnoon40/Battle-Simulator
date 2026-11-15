# Data sources (M9 mapgen)

The M9 map generator turns real elevation and hydrography data into
battle terrain maps. This file pins the exact sources, versions,
licenses, citations and checksums. Raw downloads live in
`tools/mapgen/cache/` (gitignored); only the small generated maps are
committed.

## Elevation and bathymetry: NOAA ETOPO 2022

- **Product**: ETOPO 2022 Global Relief Model, 15 arc-second, *surface*
  elevation (topography-bathymetry), distributed as 15°x15° GeoTIFF
  tiles.
- **Publisher**: U.S. Department of Commerce, National Oceanic and
  Atmospheric Administration (NOAA), National Centers for Environmental
  Information (NCEI).
- **Version**: v1 (2022) — the tile filenames carry it
  (`ETOPO_2022_v1_15s_*`).
- **Product page**: https://www.ncei.noaa.gov/products/etopo-global-relief-model
- **Download directory**:
  https://www.ngdc.noaa.gov/mgg/global/relief/ETOPO2022/data/15s/15s_surface_elev_gtif/
- **License**: U.S. government public domain (no restrictions on use).
- **Required citation** (per NCEI guidance):
  > NOAA National Centers for Environmental Information. 2022: ETOPO
  > 2022 15 arc-second Global Relief Model. NOAA National Centers for
  > Environmental Information. https://www.ncei.noaa.gov/products/etopo-global-relief-model

Tile convention used by the tool: a tile named `N{a}E{b}` covers
latitude [a−15, a] and longitude [b, b+15], stored north-up (row 0 =
latitude a) at 1/240 degree per pixel, 3600x3600 pixels (verified
against landmarks: Beirut, Qornet es-Sawda, Zugspitze, Innsbruck, the
Red Sea, the Nile at Aswan).

Pinned tiles (SHA-256, computed 2026-11-15):

| file | SHA-256 |
| --- | --- |
| ETOPO_2022_v1_15s_N30E015_surface.tif | `1ee225cb379eb7c95d446337004f413a2eb270a4682383afb6930270ec04515c` |
| ETOPO_2022_v1_15s_N30E030_surface.tif | `deaa83101bcc301484382fbc350a444df5757674b9fd814635c10beaa5167a6b` |
| ETOPO_2022_v1_15s_N45E015_surface.tif | `b4983c653eeb75a5a88d0dd5887192f3416aefa211e147e17a0298404cf2d5ea` |
| ETOPO_2022_v1_15s_N45E030_surface.tif | `f4d1005d3c43defc9e33260e119e4d36ad5fc18f7d452febf13f3d6223fb4c73` |
| ETOPO_2022_v1_15s_N60E000_surface.tif | `d69dbf93dcf2f11fb008a57223da41092921ea2f81ef6eeb1807d50609586557` |

(The generator also embeds the SHA-256 of every tile it actually used in
each map's `.meta.json` under `sources.etopo.tiles`, so any map can be
traced to its exact inputs.)

## Rivers and lakes: Natural Earth

- **Product**: Natural Earth, 1:10m scale, Physical theme — "Rivers,
  Lake Centerlines" and "Lakes".
- **Publisher**: Natural Earth (naturalearthdata.com); files served from
  the official CDN naciscdn.org.
- **Version**: the 10m zip packages current at download time (2026-11-15);
  the SHA-256 below pins the exact bytes.
- **Download pages**:
  - Rivers: https://www.naturalearthdata.com/downloads/10m-physical-vectors/10m-rivers-lake-centerlines/
    (direct: https://naciscdn.org/naturalearth/10m/physical/ne_10m_rivers_lake_centerlines.zip)
  - Lakes: https://www.naturalearthdata.com/downloads/10m-physical-vectors/10m-lakes/
    (direct: https://naciscdn.org/naturalearth/10m/physical/ne_10m_lakes.zip)
- **License**: public domain (no known restrictions; attribution
  appreciated, not required).
- **Suggested citation**:
  > Natural Earth. 1:10m Physical Vectors: Rivers, Lake Centerlines;
  > Lakes. https://www.naturalearthdata.com/

Pinned archives:

| file | SHA-256 |
| --- | --- |
| ne_10m_rivers_lake_centerlines.zip | `ded71b01870855ccfe19b51f2ec14c9bb48fae23c0e9f3c11974d426433b5c38` |
| ne_10m_lakes.zip | `0803a06f9c3cb4671d89b68c48b142aad9366ba40f665245e12a913fbc61722a` |

## What is NOT in these datasets

Natural Earth and ETOPO contain **no roads and no forests**, and none
are faked as real. The generator's optional `--synthetic-forest` flag
paints a deterministic forest mask (grassland below 500 m and below 6°
slope) that is clearly labeled `"synthetic"` in the map metadata; it is
OFF by default and no generated map in this repo uses it.

## Re-downloading (manual steps)

```sh
cd tools/mapgen/cache
BASE=https://www.ngdc.noaa.gov/mgg/global/relief/ETOPO2022/data/15s/15s_surface_elev_gtif
for t in N30E015 N30E030 N45E015 N45E030 N60E000; do
  curl -O "$BASE/ETOPO_2022_v1_15s_${t}_surface.tif"
done
curl -O https://naciscdn.org/naturalearth/10m/physical/ne_10m_rivers_lake_centerlines.zip
curl -O https://naciscdn.org/naturalearth/10m/physical/ne_10m_lakes.zip
sha256sum *   # compare against the tables above
```

Python dependencies (light, no GIS stack): numpy, Pillow, tifffile,
imagecodecs (deflate+floating-point-predictor GeoTIFF decoding).
