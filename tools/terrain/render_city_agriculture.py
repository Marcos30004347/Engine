#!/usr/bin/env python3
import argparse
import json
from pathlib import Path

from PIL import Image, ImageDraw


COLORS = {"arable": "#d8cb67", "pasture": "#9fc27b", "orchard": "#6d9d5d", "vineyard": "#9274a0"}


def render(data, output):
    image = Image.new("RGB", (1200, 1000), "#f3efe5")
    draw = ImageDraw.Draw(image)
    points = [(p[0], p[2]) for parcel in data["parcels"] for p in parcel["boundary"]]
    min_x, max_x = min(x for x, _ in points), max(x for x, _ in points)
    min_z, max_z = min(z for _, z in points), max(z for _, z in points)
    scale = min(1020 / (max_x - min_x), 780 / (max_z - min_z))

    def pt(p):
        return 90 + (p[0] - min_x) * scale, 920 - (p[2] - min_z) * scale

    holdings = {}
    for parcel in data["parcels"]:
        q = [pt(p) for p in parcel["boundary"]]
        color = COLORS.get(parcel["use"], "#ccc")
        if not parcel["selected"]:
            color = "#d8d4c9"
        draw.polygon(q, fill=color, outline="#504a42", width=2)
        if parcel["selected"]:
            holdings.setdefault(parcel["holding"], []).append(q)
            xs, ys = [x for x, _ in q], [y for _, y in q]
            mask = Image.new("L", image.size, 0)
            ImageDraw.Draw(mask).polygon(q, fill=255)
            overlay = image.copy()
            hatch = ImageDraw.Draw(overlay)
            height = max(ys) - min(ys)
            for x in range(int(min(xs) - height), int(max(xs) + height), 14):
                hatch.line((x, max(ys), x + height, min(ys)), fill="#f8f5ea", width=2)
            image.paste(overlay, mask=mask)
            draw = ImageDraw.Draw(image)
            draw.line(q + [q[0]], fill="#504a42", width=2)
    draw.text((35, 20), "C3 — food- and labour-driven farmland", fill="#211f1b")
    draw.text((35, 46), f"selected fields stop at demand + reserve: {data['usable']:.0f} / {data['required']:.0f} annual food units", fill="#514b42")
    draw.text((35, 72), "Each outlined polygon is a generated FarmField subdivision; hatch = demand-funded production", fill="#514b42")
    image.save(output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    render(json.loads(args.input.read_text()), args.output)
