#!/usr/bin/env python3
"""Expose high-level Voronoi ownership and downstream water connectivity in 2D."""

import json, math, sys
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from shapely.geometry import shape, Point
from compose_island_map import polys, lines

root = Path(sys.argv[1])
m = json.loads((root / "island-map.json").read_text())
source = json.loads((root / "landscape-foundation.json").read_text())
size = 1800
scale = 1600 / m["extent"]
font = "/System/Library/Fonts/Helvetica.ttc"
heading = ImageFont.truetype(font, 30)
small = ImageFont.truetype(font, 18)


def p(q):
    return (100 + q[0] * scale, 1700 - q[1] * scale)


def stroke(d, g, c, width):
    for l in lines(g):
        d.line([p(v) for v in l.coords], fill=c, width=width, joint="curve")


for mode in ["regions", "drainage"]:
    im = Image.new("RGB", (size, size), "#244750")
    d = ImageDraw.Draw(im)
    for region in m["planningRegions"]:
        g = shape(region["geometry"])
        rng = np.random.default_rng(int(region["id"].split("-")[-1]))
        colour = (
            {
                "alpine": "#b7b3a6",
                "montane-woodland": "#657754",
                "temperate-woodland": "#7d986b",
                "temperate-grassland": "#b8be85",
            }.get(region.get("biome"), "#9aa879")
            if mode == "regions"
            else "#8f9f77"
        )
        for poly in polys(g):
            d.polygon([p(v) for v in poly.exterior.coords], fill=colour)
        if mode == "regions":
            stroke(d, g.boundary, "#405d51", 2)
            q = p(g.representative_point().coords[0])
            d.text(
                q,
                region["id"].split("-")[-1]
                + " / "
                + {"mountain": "M", "foothill": "F", "lowland": "L"}.get(
                    region.get("landform"), ""
                ),
                font=small,
                fill="#263f3a",
            )
    if mode == "regions":
        for road in m["roads"]:
            stroke(d, shape(road["geometry"]), "#f3dfad", 3)
    else:
        for r in m["hydrology"]["reaches"]:
            curve = shape(r["geometry"])
            stroke(
                d,
                curve,
                "#356c83" if r.get("mainChannel") else "#7e9985",
                max(1, round(r["width"] * scale)),
            )
            if r.get("mainChannel", False):
                a = np.array(p(curve.interpolate(0.49, normalized=True).coords[0]))
                b = np.array(p(curve.interpolate(0.51, normalized=True).coords[0]))
                u = (b - a) / max(1e-9, np.linalg.norm(b - a))
                v = np.array([-u[1], u[0]])
                d.polygon(
                    [
                        tuple(b + u * 6),
                        tuple(b - u * 5 + v * 3),
                        tuple(b - u * 5 - v * 3),
                    ],
                    fill="#edf0d7",
                )
        for index, node_id in enumerate(
            m.get("landscape", {}).get("headwaterNodes", [])
        ):
            x, y = p(source["nodes"][node_id - 1]["position"][::2])
            d.ellipse(
                (x - 5, y - 5, x + 5, y + 5), fill="#c1eced", outline="#244750", width=2
            )
            d.text((x + 7, y - 9), f"S{index+1}", font=small, fill="#244750")
        for lake in m["hydrology"]["lakes"]:
            g = shape(lake["geometry"])
            for poly in polys(g):
                d.polygon([p(v) for v in poly.exterior.coords], fill="#79b2c3")
            x, y = p(shape(lake["outlet"]).coords[0])
            d.ellipse((x - 5, y - 5, x + 5, y + 5), fill="#f1c96c")
            d.text(
                (x + 10, y),
                lake["id"],
                font=small,
                fill="#f4e8c0",
                stroke_width=1,
                stroke_fill="#244750",
            )
        for i, node in enumerate(source["nodes"]):
            if node["receiver"] >= len(source["nodes"]):
                x, y = p(node["position"][::2])
                d.ellipse((x - 8, y - 8, x + 8, y + 8), fill="#f1c96c")
                d.text(
                    (x + 12, y),
                    "OCEAN OUTLET",
                    font=small,
                    fill="#f2e6c6",
                    stroke_width=2,
                    stroke_fill="#244750",
                )
    for s in m["settlements"]:
        x, y = p(s["position"])
        d.ellipse((x - 6, y - 6, x + 6, y + 6), fill="#efe1af")
    d.rectangle((0, 0, size, 100), fill="#213e42")
    d.text(
        (25, 18),
        (
            "VORONOI / high-level regions"
            if mode == "regions"
            else "DRAINAGE / follow the arrows to the ocean"
        ),
        font=heading,
        fill="#f2ead2",
    )
    d.text(
        (25, 60),
        (
            "Biomes: grey alpine · dark montane forest · green woodland · pale grassland"
            if mode == "regions"
            else "S: mountain springs · blue: mapped rivers · pale: minor drainage · gold: outlets"
        ),
        font=small,
        fill="#d0ddc9",
    )
    d.rectangle((0, size - 48, size, size), fill="#213e42")
    d.text(
        (25, size - 35),
        "Voronoi ownership and river graph • heightmap exported separately • no mesh",
        font=small,
        fill="#d0ddc9",
    )
    im.save(root / f"island-{mode}.png")
print("PASS Voronoi and directed-drainage views")
