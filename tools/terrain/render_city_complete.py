#!/usr/bin/env python3
import argparse
import json
import math
from pathlib import Path

from PIL import Image, ImageDraw


def xz(p):
    return float(p[0]), float(p[2])


def bounds(points):
    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    return min(xs), min(ys), max(xs), max(ys)


def mapper(box, source):
    x0, y0, x1, y1 = source
    bx0, by0, bx1, by1 = box
    scale = min((bx1 - bx0) / max(1, x1 - x0), (by1 - by0) / max(1, y1 - y0))
    ox = (bx0 + bx1 - (x1 - x0) * scale) * .5
    oy = (by0 + by1 - (y1 - y0) * scale) * .5
    return lambda p: (ox + (p[0] - x0) * scale, oy + (p[1] - y0) * scale)


def render(data, output):
    image = Image.new("RGB", (1800, 1200), "#f3efe6")
    draw = ImageDraw.Draw(image)
    world = []
    for group in ("blocks", "lots", "suburbs"):
        for item in data[group]:
            world.extend(xz(p) for p in item["boundary"])
    for road in data.get("routes", data["roads"]):
        world.extend(xz(p) for p in road["path"])
    for segment in data["wallSegments"]:
        world.extend((xz(segment["a"]), xz(segment["b"])))
    source = bounds(world)
    plan = mapper((40, 100, 880, 1130), source)

    for region in data.get("growthRegions", []):
        if region["kind"] == "absorbed":
            draw.polygon([plan(xz(p)) for p in region["boundary"]], fill="#e7d8bc")

    for block in data["blocks"]:
        draw.polygon([plan(xz(p)) for p in block["boundary"]], fill="#ded2bb", outline="#a99b83")
    for lot in data["lots"]:
        loop = [plan(xz(p)) for p in lot["boundary"]]
        if lot["developed"]:
            draw.polygon(loop, fill="#e8d4b2", outline="#a99474")
        else:
            draw.polygon(loop, fill="#cbd2ad", outline="#b4ba98")
    for region in data.get("growthRegions", []):
        if region["kind"] == "absorbed":
            loop = [plan(xz(p)) for p in region["boundary"]]
            draw.line(loop + [loop[0]], fill="#9b774e", width=3)
            cx = sum(p[0] for p in loop) / len(loop)
            cy = sum(p[1] for p in loop) / len(loop)
            draw.text((cx - 34, cy - 8), "OLD CORE", fill="#765638")
    for road in data.get("routes", data["roads"]):
        pts = [plan(xz(p)) for p in road["path"]]
        draw.line(pts, fill="#514a40", width=5)
        draw.line(pts, fill="#eee4cf", width=3)

    colours = {"absorbed": "#ad7544", "standing": "#242b31", "planned": "#4b7195"}
    for segment in data["wallSegments"]:
        a, b = plan(xz(segment["a"])), plan(xz(segment["b"]))
        colour = "#8e3f3b" if segment["sharedCastle"] else colours[segment["lifecycle"]]
        draw.line((a, b), fill=colour, width=7 if segment["built"] else 4)
    for gate in data["gatehouses"]:
        x, y = plan(xz(gate["position"]))
        draw.rectangle((x - 6, y - 6, x + 6, y + 6), fill="#eab33e", outline="#332617", width=2)
    for tower in data["towers"]:
        x, y = plan(xz(tower["position"]))
        r = 3 if not tower["built"] else 4
        draw.ellipse((x-r, y-r, x+r, y+r), fill=colours[tower["lifecycle"]], outline="#171717")
    for harbour in data["harbours"]:
        draw.line([plan(xz(p)) for p in harbour["quay"]], fill="#287d91", width=9)

    # Structural view: render the exact six-face prisms published by the
    # completion pass. Planned pieces remain a wireframe blueprint.
    iso_raw = []
    faces = []
    for segment in data["wallSegments"]:
        for face in segment["faces"]:
            pts = []
            for p in face["corners"]:
                q = (float(p[0]) - float(p[2]), (float(p[0]) + float(p[2])) * .42 - float(p[1]) * 2.4)
                pts.append(q)
                iso_raw.append(q)
            depth = sum(float(p[0]) + float(p[2]) + float(p[1]) * .1 for p in face["corners"]) / 4
            faces.append((depth, segment, face, pts))
    iso = mapper((940, 125, 1760, 1080), bounds(iso_raw))
    face_shade = {"outside": .9, "inside": .72, "up": 1.18, "bottom": .55, "left": .78, "right": .65}

    def shade(hex_colour, factor):
        rgb = tuple(int(hex_colour[i:i + 2], 16) for i in (1, 3, 5))
        return tuple(max(0, min(255, int(c * factor))) for c in rgb)

    for _, segment, face, pts in sorted(faces, key=lambda x: x[0]):
        colour = "#8e3f3b" if segment["sharedCastle"] else colours[segment["lifecycle"]]
        mapped = [iso(p) for p in pts]
        if segment["built"]:
            draw.polygon(mapped, fill=shade(colour, face_shade[face["kind"]]), outline="#171717")
        else:
            draw.line(mapped + [mapped[0]], fill=colour, width=2)
    for gate in data["gatehouses"]:
        p = gate["position"]
        q = iso((float(p[0]) - float(p[2]), (float(p[0]) + float(p[2])) * .42 - gate["height"] * 2.4))
        r = 6 if gate["built"] else 4
        draw.ellipse((q[0]-r, q[1]-r, q[0]+r, q[1]+r), fill="#eab33e", outline="#332617")
    for tower in data["towers"]:
        p = tower["position"]
        q = iso((float(p[0]) - float(p[2]), (float(p[0]) + float(p[2])) * .42 - tower["height"] * 2.4))
        r = 3 if not tower["built"] else 5
        draw.ellipse((q[0]-r, q[1]-r, q[0]+r, q[1]+r), fill=colours[tower["lifecycle"]], outline="#171717")

    draw.text((40, 30), "C9 — completed city topology", fill="#1f1d19")
    draw.text((40, 55), "blocks + lots + road-linked suburbs + exact gate contacts", fill="#514a40")
    draw.text((940, 30), "simple structural wall realization", fill="#1f1d19")
    draw.text((940, 55), "main curtain + independent castle enclosure; six stored faces per segment", fill="#514a40")
    graph = data["graph"]
    draw.text((940, 82), f"wall graph {graph['wallNodes']} nodes / {graph['wallEdges']} edges   road graph {graph['roadNodes']} / {graph['roadEdges']}   gates {graph['gates']}", fill="#514a40")
    image.save(output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    render(json.loads(args.input.read_text()), args.output)
