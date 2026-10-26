"""Actual polygon biome ownership, erosion height, rivers/lakes and wind maps."""

import argparse, json
from pathlib import Path
import numpy as np
import shapely as sh
from shapely.geometry import shape, LineString
from PIL import Image, ImageDraw
from terrain_landforms import sample
from render_terrain_landforms import font

PALETTE = {
    "desert": "#d2b47a",
    "dry-scrub": "#aea36d",
    "grassland": "#9cb674",
    "woodland": "#638e65",
    "rainforest": "#396e58",
    "cold-heath": "#c8cfbf",
    "riparian": "#70a368",
    "freshwater": "#67b7ce",
}


def panel(root, p, size=1600, bounds=None, mode="biomes"):
    extent = p["extent"]
    cell = p["surface"]["cellMetres"]
    x0, z0, x1, z1 = bounds or (0, 0, extent, extent)
    xs, zs = np.meshgrid(np.linspace(x0, x1, size), np.linspace(z1, z0, size))
    pts = np.column_stack([xs.ravel(), zs.ravel()])
    xy = lambda v: (
        (v[0] - x0) / (x1 - x0) * (size - 1),
        (z1 - v[1]) / (z1 - z0) * (size - 1),
    )
    land = shape(p["land"])
    inside = sh.contains_xy(land, xs, zs)
    im = Image.new("RGB", (size, size), "#295e73")
    d = ImageDraw.Draw(im)
    if mode in ("biomes", "relief"):
        for b in p["biomes"]:
            cover = Image.new("L", im.size)
            draw = ImageDraw.Draw(cover)
            for poly in sh.get_parts(shape(b["geometry"])):
                if poly.geom_type != "Polygon":
                    continue
                draw.polygon([xy(v) for v in poly.exterior.coords], fill=255)
                for ring in poly.interiors:
                    draw.polygon([xy(v) for v in ring.coords], fill=0)
            im.paste(PALETTE[b["id"]], (0, 0, size, size), cover)
        # Repaint excluded water holes from canonical water product afterward.
        rgb = np.asarray(im).astype(float)
        if mode == "relief":
            rgb[:] = [172, 166, 143]
        h = sample(np.load(root / "heightmap-metres.npy"), pts, cell).reshape(xs.shape)
        dz, dx = np.gradient(h, (z1 - z0) / (size - 1), (x1 - x0) / (size - 1))
        light = np.clip(
            (0.88 - 0.45 * dx + 0.6 * dz) / np.sqrt(1 + dx * dx + dz * dz), 0.25, 1
        )
        rock = (
            np.clip((h - 650) / 750, 0, 0.7)
            * np.clip(np.hypot(dx, dz) / 0.22, 0, 1)
            * inside
        )
        rgb = rgb * (1 - rock[..., None]) + np.array([157, 150, 135]) * rock[..., None]
        if mode == "biomes" and (root / "snow-cover.npy").exists():
            snow = sample(np.load(root / "snow-cover.npy"), pts, cell).reshape(xs.shape)
            rgb = (
                rgb * (1 - snow[..., None])
                + np.array([237, 244, 246]) * snow[..., None]
            )
        rgb *= 0.70 + 0.34 * light[..., None]
        rgb[~inside] = [41, 94, 115]
    else:
        name = "precipitation" if mode == "rain" else "temperature"
        a = sample(np.load(root / "climate" / f"{name}.npy"), pts, cell).reshape(
            xs.shape
        )
        stops = (
            [0, 250, 700, 1500, 3000, 6000]
            if mode == "rain"
            else [0, 8, 16, 24, 30, 36]
        )
        colors = (
            np.array(
                [
                    [207, 171, 115],
                    [193, 191, 124],
                    [125, 168, 112],
                    [58, 139, 132],
                    [40, 105, 139],
                    [43, 62, 117],
                ]
            )
            if mode == "rain"
            else np.array(
                [
                    [80, 105, 163],
                    [115, 158, 192],
                    [159, 190, 166],
                    [218, 193, 133],
                    [213, 142, 95],
                    [183, 87, 71],
                ]
            )
        )
        rgb = np.stack([np.interp(a, stops, colors[:, i]) for i in range(3)], axis=-1)
        rgb[~inside] = [41, 94, 115]
    im = Image.fromarray(np.uint8(np.clip(rgb, 0, 255)))
    d = ImageDraw.Draw(im)
    if mode in ("biomes", "relief"):
        for r in p["rivers"]:
            # Symbol widths show flow hierarchy at full-map scale; physical
            # variable-width contours and profiles are in the JSON.
            width = max(
                2,
                round(1 + 2 * np.sqrt(r["discharge"])),
                round(r["width"] / (x1 - x0) * size),
            )
            d.line(
                [xy(v) for v in r["points"]], fill="#69bad1", width=width, joint="curve"
            )
        for lake in p["lakes"]:
            for poly in sh.get_parts(shape(lake["geometry"])):
                d.polygon([xy(v) for v in poly.exterior.coords], fill="#69bad1")
    if mode == "rain":
        for i, r in enumerate(p["regions"]):
            if i % 9:
                continue
            c = np.array(xy(r["site"]))
            v = np.mean(r["climate"]["monthlyWindMetresPerSecond"], axis=0)
            norm = np.linalg.norm(v)
            if norm < 0.02:
                continue
            direction = np.array([v[0], -v[1]]) / norm
            end = c + direction * np.clip(8 + norm * 2, 10, 35)
            d.line([tuple(c), tuple(end)], fill="#f5f0d9", width=2)
            side = np.array([-direction[1], direction[0]])
            d.polygon(
                [
                    tuple(end),
                    tuple(end - direction * 6 + side * 3),
                    tuple(end - direction * 6 - side * 3),
                ],
                fill="#f5f0d9",
            )
    return im


def render(directory):
    root = Path(directory)
    p = json.loads((root / "terrain-world.json").read_text())
    size = 1800
    im = Image.new("RGB", (size, size + 190), "#203b3b")
    d = ImageDraw.Draw(im)
    im.paste(panel(root, p, size), (0, 90))
    d.text(
        (20, 15),
        "ISLAND GEOGRAPHY / ridged relief, river corridors and summit snow",
        font=font(31),
        fill="white",
    )
    m = p["hydrology"]["metrics"]
    d.text(
        (20, 55),
        f"72 km | {m['sources']} high-ground sources | {m['oceanMouths']} ocean mouths | {m['lakes']} connected lakes",
        font=font(23),
        fill="white",
    )
    for i, (name, col) in enumerate(PALETTE.items()):
        x = 20 + i * 220
        d.rectangle((x, size + 112, x + 18, size + 132), fill=col)
        d.text((x + 25, size + 110), name, font=font(21), fill="white")
    d.text(
        (20, size + 157),
        "2D maps. White: artistic summit snow. Rivers use enlarged map symbols; physical widths are exported. No meshing.",
        font=font(23),
        fill="white",
    )
    im.save(root / "island-geography.png")
    atlas = Image.new("RGB", (1800, 1000), "#203b3b")
    d = ImageDraw.Draw(atlas)
    for i, mode in enumerate(["rain", "temperature"]):
        atlas.paste(panel(root, p, 900, mode=mode), (i * 900, 60))
        d.text(
            (i * 900 + 20, 18),
            [
                "ANNUAL RAINFALL / solved wind arrows",
                "MEAN TEMPERATURE / seasonal advection",
            ][i],
            font=font(25),
            fill="white",
        )
    d.text(
        (20, 972),
        "Rain: tan <250, green ~700, teal ~1,500, blue >3,000 mm/year. Twelve monthly states in the export.",
        font=font(22),
        fill="white",
    )
    atlas.save(root / "wind-climate.png")
    if p["lakes"]:
        lake = max(p["lakes"], key=lambda l: l["areaMetres2"])
        c = shape(lake["geometry"]).centroid
        span = 6000
        bounds = (c.x - span / 2, c.y - span / 2, c.x + span / 2, c.y + span / 2)
        im = Image.new("RGB", (1200, 1280), "#203b3b")
        im.paste(panel(root, p, 1200, bounds), (0, 60))
        d = ImageDraw.Draw(im)
        d.text(
            (20, 18),
            "LAKE DETAIL / routed inflow, retained basin and outflow",
            font=font(27),
            fill="white",
        )
        im.save(root / "lake-detail.png")
    before = np.load(root / "terrain-before-erosion.npy")
    after = np.load(root / "eroded-elevation.npy")
    n = 1200
    axis = np.linspace(0, p["extent"], n)
    xx, zz = np.meshgrid(axis, axis[::-1])
    points = np.column_stack([xx.ravel(), zz.ravel()])
    change = sample(before - after, points, p["surface"]["cellMetres"]).reshape((n, n))
    cols = np.array([[75, 136, 168], [220, 219, 193], [167, 82, 49]])
    rgb = np.stack(
        [np.interp(change, [-4, 0, 12], cols[:, k]) for k in range(3)], axis=-1
    )
    inside = sh.contains_xy(shape(p["land"]), xx, zz)
    rgb[~inside] = [41, 94, 115]
    im = Image.new("RGB", (n, n + 110), "#203b3b")
    im.paste(Image.fromarray(np.uint8(rgb)), (0, 60))
    d = ImageDraw.Draw(im)
    d.text(
        (20, 18),
        "EROSION / signed terrain change in metres",
        font=font(28),
        fill="white",
    )
    d.text(
        (20, n + 76),
        "Blue: deposition to 4 m | Pale: unchanged | Rust: erosion to 12 m",
        font=font(25),
        fill="white",
    )
    im.save(root / "erosion-change.png")
    print(
        "PASS island geography, regional wind/climate and lake detail rendered from exported maps"
    )


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("directory")
    a = p.parse_args()
    render(a.directory)
