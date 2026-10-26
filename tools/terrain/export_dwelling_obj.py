"""Export native dwelling JSON to OBJ/MTL, retaining per-corner UV seams."""
import argparse
import json
import os
from pathlib import Path

from dwelling_materials import Materials


def export(source, destination, groups=None):
    data = json.loads(source.read_text())
    library = Materials(data)
    destination.parent.mkdir(parents=True, exist_ok=True)
    mtl = destination.with_suffix(".mtl")
    with mtl.open("w") as out:
        for key, record in library.records.items():
            out.write(f"newmtl material_{key}\n")
            # A texture already supplies albedo; do not multiply it by the
            # fallback color a second time in external OBJ viewers.
            color = record.get("tint", [1,1,1]) if "texture" in record else [v/255 for v in record["color"]]
            out.write("Kd " + " ".join(str(v) for v in color) + "\nKa 0.2 0.2 0.2\nillum 1\n")
            if "texture" in record:
                relative = os.path.relpath(library.path.parent / record["texture"], destination.parent)
                clamp = "-clamp on " if record.get("wrap") == "clamp" else ""
                out.write(f"map_Kd {clamp}{relative}\n")
    with destination.open("w") as out:
        out.write(f"# Repeat UVs; material texture scale baked into vt.\nmtllib {mtl.name}\n")
        for p in data["vertices"]:
            out.write("v " + " ".join(str(v) for v in p) + "\n")
        for ti, triangle in enumerate(data["triangles"]):
            _, uv, _ = library.surface(data, ti, [1,1,1])
            if uv is None:
                raise ValueError("Mesh has no UV coordinates")
            for t in uv:
                out.write(f"vt {t[0]} {t[1]}\n")
        if groups is None:
            groups = [("dwelling", range(len(data["triangles"])))]
        for name, triangle_indices in groups:
            out.write(f"o {name}\n")
            for ti in triangle_indices:
                triangle = data["triangles"][ti]
                material = data["surfaceMaterials"][ti]
                out.write(f"usemtl material_{material}\n")
                out.write("f " + " ".join(f"{v+1}/{ti*3+k+1}" for k,v in enumerate(triangle)) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    export(args.input, args.output)
