"""Compare old and current scalar elevations at identical scale and contours."""

import argparse, json
from pathlib import Path
import numpy as np
import shapely as sh
from shapely.geometry import shape
from PIL import Image, ImageDraw
from terrain_landforms import sample
from render_terrain_landforms import font


def render(directory):
    root = Path(directory)
    previous = root.parent / "2026-09-12-natural-coasts-v23"
    p = json.loads((root / "elevation-map.json").read_text())
    old = json.loads((previous / "elevation-map.json").read_text())
    extent = p["extent"]
    size = 900
    land = shape(p["land"])
    x, z = np.meshgrid(np.linspace(0, extent, size), np.linspace(extent, 0, size))
    points = np.column_stack([x.ravel(), z.ravel()])
    inside = sh.contains_xy(land, x, z)
    im = Image.new("RGB", (1800, 1030), "#203b3b")
    d = ImageDraw.Draw(im)
    stops = [0, 75, 150, 300, 600, 1100, 1800]
    colors = np.array(
        [
            [118, 162, 107],
            [145, 178, 112],
            [171, 191, 128],
            [185, 178, 124],
            [165, 149, 112],
            [151, 139, 123],
            [235, 229, 211],
        ]
    )
    levels = np.r_[np.arange(25, 301, 25), np.arange(400, 2201, 100)]
    for i, (source, title) in enumerate(
        [
            (previous, "PREVIOUS / isolated relief supports"),
            (root, "CURRENT / rolling ground and broader foothills"),
        ]
    ):
        h = np.load(source / "elevation-metres.npy")
        v = sample(h, points, extent / 1024).reshape(x.shape)
        rgb = np.stack([np.interp(v, stops, colors[:, k]) for k in range(3)], axis=-1)
        bands = np.searchsorted(levels, v, side="right")
        edges = np.zeros_like(inside)
        edges[:-1] |= bands[:-1] != bands[1:]
        edges[:, :-1] |= bands[:, :-1] != bands[:, 1:]
        rgb[edges & inside] *= 0.78
        rgb[~inside] = [41, 94, 115]
        im.paste(Image.fromarray(np.uint8(rgb)), (i * size, 65))
        d.text((i * size + 20, 20), title, font=font(26), fill="white")
    d.text(
        (20, 984),
        "Same geometry, height palette and contours: 25 m below 300 m; then 100 m. Actual generated elevations.",
        font=font(23),
        fill="white",
    )
    im.save(root / "rolling-relief-comparison.png")
    xy = np.array([c["point"] for c in p["corners"]])
    coast = sh.distance(sh.points(xy), land.boundary)
    before = np.array([c["elevationMetres"] for c in old["corners"]])
    after = np.array([c["elevationMetres"] for c in p["corners"]])
    measures = []
    for i, g in enumerate(sorted(sh.get_parts(land), key=lambda g: -g.area)):
        mask = sh.contains_xy(g, xy[:, 0], xy[:, 1]) & (coast > 300)
        measures.append(
            dict(
                island=i,
                interiorCorners=int(mask.sum()),
                beforeP10P50P90Metres=np.percentile(
                    before[mask], [10, 50, 90]
                ).tolist(),
                afterP10P50P90Metres=np.percentile(after[mask], [10, 50, 90]).tolist(),
            )
        )
    # The same graph isolates changes to height from changed region shapes.
    low = (before < 150) & (coast > 1200)
    slope_before = []
    slope_after = []
    for e in p["edges"]:
        a, b = e["corners"]
        if low[a] and low[b]:
            length = np.linalg.norm(xy[a] - xy[b])
            slope_before.append(abs(before[a] - before[b]) / length)
            slope_after.append(abs(after[a] - after[b]) / length)
    metrics = dict(
        islands=measures,
        lowlandEdgeCount=len(slope_after),
        beforeSlopeP50P90=np.percentile(slope_before, [50, 90]).tolist(),
        afterSlopeP50P90=np.percentile(slope_after, [50, 90]).tolist(),
    )
    (root / "rolling-relief-metrics.json").write_text(
        json.dumps(metrics, indent=2) + "\n"
    )
    print(
        "PASS same-scale elevation comparison and measured island/lowland relief: "
        + json.dumps(metrics)
    )


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("directory")
    a = p.parse_args()
    render(a.directory)
