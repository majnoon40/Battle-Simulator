"""Minimal ESRI shapefile reader (M9 mapgen).

Reads .shp (points, polylines, polygons) and .dbf (dBase attribute
tables) with nothing but the standard library — no GIS stack. Enough for
Natural Earth 10m rivers (polylines) and lakes (polygons):

    for shape, attrs in read_shapefile("ne_10m_rivers_lake_centerlines"):
        if attrs.get("featurecla") == "River":
            for part in shape["parts"]:
                ...  # list of (lon, lat) tuples

Deterministic: records are returned in file order.
"""
from __future__ import annotations

import struct
from pathlib import Path


def read_shp(path):
    """Yields one dict per shape record: {"type": name, "parts": [[(x, y), ...]]}."""
    data = Path(path).read_bytes()
    if len(data) < 100 or struct.unpack(">i", data[0:4])[0] != 9994:
        raise ValueError(f"{path}: not a shapefile (.shp)")
    types = {0: "null", 1: "point", 3: "polyline", 5: "polygon",
             8: "multipoint"}
    offset = 100
    out = []
    while offset + 8 <= len(data):
        _, content_words = struct.unpack(">ii", data[offset:offset + 8])
        content = data[offset + 8:offset + 8 + content_words * 2]
        offset += 8 + content_words * 2
        if len(content) < 4:
            break
        shape_type = struct.unpack("<i", content[0:4])[0]
        name = types.get(shape_type)
        if name == "null" or name is None:
            out.append({"type": name or str(shape_type), "parts": []})
            continue
        if name == "point":
            x, y = struct.unpack("<2d", content[4:20])
            out.append({"type": name, "parts": [[(x, y)]]})
            continue
        # polyline / polygon: box, num_parts, num_points, parts[], points[]
        num_parts, num_points = struct.unpack("<2i", content[36:44])
        parts_idx = struct.unpack(f"<{num_parts}i" if num_parts else "",
                                  content[44:44 + 4 * num_parts])
        points_start = 44 + 4 * num_parts
        points = struct.unpack(
            f"<{2 * num_points}d",
            content[points_start:points_start + 16 * num_points])
        parts = []
        for p in range(max(1, num_parts)):
            begin = parts_idx[p] if num_parts and p < num_parts else 0
            end = (parts_idx[p + 1] if num_parts and p + 1 < num_parts
                   else num_points)
            parts.append([(points[2 * i], points[2 * i + 1])
                          for i in range(begin, end)])
        out.append({"type": name, "parts": parts})
    return out


def read_dbf(path):
    """Returns one {field: string} dict per record, in file order."""
    data = Path(path).read_bytes()
    if len(data) < 32 or data[0] not in (0x03, 0x30, 0x31, 0x32, 0x43,
                                         0x63, 0x83, 0x8B, 0x8C, 0xF5):
        raise ValueError(f"{path}: not a dBase file (.dbf)")
    num_records = struct.unpack("<I", data[4:8])[0]
    header_size = struct.unpack("<H", data[8:10])[0]
    record_size = struct.unpack("<H", data[10:12])[0]
    fields = []
    pos = 32
    while pos < header_size and data[pos] != 0x0D:
        raw_name = data[pos:pos + 11].split(b"\x00")[0]
        ftype = chr(data[pos + 11])
        flen = data[pos + 16]
        fields.append((raw_name.decode("latin-1"), ftype, flen))
        pos += 32
    records = []
    body = data[header_size:header_size + num_records * record_size]
    for r in range(num_records):
        row = body[r * record_size:(r + 1) * record_size]
        if not row or row[0:1] == b"*":
            continue  # deleted
        values = {}
        off = 1
        for name, _, flen in fields:
            text = row[off:off + flen].decode("latin-1").strip()
            values[name] = text
            off += flen
        records.append(values)
    return records


def read_shapefile(base_path):
    """Pairs every shape record with its attribute record (by index)."""
    shp = read_shp(f"{base_path}.shp")
    dbf = read_dbf(f"{base_path}.dbf")
    if len(shp) != len(dbf):
        raise ValueError(f"{base_path}: {len(shp)} shapes but {len(dbf)} "
                         "attributes; the files do not match")
    return list(zip(shp, dbf))
