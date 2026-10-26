#!/usr/bin/env python3
import argparse
import json
from pathlib import Path

from PIL import Image, ImageDraw


COLORS = {
    "urban-district": "#b89b79",
    "arable": "#d8cf72",
    "pasture": "#a8c47c",
    "orchard": "#769f61",
    "common": "#b9ce99",
    "managed-wood": "#587a50",
    "wetland": "#6d9c91",
    "floodplain": "#9bbfba",
    "quarry": "#8e8880",
    "cemetery": "#807490",
    "road-reserve": "#d4c7b1",
    "wall-clearance": "#e4b8a4",
    "shore": "#6eabc2",
    "future-growth": "#d7b4cf",
    "military-reserve": "#9b6762",
    "unusable": "#5a5755",
    "water": "#3f78a8",
}


def render(data, output):
    size = (1400, 1050)
    image = Image.new("RGB", size, "#f3efe5")
    draw = ImageDraw.Draw(image)
    points = [(p[0], p[2]) for parcel in data["parcels"] for p in parcel["boundary"]]
    min_x, max_x = min(x for x, _ in points), max(x for x, _ in points)
    min_z, max_z = min(z for _, z in points), max(z for _, z in points)
    scale = min((size[0] - 140) / (max_x - min_x), (size[1] - 170) / (max_z - min_z))

    def pt(p):
        return 70 + (p[0] - min_x) * scale, size[1] - 80 - (p[2] - min_z) * scale

    present = []
    for parcel in data["parcels"]:
        q = [pt(p) for p in parcel["boundary"]]
        # Source-cell seams are deliberately hidden: the cells carry lineage
        # and suitability, while blocks and FarmField products own final edges.
        draw.polygon(q, fill=COLORS.get(parcel["use"], "#cccccc"))
        if parcel["use"] not in present:
            present.append(parcel["use"])
    for road in data["roads"]:
        path = [pt(p) for p in road["path"]]
        draw.line(path, fill="#f7f0df", width=max(5, int(road["width"] * scale * 2)))
        draw.line(path, fill="#4e4942", width=2)
    draw.text((35, 20), "C2 — city influence allocation", fill="#211f1b")
    draw.text((35, 46), "World biomes and water remain upstream; cells are provenance, not final parcels", fill="#514b42")
    x, y = size[0] - 250, 24
    for use in present:
        draw.rectangle((x, y, x + 16, y + 12), fill=COLORS.get(use, "#cccccc"), outline="#504a42")
        draw.text((x + 23, y - 2), use, fill="#24211d")
        y += 18
    image.save(output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    render(json.loads(args.input.read_text()), args.output)
