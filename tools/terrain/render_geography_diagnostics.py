"""Scalar elevation/contour inspection and provisional political region atlas."""

import argparse, json
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw
import contourpy
import shapely as sh
from shapely.geometry import shape
from render_map_geography import panel
from render_terrain_landforms import font


def render(root):
    root = Path(root)
    p = json.loads((root / "terrain-world.json").read_text())
    h = np.load(root / "heightmap-metres.npy")
    extent = p["extent"]
    size = 1600
    a = np.flipud(np.clip(h / h.max() * 255, 0, 255)).astype("uint8")
    im = Image.new("RGB", (size, size + 65), "#203b3b")
    im.paste(Image.fromarray(a).convert("RGB").resize((size, size)), (0, 65))
    ImageDraw.Draw(im).text(
        (15, 15),
        f"HEIGHTMAP / black 0 m — white {h.max():.0f} m / no snow or biome colours",
        font=font(25),
        fill="white",
    )
    im.save(root / "heightmap.png")
    im = Image.new("RGB", (size, size + 65), "#203b3b")
    im.paste(panel(root, p, size, mode="relief"), (0, 65))
    d = ImageDraw.Draw(im)
    grid = np.linspace(0, extent, len(h))
    cg = contourpy.contour_generator(x=grid, y=grid, z=h)
    for level in np.arange(100, h.max(), 100):
        for line in cg.lines(level):
            pts = [
                (x / extent * (size - 1), 65 + (1 - z / extent) * (size - 1))
                for x, z in line
            ]
            d.line(pts, fill="#615d4e", width=1)
    d.text(
        (15, 15),
        "RELIEF / 100 m contours / scalar terrain and rivers / no snow",
        font=font(25),
        fill="white",
    )
    im.save(root / "heightmap-contours.png")
    director = json.loads((root / "kingdom-regions.json").read_text())
    im = panel(root, p, size).convert("RGBA")
    overlay = Image.new("RGBA", im.size)
    d = ImageDraw.Draw(overlay)
    colors = ["#9b8fd3", "#55bddd", "#d87974", "#65bc96", "#d59c55"]
    for s in director["societies"]:
        col = dict(mountain="#9b8fd3",seafaring="#55bddd",desert="#d87974",forest="#65bc96",agrarian="#d59c55")[s["profile"]]
        for poly in sh.get_parts(shape(s["geometry"])):
            pts = [
                (x / extent * (size - 1), (1 - z / extent) * (size - 1))
                for x, z in poly.exterior.coords
            ]
            d.polygon(pts, fill=col + "55")
            d.line(pts, fill=col, width=3)
            for ring in poly.interiors:
                hole = [
                    (x / extent * (size - 1), (1 - z / extent) * (size - 1))
                    for x, z in ring.coords
                ]
                d.polygon(hole, fill=(0, 0, 0, 0))
        x, z = s["point"]
        x = x / extent * (size - 1)
        y = (1 - z / extent) * (size - 1)
        d.ellipse((x - 7, y - 7, x + 7, y + 7), fill="white")
        d.text(
            (x + 10, y), s["profile"] + " / seat candidate", font=font(19), fill="white"
        )
    result = Image.new("RGB", (size, size + 100), "#203b3b")
    result.paste(Image.alpha_composite(im, overlay).convert("RGB"), (0, 65))
    d = ImageDraw.Draw(result)
    d.text(
        (15, 15),
        "POLITICAL REGIONS / every land cell assigned / provisional seats",
        font=font(23),
        fill="white",
    )
    d.text(
        (15, size + 72),
        "Candidate seats only. Supply, roads and settlement footprints are not solved yet.",
        font=font(20),
        fill="white",
    )
    result.save(root / "kingdom-regions.png")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("directory")
    render(ap.parse_args().directory)
