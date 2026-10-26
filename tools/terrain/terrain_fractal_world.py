"""Terrain-first fBm/RMF island: no inherited watershed or river controls."""

import numpy as np
import shapely as sh
from shapely.geometry import Polygon, MultiPoint, box, mapping, Point, shape
from terrain_noise_mountains import gradient_noise
from terrain_landforms import smooth, cover_polygon, sample
from terrain_geography import noisy_plateau_outline, plateau_surface


def fractal(
    x, z, seed, wavelength=950, octaves=6, kind="ridged", h=1.0, lacunarity=2.0
):
    if (
        kind not in ("ridged", "fbm")
        or not 1 <= octaves <= 12
        or wavelength <= 0
        or lacunarity <= 1
        or h <= 0
    ):
        raise ValueError("invalid fractal parameters")
    total = np.zeros(np.broadcast_shapes(np.shape(x), np.shape(z)))
    feedback = np.ones_like(total)
    amplitude_sum = 0.0
    for octave in range(octaves):
        frequency = lacunarity**octave
        n = gradient_noise(x, z, wavelength / frequency, seed + octave * 101)
        amplitude = frequency ** (-h)
        if kind == "ridged":
            signal = (1 - np.minimum(abs(n), 1)) ** 2 * feedback
            feedback = np.clip(signal * 2, 0, 1)
        else:
            signal = n
        total += signal * amplitude
        amplitude_sum += amplitude
    return total / amplitude_sum


def make_island(seed=27, side=1025, extent=6000.0):
    if side < 65 or extent <= 0:
        raise ValueError("island needs a positive extent and at least 65 samples")
    cell = extent / (side - 1)
    axis = np.linspace(0, extent, side)
    x, z = np.meshgrid(axis, axis)
    # Broad fractal landmass and sea threshold. No old polygon or height input.
    continental = (
        1
        - ((x - 0.5 * extent) / (0.39 * extent)) ** 2
        - ((z - 0.5 * extent) / (0.44 * extent)) ** 2
    )
    continental += 0.60 * fractal(
        x, z, seed + 10, extent * 0.32, 4, "fbm"
    ) + 0.13 * gradient_noise(x, z, extent * 0.065, seed + 11)
    contour = cover_polygon(
        np.clip((continental + 1) / 2, 0, 1), cell, box(0, 0, extent, extent), 0.5
    )
    largest = max(sh.get_parts(contour), key=lambda p: p.area)
    land = Polygon(largest.exterior)  # Continental holes are not predeclared lakes.
    mask = sh.contains_xy(land, x, z)
    points = np.column_stack([x.ravel(), z.ravel()])
    coast = sh.distance(sh.points(points), land.boundary).reshape(x.shape)
    return x, z, land, mask, coast, cell


def planning_regions(land, seed, extent, count=65):
    rng = np.random.default_rng(np.random.SeedSequence([seed, 1601]))
    centres = []
    for c in rng.uniform(0, extent, (20000, 2)):
        if not land.covers(Point(c)):
            continue
        if centres and min(np.linalg.norm(c - p) for p in centres) < extent * 0.037:
            continue
        centres.append(c)
        if len(centres) == count:
            break
    if len(centres) != count:
        raise ValueError("could not place planning sites")
    cells = sh.get_parts(
        sh.voronoi_polygons(
            MultiPoint(centres), extend_to=box(0, 0, extent, extent), ordered=True
        )
    )
    return dict(
        seed=seed,
        extent=extent,
        planningRegions=[
            dict(
                id=i,
                site=p.tolist(),
                ring=list(cell.exterior.coords),
                geometry=mapping(cell.intersection(land)),
            )
            for i, (p, cell) in enumerate(zip(centres, cells))
        ],
    )


def construct_height(seed=27, side=1025, extent=6000.0):
    x, z, land, mask, coast, cell = make_island(seed, side, extent)
    planning = planning_regions(land, seed, extent)
    candidates = [
        (r["id"], np.array(r["site"]))
        for r in planning["planningRegions"]
        if land.boundary.distance(Point(r["site"])) > extent * 0.10
    ]
    centres = []
    # Four broad uplift domains; centres organize terrain but never draw crests.
    for k in range(4):
        if not candidates:
            raise ValueError("insufficient uplift domains")

        def score(item):
            d = min(
                [np.linalg.norm(item[1] - p) for _, p in centres]
                or [land.boundary.distance(Point(item[1]))]
            )
            return d, -item[0]

        chosen = max(candidates, key=score)
        centres.append(chosen)
        candidates = [
            v
            for v in candidates
            if v[0] != chosen[0] and np.linalg.norm(v[1] - chosen[1]) > extent * 0.15
        ]
    # Local relief dimensions are metres, independent of the map extent.
    wx = (
        x
        + 170 * gradient_noise(x, z, 900, seed + 20)
        + 55 * gradient_noise(x, z, 310, seed + 21)
    )
    wz = (
        z
        + 170 * gradient_noise(x, z, 900, seed + 22)
        + 55 * gradient_noise(x, z, 310, seed + 23)
    )
    weight = np.zeros_like(x)
    domains = []
    for k, (rid, c) in enumerate(centres):
        a = 0.35 + k * 1.04
        u = (wx - c[0]) * np.cos(a) + (wz - c[1]) * np.sin(a)
        v = -(wx - c[0]) * np.sin(a) + (wz - c[1]) * np.cos(a)
        major = 1100 if k < 2 else 740
        minor = 540 if k < 2 else 630
        envelope = np.exp(-2 * ((u / major) ** 2 + (v / minor) ** 2))
        weight = 1 - (1 - weight) * (1 - envelope)
        domains.append(
            dict(
                id=f"uplift-{rid}",
                parentRegion=rid,
                centre=c.tolist(),
                majorScaleMetres=major,
                minorScaleMetres=minor,
                rotationRadians=a,
            )
        )
    ridge = fractal(wx, wz, seed + 100, 950, 5, "ridged")
    hills = fractal(x, z, seed + 200, 430, 4, "fbm")
    coast_fade = smooth(coast / (220)) * mask
    height = (12 + 14 * hills + weight * (65 + 300 * ridge**1.5)) * coast_fade
    # Two low plateau domains use wide ramps; dedicated cliff faces stay off.
    features = []
    for r in sorted(planning["planningRegions"], key=lambda r: r["id"]):
        c = np.array(r["site"])
        if (
            sample(weight, [c], cell)[0] > 0.06
            or land.boundary.distance(Point(c)) < 450
        ):
            continue
        if (
            features
            and min(np.linalg.norm(c - np.array(f["centre"])) for f in features) < 900
        ):
            continue
        poly = Polygon(r["ring"]).intersection(land).buffer(-80)
        if poly.geom_type != "Polygon" or poly.is_empty or poly.area < 45000:
            continue
        poly = poly.intersection(Point(c).buffer(260))
        if poly.geom_type != "Polygon" or poly.is_empty:
            continue
        try:
            poly, spline, edge = noisy_plateau_outline(poly, seed, r["id"])
        except ValueError:
            continue  # Try the next planning domain when a broad cap cannot fit.
        if not land.buffer(-80).covers(poly.buffer(240)):
            continue
        level = float(sample(height, [c], cell)[0] + 23)
        height = plateau_surface(height, cell, poly, level, c, face=120, apron=150)
        features.append(
            dict(
                id=f'plateau-{r["id"]}',
                type="plateau",
                parentRegion=r["id"],
                centre=c.tolist(),
                geometry=mapping(poly),
                support=mapping(poly.buffer(270)),
                topLevelMetres=level,
                grade=[0.012, -0.006],
                faceWidthMetres=120,
                apronWidthMetres=150,
                spline=spline,
                edgeNoise=edge,
                surfaceModel="broad ramp and gently weathered design cap; no dedicated cliff",
            )
        )
        if len(features) == 2:
            break
    dz, dx = np.gradient(height, cell)
    erodibility = np.maximum(0.18 + 0.25 * smooth(np.hypot(dx, dz) / 0.24), weight)
    pts = sh.points(x.ravel(), z.ravel())
    for f in features:
        cap = sh.covers(shape(f["geometry"]), pts).reshape(x.shape)
        erodibility[cap] = 0.12
        height += cap * 0.5 * gradient_noise(x, z, 90, seed + 300)
    erodibility *= smooth(coast / (40)) * smooth(height / 9) * mask
    height[~mask] = 0
    return height, weight, erodibility, land, coast, planning, features, domains, cell
