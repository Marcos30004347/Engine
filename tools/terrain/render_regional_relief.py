"""Actual island, desert and crest-line evidence for the regional relief pass."""

import argparse, json
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw
from render_terrain_geography import panel
from render_noise_mountains import relief
from render_terrain_landforms import font


def render(directory):
    root = Path(directory)
    j = json.loads((root / "terrain-weather.json").read_text())
    cell = j["surface"]["cellMetres"]
    for preset in ["temperate", "arid"]:
        im = Image.new("RGB", (1500, 1640), "#203b3b")
        d = ImageDraw.Draw(im)
        im.paste(panel(root, j, preset, 1500), (0, 95))
        d.text(
            (22, 14),
            "ISLAND / branching ranges, separate massifs, regional erosion",
            font=font(28),
            fill="white",
        )
        d.text(
            (22, 55),
            preset.upper()
            + (
                " climate | 6 km | wind-aligned dune height detail in dry sand regions"
                if preset == "arid"
                else (
                    " climate | 6 km | eroded hills and plateaus; dedicated cliffs disabled"
                    if not j.get("cliffsEnabled", True)
                    else " climate | 6 km | eroded hills, plateau rims and coastal cliffs"
                )
            ),
            font=font(23),
            fill="white",
        )
        d.text(
            (22, 1605),
            "Actual exported 2D terrain. Existing river graph retained; no meshing or settlements.",
            font=font(21),
            fill="white",
        )
        im.save(root / (preset + "-island.png"))
    im = Image.new("RGB", (1800, 1000), "#203b3b")
    d = ImageDraw.Draw(im)
    for k, (title, name) in enumerate(
        [
            ("STRAIGHT WATERSHED CRESTS", "watershed-straight"),
            ("SHARED CREST BENDS + HEIGHT NOTCHES", "watershed-noisy-crests"),
        ]
    ):
        im.paste(
            relief(
                np.load(root / (name + ".npy")), cell, (1850, 2900, 3200, 4250), 900
            ),
            (900 * k, 55),
        )
        d.text((900 * k + 18, 17), title, font=font(23), fill="white")
    d.text(
        (18, 965),
        "Same scalar construction and light; only crest polylines differ. Regional noise mountains use this in their blend collar.",
        font=font(19),
        fill="white",
    )
    im.save(root / "crest-line-comparison.png")
    # Select the strongest actual dune patch; disclose how the crop was chosen.
    dunes = np.load(root / "arid/dune-height.npy")
    iz, ix = np.unravel_index(dunes.argmax(), dunes.shape)
    cx, cz = np.clip([ix * cell, iz * cell], 550, j["extent"] - 550)
    im = Image.new("RGB", (1100, 1190), "#203b3b")
    d = ImageDraw.Draw(im)
    im.paste(
        panel(root, j, "arid", 1100, (cx - 550, cz - 550, cx + 550, cz + 550)), (0, 55)
    )
    d.text(
        (20, 16),
        "DESERT / warped wind-aligned dune height",
        font=font(24),
        fill="white",
    )
    d.text(
        (20, 1160),
        "1.1 km crop centred on maximum exported dune height; no painted dune lines.",
        font=font(20),
        fill="white",
    )
    im.save(root / "desert-detail.png")
    # Landform weathering: show actual signed terrain change separately from cover.
    pre = np.load(root / "terrain-before-erosion.npy")
    after = np.load(root / "base-height.npy")
    im = Image.new("RGB", (1600, 910), "#203b3b")
    d = ImageDraw.Draw(im)
    plateau = next(f for f in j["geography"]["features"] if f["type"] == "plateau")
    cx, cz = plateau["centre"]
    bounds = (cx - 450, cz - 450, cx + 450, cz + 450)
    for k, (label, h) in enumerate(
        [("PLATEAU / before weathering", pre), ("PLATEAU / after weathering", after)]
    ):
        im.paste(relief(h, cell, bounds, 800), (800 * k, 60))
        d.text((800 * k + 20, 18), label, font=font(24), fill="white")
    d.text(
        (20, 876),
        "Same height palette and light; cap erosion is gentle, rim/talus erosion stronger.",
        font=font(23),
        fill="white",
    )
    im.save(root / "plateau-weathering.png")
    print(
        "PASS rendered temperate/desert islands, crest comparison, plateau weathering and dune detail"
    )


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("directory")
    a = p.parse_args()
    render(a.directory)
