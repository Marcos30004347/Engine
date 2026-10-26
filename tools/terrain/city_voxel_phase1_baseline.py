#!/usr/bin/env python3
"""Extract city voxel planning counts from an existing city-model export.

The export can exceed 2 GB. This script maps it, parses only the small
buildings prefix, and scans large numeric arrays in bounded chunks.
"""

import argparse
import collections
import json
import mmap
from pathlib import Path
import re


def array_start(data, name):
    marker = ('"' + name + '":[').encode()
    location = data.find(marker)
    if location < 0:
        raise ValueError(f"missing {name}")
    return location + len(marker) - 1


def closing_bracket(data, start):
    depth = 0
    in_string = False
    escaped = False
    for offset in range(start, len(data)):
        char = data[offset]
        if in_string:
            if escaped:
                escaped = False
            elif char == 92:
                escaped = True
            elif char == 34:
                in_string = False
        elif char == 34:
            in_string = True
        elif char == 91:
            depth += 1
        elif char == 93:
            depth -= 1
            if depth == 0:
                return offset + 1
    raise ValueError("unterminated array")


def count_in_chunks(data, start, end, needle):
    # Retain the boundary bytes required to recognize a split pattern.
    count = 0
    overlap = b""
    for offset in range(start, end, 8 * 1024 * 1024):
        block = overlap + data[offset:min(end, offset + 8 * 1024 * 1024)]
        count += block.count(needle)
        overlap = block[-(len(needle) - 1):] if len(needle) > 1 else b""
    return count


def material_counts(data, start, end):
    # Material arrays contain simple integers, so an 8 MiB scan suffices.
    result = collections.Counter()
    carry = b""
    for offset in range(start + 1, end - 1, 8 * 1024 * 1024):
        block = carry + data[offset:min(end - 1, offset + 8 * 1024 * 1024)]
        split = block.rfind(b",")
        if split < 0:
            carry = block
            continue
        for match in re.finditer(rb"\d+", block[:split + 1]):
            result[int(match.group())] += 1
        carry = block[split + 1:]
    for match in re.finditer(rb"\d+", carry):
        result[int(match.group())] += 1
    return result


def inspect(path):
    with path.open("rb") as file:
        data = mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ)
        buildings_start = array_start(data, "buildings")
        buildings_end = closing_bracket(data, buildings_start)
        buildings = json.loads(data[buildings_start:buildings_end])
        materials_start = array_start(data, "materials")
        materials_end = data.find(b"]", materials_start) + 1
        surface_start = array_start(data, "surfaceMaterials")
        surface_end = data.find(b"]", surface_start) + 1
        triangles_start = array_start(data, "triangles")
        triangles_end = data.find(b"]]", triangles_start) + 2
        if min(materials_end, surface_end, triangles_end) < 1:
            raise ValueError("missing city model data")
        materials = material_counts(data, materials_start, materials_end)
        surfaces = material_counts(data, surface_start, surface_end)
        triangle_count = count_in_chunks(data, triangles_start, triangles_end, b"],[") + 1
        if triangle_count != sum(materials.values()) or triangle_count != sum(surfaces.values()):
            raise ValueError("triangle and material arrays have different lengths")
        result = {
            "source": str(path),
            "sourceBytes": len(data),
            "buildingCount": len(buildings),
            "buildingKinds": dict(sorted(collections.Counter(b["kind"] for b in buildings).items())),
            "triangleCount": triangle_count,
            "geometryMaterialCounts": dict(sorted(materials.items())),
            "surfaceMaterialCounts": dict(sorted(surfaces.items())),
            "note": "Existing flattened city mesh baseline; voxel/instance counts require phase 2 assembly output.",
        }
        data.close()
        return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("city_model", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = inspect(args.city_model)
    encoded = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(encoded)
    else:
        print(encoded, end="")
