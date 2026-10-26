#!/usr/bin/env python3
"""Cartographic 2D views of generated island map products. No meshes or AI images."""

import argparse, json, math
from pathlib import Path
import numpy as np
import shapely as sh
from shapely.geometry import shape, Point, LineString
from PIL import Image, ImageDraw, ImageFont
from scipy.ndimage import map_coordinates, gaussian_filter
from compose_island_map import Island, polys, lines
from island_heightmap import sample_height, relief_rgb, export_heightmap

PALETTE = ["#a8ae59", "#919b54", "#b7af6b", "#c3b775", "#829750", "#9da769"]


def render(foundation, product, bounds, size, title, subtitle, out):
    x0, z0, x1, z1 = bounds
    w, h = size
    scale = w / (x1 - x0)
    # A flat map canvas. Scalar terrain, search lattices and meshing have no role
    # in presentation; every visible region below comes from polygon products.
    im = Image.new("RGB", (w, h), "#486c74")
    draw = ImageDraw.Draw(im)

    def p(xy):
        return ((xy[0] - x0) * scale, (z1 - xy[1]) * scale)

    def poly(g, fill, outline=None, width=1):
        for q in polys(g):
            draw.polygon([p(v) for v in q.exterior.coords], fill=fill)
            if outline:
                draw.line(
                    [p(v) for v in q.exterior.coords],
                    fill=outline,
                    width=width,
                    joint="curve",
                )
            # Most owned crop/building rings have no holes. Preserve holes by
            # restoring the underlying relief image when rendering a ring product.
            for ring in q.interiors:
                mask = Image.new("L", (w, h))
                md = ImageDraw.Draw(mask)
                md.polygon([p(v) for v in ring.coords], fill=255)
                im.paste(background, (0, 0), mask)

    def line(g, colour, width):
        for q in lines(g):
            if len(q.coords) > 1:
                draw.line(
                    [p(v) for v in q.coords],
                    fill=colour,
                    width=max(1, round(width * scale)),
                    joint="curve",
                )

    background = im.copy()
    poly(shape(product["land"]), "#9aa879")
    if "terrainSites" in product:
        height, mask = sample_height(product, bounds, size)
        im = Image.fromarray(relief_rgb(height, mask, (x1 - x0) / (w - 1)))
        draw = ImageDraw.Draw(im)
    background = im.copy()
    for rock in product.get("landscape", {}).get("rockFormations", []):
        poly(shape(rock["geometry"]), "#aaa594", "#6c7566")
        line(shape(rock["spine"]), "#d3cbbb", 5)
    # Riparian trees and wooded steep land remain connected landscape features.
    permanent = [
        r
        for r in product["hydrology"]["reaches"]
        if r.get(
            "mainChannel",
            r["discharge"]
            >= (product["hydrology"].get("mainChannelThresholdCumecs") or 0.08),
        )
    ]
    river = sh.union_all(
        [shape(r["geometry"]).buffer(r["width"] / 2) for r in permanent]
    )
    riparian = sh.union_all(
        [
            shape(r["geometry"]).buffer(min(12, 2 + r["discharge"] * 4))
            for r in permanent
        ]
    )
    poly(riparian, "#526e4f")
    for woodland in product.get("woodlands", []):
        if "terrainRaster" in product:
            # Preserve relief through woodland tint rather than hiding it under
            # a flat opaque silhouette at whole-island scale.
            mask = Image.new("L", (w, h))
            md = ImageDraw.Draw(mask)
            for patch in polys(shape(woodland)):
                md.polygon([p(v) for v in patch.exterior.coords], fill=170)
                for ring in patch.interiors:
                    md.polygon([p(v) for v in ring.coords], fill=0)
            im.paste(Image.new("RGB", (w, h), "#526f49"), (0, 0), mask)
        else:
            poly(shape(woodland), "#526f49")
    if product.get("deliveryStatus") != "2d-preview":
        for estate in product["estates"]:
            poly(shape(estate["geometry"]), "#72854f")
    for field in product["fields"]:
        g = shape(field["geometry"])
        poly(g, PALETTE[field["crop"]])
        if scale > 0.35:
            # Crop rows follow the field's local working direction and are clipped.
            a = field["grain"]
            u = np.array([math.cos(a), math.sin(a)])
            v = np.array([-u[1], u[0]])
            c = np.array(g.centroid.coords[0])
            r = math.hypot(g.bounds[2] - g.bounds[0], g.bounds[3] - g.bounds[1])
            for d in np.arange(-r, r, 14):
                row = LineString([c + d * v - r * u, c + d * v + r * u]).intersection(
                    g.buffer(-4)
                )
                line(row, "#a3a463" if field["crop"] % 2 else "#b4b574", 0.6)
    for e in product["fieldEdges"]:
        line(shape(e["geometry"]), "#48623e", 3.2)
    # Hedge crowns are spaced with edge-derived randomness, stable across views.
    for e in product["fieldEdges"]:
        g = shape(e["geometry"])
        erng = np.random.default_rng(int(e["id"][5:13], 16))
        for distance in np.arange(erng.uniform(0, 19), g.length, 19):
            q = g.interpolate(min(g.length, max(0, distance + erng.uniform(-6, 6))))
            r = erng.uniform(2.8, 5.5) * scale
            x, y = p(q.coords[0])
            if -10 < x < w + 10 and -10 < y < h + 10:
                draw.ellipse(
                    (x - r + 1, y - r + 1, x + r + 1, y + r + 1), fill="#506447"
                )
                draw.ellipse((x - r, y - r, x + r, y + r), fill="#637749")
    for track in product.get("farmAccess", {}).get("tracks", []):
        route = shape(track["geometry"])
        line(route, "#546448", track["widthMetres"] + 2)
        line(route, "#c5b78f", track["widthMetres"])
    for gate in product.get("farmAccess", {}).get("gates", []):
        x, y = p(shape(gate["position"]).coords[0])
        r = max(1.5, 2 * scale)
        draw.ellipse((x - r, y - r, x + r, y + r), fill="#efe0b7")
    river = river.union(
        sh.union_all([shape(l["geometry"]) for l in product["hydrology"]["lakes"]])
    )
    poly(river, "#6099a5")
    for reach in permanent:
        line(shape(reach["geometry"]), "#89adb0", max(0.8, reach["width"] * 0.18))
    for road in product["roads"]:
        g = shape(road["geometry"])
        line(g, "#5e654d", road["width"] + 4)
        line(g, "#d0c29c", road["width"])
        line(shape(road["crossings"]), "#e5d8b9", road["width"] + 1)
    for city in product["cities"]:
        poly(shape(city["envelope"]), "#bdba93")
        poly(shape(city["streets"]), "#d9cba9")
        for block in city["blocks"]:
            poly(
                shape(block["geometry"]),
                "#b1b68a" if block["kind"] == "residential" else "#c8bf9b",
            )
        for building in city["buildings"]:
            g = shape(building)
            poly(g, "#836f59", "#625e4f", max(1, round(scale)))
        line(shape(city["walls"]), "#e1d7b9", 5)
    fontpath = "/System/Library/Fonts/Helvetica.ttc"
    heading = ImageFont.truetype(fontpath, 30)
    small = ImageFont.truetype(fontpath, 18)
    for i, s in enumerate(product["settlements"]):
        x, y = p(s["position"])
        label = ("Seat " if s["kind"] == "city" else "Village ") + str(i + 1)
        if 0 < x < w and 70 < y < h - 50:
            draw.text(
                (x + 8, y + 8),
                label,
                font=small,
                fill="#f3efda",
                stroke_width=2,
                stroke_fill="#48574b",
            )
    draw.rectangle((0, 0, w, 82), fill="#213e42")
    draw.text((24, 13), title, font=heading, fill="#f3eed7")
    draw.text((24, 50), subtitle, font=small, fill="#c3d2c6")
    draw.rectangle((0, h - 40, w, h), fill="#213e42")
    draw.text(
        (24, h - 29),
        "Generated 2D map • physical coast / drainage / shared parcel edges • no meshing",
        font=small,
        fill="#d2ddce",
    )
    metres = 1000 if x1 - x0 > 4000 else 250
    draw.line(
        (w - 45 - metres * scale, h - 65, w - 45, h - 65), fill="#f4eed8", width=4
    )
    draw.text(
        (w - 45 - metres * scale, h - 90),
        f"{metres:g} m",
        font=small,
        fill="#f4eed8",
        stroke_width=1,
        stroke_fill="#34504a",
    )
    im.save(out)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("directory", type=Path)
    args = p.parse_args()
    d = args.directory
    f = json.loads((d / "foundation.json").read_text())
    m = json.loads((d / "island-map.json").read_text())
    e = m["extent"]
    export_heightmap(m, d)
    render(
        f,
        m,
        (0, 0, e, e),
        (1800, 1800),
        "THE ISLAND / 2D generation study",
        f"{len(m['settlements'])} settlements · {len(m['fields'])} fields · {len(m['hydrology']['reaches'])} drainage reaches",
        d / "island-overview.png",
    )
    # Choose a real village with the greatest amount of generated farmland nearby.
    village = max(
        m["settlements"][2:],
        key=lambda s: sum(
            shape(p["geometry"]).intersection(Point(s["position"]).buffer(950)).area
            for p in m["fields"]
        ),
    )
    x, z = village["position"]
    span = 1000
    render(
        f,
        m,
        (x - span, z - span, x + span, z + span),
        (1800, 1800),
        "COUNTRYSIDE / enclosed fields and lanes",
        "Shared bent boundaries · regional working grain · villages within their farmland",
        d / "countryside-detail.png",
    )
    x, z = m["settlements"][0]["position"]
    span = 390
    render(
        f,
        m,
        (x - span, z - span, x + span, z + span),
        (1600, 1600),
        "CITY / streets, blocks and courtyard regions",
        "2D footprint proposals · district strategies and WFC promotion remain separate",
        d / "city-detail.png",
    )
    print("PASS three data-derived 2D views")


if __name__ == "__main__":
    main()
