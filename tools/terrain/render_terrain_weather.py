"""Actual v11 island, same-light ridge comparison, and regional weather atlas."""

import argparse, json
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw
import shapely as sh
from shapely.geometry import shape
from terrain_landforms import sample
from render_terrain_geography import panel
from render_terrain_landforms import font


def render(directory):
    root = Path(directory)
    j = json.loads((root / "terrain-weather.json").read_text())
    extent = j["extent"]
    cell = j["surface"]["cellMetres"]
    im = Image.new("RGB", (1600, 1720), "#203b3b")
    im.paste(panel(root, j, "temperate", 1600), (0, 80))
    d = ImageDraw.Draw(im)
    d.text(
        (25, 12),
        "ISLAND / sharp watershed crests, wider rivers, regional wind and climate",
        font=font(27),
        fill="white",
    )
    d.text(
        (25, 49),
        f"6 km | {j['metrics']['peakMetres']:.0f} m summit | temperate maritime recipe | 12 monthly wind/temperature states",
        font=font(20),
        fill="#d3dcc5",
    )
    d.text(
        (25, 1690),
        "Actual scalar terrain and map contours. River widths are doubled; design flow is conserved. No meshes.",
        font=font(20),
        fill="white",
    )
    im.save(root / "island-overview.png")
    old = Path(j["source"])
    oldj = json.loads((old / "terrain-geography.json").read_text())
    bounds = (1500, 2500, 3500, 4500)
    comparison = Image.new("RGB", (1800, 1015), "#203b3b")
    d = ImageDraw.Draw(comparison)
    comparison.paste(panel(old, oldj, "temperate", 900, bounds, True), (0, 60))
    comparison.paste(panel(root, j, "temperate", 900, bounds, True), (900, 60))
    d.text((20, 17), "BEFORE / cross-crest smoothing", font=font(27), fill="white")
    d.text(
        (920, 17), "AFTER / preserved tips, eroded flanks", font=font(27), fill="white"
    )
    d.text(
        (20, 978),
        "Same height palette, scale and light; no rock/snow colour. Both images sample generated terrain.",
        font=font(23),
        fill="white",
    )
    comparison.save(root / "ridge-comparison.png")
    # Numeric cross-sections: show every predeclared section in thin lines plus median.
    sections = j["ridgeSections"]
    s = np.array(sections[0]["distancesMetres"])
    profiles = []
    for key in ["beforeMetres", "afterMetres"]:
        a = np.array([r[key] for r in sections])
        a = a - a[:, len(s) // 2, None]
        profiles.append(a)
    plot = Image.new("RGB", (1200, 610), "#f3f1e9")
    d = ImageDraw.Draw(plot)
    d.text(
        (30, 20),
        "RIDGE CROSS-SECTIONS / height relative to the divide",
        font=font(25),
        fill="#203b3b",
    )

    def xy(x, h):
        return (80 + (x + 120) / 240 * 1060, 105 - h / 100 * 390)

    for z in [0, -20, -40, -60, -80, -100]:
        d.line([xy(-120, z), xy(120, z)], fill="#d3d4cc", width=1)
        d.text((20, xy(0, z)[1] - 10), str(z), font=font(17), fill="#374646")
    for arrays, col, pale in zip(
        profiles, ["#ad654b", "#244d6b"], ["#e7c9b7", "#b9cdd6"]
    ):
        for a in arrays:
            d.line(
                [xy(x, h) for x, h in zip(s, np.clip(a, -100, 10))], fill=pale, width=1
            )
        d.line(
            [xy(x, h) for x, h in zip(s, np.median(arrays, axis=0))], fill=col, width=4
        )
    d.text(
        (70, 545),
        "Orange: before   Blue: after   Thin lines: all eligible sections   X: -120 to +120 metres",
        font=font(23),
        fill="#203b3b",
    )
    plot.save(root / "ridge-sections.png")
    atlas = Image.new("RGB", (1800, 1940), "#203b3b")
    d = ImageDraw.Draw(atlas)
    temperatures = np.load(root / "temperate/monthly-temperature.npy")
    precip = np.load(root / "temperate/precipitation.npy")
    x, z = np.meshgrid(np.linspace(0, extent, 900), np.linspace(extent, 0, 900))
    q = np.column_stack([x.ravel(), z.ravel()])
    mask = sh.contains_xy(shape(j["land"]), x, z)
    for k, (name, a, low, high, palette) in enumerate(
        [
            (
                "JANUARY / temperature C",
                temperatures[0],
                0,
                20,
                [[80, 118, 149], [185, 202, 138], [225, 153, 95]],
            ),
            (
                "JULY / temperature C",
                temperatures[6],
                0,
                20,
                [[80, 118, 149], [185, 202, 138], [225, 153, 95]],
            ),
            (
                "ANNUAL RAIN / mm",
                precip,
                0,
                2000,
                [[205, 177, 112], [94, 152, 105], [41, 91, 131]],
            ),
        ]
    ):
        values = sample(a, q, cell).reshape(x.shape)
        rgb = np.stack(
            [
                np.interp(
                    values, [low, (low + high) / 2, high], np.array(palette)[:, i]
                )
                for i in range(3)
            ],
            axis=-1,
        )
        rgb[~mask] = [35, 69, 78]
        view = Image.fromarray(np.uint8(rgb))
        ox = k % 2 * 900
        oy = k // 2 * 955
        atlas.paste(view, (ox, oy + 50))
        d.text((ox + 20, oy + 14), name, font=font(24), fill="white")
        d.text(
            (ox + 20, oy + 905), f"Scale {low} to {high}", font=font(18), fill="white"
        )
    ox = 900
    oy = 955
    atlas.paste(panel(root, j, "temperate", 900), (ox, oy + 50))
    d.text(
        (ox + 20, oy + 14),
        "JULY WIND / solved regional vectors",
        font=font(24),
        fill="white",
    )
    c = np.array(j["environment"]["climateGraph"]["centres"])
    v = np.array(
        j["environment"]["presets"]["temperate"]["months"][6][
            "regionalVelocityMetresPerSecond"
        ]
    )
    for p, velocity in zip(c, v):
        start = np.array(
            [ox + p[0] / extent * 900, oy + 50 + (extent - p[1]) / extent * 900]
        )
        delta = np.array([velocity[0], -velocity[1]]) * 7
        end = start + delta
        d.line([tuple(start), tuple(end)], fill="#eff5ef", width=2)
        norm = np.linalg.norm(delta)
        if norm > 1:
            u = delta / norm
            n = np.array([-u[1], u[0]])
            d.polygon(
                [tuple(end), tuple(end - u * 7 + n * 3), tuple(end - u * 7 - n * 3)],
                fill="#eff5ef",
            )
    d.text(
        (25, 1910),
        "Temperature uses one shared scale. Wind is a steady terrain-aware approximation, not a weather forecast.",
        font=font(22),
        fill="white",
    )
    atlas.save(root / "weather-atlas.png")
    climates = Image.new("RGB", (1800, 660), "#203b3b")
    d = ImageDraw.Draw(climates)
    for k, preset in enumerate(["temperate", "humid-tropical", "arid"]):
        climates.paste(panel(root, j, preset, 600), (k * 600, 50))
        d.text((k * 600 + 20, 15), preset.upper(), font=font(23), fill="white")
    climates.save(root / "biome-presets.png")
    print(
        "PASS render: terrain, ridge comparison and profiles, monthly weather atlas, three climate recipes"
    )


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("directory")
    render(p.parse_args().directory)
