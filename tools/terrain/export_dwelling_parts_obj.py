"""Diagnostic OBJ split for the textured native dwelling mesh.

The native DwellingVoxelAssembly is the source of production part identities.
This exporter recovers opening pieces from the current flat preview mesh so
the existing OBJ voxel renderer can inspect each part as an independent BLAS.
"""

import argparse
import json
from collections import defaultdict
from pathlib import Path

from export_dwelling_obj import export


def bounds(data, triangle_indices):
    positions = [data["vertices"][vertex]
                 for triangle_index in triangle_indices
                 for vertex in data["triangles"][triangle_index]]
    return ([min(point[axis] for point in positions) for axis in range(3)],
            [max(point[axis] for point in positions) for axis in range(3)])


def box_gap(a, b, axis):
    return max(0.0, a[0][axis] - b[1][axis], b[0][axis] - a[1][axis])


def opening_clusters(data, material_id, gap_metres):
    indices = [index for index, material in enumerate(data["surfaceMaterials"])
               if material == material_id]
    if not indices:
        return []
    boxes = {index: bounds(data, [index]) for index in indices}
    parent = {index: index for index in indices}

    def root(index):
        while parent[index] != index:
            parent[index] = parent[parent[index]]
            index = parent[index]
        return index

    for offset, left in enumerate(indices):
        for right in indices[offset + 1:]:
            if all(box_gap(boxes[left], boxes[right], axis) <= gap_metres
                   for axis in range(3)):
                parent[root(left)] = root(right)
    groups = defaultdict(list)
    for index in indices:
        groups[root(index)].append(index)
    return sorted(groups.values(), key=lambda group: tuple(bounds(data, group)[0]))


def partition(data, omit_window_glass=True):
    triangles = data["triangles"]
    materials = data["surfaceMaterials"]
    windows = opening_clusters(data, 5, 0.16)  # Leaded glass, both wall faces.
    doors = opening_clusters(data, 8, 0.32)    # Door leaves and their back faces.
    if len(doors) != 1:
        raise ValueError(f"Expected one entrance door, found {len(doors)}")

    grouped = {}
    opening_boxes = []
    for number, triangles_in_part in enumerate(windows):
        name = f"window_{number:02d}"
        grouped[name] = list(triangles_in_part)
        opening_boxes.append((name, bounds(data, triangles_in_part), 0.16))
    grouped["door_entrance"] = list(doors[0])
    opening_boxes.append(("door_entrance", bounds(data, doors[0]), 0.26))

    assigned = {triangle: name for name, values in grouped.items() for triangle in values}
    grouped.update({"shell": [], "roof": [], "floor": []})
    for triangle_index, triangle in enumerate(triangles):
        if triangle_index in assigned:
            continue
        material = materials[triangle_index]
        triangle_box = bounds(data, [triangle_index])
        if material == 3:  # Structural and opening timber.
            for name, opening_box, margin in opening_boxes:
                if all(triangle_box[0][axis] >= opening_box[0][axis] - margin - 1e-6 and
                       triangle_box[1][axis] <= opening_box[1][axis] + margin + 1e-6
                       for axis in range(3)):
                    grouped[name].append(triangle_index)
                    assigned[triangle_index] = name
                    break
        if triangle_index in assigned:
            continue
        name = ("roof" if material == 2 or
                (material == 3 and triangle_box[0][1] >= 3.75) else
                "floor" if material == 10 else "shell")
        grouped[name].append(triangle_index)
        assigned[triangle_index] = name

    if len(assigned) != len(triangles):
        raise ValueError("Part partition lost triangles")
    if omit_window_glass:
        for number in range(len(windows)):
            name = f"window_{number:02d}"
            grouped[name] = [index for index in grouped[name] if materials[index] != 5]
    if any(not values for values in grouped.values()):
        raise ValueError("Part partition lost triangles or emitted an empty model")
    ordered = [(name, grouped[name]) for name in ("shell", "roof", "floor", "door_entrance")]
    ordered.extend((f"window_{number:02d}", grouped[f"window_{number:02d}"])
                   for number in range(len(windows)))
    manifest = [{"name": name, "triangles": len(indices), "bounds": bounds(data, indices)}
                for name, indices in ordered]
    return ordered, manifest


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--keep-window-glass", action="store_true")
    args = parser.parse_args()
    data = json.loads(args.source.read_text())
    groups, manifest = partition(data, omit_window_glass=not args.keep_window_glass)
    export(args.source, args.destination, groups)
    report = args.destination.with_suffix(".instances.json")
    report.write_text(json.dumps({"source": str(args.source),
                                  "windowGlassOmitted": not args.keep_window_glass,
                                  "omittedGlassTriangles": sum(material == 5 for material in data["surfaceMaterials"]) if not args.keep_window_glass else 0,
                                  "parts": manifest}, indent=2) + "\n")
    print(f"Exported {len(groups)} separate dwelling parts to {args.destination}")


if __name__ == "__main__":
    main()
