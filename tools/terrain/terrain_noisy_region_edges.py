"""One deterministic, corridor-constrained curve per shared polygon edge."""

import numpy as np
import shapely as sh
from shapely.geometry import Polygon, LineString, mapping, shape
from terrain_noise_mountains import gradient_noise


def curve(a, b, sites, seed):
    """Bend within adjacent centre/corner fans, keeping shared endpoints fixed.

    This uses bounded multiscale displacement rather than copying the blog's
    recursive quadrilateral subdivision. Fan corridors partition each convex
    Voronoi cell, so neighbouring edge details cannot claim each other's space.
    """
    a, b = np.asarray(a, float), np.asarray(b, float)
    length = np.linalg.norm(b - a)
    if length < 30 or len(sites) != 2:
        return np.array([a, b]), None
    fan = sh.union_all([Polygon([a, b, c]) for c in sites])
    t = np.linspace(0, 1, max(3, int(np.ceil(length / 15)) + 1))
    base = a[None, :] * (1 - t[:, None]) + b[None, :] * t[:, None]
    normal = np.array([-(b - a)[1], (b - a)[0]]) / length
    amplitude = min(760, length * 0.38)
    signal = (
        0.60 * gradient_noise(base[:, 0], base[:, 1], 650, seed)
        + 0.28 * gradient_noise(base[:, 0], base[:, 1], 200, seed + 1)
        + 0.12 * gradient_noise(base[:, 0], base[:, 1], 75, seed + 2)
    )
    displacement = signal * np.sin(np.pi * t) * amplitude
    for _ in range(10):
        p = base + displacement[:, None] * normal
        p[0] = a
        p[-1] = b
        line = LineString(p)
        if line.is_simple and fan.buffer(1e-6).covers(line):
            return p, fan
        displacement *= 0.5
    return np.array([a, b]), fan


def apply_noisy_edges(product):
    corners = np.array([c["point"] for c in product["corners"]])
    regions = product["regions"]
    land = shape(product["land"])
    from terrain_junction_detail import warp_points

    moved = warp_points(corners, land, product["seed"])
    for c, point in zip(product["corners"], moved):
        c["displayPoint"] = point.tolist()
    edge_by_pair = {}
    changed = 0
    max_deviation = 0.0
    for index, e in enumerate(product["edges"]):
        a, b = e["corners"]
        sites = [regions[i]["site"] for i in e["regions"]]
        p, fan = curve(corners[a], corners[b], sites, product["seed"] + index * 13)
        if fan is not None and not land.buffer(1e-5).covers(LineString(p)):
            p = np.array([corners[a], corners[b]])
        if len(sites) == 2:
            p = warp_points(p, land, product["seed"])
        # Reuse exactly the same endpoint values for every incident curve.
        p[0], p[-1] = moved[a], moved[b]
        e["points"] = p.tolist()
        e["detailSpacingMetres"] = 15
        e["sharedCurveId"] = index
        edge_by_pair[(a, b)] = e
        deviation = LineString(p).hausdorff_distance(
            LineString([corners[a], corners[b]])
        )
        changed += deviation > 0.1
        max_deviation = max(max_deviation, deviation)
    for r in regions:
        parts = []
        for ring in r["cornerRings"]:
            path = []
            for a, b in zip(ring, ring[1:] + ring[:1]):
                e = edge_by_pair[tuple(sorted((a, b)))]
                p = e["points"]
                if a != e["corners"][0]:
                    p = p[::-1]
                path.extend(p[:-1])
            poly = Polygon(path)
            if not poly.is_valid:
                raise ValueError(f'invalid noisy region {r["id"]}')
            parts.append(poly)
        geom = sh.union_all(parts)
        r["logicalGeometry"] = r["geometry"]
        r["geometry"] = mapping(geom)
        r["areaMetres2"] = float(geom.area)
    parts = [shape(r["geometry"]) for r in regions]
    union = sh.union_all(parts)
    mismatch = union.symmetric_difference(land).area
    overlap = sum(p.area for p in parts) - union.area
    if mismatch > 0.1 or overlap > 0.1:
        raise ValueError(
            f"noisy region coverage mismatch {mismatch}, overlap {overlap}"
        )
    # Grouping regions into biomes later uses these same polygons/curves. This
    # elevation-only preview groups by terrain tier, without inventing biomes.
    product["terrainClassContours"] = {
        name: mapping(
            sh.union_all(
                [shape(r["geometry"]) for r in regions if r["terrainClass"] == name]
            )
        )
        for name in ["lowland", "hill", "mountain"]
    }
    product["noisyEdges"] = dict(
        method="shared multiscale curves constrained to adjacent centre/corner fans",
        changedEdges=int(changed),
        maximumDeviationMetres=float(max_deviation),
        nominalWavelengthsMetres=[650, 200, 75],
        samplesEveryMetres=15,
        maximumJunctionMoveMetres=float(np.linalg.norm(moved - corners, axis=1).max()),
        movedJunctions=int(
            np.count_nonzero(np.linalg.norm(moved - corners, axis=1) > 1)
        ),
        junctionMaskRadiusMetres=120,
        junctionBlendMetres=55,
        junctionWarp="shared world-coordinate 4000 m noise, 800 m amplitude before coast fade",
        coastUnchanged=True,
        coverageErrorMetres2=float(mismatch),
        overlapAreaMetres2=float(overlap),
        biomeReuse="union these region polygons after biome assignment; never regenerate edges per biome",
    )
    product["metrics"]["terrainAreaMetres2"] = {
        name: sum(r["areaMetres2"] for r in regions if r["terrainClass"] == name)
        for name in ["lowland", "hill", "mountain"]
    }
    return product
