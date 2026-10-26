"""Blend explicit plains, hills, ranges and plateau controls on polygon corners."""

import heapq
import numpy as np
import shapely as sh
from shapely.geometry import LineString, Point, mapping
from terrain_landforms import smooth
from terrain_noise_mountains import gradient_noise


def design_elevation(xy, land, seed, extent=48000.0, rolling_relief=False):
    points = sh.points(xy)
    coast = sh.distance(points, land.boundary)
    base = 25 + 0.007 * coast
    features = []
    foothills = []
    if rolling_relief:
        from terrain_rolling_relief import lowland_height, foothill_height

        base = lowland_height(xy, coast, seed)
        features.append(
            dict(
                id="continuous-lowlands",
                type="rolling-lowlands",
                geometry=mapping(land),
                wavelengthsMetres=[18000, 7000, 3500],
                baseMetres=85,
                coastRiseMetresPerMetre=0.003,
                formula="85 * exp(0.48*N18km + 0.28*N7km + 0.14*N3.5km) + 0.003*coastDistance",
                coastFadeMetres=1000,
                purpose="gentle relief on all land, including offshore islands",
            )
        )
    ridges = []
    # Positions belong to composition. Width and height values are metres and
    # never grow simply because an output raster has fewer samples.
    recipes = [
        (
            "northwestern-range",
            [[13500, 26000], [17500, 30500], [22500, 33000], [26500, 34000]],
            3300,
            1550,
        ),
        (
            "southern-range",
            [[14000, 10500], [18500, 15000], [23000, 16500]],
            3700,
            1800,
        ),
        ("eastern-range", [[31000, 22500], [32500, 27000], [32500, 32000]], 3000, 1300),
    ]
    for index, (name, control, width, height) in enumerate(recipes):
        control = np.array(control) * extent / 48000
        line = LineString(control)
        d = sh.distance(points, line)
        along = sh.line_locate_point(line, points) / line.length
        # Distinct summits and saddles along an angular crest, rather than a
        # uniform-height tube. This remains a high-level shape, not erosion.
        modulation = (
            0.70 + 0.20 * np.sin(along * np.pi * (2.7 + index * 0.4) + index) ** 2
        )
        modulation += 0.10 * gradient_noise(
            xy[:, 0], xy[:, 1], 3600, seed + 510 + index
        )
        cross = np.maximum(0, 1 - d / width) ** 1.6
        if rolling_relief:
            foothills.append(foothill_height(d, width, height))
        elevation = height * cross * modulation
        ridges.append(elevation)
        features.append(
            dict(
                id=name,
                type="mountain-range",
                guide=control.tolist(),
                halfWidthMetres=width,
                peakBudgetMetres=height,
                geometry=mapping(line.buffer(width)),
                transition=(
                    "narrow crest/flank plus broad exponential foothills"
                    if rolling_relief
                    else "finite tapered flank"
                ),
            )
        )
        # Tributary ridge/spur, merged continuously into its parent range.
        mid = control[1]
        tip = mid + np.array([(-1) ** index * 2100, 1900])
        branch = LineString([mid, tip])
        branch_d = sh.distance(points, branch)
        taper = 1 - 0.6 * sh.line_locate_point(branch, points) / branch.length
        ridges.append(
            height * 0.55 * np.maximum(0, 1 - branch_d / (width * 0.62)) ** 1.6 * taper
        )
    relief = np.sum(np.array(ridges) ** 4, axis=0) ** 0.25
    if rolling_relief:
        # L4 combination avoids stacking overlapping skirts into new mountains.
        base = base + np.sum(np.array(foothills) ** 4, axis=0) ** 0.25
    h = base + relief
    for name, c, radius, height in [
        ("western-hills", [13000, 20500], 4000, 150),
        ("northern-hills", [26500, 38500], 4200, 190),
    ]:
        c = np.array(c) * extent / 48000
        d = np.linalg.norm(xy - c, axis=1)
        h += height * smooth(1 - d / radius)
        features.append(
            dict(
                id=name,
                type="hills",
                centre=c.tolist(),
                radiusMetres=radius,
                heightBudgetMetres=height,
                geometry=mapping(Point(c).buffer(radius)),
            )
        )
    # Select broad low terrain for caps, so a plateau does not slice through an
    # already assigned mountain range. Preference controls are world positions.
    used = []
    for k, target in enumerate([[28500, 16000], [23000, 25000]]):
        target = np.array(target) * extent / 48000
        candidates = np.flatnonzero((relief < 15) & (coast > 4500))
        candidates = sorted(candidates, key=lambda i: np.linalg.norm(xy[i] - target))
        selected = None
        for i in candidates:
            c = xy[i]
            if any(np.linalg.norm(c - other) < 6000 for other in used):
                continue
            cap = np.linalg.norm(xy - c, axis=1) < 1700
            if np.count_nonzero(cap) < 4 or relief[cap].max() > 120:
                continue
            selected = i
            break
        if selected is None:
            continue
        c = xy[selected]
        used.append(c)
        # Smooth support starts at the true cap edge; caps are nearly planar,
        # with a gentle grade. Uneven radial edge controls remain shared data.
        delta = xy - c
        angle = np.arctan2(delta[:, 1], delta[:, 0])
        radial = np.linalg.norm(delta, axis=1)
        rim = 1700 * (
            1 + 0.12 * np.sin(3 * angle + k) + 0.06 * np.sin(7 * angle + seed)
        )
        signed = radial - rim
        weight = 1 - smooth(signed / 1700)
        level = 360 + 180 * k
        plane = level + 0.002 * delta[:, 0] - 0.001 * delta[:, 1]
        h = h * (1 - weight) + plane * weight
        theta = np.linspace(0, 2 * np.pi, 181)
        rr = 1700 * (1 + 0.12 * np.sin(3 * theta + k) + 0.06 * np.sin(7 * theta + seed))
        ring = c + np.column_stack([np.cos(theta) * rr, np.sin(theta) * rr])
        from shapely.geometry import Polygon

        features.append(
            dict(
                id=f"plateau-{k}",
                type="plateau",
                centre=c.tolist(),
                capLevelMetres=level,
                capRadiusMetres=1700,
                transitionMetres=1700,
                grade=[0.002, -0.001],
                geometry=mapping(Polygon(ring)),
            )
        )
    # Plains are low-relief base terrain, not flattened pits enclosed by hills.
    for name, c, radius in [
        ("western-plain", [10000, 21000], 5500),
        ("southeastern-plain", [31000, 11000], 5000),
    ]:
        c = np.array(c) * extent / 48000
        features.append(
            dict(
                id=name,
                type="plain",
                centre=c.tolist(),
                geometry=mapping(Point(c).buffer(radius).intersection(land)),
                surface="gentle coastal base beneath blended landform supports",
            )
        )
    h *= smooth(coast / 1000)
    return h, features


def preserve_downhill(authored, coastal, adjacent):
    """Minimal graph conditioning with a small positive downstream margin.

    Uses assigned elevations, not coast-distance rank, so separate peaks and
    intervening plains survive. This selects no rivers and creates no lakes.
    """
    h = np.full(len(authored), np.inf)
    parent = np.full(len(authored), -1, int)
    queue = []
    for i in np.flatnonzero(coastal):
        h[i] = 0
        heapq.heappush(queue, (0, int(i)))
    while queue:
        v, i = heapq.heappop(queue)
        if v != h[i]:
            continue
        for j, cost in adjacent[i]:
            if coastal[j]:
                continue
            candidate = max(float(authored[j]), v + 0.05)
            if candidate < h[j]:
                h[j] = candidate
                parent[j] = i
                heapq.heappush(queue, (candidate, j))
    if not np.isfinite(h).all():
        raise ValueError("disconnected elevation composition")
    return h, parent
