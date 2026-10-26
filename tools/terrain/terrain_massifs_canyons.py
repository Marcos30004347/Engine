"""Branching massif composition and drainage-attached canyon design."""

import numpy as np
from scipy.ndimage import distance_transform_edt
from shapely.geometry import LineString
from terrain_landforms import smooth
from terrain_noise_mountains import gradient_noise
from terrain_island_drainage import route, accumulate


def compose_massifs(height, features, coast, cell, seed, protected):
    rng = np.random.default_rng(seed + 3500)
    x, z = np.meshgrid(np.arange(len(height)) * cell, np.arange(len(height)) * cell)
    power = np.zeros_like(height)
    support = np.zeros_like(height)
    records = []
    wobble = (
        1
        + 0.18 * gradient_noise(x, z, 1500, seed + 3501)
        + 0.08 * gradient_noise(x, z, 450, seed + 3510)
    )
    crest_noise = (
        1
        + 0.10 * gradient_noise(x, z, 650, seed + 3511)
        + 0.045 * gradient_noise(x, z, max(300, 4 * cell), seed + 3512)
    )
    warped_x = (
        x
        + 850 * gradient_noise(x, z, 4200, seed + 3502)
        + 180 * gradient_noise(x, z, 1100, seed + 3513)
        + 55 * gradient_noise(x, z, 400, seed + 3514)
    )
    warped_z = (
        z
        + 850 * gradient_noise(x, z, 4200, seed + 3503)
        + 180 * gradient_noise(x, z, 1100, seed + 3515)
        + 55 * gradient_noise(x, z, 400, seed + 3516)
    )

    def segment(a, b, ha, hb, width, kind, parent, parent_segment=None):
        a = np.array(a)
        b = np.array(b)
        v = b - a
        length = np.linalg.norm(v)
        if length < 1:
            return
        t = np.clip(
            ((warped_x - a[0]) * v[0] + (warped_z - a[1]) * v[1]) / length**2, 0, 1
        )
        d = np.hypot(warped_x - a[0] - t * v[0], warped_z - a[1] - t * v[1])
        q = np.clip(1 - d / (width * wobble), 0, 1) ** 1.55
        crest = (ha * (1 - t) + hb * t) * crest_noise
        field = crest * q
        np.maximum(power, field, out=power)
        np.maximum(support, smooth(1 - d / (width * 2.4)), out=support)
        records.append(
            dict(
                id=len(records),
                kind=kind,
                parentRange=parent,
                parentSegment=parent_segment,
                points=[a.tolist(), b.tolist()],
                crestMetres=[float(ha), float(hb)],
                halfWidthMetres=float(width),
            )
        )

    for f in features:
        if f["type"] != "mountain-range":
            continue
        line = LineString(f["guide"])
        distances = np.linspace(0, line.length, max(4, int(line.length / 2400) + 1))
        nodes = np.array([line.interpolate(float(d)).coords[0] for d in distances])
        peaks = f["peakBudgetMetres"] * rng.uniform(0.55, 1.0, len(nodes))
        peaks[0] *= 0.6
        peaks[-1] *= 0.6
        for i in range(len(nodes) - 1):
            segment(
                nodes[i],
                nodes[i + 1],
                peaks[i],
                peaks[i + 1],
                rng.uniform(1500, 2700),
                "axial-crest",
                f["id"],
            )
        for i in range(1, len(nodes) - 1):
            tangent = nodes[i + 1] - nodes[i - 1]
            tangent /= np.linalg.norm(tangent)
            for side in rng.choice(
                [-1, 1], size=int(rng.choice([1, 2], p=[0.65, 0.35])), replace=False
            ):
                normal = np.array([-tangent[1], tangent[0]]) * side
                direction = normal * 0.86 + tangent * rng.uniform(-0.5, 0.5)
                direction /= np.linalg.norm(direction)
                elbow = nodes[i] + direction * rng.uniform(1800, 3000)
                tip = (
                    elbow
                    + direction * rng.uniform(1500, 3200)
                    + tangent * rng.uniform(-1200, 1200)
                )
                middle = peaks[i] * rng.uniform(0.48, 0.75)
                segment(
                    nodes[i],
                    elbow,
                    peaks[i],
                    middle,
                    rng.uniform(1100, 1900),
                    "primary-spur",
                    f["id"],
                )
                segment(
                    elbow,
                    tip,
                    middle,
                    80,
                    rng.uniform(800, 1500),
                    "primary-spur",
                    f["id"],
                )
                branch = elbow + (
                    0.65 * direction + 0.75 * tangent * side
                ) * rng.uniform(1500, 2800)
                segment(
                    elbow,
                    branch,
                    middle * 0.85,
                    60,
                    rng.uniform(650, 1050),
                    "secondary-spur",
                    f["id"],
                )
                secondary_id = records[-1]["id"]
                branch_vector = branch - elbow
                branch_axis = branch_vector / np.linalg.norm(branch_vector)
                branch_normal = np.array([-branch_axis[1], branch_axis[0]])
                for fraction in [0.32, 0.66]:
                    start = elbow + fraction * branch_vector
                    start_height = middle * 0.85 * (1 - fraction) + 60 * fraction
                    end = start + (
                        branch_axis * 0.35 + branch_normal * rng.choice([-1, 1])
                    ) * rng.uniform(900, 1700)
                    segment(
                        start,
                        end,
                        start_height,
                        40,
                        rng.uniform(400, 650),
                        "tertiary-spur",
                        f["id"],
                        secondary_id,
                    )
                    tertiary_id = records[-1]["id"]
                    twig_start = start + 0.52 * (end - start)
                    twig_end = twig_start + (
                        branch_axis * rng.choice([-1, 1]) + 0.25 * branch_normal
                    ) * rng.uniform(450, 900)
                    segment(
                        twig_start,
                        twig_end,
                        start_height * 0.48 + 40 * 0.52,
                        20,
                        rng.uniform(240, 380),
                        "quaternary-spur",
                        f["id"],
                        tertiary_id,
                    )
    # Replace the former raised ribbon in its envelope, while extending spurs.
    blend = np.maximum(support, smooth((height - 250) / 450))
    blend *= smooth(coast / 1100) * (height > 0)
    blend[protected] = 0
    base = np.minimum(
        height,
        100
        + 0.004 * coast
        + 30 * gradient_noise(x, z, 2400, seed + 3504)
        + 12 * gradient_noise(x, z, 800, seed + 3505),
    )
    result = height * (1 - blend) + (base + power) * blend
    return result, dict(
        model="five crest/spur levels with asymmetric branching and 4200/1100/400 m domain warp plus 650/300+ m crest roughness",
        ridgeLevels=5,
        segments=records,
        maximumChangeMetres=float(np.abs(result - height).max()),
        volumeChangeCubicMetres=float((result - height).sum() * cell**2),
    )


def carve_canyons(height, mask, coast, cell, rain, seed, protected):
    _, parent, _, order = route(height, mask, "distributed")
    area = accumulate(mask.astype(float) * cell**2, parent, order)
    dz, dx = np.gradient(height, cell)
    eligible = (
        (area > 1.2e6)
        & (height > 160)
        & (height < 850)
        & (coast > 1400)
        & (rain < 1600)
        & (~protected)
        & (np.hypot(dx, dz) > 0.012)
    )
    if not eligible.any():
        return height.copy(), dict(
            active=False, excavatedCubicMetres=0, maximumCutMetres=0
        )
    distance, indices = distance_transform_edt(
        ~eligible, sampling=cell, return_indices=True
    )
    x, z = np.meshgrid(np.arange(len(height)) * cell, np.arange(len(height)) * cell)
    variation = gradient_noise(x, z, 850, seed + 3700)
    width = 260 + 90 * (variation + 1)
    depth = np.minimum(130, 35 + 20 * np.sqrt(area[tuple(indices)] / 1e6))
    wall = smooth((width - distance) / (width * 0.65))
    cut = depth * wall * smooth(coast / 1400) * smooth(height / 250) * mask
    cut[protected] = 0
    # Keep incision bounded by available altitude; later routing re-solves water.
    cut = np.minimum(cut, height * 0.38)
    return height - cut, dict(
        active=True,
        model="drainage-selected dry upland canyon corridors with noisy variable widths; no isolated noise grooves",
        seedCells=int(eligible.sum()),
        excavatedCubicMetres=float(cut.sum() * cell**2),
        maximumCutMetres=float(cut.max()),
        widthRangeMetres=[260, 440],
        depositionSimulated=False,
    )


def breach_outlets(height, mask, protected, cell):
    """Cut the existing spill-tree outlets instead of raising valley interiors.

    A design-stage breach, not least-cost geomorphic sediment transport.
    """
    _, parent, _, order = route(height, mask, "distributed")
    h = height.copy()
    hf = h.ravel()
    pf = parent.ravel()
    mf = mask.ravel()
    locked = protected.ravel()
    for i in order[::-1]:
        j = pf[i]
        if j >= 0 and mf[j] and not locked[j]:
            hf[j] = min(hf[j], max(0.0, hf[i] - 0.02))
    cut = height - h
    return h, dict(
        model="reverse spill-tree outlet incision, 0.02 m per routing step; protected caps retained",
        maximumCutMetres=float(cut.max()),
        excavatedCubicMetres=float(cut.sum() * cell**2),
    )
