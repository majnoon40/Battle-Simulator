#!/usr/bin/env python3
"""M9 mapgen tests (unittest, no network, no ETOPO tiles).

Runs the whole generation pipeline on a synthetic 64x64 fixture: an
analytic DEM and a hand-made river, covering classification thresholds,
river rasterizing, fords, connectivity repair, determinism, bbox
validation, palette consistency, and the shapefile reader (against a
shapefile the test writes itself).
"""
from __future__ import annotations

import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from mapgen import (  # noqa: E402
    BAND_FRACTIONS, CELL_SIZE, EtopoMosaic, bands_connectable,
    classify, connected_components, dilate, generate_map, grid_axes,
    load_palette, rasterize_lakes, snap_to_passable, slope_degrees,
    stamp_rivers, validate_bbox, write_outputs)

TERRAIN_JSON = HERE.parents[1] / "data" / "terrain.json"
CFG = json.loads((HERE / "classify.json").read_text())
PALETTE = load_palette(TERRAIN_JSON)
TABLE, INDEX, COLORS = PALETTE

# A tiny synthetic world: 0.64 x 0.64 degrees, 64x64 cells.
BBOX = (0.0, 0.0, 0.64, 0.64)
W = H = 64


def synthetic_elev(lons, lats):
    """Analytic DEM: a west plain (0-100 m), a ridge rising east to
    1600 m, sea in the far south (negative), and a steep cliff band."""
    elev = np.zeros(lons.shape, dtype=np.float64)
    elev += 50.0 + 1550.0 * np.clip((lons - 0.2) / 0.4, 0, 1)  # ramp east
    elev += 30.0 * np.sin(8.0 * lons) * np.cos(6.0 * lats)     # texture
    elev[lats < 0.06] -= 300.0                                  # southern sea
    # A steep cliff: 600 m over one cell at lon ~0.32 (slope >> 14 deg).
    elev[(lons > 0.315) & (lons < 0.325)] += 600.0
    return elev


def sample_grid(lons, lats):
    return synthetic_elev(lons, lats)


def synthetic_river(vertical=True):
    """A hand-made river: north-south down the map middle."""
    if vertical:
        return [[(0.30 + 0.02 * math_sin(i), 0.64 - 0.01 * i)
                 for i in range(65)]]
    return [[(0.01 * i, 0.32) for i in range(65)]]


def math_sin(x):
    import math
    return math.sin(x * 20.0) * 0.1 + x * 0.2


class ClassifyTest(unittest.TestCase):
    def test_thresholds(self):
        lons, lats = grid_axes(BBOX, W, H)
        lo, la = np.meshgrid(lons, lats)
        elev = synthetic_elev(lo, la)
        slope = slope_degrees(elev, BBOX, W, H)
        types = classify(elev, slope, CFG, INDEX)
        # The far south is sea.
        self.assertEqual(types[-1, 0], INDEX["water"])
        # The west plain is grassland (low, gentle).
        self.assertEqual(types[2, 2], INDEX["grassland"])
        # The east is high mountain by elevation.
        self.assertEqual(types[2, -2], INDEX["mountain"])
        # The cliff cells are mountain by slope even at modest elevation.
        cliff = (lo > 0.315) & (lo < 0.325)
        self.assertTrue((types[cliff] == INDEX["mountain"]).all() or
                        (types[cliff] == INDEX["hills"]).any())
        # A cell in the ramp's middle elevation range (400-1200 m, away
        # from the cliff) is hills.
        mid = (lo > 0.29) & (lo < 0.31) & (la > 0.4)
        self.assertTrue((types[mid] == INDEX["hills"]).any())

    def test_synthetic_forest_off_by_default(self):
        lons, lats = grid_axes(BBOX, W, H)
        lo, la = np.meshgrid(lons, lats)
        elev = synthetic_elev(lo, la)
        slope = slope_degrees(elev, BBOX, W, H)
        plain = classify(elev, slope, CFG, INDEX)
        forest = classify(elev, slope, CFG, INDEX, synthetic_forest=True)
        # With forest on, some low grassland becomes forest; without it,
        # no forest cell exists.
        self.assertEqual(np.sum(plain == INDEX["forest"]), 0)
        self.assertGreater(np.sum(forest == INDEX["forest"]), 0)


class RiverTest(unittest.TestCase):
    def test_rasterizing_and_width(self):
        river, fords = stamp_rivers(
            synthetic_river(), BBOX, W, H, ford_spacing=10)
        self.assertGreater(len(river), 40)
        # Dilated rivers are at least one cell wide everywhere: every
        # river cell has a stamped neighbor (no isolated single cells).
        mask = np.zeros((H, W), dtype=bool)
        for cy, cx in river:
            mask[cy, cx] = True
        dilated = dilate(mask)
        self.assertTrue((mask == (dilated & mask)).all())

    def test_ford_spacing(self):
        river, fords = stamp_rivers(
            synthetic_river(), BBOX, W, H, ford_spacing=10)
        # The stamps are ordered along the river; every 10th is a ford.
        self.assertGreaterEqual(len(fords), 4)
        for cell in fords:
            self.assertIn(cell, river)

    def test_no_fords_when_disabled(self):
        river, fords = stamp_rivers(
            synthetic_river(), BBOX, W, H, ford_spacing=0)
        self.assertEqual(len(fords), 0)


class ConnectivityTest(unittest.TestCase):
    def test_river_seals_and_repair_connects(self):
        # A horizontal river across the whole map splits west from east;
        # generate_map must add fords until the bands connect.
        types, meta = generate_map(
            "fixture", BBOX, W, H, sample_grid, synthetic_river(False),
            [], CFG, PALETTE)
        self.assertTrue(meta["ford_rounds"][-1]["connected"])
        self.assertGreater(meta["fords"], 0)
        passable = (types != INDEX["water"]) & (types != INDEX["obstacle"])
        self.assertTrue(bands_connectable(passable, W, H,
                                          meta["spawn_band_fraction"]))

    def test_corner_cutting_is_not_connectivity(self):
        # Two land cells touching only at a corner, with water at both
        # orthogonal companions, are NOT connected (the sim's flow-field
        # rule; no corner cutting).
        land = np.zeros((3, 3), dtype=bool)
        land[0, 0] = True
        land[1, 1] = True
        labels, count = connected_components(land)
        self.assertEqual(count, 2)
        # With one orthogonal companion passable they do connect.
        land[0, 1] = True
        labels, count = connected_components(land)
        self.assertEqual(count, 1)

    def test_snap_to_passable(self):
        allowed = np.zeros((5, 5), dtype=bool)
        allowed[2, 3] = True
        self.assertEqual(snap_to_passable(0, 0, allowed), (3, 2))
        self.assertIsNone(snap_to_passable(0, 0, np.zeros((3, 3), bool)))


class LakeTest(unittest.TestCase):
    def test_polygon_fill(self):
        lake = [(0.2, 0.4), (0.4, 0.4), (0.4, 0.5), (0.2, 0.5),
                (0.2, 0.4)]
        water = rasterize_lakes([lake], BBOX, W, H)
        # The lake spans lat 0.4-0.5 (rows ~14-24), lon 0.2-0.4.
        self.assertTrue(water[19, 30])   # inside
        self.assertFalse(water[5, 30])   # north of it
        self.assertFalse(water[5, 5])    # far corner


class GenerateTest(unittest.TestCase):
    def setUp(self):
        self.out = Path(tempfile.mkdtemp(prefix="m9_mapgen_"))

    def tearDown(self):
        import shutil
        shutil.rmtree(self.out, ignore_errors=True)

    def generate(self, name="fixture"):
        return generate_map(name, BBOX, W, H, sample_grid,
                            synthetic_river(), [], CFG, PALETTE)

    def test_determinism_byte_identical(self):
        types_a, meta_a = self.generate()
        types_b, meta_b = self.generate()
        np.testing.assert_array_equal(types_a, types_b)
        self.assertEqual(meta_a, meta_b)
        # And the written files are byte-identical across regenerations.
        lons, lats = grid_axes(BBOX, W, H)
        lo, la = np.meshgrid(lons, lats)
        elev = sample_grid(lo, la)
        paths_a = write_outputs(types_a, meta_a, "fixture", elev, BBOX, W,
                                H, PALETTE, out_root=self.out)
        paths_b = write_outputs(types_b, meta_b, "fixture", elev, BBOX, W,
                                H, PALETTE, out_root=self.out)
        for a, b in zip(paths_a, paths_b):
            self.assertEqual(a.read_bytes(), b.read_bytes())

    def test_palette_matches_terrain_json(self):
        types, meta = self.generate()
        lons, lats = grid_axes(BBOX, W, H)
        lo, la = np.meshgrid(lons, lats)
        elev = sample_grid(lo, la)
        write_outputs(types, meta, "fixture", elev, BBOX, W, H, PALETTE,
                      out_root=self.out)
        image = Image.open(self.out / "data" / "maps" / "fixture.png")
        image = image.convert("RGB")
        used = set(image.getdata())
        allowed = set(COLORS.values())
        self.assertTrue(used <= allowed, f"colors outside palette: "
                                         f"{used - allowed}")
        # Every type index in the grid maps to the right palette color.
        for i, color in COLORS.items():
            cells = types == i
            if cells.any():
                pixels = image.crop((0, 0, W, H))
                data = np.array(pixels)
                self.assertTrue((data[cells] == np.array(color)).all())

    def test_meta_records_everything(self):
        types, meta = self.generate()
        for key in ("region", "bbox", "grid", "cell_size", "terrain_counts",
                    "objectives", "fords", "ford_rounds",
                    "spawn_band_fraction", "isolated_land_cells_flooded",
                    "synthetic_forest"):
            self.assertIn(key, meta)
        self.assertEqual(sum(meta["terrain_counts"].values()), W * H)
        self.assertFalse(meta["synthetic_forest"])

    def test_scenario_shape(self):
        types, meta = self.generate()
        lons, lats = grid_axes(BBOX, W, H)
        lo, la = np.meshgrid(lons, lats)
        elev = sample_grid(lo, la)
        write_outputs(types, meta, "fixture", elev, BBOX, W, H, PALETTE,
                      out_root=self.out)
        scenario = json.loads(
            (self.out / "data" / "scenarios" / "earth-fixture.json")
            .read_text())
        self.assertEqual(scenario["battlefield"], {"width": W * CELL_SIZE,
                                                   "height": H * CELL_SIZE})
        self.assertEqual(scenario["map"]["cell_size"], CELL_SIZE)
        self.assertEqual(scenario["map"]["file"], "fixture.png")
        # Objectives are the generated (passable) points.
        self.assertEqual(scenario["objectives"]["red"]["x"],
                         meta["objectives"]["red"][0])
        # Mixed armies, about 2000 units, squads on.
        total = sum(scenario["armies"]["red"].values())
        self.assertEqual(total, 1040)
        self.assertEqual(sum(scenario["armies"]["blue"].values()), 1040)
        self.assertEqual(scenario["squads"]["red"],
                         {"count": 40, "size": 25})
        self.assertIn("spawn_band_fraction", scenario)


class BboxTest(unittest.TestCase):
    def test_valid(self):
        self.assertEqual(validate_bbox((35.0, 33.0, 36.7, 34.7)),
                         (35.0, 33.0, 36.7, 34.7))

    def test_invalid(self):
        for bad in [(3, 2, 2, 4),          # lon_min > lon_max
                    (-181, 0, 180, 1),     # lon out of range
                    (0, -91, 1, 90),       # lat out of range
                    (-180, -90, 180, 90),  # span > 30 degrees
                    (0, 0, 0, 1)]:         # empty lon range
            with self.assertRaises(ValueError):
                validate_bbox(bad)


class ShapefileTest(unittest.TestCase):
    def _write_shp(self, path, points):
        """A one-part polyline shapefile, hand-packed."""
        record = struct.pack("<i", 3)
        record += struct.pack("<4d", *[v for p in [(min(x for x, _ in points),
                                                    min(y for _, y in points),
                                                    max(x for x, _ in points),
                                                    max(y for _, y in points))]
                                           for v in p])
        record += struct.pack("<2i", 1, len(points))
        record += struct.pack("<i", 0)
        for x, y in points:
            record += struct.pack("<2d", x, y)
        header = struct.pack(">i", 9994) + b"\x00" * 20
        header += struct.pack(">i", 50 + len(record) // 2)
        header += struct.pack("<i", 1000) + struct.pack("<i", 3)
        header += struct.pack("<8d", 0, 0, 0, 0, 0, 0, 0, 0)
        Path(path).write_bytes(header + struct.pack(">ii", 1,
                                                    len(record) // 2) + record)

    def _write_dbf(self, path, rows):
        fields = [("NAME", "C", 10)]
        out = struct.pack("<B", 0x03) + struct.pack("<3B", 99, 1, 1)
        out += struct.pack("<I", len(rows))
        out += struct.pack("<H", 32 + 32 * len(fields) + 1)
        out += struct.pack("<H", 1 + sum(f[2] for f in fields))
        out += b"\x00" * 20  # reserved: real dBase headers are 32 bytes
        for name, ftype, flen in fields:
            out += name.ljust(11, "\x00").encode("latin-1")
            out += ftype.encode() + b"\x00" * 4
            out += struct.pack("<B", flen) + struct.pack("<B", 0) + b"\x00" * 14
        out += b"\x0D"
        for name, in rows:
            out += b" " + name.ljust(10)[:10].encode("latin-1")
        Path(path).write_bytes(out)

    def test_reader(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp) / "fixture"
            pts = [(0.0, 0.0), (1.0, 1.0), (2.0, 1.0)]
            self._write_shp(f"{base}.shp", pts)
            self._write_dbf(f"{base}.dbf", [("Litani",)])
            from shapefile import read_shapefile
            records = read_shapefile(str(base))
            self.assertEqual(len(records), 1)
            shape, attrs = records[0]
            self.assertEqual(shape["type"], "polyline")
            self.assertEqual(shape["parts"], [pts])
            self.assertEqual(attrs["NAME"], "Litani")


class EtopoNamingTest(unittest.TestCase):
    def test_tile_names(self):
        mosaic = EtopoMosaic()
        self.assertEqual(mosaic._tile_name(45, 30),
                         "ETOPO_2022_v1_15s_N45E030_surface.tif")
        self.assertEqual(mosaic._tile_name(60, 0),
                         "ETOPO_2022_v1_15s_N60E000_surface.tif")
        self.assertEqual(mosaic._tile_name(30, -30),
                         "ETOPO_2022_v1_15s_N30W030_surface.tif")


if __name__ == "__main__":
    unittest.main()
