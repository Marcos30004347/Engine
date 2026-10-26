"""Render native dwelling triangles, with an explicit ground-floor section."""
import argparse
import json
from pathlib import Path
from render_city_model import render


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--section-height", type=float, default=2.5)
    parser.add_argument("--yaw", type=float, default=-.72)
    args = parser.parse_args()
    data = json.loads(args.input.read_text())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    render(data, args.output.with_name(args.output.name + "-exterior.png"),
           args.yaw, .65, title="DWELLING — NATIVE STRUCTURAL MESH")
    # A diagnostic section only, never a replacement mesh. Clip triangles
    # at the cut plane so tall wall panels remain visible below the cut.
    section = dict(data)
    section["vertices"], section["triangles"], section["materials"] = [], [], []
    section["uvs"], section["surfaceMaterials"] = [], []
    section.pop("triangleUVs", None)
    section.pop("uvIslands", None)
    source_uvs=data.get("uvs")
    source_materials=data.get("surfaceMaterials", [0]*len(data["triangles"]))
    for ti, (triangle, material) in enumerate(zip(data["triangles"], data["materials"])):
        uv = data["triangleUVs"][ti] if "triangleUVs" in data else [source_uvs[i] if source_uvs else [0,0] for i in triangle]
        polygon = [data["vertices"][i]+list(t) for i,t in zip(triangle,uv)]
        clipped = []
        previous = polygon[-1]
        for current in polygon:
            dp = args.section_height - previous[1]
            dc = args.section_height - current[1]
            if (dp >= 0) != (dc >= 0):
                t = dp / (dp - dc)
                clipped.append([a + t * (b - a) for a, b in zip(previous, current)])
            if dc >= 0:
                clipped.append(current)
            previous = current
        first = len(section["vertices"])
        section["vertices"].extend(p[:3] for p in clipped)
        section["uvs"].extend(p[3:] for p in clipped)
        for i in range(2, len(clipped)):
            section["triangles"].append([first, first + i - 1, first + i])
            section["materials"].append(material)
            section["surfaceMaterials"].append(source_materials[ti])
    # Close the diagnostic cut through wall thickness using structural opaque
    # panels, not room outlines. These caps do not modify the source mesh.
    for wall in data.get("sectionWalls", []):
        panel = wall["polygon"]
        intersections = []
        for a, b in zip(panel, panel[1:] + panel[:1]):
            if (a[1] < args.section_height) != (b[1] < args.section_height):
                t = (args.section_height - a[1]) / (b[1] - a[1])
                intersections.append([a[i] + t * (b[i] - a[i]) for i in range(3)])
        if len(intersections) != 2:
            continue
        a, b = intersections
        through = wall["through"]
        cap = [a, b, [b[i] + through[i] for i in range(3)],
               [a[i] + through[i] for i in range(3)]]
        first = len(section["vertices"])
        section["vertices"].extend(cap)
        section["uvs"].extend([p[0],p[2]] for p in cap)
        section["triangles"].extend([[first, first+1, first+2], [first, first+2, first+3]])
        section["materials"].extend([12, 12])
        section["surfaceMaterials"].extend([1,1])
    render(section, args.output.with_name(args.output.name + "-interior.png"),
           args.yaw, .4, title="GROUND-FLOOR SECTION — ROOMS, STAIRS AND FURNITURE")


if __name__ == "__main__":
    main()
