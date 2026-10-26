"""Field gates and a shared track graph connected to the public road network.

Track corridors are explicit easements over field ownership, not a second crop
partition. Later material reconciliation subtracts them from cultivation masks.
"""

import heapq
import hashlib
import numpy as np
import shapely as sh
from shapely.geometry import Point, LineString
from shapely.ops import substring, nearest_points
from scipy.spatial import cKDTree


def build_farm_access(fields, edges, roads, cities, water, land, sample):
    inner = sh.union_all([f["geometry"].buffer(-1.55) for f in fields])
    urban = sh.union_all([c["envelope"] for c in cities]).difference(
        sh.union_all([r["geometry"].buffer(r["width"] / 2 + 2) for r in roads])
    )
    forbidden = inner.union(water.buffer(2)).union(urban)
    sh.prepare(forbidden)
    navigable_land = land.buffer(0.01)
    sh.prepare(navigable_land)
    sh.prepare(land)
    positions = []
    identity = {}
    adj = []
    segments = {}
    gate_candidates = {f["id"]: [] for f in fields}

    def node(xy):
        key = tuple(round(float(v), 6) for v in xy)
        if key not in identity:
            identity[key] = len(positions)
            positions.append(np.array(xy, dtype=float))
            adj.append([])
        return identity[key]

    def add(a, b, curve, kind):
        if a == b or curve.length < 1e-6:
            return False
        if not navigable_land.covers(curve) or forbidden.intersects(curve):
            return False
        p = np.array(
            [
                curve.interpolate(t).coords[0]
                for t in np.linspace(
                    0, curve.length, max(3, int(curve.length / 12) + 1)
                )
            ]
        )
        ds = np.linalg.norm(np.diff(p, axis=0), axis=1)
        grades = np.abs(np.diff(sample(p))) / np.maximum(ds, 1e-8)
        if grades.max() > 0.30:
            return False
        key = tuple(sorted((a, b)))
        cost = curve.length * (1 + 20 * np.mean(grades**2))
        if key in segments:
            return True
        segments[key] = {
            "geometry": curve,
            "kind": kind,
            "lengthMetres": curve.length,
            "maximumSampledGrade": float(grades.max()),
            "cost": float(cost),
        }
        adj[a].append((b, cost))
        adj[b].append((a, cost))
        return True

    # Midpoints are stable gate candidates. Junctions share one graph vertex.
    for e in edges:
        curve = e["geometry"]
        mid = curve.length / 2
        a = node(curve.coords[0])
        b = node(curve.coords[-1])
        g = node(curve.interpolate(mid).coords[0])
        add(a, g, substring(curve, 0, mid), "field-boundary")
        add(g, b, substring(curve, mid, curve.length), "field-boundary")
        for owner in e["owners"]:
            gate_candidates[owner].append((g, e["id"]))
    # A deterministic sparse roadmap supplies detours through uncultivated land.
    # It is a search aid, not a region partition or an exported terrain grid.
    x0, y0, x1, y1 = land.bounds
    for row, y in enumerate(np.arange(y0 + 70, y1, 140)):
        for x in np.arange(x0 + 70 + (row % 2) * 70, x1, 140):
            point = Point(x, y)
            if land.contains(point) and not forbidden.intersects(point):
                node((x, y))
    field_count = len(positions)
    # Connect nearby boundary components through common land, never crop interiors.
    if positions:
        tree = cKDTree(positions)
        _, neighbours = tree.query(
            positions, k=min(20, len(positions)), distance_upper_bound=260
        )
        for a, near in enumerate(np.atleast_2d(neighbours)):
            for b in np.atleast_1d(near):
                b = int(b)
                if a < b < field_count:
                    add(a, b, LineString([positions[a], positions[b]]), "common-land")
    terminals = {}
    for a in range(field_count):
        for r in roads:
            q = nearest_points(Point(positions[a]), r["geometry"])[1]
            if q.distance(Point(positions[a])) > 1800:
                continue
            # Do not register unreachable terminal candidates.
            curve = LineString([positions[a], q.coords[0]])
            if forbidden.intersects(curve) or not land.covers(curve):
                continue
            b = node(q.coords[0])
            if add(a, b, curve, "road-approach"):
                terminals[b] = r["id"]
    distance = {i: 0.0 for i in terminals}
    parent = {}
    todo = [(0.0, i) for i in terminals]
    heapq.heapify(todo)
    while todo:
        cost, a = heapq.heappop(todo)
        if cost != distance[a]:
            continue
        for b, weight in adj[a]:
            new = cost + weight
            if new < distance.get(b, float("inf")):
                distance[b] = new
                parent[b] = a
                heapq.heappush(todo, (new, b))
    gates = []
    used = set()
    unserved = []
    for f in fields:
        choices = [
            (distance[g], g, e) for g, e in gate_candidates[f["id"]] if g in distance
        ]
        if not choices:
            unserved.append(f["id"])
            continue
        _, g, edge = min(choices)
        path = [g]
        while path[-1] not in terminals:
            path.append(parent[path[-1]])
        keys = [tuple(sorted((a, b))) for a, b in zip(path, path[1:])]
        used.update(keys)
        gates.append(
            {
                "id": f["id"] + "-gate",
                "field": f["id"],
                "edge": edge,
                "position": Point(positions[g]),
                "widthMetres": 4.0,
                "road": terminals[path[-1]],
                "routeKeys": keys,
                "graphNodes": path,
                "networkCost": distance[g],
            }
        )

    def segment_id(key):
        return "track-" + hashlib.sha256(segments[key]["geometry"].wkb).hexdigest()[:16]

    for g in gates:
        g["tracks"] = [segment_id(k) for k in g.pop("routeKeys")]
    tracks = [
        dict(
            segments[k],
            id=segment_id(k),
            nodes=list(k),
            widthMetres=3.0,
            corridor=segments[k]["geometry"].buffer(1.5),
            ownership="access-easement",
        )
        for k in sorted(used)
    ]
    return {
        "version": 1,
        "gates": gates,
        "tracks": tracks,
        "nodes": [{"id": i, "position": p} for i, p in enumerate(positions)],
        "roadPorts": [{"node": i, "road": r} for i, r in sorted(terminals.items())],
        "unservedFields": unserved,
        "allFieldsConnected": not unserved,
        "maximumGrade": 0.30,
        "gradeSampleSpacingMetres": 12,
        "cropOwnershipIncludesEasements": True,
        "geometryGenerated": False,
    }
