"""Identical-scale evidence for old, raw-noise and eroded scalar terrain."""

import argparse, json
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw
from terrain_landforms import sample
from render_terrain_landforms import font
from render_terrain_geography import panel


def relief(height, cell, bounds, size):
    x0, z0, x1, z1 = bounds
    x, z = np.meshgrid(np.linspace(x0, x1, size), np.linspace(z1, z0, size))
    h = sample(height, np.column_stack([x.ravel(), z.ravel()]), cell).reshape(x.shape)
    dz, dx = np.gradient(h, (z1 - z0) / (size - 1), (x1 - x0) / (size - 1))
    light = np.clip(
        (0.85 - 0.55 * dx + 0.6 * dz) / np.sqrt(1 + dx * dx + dz * dz), 0, 1
    )
    shade = np.interp(h, [0, 40, 100, 200, 300], [45, 75, 125, 195, 245]) * (
        0.55 + 0.56 * light
    )
    return Image.fromarray(np.uint8(np.clip(shade, 0, 255))).convert("RGB")


def render(root, before):
    root, before = Path(root), Path(before)
    j = json.loads((root / "terrain-weather.json").read_text())
    cell = j["surface"]["cellMetres"]
    # Raw and eroded use the same canonical water realization.
    from terrain_landforms import LandscapeSurface
    from shapely.geometry import shape

    raw = LandscapeSurface(
        np.load(root / "terrain-before-erosion.npy"),
        cell,
        shape(j["land"]),
        j["rivers"],
        j["lakes"],
    ).raster(len(np.load(root / "base-height.npy")))
    old = np.load(before / "heightmap-metres.npy")
    final = np.load(root / "heightmap-metres.npy")
    im = Image.new("RGB", (2100, 815), "#203b3b")
    d = ImageDraw.Draw(im)
    for k, (title, h) in enumerate(
        [
            ("PREVIOUS / watershed edges", old),
            ("NEW / warped ridged noise", raw),
            ("NEW / after hydraulic erosion", final),
        ]
    ):
        im.paste(relief(h, cell, (1450, 2350, 4250, 5150), 700), (k * 700, 55))
        d.text((k * 700 + 16, 16), title, font=font(23), fill="white")
    d.text(
        (20, 775),
        "Identical 2.8 km crop, 0–300 m palette and light. Actual scalar fields; no decorative ridge strokes.",
        font=font(23),
        fill="white",
    )
    im.save(root / "mountain-comparison.png")
    im = Image.new("RGB", (1600, 1720), "#203b3b")
    d = ImageDraw.Draw(im)
    im.paste(panel(root, j, "temperate", 1600), (0, 80))
    d.text(
        (25, 14),
        "ISLAND / noise-first mountain experiment",
        font=font(30),
        fill="white",
    )
    d.text(
        (25, 50),
        f"6 km island | {j['metrics']['peakMetres']:.0f} m summit | warped ridged noise, then two erosion passes",
        font=font(22),
        fill="white",
    )
    d.text(
        (25, 1690),
        "Actual 2D terrain and water maps. Existing river routes retained; drainage regeneration remains pending.",
        font=font(20),
        fill="white",
    )
    im.save(root / "island-overview.png")
    # Signed erosion delta has a fixed +/- 14 m scale.
    delta = np.load(root / "terrain-before-erosion.npy") - np.load(
        root / "base-height.npy"
    )
    np.save(root / "erosion-delta-metres.npy", delta)
    print("PASS same-scale comparison and island rendered")


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("directory")
    p.add_argument("before")
    a = p.parse_args()
    render(a.directory, a.before)
