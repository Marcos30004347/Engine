"""Inspect real v9 height and cover products in top-down 2D views."""

import argparse, json
from pathlib import Path
import numpy as np
import shapely as sh
from shapely.geometry import shape, LineString
from PIL import Image, ImageDraw, ImageFont
from terrain_landforms import LandscapeSurface, sample

FONT = "/System/Library/Fonts/Helvetica.ttc"


def font(size):
    return ImageFont.truetype(FONT, size)


def panel(
    root, j, bounds, size, mode="cover", height_override=None, draw_features=True
):
    x0, z0, x1, z1 = bounds
    cell = j["surface"]["cellMetres"]
    x, z = np.meshgrid(np.linspace(x0, x1, size), np.linspace(z1, z0, size))
    pts = np.column_stack([x.ravel(), z.ravel()])
    terrain = (
        np.load(root / "heightmap-metres.npy")
        if height_override is None
        else height_override
    )
    h = sample(terrain, pts, j["extent"] / (len(terrain) - 1)).reshape(x.shape)
    # No painted snow/rock: sample the exported cover fractions.
    fields = {
        key: sample(np.load(root / (key + ".npy")), pts, cell).reshape(x.shape)
        for key in ["snow", "sand", "rock"]
    }
    dz, dx = np.gradient(h, (z1 - z0) / (size - 1), (x1 - x0) / (size - 1))
    light = np.clip((0.8 - 0.5 * dx + 0.55 * dz) / np.sqrt(1 + dx * dx + dz * dz), 0, 1)
    colours = np.array(
        [
            [159, 176, 122],
            [139, 161, 104],
            [120, 148, 101],
            [128, 145, 116],
            [157, 161, 151],
        ]
    )
    rgb = np.stack(
        [np.interp(h, [0, 35, 90, 160, 300], colours[:, c]) for c in range(3)], axis=-1
    )
    if mode == "height":
        rgb = np.stack(
            [np.interp(h, [0, 40, 100, 170, 280], [42, 81, 134, 196, 244])] * 3, axis=-1
        )
    else:
        for key, col in [
            ("rock", [145, 142, 131]),
            ("sand", [223, 205, 151]),
            ("snow", [239, 244, 241]),
        ]:
            w = fields[key][..., None]
            rgb = rgb * (1 - w) + np.array(col) * w
    rgb *= 0.54 + 0.6 * light[..., None]
    mask = sh.contains_xy(shape(j["land"]), x, z)
    distance = sample(np.load(root / "coast-distance.npy"), pts, cell).reshape(x.shape)
    ocean = np.stack(
        [
            np.interp(distance, [0, 200, 700], [71, 48, 38]),
            np.interp(distance, [0, 200, 700], [135, 109, 86]),
            np.interp(distance, [0, 200, 700], [149, 126, 109]),
        ],
        axis=-1,
    )
    rgb[~mask] = ocean[~mask]
    im = Image.fromarray(np.uint8(np.clip(rgb, 0, 255)))
    if not draw_features:
        return im
    d = ImageDraw.Draw(im)

    def xy(p):
        return (
            (p[0] - x0) / (x1 - x0) * (size - 1),
            (z1 - p[1]) / (z1 - z0) * (size - 1),
        )

    # Actual width law; retain one pixel minimum for cartographic readability.
    for r in j["rivers"]:
        width = max(1, round(r["width"] / (x1 - x0) * size))
        d.line([xy(p) for p in r["points"]], fill="#72bdcc", width=width, joint="curve")
    for l in j["lakes"]:
        p = shape(l["geometry"])
        d.polygon([xy(p) for p in p.exterior.coords], fill="#68aebf")
    return im


def main(directory):
    root = Path(directory)
    j = json.loads((root / "terrain-landforms.json").read_text())
    extent = j["extent"]
    m = j["metrics"]
    canvas = Image.new("RGB", (1800, 1930), "#203d40")
    canvas.paste(panel(root, j, (0, 0, extent, extent), 1800), (0, 90))
    d = ImageDraw.Draw(canvas)
    d.text(
        (25, 16),
        "ISLAND TERRAIN / watershed ridges, connected water, seasonal cover",
        font=font(28),
        fill="white",
    )
    d.text(
        (25, 53),
        f"6 km | peak {m['peakMetres']:.0f} m | {m['highGroundSources']} upland sources to {m['oceanMouths']} ocean mouths | winter preset",
        font=font(21),
        fill="#dce2cc",
    )
    d.text(
        (25, 1898),
        "Sand = coastal cover; white = seasonal snow; grey = exposed rock. Scalar terrain + vector features; no mesh.",
        font=font(19),
        fill="white",
    )
    d.line((1425, 1840, 1725, 1840), fill="white", width=4)
    d.text((1425, 1806), "1 km", font=font(24), fill="white")
    canvas.save(root / "island-overview.png")
    # A close view makes the lake ports and actual relief inspectable.
    lake = max(j["lakes"], key=lambda l: l["waterLevelMetres"])
    c = shape(lake["geometry"]).centroid
    cliff = j["cliffs"][0]
    centre = np.mean(np.array(cliff["crest"]), axis=0)
    bounds_list = [
        (c.x - 360, c.y - 360, c.x + 360, c.y + 360),
        (centre[0] - 300, centre[1] - 300, centre[0] + 300, centre[1] + 300),
    ]
    detail = Image.new("RGB", (1600, 900), "#203d40")
    dd = ImageDraw.Draw(detail)
    for k, (b, title) in enumerate(
        zip(
            bounds_list,
            [
                "CONNECTED LAKE / inlet, level water, outlet",
                "ROCK SCARP / paired crest and toe splines",
            ],
        )
    ):
        detail.paste(panel(root, j, b, 800), (k * 800, 65))
        dd.text((k * 800 + 20, 20), title, font=font(21), fill="white")
    dd.text(
        (20, 874),
        "Physical widths and heights; lake water is a steady design stage. No dynamic flood simulation.",
        font=font(20),
        fill="white",
    )
    detail.save(root / "lake-cliff-detail.png")
    height = panel(root, j, (0, 0, extent, extent), 1500, "height")
    height.save(root / "heightmap-overview.png")
    study = Image.new("RGB", (1800, 1010), "#203d40")
    bounds = (1200, 1800, 4600, 5200)
    study.paste(
        panel(
            root, j, bounds, 900, "height", np.load(root / "source-height.npy"), False
        ),
        (0, 65),
    )
    study.paste(panel(root, j, bounds, 900, "height", draw_features=False), (900, 65))
    sd = ImageDraw.Draw(study)
    sd.text((20, 20), "SOURCE / compact watershed terrain", font=font(26), fill="white")
    sd.text(
        (920, 20), "REFINED / crests, valleys and erosion", font=font(26), fill="white"
    )
    sd.text(
        (20, 976),
        "Same scale and light; no snow or rock colouring. Both derive from the regenerated SIGGRAPH river/crest graph.",
        font=font(21),
        fill="white",
    )
    study.save(root / "mountain-study.png")
    print("PASS rendered full island, lake/cliff details and physical height overview")


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("directory")
    main(p.parse_args().directory)
