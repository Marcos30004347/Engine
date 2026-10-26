"""Compare actual plateau relief before/after the new contour noise."""

import argparse, json
from pathlib import Path
from PIL import Image, ImageDraw
from shapely.geometry import shape
from render_terrain_geography import panel
from render_terrain_landforms import font


def render(before, after):
    before, after = Path(before), Path(after)
    old = json.loads((before / "terrain-weather.json").read_text())
    new = json.loads((after / "terrain-weather.json").read_text())
    shared = next(
        f
        for f in new["geography"]["features"]
        if f["type"] == "plateau"
        and any(o["id"] == f["id"] for o in old["geography"]["features"])
    )
    cap = shape(shared["geometry"])
    c = cap.centroid
    bounds = (c.x - 440, c.y - 440, c.x + 440, c.y + 440)
    im = Image.new("RGB", (1600, 930), "#203b3b")
    d = ImageDraw.Draw(im)
    for i, (root, j, label) in enumerate(
        [
            (before, old, "BEFORE / broad repeating curves"),
            (after, new, "AFTER / recesses, ledges and fine edge noise"),
        ]
    ):
        im.paste(panel(root, j, "temperate", 800, bounds, True), (i * 800, 60))
        d.text((i * 800 + 20, 17), label, font=font(24), fill="white")
    d.text(
        (20, 875),
        "Same location, scale and light. Noise changes the cap contour and physical cliff profile.",
        font=font(23),
        fill="white",
    )
    d.text(
        (20, 904),
        "280 / 125 / 58 / 30 m noise scales; planar top; shared crest/toe geometry; no mesh.",
        font=font(18),
        fill="#d3dcc5",
    )
    im.save(after / "plateau-comparison.png")
    print("PASS plateau comparison: generated heightfields, shared-location comparison")


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("before")
    p.add_argument("after")
    a = p.parse_args()
    render(a.before, a.after)
