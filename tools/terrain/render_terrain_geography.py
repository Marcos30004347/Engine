"""Render exported v10 scalar and vector products; no illustrative terrain."""

import argparse, json
from pathlib import Path
import numpy as np
import shapely as sh
from shapely.geometry import shape
from PIL import Image, ImageDraw
from terrain_landforms import sample
from terrain_geography import BIOMES, COLOURS
from render_terrain_landforms import font


def panel(root, j, preset, size=1400, bounds=None, height_only=False):
    extent = j["extent"]
    cell = j["surface"]["cellMetres"]
    x0, z0, x1, z1 = bounds or (0, 0, extent, extent)
    x, z = np.meshgrid(np.linspace(x0, x1, size), np.linspace(z1, z0, size))
    q = np.column_stack([x.ravel(), z.ravel()])

    def field(name, directory=root):
        return sample(np.load(directory / (name + ".npy")), q, cell).reshape(x.shape)

    h = (
        field("heightmap-metres", root / preset)
        if (root / preset / "heightmap-metres.npy").exists()
        else field("heightmap-metres")
    )
    dz, dx = np.gradient(h, (z1 - z0) / (size - 1), (x1 - x0) / (size - 1))
    light = np.clip(
        (0.85 - 0.55 * dx + 0.6 * dz) / np.sqrt(1 + dx * dx + dz * dz), 0, 1
    )
    weights = np.load(root / preset / "biome-weights.npy")
    rgb = np.zeros((*x.shape, 3))
    for w, c in zip(weights, COLOURS):
        rgb += sample(w, q, cell).reshape(*x.shape, 1) * c
    if height_only:
        rgb = np.repeat(
            np.interp(h, [0, 40, 100, 193], [50, 93, 163, 245])[..., None], 3, axis=2
        )
    else:
        for name, col in [("rock", [151, 143, 129]), ("sand", [221, 203, 152])]:
            w = field(name)[..., None]
            rgb = rgb * (1 - w) + np.array(col) * w
        if (root / preset / "dune-weight.npy").exists():
            dune = field("dune-weight", root / preset)[..., None]
            rgb = rgb * (1 - dune * 0.7) + np.array([209, 180, 121]) * dune * 0.7
        if preset == "mixed-showcase":
            # Canopy fraction is exported; this shading doesn't create tree placements.
            canopy = field("canopy")
            rgb *= 1 - 0.10 * canopy[..., None]
    rgb *= 0.55 + 0.56 * light[..., None]
    mask = sh.contains_xy(shape(j["land"]), x, z)
    d = field("coast-distance")
    ocean = np.stack(
        [
            np.interp(d, [0, 250, 800], v)
            for v in ([70, 44, 33], [132, 104, 79], [145, 119, 100])
        ],
        axis=-1,
    )
    rgb[~mask] = ocean[~mask]
    im = Image.fromarray(np.uint8(np.clip(rgb, 0, 255)))
    draw = ImageDraw.Draw(im)

    def xy(p):
        return (
            (p[0] - x0) / (x1 - x0) * (size - 1),
            (z1 - p[1]) / (z1 - z0) * (size - 1),
        )

    for river in j["rivers"]:
        draw.line(
            [xy(p) for p in river["points"]],
            fill="#75bfce",
            width=max(1, round(river["width"] / (x1 - x0) * size)),
            joint="curve",
        )
    for lake in j["lakes"]:
        draw.polygon(
            [xy(p) for p in shape(lake["geometry"]).exterior.coords], fill="#6bb8c9"
        )
    return im


def render(directory):
    root = Path(directory)
    j = json.loads((root / "terrain-geography.json").read_text())
    im = Image.new("RGB", (1600, 1760), "#203b3b")
    im.paste(panel(root, j, "mixed-showcase", 1600), (0, 95))
    d = ImageDraw.Draw(im)
    d.text(
        (25, 14),
        "ISLAND GEOGRAPHY / forests, drylands, plateaus and coastal cliffs",
        font=font(27),
        fill="white",
    )
    d.text(
        (25, 53),
        "6 km | authored mixed-climate showcase | SIGGRAPH mountain source | actual 2D map products",
        font=font(21),
        fill="#d3dcc5",
    )
    for i, (name, col) in enumerate(zip(BIOMES, COLOURS)):
        xx = 25 + i * 260
        d.rectangle((xx, 1706, xx + 19, 1725), fill=tuple(col))
        d.text((xx + 28, 1704), name, font=font(18), fill="white")
    d.text(
        (25, 1735),
        "Biome suitability and cover; tree instances, settlements and meshes are not generated here.",
        font=font(18),
        fill="white",
    )
    im.save(root / "island-overview.png")
    atlas = Image.new("RGB", (1600, 1790), "#203b3b")
    d = ImageDraw.Draw(atlas)
    for k, preset in enumerate(
        ["temperate", "humid-tropical", "arid", "mixed-showcase"]
    ):
        ox = k % 2 * 800
        oy = k // 2 * 860
        atlas.paste(panel(root, j, preset, 800), (ox, oy + 55))
        d.text((ox + 20, oy + 15), preset.upper(), font=font(24), fill="white")
    d.text(
        (20, 1735),
        "Same terrain; different annual climate recipes. Mixed view uses an explicit rainfall override.",
        font=font(22),
        fill="white",
    )
    d.text(
        (20, 1764),
        "Water curves retain the v9 discharge scenario; annual climate/runoff reconciliation remains pending.",
        font=font(19),
        fill="#d3dcc5",
    )
    atlas.save(root / "biome-presets.png")
    features = j["geography"]["features"]
    plateau = next(f for f in features if f["type"] == "plateau")
    cliff = next(f for f in features if f["type"] == "coastal-cliff")
    targets = [
        (
            "PLATEAU / graded top, cliff face, talus",
            shape(plateau["geometry"]).centroid,
            530,
        ),
        ("COASTAL BLUFF / sea-level toe", shape(cliff["geometry"]).centroid, 400),
    ]
    detail = Image.new("RGB", (1600, 895), "#203b3b")
    d = ImageDraw.Draw(detail)
    for i, (title, c, r) in enumerate(targets):
        detail.paste(
            panel(
                root,
                j,
                "mixed-showcase",
                800,
                (c.x - r, c.y - r, c.x + r, c.y + r),
                True,
            ),
            (i * 800, 55),
        )
        d.text((i * 800 + 20, 16), title, font=font(23), fill="white")
    d.text(
        (20, 865),
        "Uncoloured height and real water. These forms exist in the scalar terrain, not just the overview palette.",
        font=font(21),
        fill="white",
    )
    detail.save(root / "landform-detail.png")
    panel(root, j, "mixed-showcase", 1400, height_only=True).save(
        root / "heightmap-overview.png"
    )
    print(
        "PASS render: island, four biome presets, plateau/coastal cliff details, uncoloured height"
    )


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("directory")
    render(p.parse_args().directory)
