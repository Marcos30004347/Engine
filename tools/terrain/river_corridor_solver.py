"""Bounded terrain-cost river routing with frozen topology and junctions."""

from copy import deepcopy
import numpy as np
import shapely as sh
from shapely.geometry import LineString, Point
from scipy.interpolate import LinearNDInterpolator


def sampled_curve(curve, spacing=8):
    stations = np.linspace(
        0, curve.length, max(9, int(np.ceil(curve.length / spacing)) + 1)
    )
    return np.array([curve.interpolate(t).coords[0] for t in stations])


def route_cost(curve, scalar, water_from, water_to):
    p = sampled_curve(curve)
    ground = np.asarray(scalar(p), dtype=float)
    if not np.isfinite(ground).all():
        return float("inf")
    distance = np.r_[0, np.cumsum(np.linalg.norm(np.diff(p, axis=0), axis=1))]
    water = water_from + (water_to - water_from) * distance / distance[-1]
    mismatch = ground - water
    density = (
        1 + 0.012 * np.maximum(mismatch, 0) ** 2 + 0.06 * np.maximum(-mismatch, 0) ** 2
    )
    return float(np.sum(np.diff(distance) * (density[:-1] + density[1:]) * 0.5))


def corridor_candidate(curve, scalar, water_from, water_to, width=48):
    """Layered shortest path: station, lateral offset; bounded neighbour moves."""
    base = sampled_curve(curve, 14)
    count = len(base)
    offsets = np.linspace(-width, width, 17)
    middle = 8
    tangent = np.gradient(base, axis=0)
    tangent /= np.maximum(np.linalg.norm(tangent, axis=1)[:, None], 1e-9)
    normals = np.column_stack([-tangent[:, 1], tangent[:, 0]])
    points = base[:, None, :] + normals[:, None, :] * offsets[None, :, None]
    h = scalar(points.reshape(-1, 2)).reshape(count, 17)
    water = np.linspace(water_from, water_to, count)[:, None]
    mismatch = h - water
    local = (
        1 + 0.012 * np.maximum(mismatch, 0) ** 2 + 0.06 * np.maximum(-mismatch, 0) ** 2
    )
    local[~np.isfinite(local)] = np.inf
    cost = np.full((count, 17), np.inf)
    previous = np.full((count, 17), -1, int)
    cost[0, middle] = 0
    for k in range(1, count):
        choices = [middle] if k in (1, count - 2, count - 1) else range(17)
        for j in choices:
            for before in range(max(0, j - 2), min(17, j + 3)):
                length = np.linalg.norm(points[k, j] - points[k - 1, before])
                proposal = (
                    cost[k - 1, before]
                    + length * (local[k, j] + local[k - 1, before]) * 0.5
                    + 3 * (j - before) ** 2
                )
                if proposal < cost[k, j]:
                    cost[k, j] = proposal
                    previous[k, j] = before
    if not np.isfinite(cost[-1, middle]):
        return curve
    indices = [middle]
    for k in range(count - 1, 0, -1):
        indices.append(previous[k, indices[-1]])
    p = points[np.arange(count), indices[::-1]]
    # Corner cutting stays local; final acceptance scores the actual smoothed curve.
    for _ in range(2):
        q = np.empty((2 * len(p), 2))
        q[0], q[-1] = p[0], p[-1]
        q[1:-1:2] = 0.75 * p[:-1] + 0.25 * p[1:]
        q[2:-1:2] = 0.25 * p[:-1] + 0.75 * p[1:]
        p = q
    return LineString(p)


def refine_corridors(reaches, sites, land):
    result = deepcopy(reaches)
    sites = np.asarray(sites)
    scalar = LinearNDInterpolator(sites[:, :2], sites[:, 2])
    original = [r["geometry"] for r in result]
    tree = sh.STRtree(original)
    accepted = []
    for index, r in enumerate(result):
        baseline = r["geometry"]
        before = route_cost(baseline, scalar, r["waterFrom"], r["waterTo"])
        best = baseline
        after = before
        reason = "baseline-retained"
        # Frozen source geometry limits movement across other tributary domains.
        for width in ((24, 48) if r.get("mainChannel", True) else ()):

            candidate = corridor_candidate(
                baseline,
                scalar,
                r["waterFrom"],
                r["waterTo"],
                min(width, baseline.length * 0.24),
            )
            p = np.array(candidate.coords)
            chord = p[-1] - p[0]
            if not candidate.is_simple or np.any(np.diff(p, axis=0) @ chord < -1e-7):
                continue
            if not land.buffer(0.1).covers(candidate):
                continue
            endpoints = sh.union_all(
                [Point(p[0]).buffer(0.001), Point(p[-1]).buffer(0.001)]
            )
            obstacles = [
                original[int(j)]
                for j in tree.query(candidate, predicate="intersects")
                if j != index
            ]
            obstacles.extend(old for old in accepted if old.intersects(candidate))
            if any(
                not candidate.intersection(old).difference(endpoints).is_empty
                for old in obstacles
            ):
                continue
            score = route_cost(candidate, scalar, r["waterFrom"], r["waterTo"])
            if score < after * (1 - 1e-8):
                best = candidate
                after = score
                reason = "lower-terrain-cost"
        r["geometry"] = best
        r["sinuosity"] = best.length / Point(best.coords[0]).distance(
            Point(best.coords[-1])
        )
        r["corridorRouting"] = {
            "baselineCost": before,
            "waterFromAtRouting": r["waterFrom"],
            "waterToAtRouting": r["waterTo"],
            "acceptedCost": after,
            "status": reason,
            "maximumSearchOffsetMetres": min(48, baseline.length * 0.24),
            "baselineGeometry": baseline,
        }
        accepted.append(best)
    return result
