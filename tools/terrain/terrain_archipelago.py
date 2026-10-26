"""Direct 2D island composition: broad land, open curved bays, coastal detail."""

import numpy as np
import shapely as sh
from shapely.geometry import Polygon, LineString, MultiPoint, box, mapping
from scipy.interpolate import CubicSpline
from terrain_noise_mountains import gradient_noise


def _outline(centre, radius, seed, aspect=1.0):
    rng = np.random.default_rng(seed)
    t = np.linspace(0, 2 * np.pi, 25)
    r = radius * rng.uniform(0.82, 1.18, len(t))
    r[-1] = r[0]
    angle = np.linspace(
        0, 2 * np.pi, max(180, int(radius * 2 * np.pi / 80)), endpoint=False
    )
    r = CubicSpline(t, r, bc_type="periodic")(angle)
    return Polygon(
        np.column_stack(
            [centre[0] + r * np.cos(angle), centre[1] + aspect * r * np.sin(angle)]
        )
    )


def _coastal_detail(poly, seed):
    # Sample by metres, not by the output image resolution. Smooth vector noise
    # adds coves/headlands; the coarse bay composition controls the large turns.
    ring = sh.segmentize(poly.exterior, 45)
    p = np.array(ring.coords)
    delta = np.zeros_like(p)
    for amplitude, wavelength, offset in [(600, 3000, 0), (180, 800, 2), (50, 220, 4)]:
        for k in range(2):
            delta[:, k] += amplitude * gradient_noise(
                p[:, 0], p[:, 1], wavelength, seed + 900 + offset + k
            )
    for attenuation in [1, 0.5, 0.25, 0]:
        result = Polygon(p + attenuation * delta)
        if result.is_valid:
            return result
    raise ValueError("invalid coastal detail")


def archipelago(seed, extent):
    main = _outline(np.array([0.49, 0.50]) * extent, 0.335 * extent, seed + 71, 1.04)
    # Curved cubic channels begin outside land, creating open bays, never lakes.
    recipes = [
        [[-0.05, 0.42], [0.12, 0.32], [0.18, 0.51], [0.30, 0.46]],
        [[1.05, 0.56], [0.86, 0.65], [0.82, 0.46], [0.70, 0.52]],
        [[0.57, 1.05], [0.47, 0.84], [0.69, 0.85], [0.60, 0.75]],
    ]
    bays = []
    for i, control in enumerate(recipes):
        p = np.array(control) * extent
        t = np.linspace(0, 1, 240)[:, None]
        points = (
            (1 - t) ** 3 * p[0]
            + 3 * (1 - t) ** 2 * t * p[1]
            + 3 * (1 - t) * t * t * p[2]
            + t**3 * p[3]
        )
        line = LineString(points)
        width = extent * [0.022, 0.020, 0.016][i]
        main = main.difference(line.buffer(width, quad_segs=24))
        bays.append(
            dict(id=f"bay-{i}", centreline=points.tolist(), halfWidthMetres=width)
        )
    main = max(sh.get_parts(main), key=lambda p: p.area)
    main = _coastal_detail(Polygon(main.exterior), seed)
    parts = [main]
    for i, (x, y, r, aspect) in enumerate(
        [
            (0.14, 0.78, 0.067, 1.3),
            (0.88, 0.75, 0.060, 0.95),
            (0.84, 0.20, 0.062, 1.1),
            (0.17, 0.16, 0.047, 0.9),
        ]
    ):
        island = _outline(np.array([x, y]) * extent, r * extent, seed + 150 + i, aspect)
        island = _coastal_detail(island, seed + i + 1)
        island = island.difference(sh.union_all(parts).buffer(1001, quad_segs=32))
        parts.extend(
            Polygon(p.exterior) for p in sh.get_parts(island) if p.area > 1_000_000
        )
    land = sh.union_all(parts)
    if not land.is_valid or not box(0, 0, extent, extent).covers(land):
        raise ValueError("invalid archipelago or coast outside map")
    return land, dict(
        method="periodic spline island outlines, open cubic bays, multiscale coastal vector noise",
        islandCount=len(sh.get_parts(land)),
        coastDetailMetres=[3000, 800, 220],
        coastSamplesEveryMetres=45,
        minimumIslandAreaMetres2=1_000_000,
        minimumIslandSeparationMetres=1000,
        bays=bays,
        geometry=mapping(land),
    )


def island_cells(land, seed, count):
    """Area-budgeted, independently relaxed Voronoi regions on every island."""
    parts = sorted(sh.get_parts(land), key=lambda p: -p.area)
    if count < len(parts) * 8:
        raise ValueError("need at least eight sites per island")
    quota = (count - len(parts) * 8) * np.array([p.area for p in parts]) / land.area
    counts = np.floor(quota).astype(int) + 8
    for i in np.argsort(-(quota - np.floor(quota)))[: count - int(counts.sum())]:
        counts[i] += 1
    cells, sites, islands = [], [], []
    for island_id, (part, n) in enumerate(zip(parts, counts)):
        rng = np.random.default_rng(np.random.SeedSequence([seed, 1900, island_id]))
        lo, hi = np.array(part.bounds[:2]), np.array(part.bounds[2:])
        accepted = []
        while sum(len(p) for p in accepted) < n:
            p = rng.uniform(lo, hi, (int(n) * 3, 2))
            accepted.append(p[sh.contains_xy(part, p[:, 0], p[:, 1])])
        current = np.concatenate(accepted)[:n]
        for iteration in range(3):
            raw = sh.get_parts(
                sh.voronoi_polygons(
                    MultiPoint(current), extend_to=box(*part.bounds), ordered=True
                )
            )
            clipped = [p.intersection(part) for p in raw]
            if iteration < 2:
                centres = [
                    p.centroid if p.covers(p.centroid) else p.representative_point()
                    for p in clipped
                ]
                current = np.array([[p.x, p.y] for p in centres])
        cells.extend(clipped)
        sites.extend(current)
        islands.extend([island_id] * int(n))
    return cells, np.array(sites), islands
