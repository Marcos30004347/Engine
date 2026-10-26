"""Render the exported geographic products; no invented settlements or meshes."""

import argparse, json
from pathlib import Path
import numpy as np
import shapely as sh
from shapely.geometry import shape, LineString
from PIL import Image, ImageDraw
from terrain_landforms import sample
from terrain_geography import COLOURS, BIOMES
from render_terrain_landforms import font


def panel(root, j, size=1800, bounds=None, mode="biomes"):
    extent = j["extent"]
    cell = j["surface"]["cellMetres"]
    x0, z0, x1, z1 = bounds or (0, 0, extent, extent)
    x, z = np.meshgrid(np.linspace(x0, x1, size), np.linspace(z1, z0, size))
    q = np.column_stack([x.ravel(), z.ravel()])

    def at(name, folder=root):
        return sample(np.load(folder / (name + ".npy")), q, cell).reshape(x.shape)

    h = at("heightmap-metres")
    dz, dx = np.gradient(h, (z1 - z0) / (size - 1), (x1 - x0) / (size - 1))
    light = np.clip(
        (0.85 - 0.55 * dx + 0.6 * dz) / np.sqrt(1 + dx * dx + dz * dz), 0, 1
    )
    rgb = np.zeros((*x.shape, 3))
    if mode == "rain":
        rain = at("precipitation", root / "island")
        palette = np.array(
            [
                [196, 156, 98],
                [207, 204, 123],
                [137, 184, 121],
                [61, 146, 142],
                [35, 86, 139],
            ]
        )
        for c in range(3):
            rgb[..., c] = np.interp(rain, [0, 300, 1000, 2500, 6000], palette[:, c])
    else:
        weights = np.load(root / "island/biome-weights.npy")
        for a, c in zip(weights, COLOURS):
            rgb += sample(a, q, cell).reshape((*x.shape, 1)) * c
        for name, col in [("rock", [160, 153, 141]), ("sand", [232, 215, 165])]:
            w = at(name)[..., None]
            rgb = rgb * (1 - w) + np.array(col) * w
    rgb *= 0.55 + 0.56 * light[..., None]
    mask = sh.contains_xy(shape(j["land"]), x, z)
    dist = at("coast-distance")
    ocean = np.stack(
        [
            np.interp(dist, [0, 1500, 7000], a)
            for a in ([67, 34, 21], [131, 92, 58], [145, 112, 82])
        ],
        axis=-1,
    )
    rgb[~mask] = ocean[~mask]
    im = Image.fromarray(np.uint8(np.clip(rgb, 0, 255)))
    d = ImageDraw.Draw(im)

    def xy(p):
        return (
            (p[0] - x0) / (x1 - x0) * (size - 1),
            (z1 - p[1]) / (z1 - z0) * (size - 1),
        )

    # At island scale subpixel streams get a one-pixel cartographic stroke.
    # Widths in the JSON remain physical; zooms use those same metre values.
    for r in j["rivers"]:
        d.line(
            [xy(p) for p in r["points"]],
            fill="#76bfd0",
            width=max(1, round(r["width"] / (x1 - x0) * (size - 1))),
            joint="curve",
        )
    for lake in j["lakes"]:
        for poly in sh.get_parts(shape(lake["geometry"])):
            d.polygon([xy(p) for p in poly.exterior.coords], fill="#68b3c8")
    return im


def scalebar(d, x, y, metres, pixels_per_metre):
    end = x + metres * pixels_per_metre
    d.rectangle((x - 12, y - 33, end + 16, y + 15), fill="#203b3b")
    d.line((x, y, end, y), fill="white", width=5)
    d.text((x, y - 28), f"{metres/1000:g} km", font=font(22), fill="white")


def render(directory):
    root = Path(directory)
    j = json.loads((root / "terrain-world.json").read_text())
    m = j["hydrology"]["metrics"]
    size = 2200
    im = Image.new("RGB", (size, size + 190), "#203b3b")
    d = ImageDraw.Draw(im)
    im.paste(panel(root, j, size), (0, 100))
    d.text((25, 15), "COMPLETE GEOGRAPHIC ISLAND / 48 km", font=font(34), fill="white")
    d.text(
        (25, 60),
        f"{j['metrics']['peakMetres']:.0f} m summit | {m['sources']} high-ground sources | {m['oceanMouths']} ocean mouths | {m['lakes']} lakes",
        font=font(24),
        fill="#dce6d9",
    )
    scalebar(d, 45, size + 48, 10000, (size - 1) / 48000)
    d.text((size - 460, 130), "Prevailing wind: W to E", font=font(25), fill="white")
    for k, (name, col) in enumerate(zip(BIOMES, COLOURS)):
        xx = 25 + k * 350
        d.rectangle((xx, size + 115, xx + 22, size + 137), fill=tuple(col))
        d.text((xx + 32, size + 111), name, font=font(24), fill="white")
    d.text(
        (25, size + 157),
        "Actual 2D terrain and drainage. Small streams use a minimum 1 px map stroke; no settlements or meshes.",
        font=font(22),
        fill="#dce6d9",
    )
    im.save(root / "complete-island.png")
    # Inspect an actually connected lake and the largest dry region.
    lake = max(
        (l for l in j["lakes"] if l["connectedRiverIds"]),
        key=lambda l: l["areaMetres2"],
    )
    c = shape(lake["geometry"]).centroid
    linked = next(r for r in j["rivers"] if r["id"] == lake["connectedRiverIds"][0])
    targets = [("LAKE / terrain basin and connected rivers", (c.x, c.y), 4500)]
    river = max(j["rivers"], key=lambda r: len(r["points"]))
    c = LineString(river["points"]).interpolate(0.6, normalized=True)
    targets.append(("RIVER VALLEY / terrain-derived drainage", (c.x, c.y), 5000))
    atlas = Image.new("RGB", (1800, 1010), "#203b3b")
    draw = ImageDraw.Draw(atlas)
    for k, (title, c, width) in enumerate(targets):
        cx, cz = np.clip(c, width / 2, 48000 - width / 2)
        bounds = (cx - width / 2, cz - width / 2, cx + width / 2, cz + width / 2)
        atlas.paste(panel(root, j, 900, bounds), (k * 900, 60))
        draw.text((k * 900 + 18, 20), title, font=font(23), fill="white")
        scalebar(draw, k * 900 + 30, 930, 1000, 899 / width)
    draw.text(
        (20, 975),
        "Lake levels and river flows come from the exported drainage graph. Erosion and backwater remain approximations.",
        font=font(21),
        fill="white",
    )
    atlas.save(root / "water-detail.png")
    climate = Image.new("RGB", (1400, 1510), "#203b3b")
    draw = ImageDraw.Draw(climate)
    climate.paste(panel(root, j, 1400, mode="rain"), (0, 60))
    draw.text(
        (20, 15),
        "RAINFALL / ocean-fed prevailing winds, uplift and rain shadow",
        font=font(26),
        fill="white",
    )
    draw.text(
        (20, 1470),
        "Tan: <300 mm/year   |   Green: ~1,000   |   Teal: ~2,500   |   Blue: ≥6,000",
        font=font(25),
        fill="white",
    )
    climate.save(root / "rainfall-map.png")
    print(
        "PASS complete-island, water-detail and rainfall-map views from exported scalar/vector products"
    )


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("directory")
    a = p.parse_args()
    render(a.directory)
