"""Render actual coast polygons for several seeds, independent of elevation colour."""

import argparse, json
from pathlib import Path
import shapely as sh
from shapely.geometry import mapping, shape
from PIL import Image, ImageDraw
from terrain_natural_coast import natural_coast
from render_terrain_landforms import font


def draw_land(d, land, left, top, size, extent):
    d.rectangle((left, top, left + size, top + size), fill="#295e73")
    for part in sh.get_parts(land):
        d.polygon(
            [
                (left + p[0] / extent * size, top + (1 - p[1] / extent) * size)
                for p in part.exterior.coords
            ],
            fill="#95b777",
        )


def render(directory):
    root = Path(directory)
    im = Image.new("RGB", (1800, 1330), "#203b3b")
    d = ImageDraw.Draw(im)
    results = []
    for i, seed in enumerate([7, 27, 51, 13, 39, 81]):
        land, meta = natural_coast(seed, 72000)
        x, y = (i % 3) * 600, (i // 3) * 650
        d.text(
            (x + 15, y + 12),
            f"SEED {seed} / {meta['islandCount']} islands",
            font=font(23),
            fill="white",
        )
        draw_land(d, land, x, y + 45, 600, 72000)
        results.append(dict(seed=seed, land=mapping(land), coastline=meta))
    d.text(
        (20, 1300),
        "Same 72 km extent and land budget. Island count, spacing, straits and bays emerge from the seed.",
        font=font(24),
        fill="white",
    )
    im.save(root / "coast-seed-study.png")
    (root / "coast-seed-study.json").write_text(json.dumps(results) + "\n")
    old = json.loads(
        (
            root.parent / "2026-09-12-archipelago-edges-v22/elevation-map.json"
        ).read_text()
    )
    new = json.loads((root / "elevation-map.json").read_text())
    im = Image.new("RGB", (1800, 1020), "#203b3b")
    d = ImageDraw.Draw(im)
    for i, (p, title) in enumerate(
        [
            (old, "PREVIOUS / fixed islands and three carved bays"),
            (new, "CURRENT / emergent land, islands and straits"),
        ]
    ):
        d.text((i * 900 + 20, 20), title, font=font(26), fill="white")
        draw_land(d, shape(p["land"]), i * 900, 65, 900, p["extent"])
    d.text(
        (20, 984),
        "Same seed 27 and map scale. Coast shapes are generated before Voronoi regions.",
        font=font(24),
        fill="white",
    )
    im.save(root / "coast-selection-comparison.png")
    print("PASS six seeded vector coast previews and same-scale coastline comparison")


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("directory")
    a = p.parse_args()
    render(a.directory)
