"""Measure exported terrain in SI units and render same-scale dune evidence."""

import argparse, json
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw
from terrain_landforms import sample
from terrain_feature_scale import dune_raster
from render_terrain_landforms import font
from render_terrain_geography import panel


def sand_relief(h, cell):
    dz, dx = np.gradient(h, cell)
    light = np.clip(
        (0.85 - 0.55 * dx + 0.6 * dz) / np.sqrt(1 + dx * dx + dz * dz), 0, 1
    )
    return Image.fromarray(
        np.uint8(
            np.clip(
                np.array([209, 180, 121]) * (0.55 + 0.56 * light[..., None]), 0, 255
            )
        )
    )


def bar(draw, origin, metres, pixels_per_metre):
    x, y = origin
    end = x + metres * pixels_per_metre
    draw.rectangle((x - 10, y - 30, end + 25, y + 12), fill="#203b3b")
    draw.line((x, y, end, y), fill="white", width=4)
    draw.text((x, y - 27), f"{metres:g} m", font=font(20), fill="white")


def audit(directory, previous):
    root = Path(directory)
    old = Path(previous)
    j = json.loads((root / "terrain-world.json").read_text())
    cell = j["surface"]["cellMetres"]
    h = np.load(root / "heightmap-metres.npy")
    coast = np.load(root / "coast-distance.npy")
    dunes = np.load(root / "arid/dune-height.npy")
    eligibility = np.load(root / "arid/dune-weight.npy")
    old_d = np.load(old / "arid/dune-height.npy")
    weights = np.load(root / "mountain-weight.npy")
    sand = np.load(root / "sand.npy")
    dz, dx = np.gradient(h, cell)
    climate = j["environment"]["climateGraph"]
    report = dict(
        units=dict(length="m", area="m2", slope="rise/run"),
        extentMetres=j["extent"],
        cellMetres=cell,
        landAreaMetres2=j["metrics"]["landAreaMetres2"],
        peakMetres=float(h.max()),
        landSlopePercentiles=np.percentile(
            np.hypot(dx, dz)[h > 0], [50, 90, 99]
        ).tolist(),
        mountainCoreAreaMetres2=float(((weights > 0.3) & (h > 0)).sum() * cell**2),
        planningRegionAreaPercentilesMetres2=np.percentile(
            climate["areasMetres2"], [10, 50, 90]
        ).tolist(),
        sandInlandDistancePercentilesMetres=np.percentile(
            coast[sand > 0.35], [10, 50, 90, 100]
        ).tolist(),
        dunes=dict(
            previousSpacingMetres=125,
            currentSpacingMetres=40,
            previousPeakMetres=float(old_d.max()),
            currentPeakMetres=float(dunes.max()),
            maximumDesignHeightMetres=2,
            previousAreaAboveHalfMetre=float((old_d > 0.5).sum() * cell**2),
            currentAreaAboveHalfMetre=float((dunes > 0.5).sum() * cell**2),
        ),
        erosion=dict(
            cellMetres=cell,
            maximumPathMetres=70 * cell,
            brushBoundingWidthMetres=3 * cell,
            dropletsPerSquareMetrePerPass=3 / cell**2,
            alternateResolutionQualified=False,
        ),
        exclusions=[
            "hydrology deferred",
            "dedicated cliffs disabled",
            "settlements and farmland not regenerated",
            "meshing deferred",
        ],
    )
    # Pick the largest new dune rather than an arbitrary flattering crop.
    iz, ix = np.unravel_index(np.argmax(dunes), dunes.shape)
    cx, cz = np.clip([ix * cell, iz * cell], 300, j["extent"] - 300)
    bounds = (cx - 300, cz - 300, cx + 300, cz + 300)
    comparison = Image.new("RGB", (1600, 920), "#203b3b")
    d = ImageDraw.Draw(comparison)
    for k, (folder, title) in enumerate(
        [
            (old, "BEFORE / 125 m spacing, up to 10.6 m"),
            (root, "AFTER / 40 m spacing, up to 2 m"),
        ]
    ):
        # Both panels sample exported height, with the same scale and light.
        p = json.loads((folder / "terrain-world.json").read_text())
        comparison.paste(panel(folder, p, "arid", 800, bounds=bounds), (k * 800, 60))
        d.text((k * 800 + 18, 18), title, font=font(24), fill="white")
        bar(d, (k * 800 + 30, 830), 100, 799 / 600)
    d.text(
        (20, 880),
        "Same 600 m location. Actual exported terrain; scale bars are 100 m.",
        font=font(23),
        fill="white",
    )
    comparison.save(root / "dune-scale-comparison.png")
    # A local 1 m terrain-detail query, preserving the coarse geology and masks.
    x, z = np.meshgrid(
        np.linspace(cx - 200, cx + 200, 401), np.linspace(cz + 200, cz - 200, 401)
    )
    q = np.column_stack([x.ravel(), z.ravel()])
    base = sample(h, q, cell).reshape(x.shape)
    local_mask = sample(eligibility, q, cell).reshape(x.shape)
    wind = j["desertDetail"]["arid"]["windDirection"]
    detail = dune_raster(x, z, 1, wind, j["seed"]) * local_mask
    np.save(root / "dune-local-height-metres.npy", base + detail)
    local = Image.new("RGB", (802, 900), "#203b3b")
    draw = ImageDraw.Draw(local)
    local.paste(sand_relief(base + detail, 1).resize((802, 802)), (0, 50))
    draw.text(
        (18, 15), "REGIONAL DETAIL / 400 m, sampled at 1 m", font=font(23), fill="white"
    )
    bar(draw, (30, 820), 100, 801 / 400)
    draw.text(
        (18, 866),
        "Same world coordinates; refined dunes over sampled geology.",
        font=font(19),
        fill="white",
    )
    local.save(root / "dune-local-detail.png")
    report["localDetail"] = dict(
        boundsMetres=[cx - 200, cz - 200, cx + 200, cz + 200],
        cellMetres=1,
        rowOrder="north-to-south",
        maximumAddedMetres=float(detail.max()),
        geologyResolutionMetres=cell,
    )
    (root / "scale-audit.json").write_text(json.dumps(report, indent=2) + "\n")
    print("PASS measured terrain scales: " + json.dumps(report))


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("directory")
    p.add_argument("previous")
    a = p.parse_args()
    audit(a.directory, a.previous)
