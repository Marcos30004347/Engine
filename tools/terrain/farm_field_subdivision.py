"""Settlement-scaled field subdivision, independent of terrain and meshing.

An estate is a land-use reservation. It must not silently become one oversized
crop field when a preferred cut fails. Shared cut curves are inherited exactly.
"""

import math
import numpy as np
from shapely.geometry import LineString
from shapely.ops import split
from regional_solver_queue import purpose_seed


def polygons(g):
    if g.is_empty:
        return []
    if g.geom_type == "Polygon":
        return [g]
    return [p for child in getattr(g, "geoms", ()) for p in polygons(child)]


def settlement_scale(geometry, cities):
    if not cities:
        raise ValueError("field scale requires a settlement footprint")
    nearest = min(cities, key=lambda c: (geometry.distance(c["envelope"]), c["id"]))
    area = nearest["envelope"].area
    if not math.isfinite(area) or area < 1000:
        raise ValueError("invalid settlement footprint")
    return {
        "settlementId": nearest["id"],
        "settlementAreaMetres2": area,
        "targetAreaMetres2": float(np.clip(area * 0.09, 2500, 6000)),
        "maximumAreaMetres2": min(9000, area * 0.18),
        "minimumAreaMetres2": min(500, area * 0.025),
        "minimumInteriorRadiusMetres": 6.0,
    }


def working_axes(p):
    # Stable local-coordinate rectangle frame; GEOS oriented_envelope emits
    # floating-point warnings on nearly collinear inherited boundary segments.
    hull = np.array(p.convex_hull.exterior.coords)[:-1]
    local = hull - hull.mean(axis=0)
    best = None
    for v in np.roll(local, -1, axis=0) - local:
        length = np.linalg.norm(v)
        if length < 1e-9:
            continue
        u = v / length
        n = np.array([-u[1], u[0]])
        area = np.ptp(local @ u) * np.ptp(local @ n)
        angle = math.atan2(u[1], u[0]) % math.pi
        candidate = (float(area), angle)
        if best is None or candidate < best:
            best = candidate
    if best is None:
        raise ValueError("degenerate working frame")
    return [best[1], best[1] + math.pi / 2]


def subdivide_fields(seed, estate):
    scale = estate["settlementScale"]
    parent = estate["geometry"]
    grain = float(estate["grain"])
    values = [
        scale[k]
        for k in (
            "targetAreaMetres2",
            "maximumAreaMetres2",
            "minimumAreaMetres2",
            "minimumInteriorRadiusMetres",
        )
    ]
    if (
        not parent.is_valid
        or parent.is_empty
        or not math.isfinite(grain)
        or not all(math.isfinite(v) and v > 0 for v in values)
    ):
        raise ValueError("invalid field geometry or scale")
    target, cap, minimum, radius = values
    if minimum >= cap:
        raise ValueError("field minimum exceeds settlement-scaled cap")
    rng = np.random.default_rng(purpose_seed(seed, estate["id"], estate["strategy"]))
    fields = []
    margins = []
    attempts = 0

    def usable(p):
        return (
            p.is_valid
            and p.area >= minimum
            and not p.buffer(-radius).is_empty
            and p.length * p.length / p.area < 85
        )

    def recurse(p, depth):
        nonlocal attempts
        local_target = min(cap, target * float(rng.uniform(0.75, 1.25)))
        if (p.area > cap or p.area > local_target * 1.4) and depth < 16:
            cx, cy = p.centroid.coords[0]
            bounds = p.bounds
            span = max(bounds[2] - bounds[0], bounds[3] - bounds[1]) * 3
            # Preferred bends preserve estate grain. Deterministic straight
            # cuts in the minimum rectangle's two axes provide a robust fallback.
            axes = working_axes(p)
            for attempt in range(36):
                attempts += 1
                if attempt < 20:
                    angle = grain + (depth % 2) * math.pi / 2 + rng.uniform(-0.16, 0.16)
                    if estate["strategy"] == "strips":
                        angle = grain + math.pi / 2 + rng.uniform(-0.08, 0.08)
                    offset = rng.uniform(-0.10, 0.10) * math.sqrt(p.area)
                    bend = rng.uniform(-0.015, 0.015) * math.sqrt(p.area)
                else:
                    angle = axes[(attempt - 20) % 2]
                    offset = ((attempt - 20) // 2 - 3.5) * 0.025 * math.sqrt(p.area)
                    bend = 0
                axis = np.array([math.cos(angle), math.sin(angle)])
                normal = np.array([-axis[1], axis[0]])
                t = np.linspace(-1, 1, 9)
                coords = (
                    np.array([cx, cy])
                    + offset * normal
                    + t[:, None] * span * axis
                    + np.cos(t * math.pi / 2)[:, None] ** 2 * bend * normal
                )
                parts = polygons(split(p, LineString(coords)))
                if len(parts) < 2 or any(not usable(q) for q in parts):
                    continue
                if max(q.area for q in parts) > p.area * 0.94:
                    continue
                for q in sorted(
                    parts, key=lambda q: (q.centroid.x, q.centroid.y, q.area)
                ):
                    recurse(q, depth + 1)
                return
        if p.area <= cap * (1 + 1e-9) and usable(p):
            fields.append(p)
        else:
            margins.append(
                {
                    "geometry": p,
                    "reason": (
                        "oversized-unsplittable"
                        if p.area > cap
                        else "insufficient-working-width"
                    ),
                }
            )

    for part in polygons(parent):
        recurse(part, 0)
    records = [
        {
            "id": estate["id"] + f"-field-{i}",
            "parent": estate.get("sourceEstate", estate["id"]),
            "geometry": p,
            "grain": grain + float(rng.uniform(-0.08, 0.08)),
            "crop": int(rng.integers(0, 6)),
            "recipe": estate["strategy"],
            "settlementScale": scale,
        }
        for i, p in enumerate(fields)
    ]
    return {
        "id": estate["id"],
        "fields": records,
        "margins": [m["geometry"] for m in margins],
        "marginReasons": margins,
        "diagnostics": {
            "splitAttempts": attempts,
            "parentAreaMetres2": parent.area,
            "fieldAreaMetres2": sum(p.area for p in fields),
            "marginAreaMetres2": sum(m["geometry"].area for m in margins),
        },
    }
