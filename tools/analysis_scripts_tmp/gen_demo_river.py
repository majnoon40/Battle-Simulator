#!/usr/bin/env python3
"""Generates data/maps/demo-river.png (M8).

A 200x200-cell battlefield (cell_size 5, world 1000x1000): a wavy river
crossing north-south near the middle, a road bridge at the vertical
center, a slow ford to the south, forest patches on both banks, hills in
the southwest and a small mountain cluster in the northeast. Spawn bands
(the outer 15% columns) stay open. Every pixel is an exact terrain type
color from data/terrain.json; run scripts/gen_demo_river.py to regenerate
byte-identically (no RNG).
"""
from PIL import Image

W = H = 200
GRASS = (0x6F, 0xA3, 0x5C)
FOREST = (0x3E, 0x6B, 0x3A)
HILLS = (0xA0, 0x8A, 0x5A)
WATER = (0x3D, 0x6E, 0x9E)
ROAD = (0xB8, 0xA7, 0x7E)
MOUNTAIN = (0x7A, 0x7A, 0x82)

image = Image.new("RGB", (W, H), GRASS)
pixels = image.load()


def stamp(cx, cy, color):
    if 0 <= cx < W and 0 <= cy < H:
        pixels[cx, cy] = color


def blob(cx, cy, radius, color):
    for y in range(int(cy - radius), int(cy + radius) + 1):
        for x in range(int(cx - radius), int(cx + radius) + 1):
            if (x - cx) ** 2 + (y - cy) ** 2 <= radius * radius:
                stamp(x, y, color)


# Forest patches on both banks (away from the spawn bands).
blob(45, 45, 14, FOREST)
blob(70, 120, 10, FOREST)
blob(60, 90, 7, FOREST)
blob(150, 60, 12, FOREST)
blob(140, 140, 9, FOREST)
blob(160, 100, 6, FOREST)
# Hills in the southwest, mountains in the northeast.
blob(30, 160, 18, HILLS)
blob(55, 175, 10, HILLS)
blob(175, 25, 9, MOUNTAIN)
blob(165, 40, 5, MOUNTAIN)

# The river: a wavy vertical band ~7 cells wide around x = 100 + 6 sin.
import math

for y in range(H):
    center = 100.0 + 6.0 * math.sin(y / 22.0)
    for x in range(int(center) - 3, int(center) + 4):
        stamp(x, y, WATER)

# The bridge: a road strip crossing the river at y = 100 (3 cells thick).
for y in range(99, 102):
    for x in range(88, 113):
        stamp(x, y, ROAD)

# The ford: river cells replaced by slow ford cells at y = 150 (3 thick).
FORD = (0x5E, 0x8F, 0xB8)
for y in range(149, 152):
    for x in range(94, 107):
        stamp(x, y, FORD)

# Keep the spawn bands (outer 30 columns) free of blocking terrain so
# every army deploys in the open.
for y in range(H):
    for x in range(30):
        if pixels[x, y] in (WATER, MOUNTAIN):
            stamp(x, y, GRASS)
    for x in range(W - 30, W):
        if pixels[x, y] in (WATER, MOUNTAIN):
            stamp(x, y, GRASS)

image.save("data/maps/demo-river.png", optimize=False)
print(f"wrote data/maps/demo-river.png ({W}x{H})")
