#!/usr/bin/env python3
"""Whole-island 2D overview, ownership view and city/countryside close-up."""

import argparse, json
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from shapely.geometry import shape, Point
from render_island_map import render


def main(root):
    root = Path(root)
    m = json.loads((root / "island-map.json").read_text())
    extent = m["extent"]
    side = 2100
    count = sum(s["kind"] == "city" for s in m["settlements"])
    villages = len(m["settlements"]) - count
    render(
        {},
        m,
        (0, 0, extent, extent),
        (side, side),
        "FULL ISLAND / integrated 2D preview",
        f'{extent/1000:g} km across | {count} cities | {villages} villages | {len(m["fields"])} fields | terrain + drainage + roads',
        root / "island-overview.png",
    )
    image = Image.open(root / "island-overview.png").convert("RGB")
    draw = ImageDraw.Draw(image)
    font = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 24)
    raster = m["terrainRaster"]
    h = np.array(raster["height"]).reshape(raster["side"], raster["side"])
    z, x = np.unravel_index(np.argmax(h), h.shape)
    px = x * raster["cellMetres"] / extent * side
    py = side - z * raster["cellMetres"] / extent * side
    draw.polygon(
        [(px, py - 7), (px - 6, py + 5), (px + 6, py + 5)],
        fill="#eee5c4",
        outline="#5d604c",
    )
    draw.text(
        (px + 12, py - 18),
        f"High ridge  {h.max():.0f} m",
        font=font,
        fill="#fff7df",
        stroke_width=2,
        stroke_fill="#4c5948",
    )
    draw.rectangle((25, side - 204, 660, side - 62), fill="#213e42")
    draw.text(
        (44, side - 188),
        "2D PREVIEW — generated from one terrain revision",
        font=font,
        fill="#ede7d2",
    )
    draw.text(
        (44, side - 147),
        "Mountains · woodland · fields · connected settlements",
        font=font,
        fill="#c5d3bf",
    )
    draw.text(
        (44, side - 106),
        "Detailed water/crossing validation still pending",
        font=font,
        fill="#c5d3bf",
    )
    image.save(root / "island-overview.png")
    regions = image.copy()
    d = ImageDraw.Draw(regions)
    for r in m["planningRegions"]:
        for p in [shape(r["geometry"])]:
            points = [
                (x / extent * side, side - z / extent * side)
                for x, z in p.exterior.coords
            ]
            d.line(
                points,
                fill="#e2cea3" if r["landform"] == "mountain" else "#a7aa84",
                width=2,
            )
            q = p.representative_point()
            label = (
                "M"
                if r["landform"] == "mountain"
                else "F" if r["landform"] == "foothill" else "L"
            )
            d.text(
                (q.x / extent * side, side - q.y / extent * side),
                label,
                font=font,
                fill="#eee6c9",
                stroke_width=2,
                stroke_fill="#4c5948",
            )
    d.rectangle((0, 0, side, 82), fill="#213e42")
    d.text(
        (25, 20),
        "REGIONAL OWNERSHIP / M mountains · F foothills · L lowlands",
        font=font,
        fill="#eee6c9",
    )
    regions.save(root / "island-regions.png")
    city = m["settlements"][0]
    x, z = city["position"]
    span = 650
    render(
        {},
        m,
        (x - span, z - span, x + span, z + span),
        (1800, 1800),
        "CITY AND COUNTRYSIDE / integrated 2D preview",
        "Same island, terrain revision and parcel scale",
        root / "city-countryside.png",
    )
    print("PASS full-island overview, ownership map and city/countryside view")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("directory")
    args = parser.parse_args()
    main(args.directory)
