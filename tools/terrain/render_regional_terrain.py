#!/usr/bin/env python3
"""Comparable metric terrain views and an unexaggerated elevation profile."""

import argparse
import json
from pathlib import Path
import tempfile
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from build_hydrology_terrain import render


def main(directory, previous):
    out = Path(directory)
    old = Path(previous)
    f = json.loads((out / "foundation.json").read_text())
    p = json.loads((out / "terrain-map.json").read_text())
    h = np.load(out / "heightmap-metres.npy")
    mask = np.load(out / "land-mask.npy")
    cell = p["heightmap"]["cellMetres"]
    extent = p["extent"]
    side = 1539
    scale = (side - 1) / extent
    im = Image.open(out / "terrain-after-erosion.png").convert("RGB")
    d = ImageDraw.Draw(im)
    font = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 22)
    selected = set(f["mountainRegions"])
    for region in f["planningRegions"]:
        if region["id"] not in selected:
            continue
        ring = [(x * scale, side - 1 - z * scale) for x, z in region["ring"]]
        d.line(ring + [ring[0]], fill="#c5ae77", width=2)
    flows = [r["discharge"] for r in p["rivers"]]
    threshold = float(np.quantile(flows, 0.65))
    for r in p["rivers"]:
        if r["discharge"] < threshold:
            continue
        pts = [(x * scale, side - 1 - z * scale) for x, z in r["points"]]
        d.line(
            pts, fill="#4d929f", width=max(2, round(r["width"] * scale)), joint="curve"
        )
    d.rectangle((0, 0, side, 68), fill="#192e35")
    d.text(
        (22, 18),
        "TERRAIN ANALYSIS | gold: mountain ownership | blue: drainage graph",
        fill="white",
        font=font,
    )
    im.save(out / "terrain-regions-drainage.png")
    oldh = np.load(old / "heightmap-metres.npy")
    oldmask = np.load(old / "land-mask.npy")
    with tempfile.TemporaryDirectory() as temp:
        path = Path(temp) / "old.png"
        render(
            oldh, oldmask, cell, path, f"PREVIOUS TERRAIN | summit {oldh.max():.0f} m"
        )
        left = (
            Image.open(path).convert("RGB").resize((900, 900), Image.Resampling.LANCZOS)
        )
    right = (
        Image.open(out / "terrain-after-erosion.png")
        .convert("RGB")
        .resize((900, 900), Image.Resampling.LANCZOS)
    )
    comparison = Image.new("RGB", (1800, 1210), "#192e35")
    comparison.paste(left, (0, 0))
    comparison.paste(right, (900, 0))
    d = ImageDraw.Draw(comparison)
    big = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 27)
    region = p["regionalRelief"]
    d.text(
        (35, 925),
        f'Same 6 km island, colour scale and lighting | mountain core {region["mountainCoreLandFraction"]:.0%} | lowlands {region["lowlandFraction"]:.0%}',
        font=big,
        fill="white",
    )
    # 200 pixels per kilometre horizontally AND vertically: no vertical exaggeration.
    baseline = 1160
    x0 = 290
    px_per_m = 0.2
    row = round(3500 / cell)
    d.text(
        (35, 985),
        "West-east profile at z = 3.5 km | vertical exaggeration 1x",
        font=font,
        fill="white",
    )
    d.line((x0, baseline, x0 + extent * px_per_m, baseline), fill="#6c8790", width=2)
    for profile, color in ((oldh[row], "#b68a73"), (h[row], "#d5dca8")):
        d.line(
            [
                (x0 + i * cell * px_per_m, baseline - v * px_per_m)
                for i, v in enumerate(profile)
            ],
            fill=color,
            width=3,
        )
    d.text((35, 1120), "Previous", font=font, fill="#b68a73")
    d.text((35, 1150), "Regional", font=font, fill="#d5dca8")
    d.text((x0, 1175), "0 km", font=font, fill="white")
    d.text((x0 + extent * px_per_m - 45, 1175), "6 km", font=font, fill="white")
    comparison.save(out / "terrain-scale-comparison.png")
    print("PASS comparable terrain views and 1x metric profile")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("directory")
    parser.add_argument("previous")
    args = parser.parse_args()
    main(args.directory, args.previous)
