#!/usr/bin/env python3
import argparse
import json
import math
from pathlib import Path

from PIL import Image, ImageDraw


def xz(p):
    return float(p[0]), float(p[2])


def dashed(draw, points, fill, width, dash=10, gap=7):
    for a, b in zip(points, points[1:]):
        dx, dy = b[0] - a[0], b[1] - a[1]
        length = math.hypot(dx, dy)
        if length == 0:
            continue
        at = 0.0
        while at < length:
            end = min(length, at + dash)
            draw.line((a[0] + dx * at / length, a[1] + dy * at / length,
                       a[0] + dx * end / length, a[1] + dy * end / length), fill=fill, width=width)
            at += dash + gap


def render(data, output):
    raw = []
    for key in ("walls", "zones", "blocks", "lots"):
        for item in data[key]:
            raw.extend(map(xz, item["boundary"]))
    for road in data["roads"]:
        raw.extend(map(xz, road["path"]))
    if not raw:
        raise ValueError("empty city growth plan")
    min_x, max_x = min(x for x, _ in raw), max(x for x, _ in raw)
    min_z, max_z = min(z for _, z in raw), max(z for _, z in raw)
    size = (1600, 1200)
    scale = min((size[0] - 160) / max(1, max_x - min_x), (size[1] - 190) / max(1, max_z - min_z))

    def pt(p):
        x, z = xz(p)
        return 80 + (x - min_x) * scale, size[1] - 90 - (z - min_z) * scale

    image = Image.new("RGB", size, "#f4f0e7")
    draw = ImageDraw.Draw(image)

    for zone in data["zones"]:
        color = "#dca178" if zone["kind"] == "suburb" else "#9b5a55"
        draw.polygon([pt(p) for p in zone["boundary"]], fill=color, outline="#60483e", width=2)

    for block in data["blocks"]:
        draw.polygon([pt(p) for p in block["boundary"]], fill="#d8c5a4", outline="#74634f", width=2)
    for lot in data["lots"]:
        shade = "#f0dfbf" if int(lot["id"]) & 1 else "#e7d2af"
        draw.polygon([pt(p) for p in lot["boundary"]], fill=shade, outline="#a58d6e", width=1)

    for road in data["roads"]:
        path = [pt(p) for p in road["path"]]
        draw.line(path, fill="#514b42", width=max(5, int(road["width"] * scale + 3)))
        draw.line(path, fill="#efe6d3", width=max(3, int(road["width"] * scale)))

    # Blocks deliberately occupy the same land as growth zones. Redraw the
    # semantic boundary above the cadastral layer so suburb cells remain
    # inspectable instead of disappearing beneath their lots.
    for zone in data["zones"]:
        color = "#c97948" if zone["kind"] == "suburb" else "#873f3b"
        loop = [pt(p) for p in zone["boundary"]]
        loop.append(loop[0])
        draw.line(loop, fill=color, width=3, joint="curve")
        if zone["kind"] == "castle":
            cx = sum(p[0] for p in loop[:-1]) / max(1, len(loop) - 1)
            cy = sum(p[1] for p in loop[:-1]) / max(1, len(loop) - 1)
            draw.text((cx - 22, cy - 6), "CASTLE", fill="#fff8ea", stroke_width=2, stroke_fill=color)

    styles = {
        "absorbed": ("#b07b48", 4),
        "standing": ("#252b31", 9),
        "planned": ("#4b6f91", 5),
    }
    for wall in data["walls"]:
        loop = [pt(p) for p in wall["boundary"]]
        loop.append(loop[0])
        color, width = styles[wall["lifecycle"]]
        if wall["lifecycle"] == "standing":
            draw.line(loop, fill=color, width=width, joint="curve")
        else:
            dashed(draw, loop, color, width, 13 if wall["lifecycle"] == "planned" else 8, 8)
        for tower in wall["towers"]:
            x, y = pt(tower)
            r = 3 if wall["lifecycle"] != "standing" else 5
            draw.ellipse((x-r, y-r, x+r, y+r), fill=color)
        for gate in wall["gates"]:
            x, y = pt(gate)
            draw.ellipse((x-7, y-7, x+7, y+7), fill="#efb83f", outline="#382b18", width=2)

    draw.text((38, 24), "C5 — city wall and growth allocation", fill="#211f1b")
    draw.text((38, 50), "one city curtain + castle precinct + sparse gate suburbs", fill="#514b42")
    legend = [("#252b31", "main city wall"), ("#dca178", "suburb land"), ("#9b5a55", "castle / own enclosure in C9")]
    x, y = size[0] - 275, 24
    for color, label in legend:
        draw.rectangle((x, y, x + 20, y + 12), fill=color)
        draw.text((x + 28, y - 2), label, fill="#27231e")
        y += 21
    image.save(output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    render(json.loads(args.input.read_text()), args.output)
