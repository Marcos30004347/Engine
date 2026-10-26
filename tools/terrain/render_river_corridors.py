"""Data-derived comparison of baseline and accepted river corridors."""

import sys, json
from pathlib import Path
from copy import deepcopy
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from shapely.geometry import shape
from island_heightmap import sample_height, relief_rgb

root = Path(sys.argv[1])
m = json.loads((root / "island-map.json").read_text())
r = max(
    (r for r in m["hydrology"]["reaches"] if r["mainChannel"]),
    key=lambda r: r["corridorRouting"]["baselineCost"]
    - r["corridorRouting"]["acceptedCost"],
)
x, z = shape(r["geometry"]).interpolate(0.5, normalized=True).coords[0]
span = 220
bounds = (x - span, z - span, x + span, z + span)
size = 800
height, mask = sample_height(m, bounds, (size, size))
rgb = relief_rgb(height, mask, 2 * span / (size - 1))
im = Image.new("RGB", (size * 2, 920), "#213e42")
font = "/System/Library/Fonts/Helvetica.ttc"
for panel in (0, 1):
    im.paste(Image.fromarray(rgb), (panel * size, 100))
    d = ImageDraw.Draw(im)

    def p(q):
        return (
            panel * size + (q[0] - bounds[0]) * size / (2 * span),
            100 + (bounds[3] - q[1]) * size / (2 * span),
        )

    for rr in m["hydrology"]["reaches"]:
        if not rr["mainChannel"]:
            continue
        g = shape(
            rr["corridorRouting"]["baselineGeometry"] if panel == 0 else rr["geometry"]
        )
        # Clip lines to the panel domain so one panel never paints over its neighbour.
        from shapely.geometry import box

        g = g.intersection(box(*bounds))
        for curve in getattr(g, "geoms", [g]):
            if curve.geom_type != "LineString" or curve.is_empty:
                continue
            d.line(
                [p(q) for q in curve.coords],
                fill="#e5bb7f" if panel == 0 else "#96dce5",
                width=5,
                joint="curve",
            )
    score = r["corridorRouting"]["baselineCost" if panel == 0 else "acceptedCost"]
    d.text(
        (panel * size + 24, 18),
        (
            "BASELINE / cubic offset candidates"
            if panel == 0
            else "ACCEPTED / terrain corridor search"
        ),
        font=ImageFont.truetype(font, 25),
        fill="#f3eed7",
    )
    d.text(
        (panel * size + 24, 56),
        f"{r['id']} | terrain-cost score {score:.1f}",
        font=ImageFont.truetype(font, 20),
        fill="#c9ddd2",
    )
d = ImageDraw.Draw(im)
d.text(
    (24, 901),
    "Same height field, river junctions and discharge. Search is bounded to 48 m; this is a 2D map comparison.",
    font=ImageFont.truetype(font, 16),
    fill="#d2ddce",
)
im.save(root / "river-corridor-comparison.png")
print("PASS before/after river corridor comparison")
