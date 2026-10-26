"""Terrain-only evidence rendered from accepted scalar/feature products."""

import argparse
import json
from pathlib import Path

import numpy as np
import shapely as sh
from PIL import Image, ImageDraw, ImageFont
from scipy.ndimage import map_coordinates
from shapely.geometry import shape

from terrain_water_realization import ChannelSurface

FONT = "/System/Library/Fonts/Helvetica.ttc"


def font(size):
    return ImageFont.truetype(FONT, size)


def panel(surface, land, bounds, size, sampler=None):
    x0, z0, x1, z1 = bounds
    xs = np.linspace(x0, x1, size)
    zs = np.linspace(z1, z0, size)
    gx, gz = np.meshgrid(xs, zs)
    sample = sampler or surface.sample
    h = np.empty((size, size))
    for row in range(0, size, 32):
        h[row : row + 32] = sample(
            np.column_stack([gx[row : row + 32].ravel(), gz[row : row + 32].ravel()])
        ).reshape(gx[row : row + 32].shape)
    mask = sh.contains_xy(land, gx, gz)
    dz, dx = np.gradient(h, (z1 - z0) / (size - 1), (x1 - x0) / (size - 1))
    normal = np.sqrt(1 + dx * dx + dz * dz)
    illumination = np.clip((0.80 - 0.40 * dx + 0.45 * dz) / normal, 0.18, 1.15)
    stops = [0, 20, 65, 130, 220, 300]
    colours = np.array(
        [
            [166, 183, 123],
            [148, 168, 113],
            [126, 148, 100],
            [131, 141, 113],
            [163, 162, 143],
            [196, 193, 173],
        ]
    )
    rgb = np.stack([np.interp(h, stops, colours[:, c]) for c in range(3)], axis=-1)
    rgb *= (0.55 + 0.55 * illumination)[..., None]
    rgb[~mask] = [62, 101, 113]
    return Image.fromarray(np.uint8(np.clip(rgb, 0, 255)))


def overlay(image, product, bounds, lakes=True):
    d = ImageDraw.Draw(image)
    w, h = image.size
    x0, z0, x1, z1 = bounds

    def xy(p):
        return ((p[0] - x0) / (x1 - x0) * (w - 1), (z1 - p[1]) / (z1 - z0) * (h - 1))

    for r in product["rivers"]:
        width = max(1, round(r["width"] / (x1 - x0) * w))
        d.line([xy(p) for p in r["points"]], fill="#85c1ce", width=width)
    if lakes:
        for lake in product["lakes"]:
            for p in sh.get_parts(shape(lake["geometry"])):
                d.polygon(
                    [xy(q) for q in p.exterior.coords],
                    fill="#74b5c8",
                    outline="#c4e1d6",
                    width=2,
                )
    return image


def main(directory):
    root = Path(directory)
    j = json.loads((root / "terrain-water.json").read_text())
    h = np.load(root / "base-height.npy")
    original = np.load(root / "source-height.npy")
    s = ChannelSurface(h, j["surface"]["baseCellMetres"], shape(j["land"]), j["rivers"])
    land = shape(j["land"])
    extent = j["extent"]
    overview = Image.new("RGB", (1800, 1900), "#1f393d")
    im = overlay(
        panel(s, land, (0, 0, extent, extent), 1800), j, (0, 0, extent, extent)
    )
    overview.paste(im, (0, 75))
    d = ImageDraw.Draw(overview)
    d.text(
        (25, 15),
        "TERRAIN / continuous river beds and physical basin contours",
        font=font(29),
        fill="white",
    )
    d.text(
        (25, 51),
        f"6 km across | summit {j['metrics']['peakMetres']:.0f} m | {len(j['lakes'])} static lake surfaces | no settlements or mesh",
        font=font(18),
        fill="#d3dcc6",
    )
    d.text(
        (25, 1865),
        "Blue rivers show source corridors; lake levels are static stages, not a dynamic water simulation.",
        font=font(19),
        fill="#d3dcc6",
    )
    d.line((1450, 1810, 1750, 1810), fill="white", width=4)
    d.text((1450, 1778), "1000 m", font=font(22), fill="white")
    overview.save(root / "terrain-overview.png")
    lake = max(j["lakes"], key=lambda l: l["areaMetres2"])
    p = shape(lake["geometry"])
    cx, cz = p.centroid.coords[0]
    span = max(180, max(p.bounds[2] - p.bounds[0], p.bounds[3] - p.bounds[1]) * 0.75)
    bounds = (cx - span, cz - span, cx + span, cz + span)
    detail = Image.new("RGB", (1200, 1300), "#1f393d")
    detail.paste(overlay(panel(s, land, bounds, 1200), j, bounds), (0, 60))
    dd = ImageDraw.Draw(detail)
    dd.text(
        (20, 15),
        f"LAKE / {lake['areaMetres2']/10000:.2f} ha | depth {lake['maximumDepthMetres']:.2f} m",
        font=font(27),
        fill="white",
    )
    dd.text(
        (20, 1265),
        f"Water {lake['waterLevelMetres']:.2f} m | spill {lake['spillLevelMetres']:.2f} m | refined contour",
        font=font(21),
        fill="white",
    )
    detail.save(root / "lake-detail.png")
    # Choose the actual worst coarse-raster disagreement for a reproducible section.
    worst = (-1, None, None)
    for r in j["rivers"]:
        points = np.asarray(r["points"])
        old = map_coordinates(
            original, [points[:, 1] / s.cell, points[:, 0] / s.cell], order=1
        )
        valid = sh.covers(land, sh.points(points))
        error = np.where(valid, np.abs(old - r["bed"]), -1)
        k = int(error.argmax())
        if error[k] > worst[0]:
            worst = (float(error[k]), r, k)
    _, r, k = worst
    points = np.asarray(r["points"])
    centre = points[k]
    tangent = points[min(k + 1, len(points) - 1)] - points[max(k - 1, 0)]
    normal = np.array([-tangent[1], tangent[0]]) / np.linalg.norm(tangent)
    distances = np.linspace(-30, 30, 601)
    probes = centre + distances[:, None] * normal
    before = map_coordinates(
        original, [probes[:, 1] / s.cell, probes[:, 0] / s.cell], order=1
    )
    after = s.sample(probes)
    plot = Image.new("RGB", (1400, 800), "#f4f1e6")
    dd = ImageDraw.Draw(plot)
    lo = min(before.min(), after.min()) - 1
    hi = max(before.max(), after.max()) + 1

    def graph(values):
        return [
            (110 + (x + 30) / 60 * 1190, 690 - (y - lo) / (hi - lo) * 560)
            for x, y in zip(distances, values)
        ]

    for level in np.linspace(lo, hi, 7):
        y = 690 - (level - lo) / (hi - lo) * 560
        dd.line((110, y, 1300, y), fill="#c9c9bd")
        dd.text((15, y - 10), f"{level:.1f} m", font=font(19), fill="#333c36")
    dd.line(graph(before), fill="#a2744e", width=4)
    dd.line(graph(after), fill="#286a85", width=4)
    dd.text(
        (25, 20),
        "RIVER CROSS-SECTION / worst v5 height-raster mismatch",
        font=font(28),
        fill="#233d40",
    )
    dd.text(
        (25, 65),
        f"Brown: previous terrain. Blue: continuous profile. Original discrepancy: {worst[0]:.2f} m.",
        font=font(21),
        fill="#233d40",
    )
    for x in [-30, -15, 0, 15, 30]:
        dd.text(
            (100 + (x + 30) / 60 * 1190, 710), f"{x} m", font=font(19), fill="#333c36"
        )
    dd.text(
        (25, 760),
        "Horizontal distance across the channel; vertical values are physical elevations, not a shaded mesh.",
        font=font(20),
        fill="#333c36",
    )
    plot.save(root / "river-section-comparison.png")
    print("PASS terrain overview, refined lake detail and measured river cross-section")


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("directory")
    main(p.parse_args().directory)
