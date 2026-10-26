#!/usr/bin/env python3
import argparse
import json
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


COLORS = {
    "urban-district": "#d7c7a6",
    "urban-block": "#c4a77d",
    "urban-lot": "#ead6b8",
    "arable": "#b7c979",
    "pasture": "#a8c47c",
    "orchard": "#769f61",
    "vineyard": "#9274a0",
    "common": "#b9ce99",
    "managed-wood": "#587a50",
    "wild-land": "#75846b",
    "wetland": "#6d9c91",
    "floodplain": "#9bbfba",
    "unusable": "#8e8880",
    "shore": "#6eabc2",
    "future-growth": "#d7b4cf",
    "farmstead": "#d68a52",
    "military-reserve": "#986a65",
}


def xz(p):
    return float(p[0]), float(p[2])


def render(data, output):
    points = []
    for region in data["regions"]:
        points.extend(map(xz, region["boundary"]))
    for road in data["roads"]:
        points.extend(map(xz, road["path"]))
    if not points:
        raise ValueError("empty city skeleton")
    min_x = min(p[0] for p in points) - 15
    max_x = max(p[0] for p in points) + 15
    min_z = min(p[1] for p in points) - 15
    max_z = max(p[1] for p in points) + 15
    size = 1400, 900
    scale = min((size[0] - 120) / (max_x - min_x), (size[1] - 120) / (max_z - min_z))

    def pt(p):
        x, z = xz(p)
        return 60 + (x - min_x) * scale, size[1] - 60 - (z - min_z) * scale

    image = Image.new("RGB", size, "#f4f0e7")
    draw = ImageDraw.Draw(image)
    for region in sorted(data["regions"], key=lambda r: {"rural-patch": -3, "district": -2, "reservation": -2, "exterior-parcel": -1, "block": 1, "field": 2, "farmyard": 3, "lot": 3}.get(r["kind"], 0)):
        polygon = [pt(p) for p in region["boundary"]]
        if region["kind"] == "rural-patch":
            # A rural patch is a containment/budget node, not a final terrain
            # face. Filling it hid its fields and printed the coarse parent
            # silhouette back into the supposedly refined map.
            draw.line(polygon + [polygon[0]], fill="#8c7a4a", width=1)
            continue
        color = COLORS.get(region["use"], "#cccccc")
        draw.polygon(polygon, fill=color, outline="#574f45", width=2)
    for contact in data["contacts"]:
        if contact["kind"] == 0:
            x, y = pt(contact["point"])
            draw.ellipse((x - 4, y - 4, x + 4, y + 4), fill="#6b4fa1")
    for road in data["roads"]:
        path = [pt(p) for p in road["path"]]
        width = max(3, int(road["width"] * scale * 2))
        draw.line(path, fill="#ede5d5", width=width)
        draw.line(path, fill="#4c4842", width=2)
    for wall in data["walls"]:
        draw.line((pt(wall["a"]), pt(wall["b"])), fill="#30343b", width=8)
    for gate in data["gates"]:
        x, y = pt(gate["position"])
        draw.ellipse((x - 8, y - 8, x + 8, y + 8), fill="#e8b23f", outline="#332711", width=2)
    draw.text((35, 22), "C1 — unified city skeleton", fill="#26231f")
    draw.text((35, 45), "polygons + containment/shared-boundary contacts + road graph + wall/gate graph", fill="#544d43")
    image.save(output)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    render(json.loads(args.input.read_text()), args.output)


if __name__ == "__main__":
    main()
