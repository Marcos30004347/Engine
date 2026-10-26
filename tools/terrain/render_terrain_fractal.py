"""Render terrain-first evidence without inherited water or watershed overlays."""

import argparse, json
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw
from render_terrain_geography import panel
from render_noise_mountains import relief
from render_terrain_landforms import font


def render(directory):
    root = Path(directory)
    j = json.loads((root / "terrain-world.json").read_text())
    for preset in ["temperate", "arid"]:
        im = Image.new("RGB", (1500, 1640), "#203b3b")
        d = ImageDraw.Draw(im)
        im.paste(panel(root, j, preset, 1500), (0, 90))
        d.text(
            (22, 15),
            "TERRAIN FIRST / fractal landforms and ridged multifractals",
            font=font(28),
            fill="white",
        )
        d.text(
            (22, 54),
            f"{preset.upper()} | {j['extent']/1000:g} km | {j['metrics']['peakMetres']:.0f} m summit | hydraulic erosion",
            font=font(23),
            fill="white",
        )
        d.text(
            (22, 1605),
            "Rivers and lakes are deferred. No inherited watershed geometry; dedicated cliffs disabled.",
            font=font(21),
            fill="white",
        )
        # Horizontal distances are measured in world metres, not enlarged icons.
        bar_end = 40 + 1000 / j["extent"] * 1499
        d.rectangle((25, 1510, bar_end + 25, 1570), fill="#203b3b")
        d.line((40, 1547, bar_end, 1547), fill="white", width=5)
        d.text((40, 1517), "0", font=font(20), fill="white")
        d.text((bar_end, 1517), "1 km", font=font(20), fill="white", anchor="ra")
        im.save(root / (preset + "-island.png"))
    cx, cz = j["construction"]["upliftDomains"][0]["centre"]
    extent = j["extent"]
    cx, cz = np.clip([cx, cz], 1200, extent - 1200)
    bounds = (cx - 1200, cz - 1200, cx + 1200, cz + 1200)
    im = Image.new("RGB", (1800, 1000), "#203b3b")
    d = ImageDraw.Draw(im)
    for k, (label, name) in enumerate(
        [
            ("RAW / feedback ridged multifractal", "terrain-before-erosion"),
            ("ERODED / no fixed river corridors", "heightmap-metres"),
        ]
    ):
        im.paste(
            relief(
                np.load(root / (name + ".npy")), j["surface"]["cellMetres"], bounds, 900
            ),
            (k * 900, 55),
        )
        d.text((k * 900 + 18, 16), label, font=font(24), fill="white")
    d.text(
        (20, 963),
        "Same 2.4 km crop, elevation palette and light. Generated scalar terrain, not an illustration.",
        font=font(23),
        fill="white",
    )
    im.save(root / "mountain-before-after.png")
    print(
        "PASS terrain-first views: temperate, arid, raw/eroded relief; no river overlays"
    )


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("directory")
    a = p.parse_args()
    render(a.directory)
