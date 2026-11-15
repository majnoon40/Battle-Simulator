#!/usr/bin/env python3
"""One-off M4 fixture update for tests/test_unit_types.cpp.

Adds the now-required "sight_radius" field to every unit-type JSON fixture
(mirroring data/units.json: infantry 40, archer 60, cavalry 50; generic
fixtures get 40, which exceeds every fixture range). Also asserts the parsed
sight values in ParsesValidTable. Additive only; no assertion is weakened.
"""
import re

PATH = "tests/test_unit_types.cpp"

with open(PATH) as f:
    text = f.read()

# 1. kValidTable entries: type-specific sight values, inserted before cooldown.
text = text.replace(
    '{"id": "infantry", "health": 100.0, "damage": 12.0, "speed": 2.0, "range": 2.0, "cooldown": 5}',
    '{"id": "infantry", "health": 100.0, "damage": 12.0, "speed": 2.0, "range": 2.0, "sight_radius": 40.0, "cooldown": 5}')
text = text.replace(
    '{"id": "archer", "health": 70.0, "damage": 9.0, "speed": 1.6, "range": 8.0, "cooldown": 8}',
    '{"id": "archer", "health": 70.0, "damage": 9.0, "speed": 1.6, "range": 8.0, "sight_radius": 60.0, "cooldown": 8}')
text = text.replace(
    '{"id": "cavalry", "health": 160.0, "damage": 20.0, "speed": 4.5, "range": 2.0, "cooldown": 7}',
    '{"id": "cavalry", "health": 160.0, "damage": 20.0, "speed": 4.5, "range": 2.0, "sight_radius": 50.0, "cooldown": 7}')

# 2. kFullEntry (escaped C-string form).
text = text.replace(
    '"\\"range\\": 3.0, \\"cooldown\\": 4}"',
    '"\\"range\\": 3.0, \\"cooldown\\": 4, \\"sight_radius\\": 40.0}"')

# 3. Every remaining raw-string fixture ending in "cooldown": N} gains
#    sight_radius 40.0 (all those fixtures use range <= 3.0). Lines that
#    already carry a sight_radius from steps 1-2 are skipped.
pattern = re.compile(r'"cooldown": (\d+)\}')
lines = text.split("\n")
for i, line in enumerate(lines):
    if "sight_radius" in line:
        continue
    lines[i] = pattern.sub(r'"cooldown": \1, "sight_radius": 40.0}', line)
text = "\n".join(lines)

with open(PATH, "w") as f:
    f.write(text)
print("done")
