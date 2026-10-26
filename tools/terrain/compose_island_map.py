#!/usr/bin/env python3
"""2D island composition: consumes actual C++ rewrite terrain/drainage products.

Produces map polygons, curves, semantic regions and canonical edge records only.
There is no mesh generation or renderer dependency. This is an offline integration
entry point; the C++ runtime driver does not yet dispatch these regional recipes.
"""

import argparse
import os
import tempfile
import hashlib
import heapq
import json
import math
import time
from pathlib import Path
import numpy as np
import shapely as sh
from shapely.geometry import Polygon, Point, LineString, MultiPoint, box, mapping
from shapely.ops import unary_union, split
from shapely.affinity import scale as scale_geometry
from scipy.ndimage import distance_transform_edt, gaussian_filter, map_coordinates
from scipy.interpolate import (
    CubicHermiteSpline,
    LinearNDInterpolator,
    NearestNDInterpolator,
)
from regional_solver_queue import SolverQueue, SolverJob, purpose_seed
from lake_basin_solver import solve_lake_basins
from island_landscape import prepare_landscape
from farm_access_solver import build_farm_access
from farm_field_subdivision import settlement_scale, subdivide_fields
from river_corridor_solver import refine_corridors
from river_hydraulics import refresh_reach, refresh_hydrology


def polys(g):
    if g.is_empty:
        return []
    if g.geom_type == "Polygon":
        return [g]
    return [p for child in getattr(g, "geoms", ()) for p in polys(child)]


def lines(g):
    if g.is_empty:
        return []
    if g.geom_type in ("LineString", "LinearRing"):
        return [LineString(g)]
    return [p for child in getattr(g, "geoms", ()) for p in lines(child)]


def smooth_path(coords, passes=2):
    p = np.asarray(coords, dtype=float)
    for _ in range(passes):
        q = np.empty((2 * len(p), 2))
        q[0], q[-1] = p[0], p[-1]
        q[1:-1:2] = 0.75 * p[:-1] + 0.25 * p[1:]
        q[2:-1:2] = 0.25 * p[:-1] + 0.75 * p[1:]
        p = q
    return p


def oriented_rectangle(p):
    """Minimum-area convex-hull edge frame, in local coordinates for stability."""
    hull = np.array(p.convex_hull.exterior.coords)[:-1]
    origin = hull.mean(axis=0)
    local = hull - origin
    best = None
    for edge in np.roll(local, -1, axis=0) - local:
        length = np.linalg.norm(edge)
        if length < 1e-7:
            continue
        u = edge / length
        v = np.array([-u[1], u[0]])
        projected = np.column_stack([local @ u, local @ v])
        lo, hi = projected.min(axis=0), projected.max(axis=0)
        area = float(np.prod(hi - lo))
        if best is None or area < best[0]:
            corners = np.array(
                [[lo[0], lo[1]], [hi[0], lo[1]], [hi[0], hi[1]], [lo[0], hi[1]]]
            )
            best = (
                area,
                Polygon(corners[:, 0, None] * u + corners[:, 1, None] * v + origin),
            )
    if best is None:
        raise ValueError("degenerate building lot")
    return best[1]


class Island:
    def __init__(self, foundation, *, preview=False):
        foundation = prepare_landscape(foundation, preview=preview)
        self.source = foundation
        self.seed, self.extent = foundation["seed"], foundation["extent"]
        self.land = unary_union(
            [Polygon(c["ring"]) for c in foundation["coasts"] if c["area"] > 0]
        )
        for c in foundation["coasts"]:
            if c["area"] < 0:
                self.land = self.land.difference(Polygon(c["ring"]))
        # The temporary search lattice is a solver implementation detail, not a
        # world partition or presentation surface. Region ownership is Voronoi.
        self.n = 161
        self.spacing = self.extent / (self.n - 1)
        gx, gz = np.meshgrid(
            np.arange(self.n) * self.spacing, np.arange(self.n) * self.spacing
        )
        if "terrainRaster" in foundation:
            raster = foundation["terrainRaster"]
            source = np.asarray(raster["height"]).reshape(
                raster["side"], raster["side"]
            )
            self.height = map_coordinates(
                source, [gz / raster["cellMetres"], gx / raster["cellMetres"]], order=1
            )
        else:
            sites = np.asarray(foundation["terrainSites"])
            scalar = LinearNDInterpolator(sites[:, :2], sites[:, 2])
            nearest = NearestNDInterpolator(sites[:, :2], sites[:, 2])
            raw = scalar(gx, gz)
            self.height = np.where(np.isfinite(raw), raw, nearest(gx, gz))
        dz, dx = np.gradient(self.height, self.spacing)
        self.slope = np.hypot(dx, dz)
        self.grid = np.stack(
            np.meshgrid(
                np.arange(self.n) * self.spacing, np.arange(self.n) * self.spacing
            ),
            -1,
        )
        self.on_land = sh.contains_xy(self.land, self.grid[..., 0], self.grid[..., 1])

    def sample(self, xy, field=None):
        a = np.asarray(xy)
        if a.ndim == 1:
            return float(self.sample(a[None, :], field)[0])
        return map_coordinates(
            self.height if field is None else field,
            [a[..., 1] / self.spacing, a[..., 0] / self.spacing],
            order=1,
            mode="nearest",
        )

    def hydrology(self):
        src = self.source
        if "mapHydrology" in src:
            from island_preview_handoff import decode_hydrology

            return decode_hydrology(src["mapHydrology"])
        nodes = np.array([v["position"] for v in src["nodes"]])
        receivers = [v["receiver"] for v in src["nodes"]]
        incoming = [[] for _ in nodes]
        for i, r in enumerate(receivers):
            if r < len(nodes):
                incoming[r].append(i)
        discharge = {int(r["from"]) - 1: r["discharge"] for r in src["reaches"]}
        accepted = []
        out = []
        # Graph topology/discharge stay frozen. Cubic endpoint tangents and a
        # bounded lateral candidate search refine each physical reach in 2D.
        for reach in src["reaches"]:
            i, j = int(reach["from"]) - 1, int(reach["to"]) - 1
            a, b = nodes[i, [0, 2]], nodes[j, [0, 2]]
            delta = b - a
            length = np.linalg.norm(delta)
            tangent0 = (
                delta
                if not incoming[i]
                else b
                - nodes[max(incoming[i], key=lambda k: discharge.get(k, 0)), [0, 2]]
            )
            r = receivers[j]
            tangent1 = delta if r >= len(nodes) else nodes[r, [0, 2]] - a
            tangent0 = tangent0 / max(1e-9, np.linalg.norm(tangent0)) * length
            tangent1 = tangent1 / max(1e-9, np.linalg.norm(tangent1)) * length
            spline = CubicHermiteSpline(
                [0, 1], np.array([a, b]), np.array([tangent0, tangent1])
            )
            t = np.linspace(0, 1, max(9, int(length / 12)))
            base = spline(t)
            normal = np.array([-delta[1], delta[0]]) / length
            water = nodes[i, 1] * (1 - t) + nodes[j, 1] * t
            max_offset = min(length * 0.06, 12)
            best = None
            for offset in [
                0,
                -max_offset,
                max_offset,
                -0.5 * max_offset,
                0.5 * max_offset,
            ]:
                p = base + np.sin(np.pi * t)[:, None] ** 2 * offset * normal
                curve = LineString(p)
                if not self.land.buffer(0.1).covers(curve):
                    continue
                # Only endpoint contacts are allowed; reject new drainage crossings.
                inner = curve.difference(
                    unary_union([Point(a).buffer(0.2), Point(b).buffer(0.2)])
                )
                if any(inner.crosses(old) or inner.overlaps(old) for old in accepted):
                    continue
                ground = self.sample(p)
                score = np.mean(np.maximum(0, ground - water) ** 2) + 0.02 * offset**2
                if best is None or score < best[0]:
                    best = (score, curve)
            if best is None:
                curve = LineString([a, b])
                if not self.land.buffer(0.1).covers(curve):
                    raise ValueError("source river escapes physical island")
            else:
                curve = best[1]
            accepted.append(curve)
            out.append(
                dict(
                    reach,
                    id=f"river-{i}",
                    geometry=curve,
                    waterFrom=float(nodes[i, 1]),
                    waterTo=float(nodes[j, 1]),
                    sinuosity=curve.length / length,
                )
            )
        out = refine_corridors(out, src["terrainSites"], self.land)
        for reach in out:
            refresh_reach(reach)
        # Widths derive from discharge; depths/velocities use the current gradients.
        water = unary_union(
            [r["geometry"].buffer(r["width"] / 2, quad_segs=4) for r in out]
        ).intersection(self.land)
        main_water = unary_union(
            [
                r["geometry"].buffer(r["width"] / 2, quad_segs=4)
                for r in out
                if r.get("mainChannel", r["discharge"] >= 0.08)
            ]
        ).intersection(self.land)
        lakes = []
        for reach in sorted(out, key=lambda r: -r["discharge"]):
            node = int(reach["from"]) - 1
            centre = Point(reach["geometry"].coords[0])
            if (
                len(incoming[node]) < 2
                or not reach.get("mainChannel", True)
                or nodes[node, 1] > 45
                or centre.distance(self.land.boundary) < 220
                or any(centre.distance(l["geometry"]) < 600 for l in lakes)
            ):
                continue
            curve = reach["geometry"]
            radius = min(90, 45 + reach["discharge"] * 12)
            rng = np.random.default_rng(purpose_seed(self.seed, node, "lake-basin"))
            angles = np.linspace(0, 2 * np.pi, 15, endpoint=False)
            c = np.array(centre.coords[0])
            lake = (
                Polygon(
                    [
                        c
                        + np.array([math.cos(a), math.sin(a)])
                        * radius
                        * rng.uniform(0.7, 1.2)
                        for a in angles
                    ]
                )
                .buffer(15)
                .buffer(-15)
                .intersection(self.land)
            )
            contacts = curve.intersection(lake.boundary)
            points = [
                p
                for p in getattr(contacts, "geoms", [contacts])
                if p.geom_type == "Point"
            ]
            if not points:
                continue
            outlet = max(points, key=lambda p: curve.project(p))
            entries = []
            exits = []
            for r2 in out:
                route = r2["geometry"]
                crossings = route.intersection(lake.boundary)
                for hit in getattr(crossings, "geoms", [crossings]):
                    if hit.geom_type != "Point":
                        continue
                    station = route.project(hit)
                    before = lake.contains(route.interpolate(max(0, station - 0.01)))
                    after = lake.contains(
                        route.interpolate(min(route.length, station + 0.01))
                    )
                    if before != after:
                        (exits if before else entries).append(
                            {
                                "reach": r2["id"],
                                "point": hit,
                                "discharge": r2["discharge"],
                            }
                        )
            if len(exits) != 1 or exits[0]["reach"] != reach["id"]:
                continue
            local = sum(
                l["local"]
                for k, l in enumerate(src["ledger"])
                if lake.contains(Point(nodes[k, [0, 2]]))
            )
            inflow = sum(e["discharge"] for e in entries)
            if abs(inflow + local - reach["discharge"]) > 1e-7:
                continue
            lakes.append(
                {
                    "id": f"lake-{len(lakes)}",
                    "geometry": lake,
                    "receiverReach": reach["id"],
                    "node": node,
                    "outlet": outlet,
                    "inlets": entries,
                    "inflow": inflow,
                    "localRunoff": local,
                    "outflow": reach["discharge"],
                    "storageChangeCumecs": 0.0,
                    "status": "2d-basin-reservation",
                    "requiresElevationSolve": True,
                }
            )
            if len(lakes) == 3:
                break
        main_water = main_water.union(unary_union([l["geometry"] for l in lakes]))
        return {
            "reaches": out,
            "lakes": lakes,
            "water": main_water,
            "drainageFootprint": water,
            "mainWater": main_water,
            "mainChannelThresholdCumecs": 0.08,
            "ledger": src["ledger"],
        }

    def locate(self, hydro):
        river = hydro["mainWater"]
        coast_distance = sh.distance(
            sh.points(self.grid.reshape(-1, 2)), self.land.boundary
        ).reshape((self.n, self.n))
        water_distance = sh.distance(
            sh.points(self.grid.reshape(-1, 2)), river
        ).reshape((self.n, self.n))
        score = (
            np.exp(-self.slope * 12)
            * np.exp(-self.height / 160)
            * np.exp(-(((water_distance - 230) / 900) ** 2))
        )
        score[
            ~self.on_land
            | (coast_distance < 180)
            | (water_distance < 70)
            | (self.slope > 0.18)
        ] = -1
        chosen = []
        candidates = [
            np.array(Polygon(c["ring"]).representative_point().coords[0])
            for c in self.source["planningRegions"]
            if c.get("landUse") == "rural-mosaic"
        ]
        # Major seats first; villages then inherit their nearest administrative seat.
        for i in range(11):
            p = max(candidates, key=lambda p: float(self.sample(p, score)))
            index = tuple(
                np.clip(np.rint(p[::-1] / self.spacing).astype(int), 0, self.n - 1)
            )
            if score[index] <= 0:
                break
            major = i < 2
            radius = 230 if major else 105
            chosen.append(
                {
                    "id": f"settlement-{i}",
                    "position": p,
                    "kind": "city" if major else "village",
                    "radius": radius,
                    "parent": (
                        None
                        if major
                        else min(
                            chosen[:2], key=lambda c: np.linalg.norm(c["position"] - p)
                        )["id"]
                    ),
                }
            )
            d = np.linalg.norm(self.grid - p, axis=-1)
            score[d < (1150 if major else 650)] = -1
        for settlement in chosen:
            owner = min(
                self.source["planningRegions"],
                key=lambda r: Polygon(r["ring"]).distance(
                    Point(settlement["position"])
                ),
            )
            settlement["homeRegion"] = f"province-{owner['id']}"
        if len(chosen) < 4:
            raise ValueError("island does not support required settlement hierarchy")
        return tuple(chosen)

    def roads(self, settlements, hydro):
        # Global grid search can bend around mountains and coasts. Existing roads
        # are cheaper, allowing reuse. Crossing reservations are explicit products.
        pts = [s["position"] for s in settlements]
        connected = {0}
        links = []
        while len(connected) < len(pts):
            _, i, j = min(
                (np.linalg.norm(pts[i] - pts[j]), i, j)
                for i in connected
                for j in range(len(pts))
                if j not in connected
            )
            links.append((i, j))
            connected.add(j)
        water_grid = sh.contains_xy(
            hydro["water"].buffer(10), self.grid[..., 0], self.grid[..., 1]
        )
        base_cost = 1 + 25 * self.slope**2 + water_grid * 8
        base_cost[~self.on_land] = np.inf
        reuse = np.ones_like(base_cost)
        result = []
        directions = [(x, y) for x in [-1, 0, 1] for y in [-1, 0, 1] if x or y]
        for number, (a, b) in enumerate(links):
            origin = tuple(np.rint(pts[a][::-1] / self.spacing).astype(int))
            target = tuple(np.rint(pts[b][::-1] / self.spacing).astype(int))
            todo = [(0.0, origin)]
            distances = {origin: 0.0}
            previous = {}
            while todo:
                distance, p = heapq.heappop(todo)
                if distance != distances[p]:
                    continue
                if p == target:
                    break
                for dz, dx in directions:
                    q = (p[0] + dz, p[1] + dx)
                    if not (0 <= q[0] < self.n and 0 <= q[1] < self.n):
                        continue
                    if (
                        dx
                        and dz
                        and (
                            not np.isfinite(base_cost[p[0], q[1]])
                            or not np.isfinite(base_cost[q[0], p[1]])
                        )
                    ):
                        continue
                    cost = (
                        (base_cost[p] + base_cost[q])
                        * 0.5
                        * math.hypot(dx, dz)
                        * reuse[q]
                    )
                    candidate = distance + cost
                    if candidate < distances.get(q, float("inf")):
                        distances[q] = candidate
                        previous[q] = p
                        heapq.heappush(todo, (candidate, q))
            if target not in distances:
                raise ValueError("required settlement connection is unreachable")
            cells = [target]
            while cells[-1] != origin:
                cells.append(previous[cells[-1]])
            path = np.array(
                [[x * self.spacing, z * self.spacing] for z, x in cells[::-1]]
            )
            route = LineString(smooth_path(path, 2))
            if not self.land.covers(route):
                route = LineString(path)
            width = 9 if a < 2 and b < 2 else 5
            crossings = route.intersection(hydro["water"].buffer(5))
            result.append(
                {
                    "id": f"road-{number}",
                    "from": settlements[a]["id"],
                    "to": settlements[b]["id"],
                    "geometry": route,
                    "corridor": route.buffer(width / 2),
                    "width": width,
                    "crossings": crossings,
                }
            )
            for z, x in cells:
                reuse[max(0, z - 1) : z + 2, max(0, x - 1) : x + 2] = 0.6
        return tuple(result)

    def city(self, settlement, roads, hydro):
        rng = np.random.default_rng(
            purpose_seed(self.seed, settlement["id"], "settlement-layout")
        )
        c = settlement["position"]
        radius = settlement["radius"]
        if radius > 200:
            angles = np.linspace(0, 2 * np.pi, 11, endpoint=False)
            ring = [
                c + np.array([np.cos(a), np.sin(a)]) * radius * rng.uniform(0.8, 1.2)
                for a in angles
            ]
            envelope = Polygon(ring).buffer(18).buffer(-18)
        else:
            local = unary_union(
                [
                    r["geometry"].intersection(Point(c).buffer(radius * 1.65))
                    for r in roads
                ]
            )
            envelope = (
                local.buffer(42, quad_segs=8)
                .union(Point(c).buffer(55))
                .buffer(10)
                .buffer(-10)
            )
        envelope = envelope.intersection(self.land).difference(
            hydro["water"].buffer(12)
        )
        seeds = [c]
        for _ in range(1000):
            p = c + rng.uniform(-radius, radius, 2)
            if envelope.contains(Point(p)) and min(
                np.linalg.norm(p - q) for q in seeds
            ) > (64 if radius > 200 else 35):
                seeds.append(p)
            if len(seeds) >= (28 if radius > 200 else 16):
                break
        # District anchors are selected before their blocks; source road arrivals and
        # terrain high ground determine merchant and defensive locations.
        district_seeds = [c]
        district_kinds = ["market"]
        if radius > 200:
            high = max(seeds, key=lambda p: float(self.sample(p)))
            district_seeds.append(np.asarray(high))
            district_kinds.append("military")
            for angle, kind in [
                (0, "merchant"),
                (2.2, "religious"),
                (4.3, "residential"),
            ]:
                candidate = (
                    c + np.array([math.cos(angle), math.sin(angle)]) * radius * 0.55
                )
                if min(np.linalg.norm(candidate - q) for q in district_seeds) > 10:
                    district_seeds.append(candidate)
                    district_kinds.append(kind)
        districts = []
        if len(district_seeds) > 1:
            for part in polys(
                sh.voronoi_polygons(
                    MultiPoint(district_seeds), extend_to=envelope.envelope
                )
            ):
                owner = min(
                    range(len(district_seeds)),
                    key=lambda i: part.distance(Point(district_seeds[i])),
                )
                for region in polys(part.intersection(envelope)):
                    districts.append(
                        {
                            "id": settlement["id"] + f"-district-{len(districts)}",
                            "kind": district_kinds[owner],
                            "geometry": region,
                        }
                    )
        else:
            districts = [
                {
                    "id": settlement["id"] + "-district-0",
                    "kind": "village",
                    "geometry": envelope,
                }
            ]
        raw_cells = polys(
            sh.voronoi_polygons(MultiPoint(seeds), extend_to=envelope.envelope)
        )
        cells = [
            part for cell in raw_cells for part in polys(cell.intersection(envelope))
        ]
        blocks = []
        buildings = []
        streetlines = [
            r["geometry"].intersection(envelope)
            for r in roads
            if r["geometry"].intersects(envelope)
        ]
        for cell in cells:
            for p in polys(cell.intersection(envelope)):
                streetlines.append(p.boundary)
        streets = unary_union(streetlines).buffer(3 if radius > 200 else 2)
        block_shapes = polys(envelope.difference(streets))
        market = (
            min(
                range(len(block_shapes)),
                key=lambda i: block_shapes[i].distance(Point(c)),
            )
            if block_shapes
            else -1
        )

        def lots(p, depth=0):
            if p.area < 360 or depth >= 8:
                return [p]
            rect = np.array(oriented_rectangle(p).exterior.coords)[:4]
            vectors = np.roll(rect, -1, axis=0) - rect
            u = vectors[np.argmax(np.linalg.norm(vectors, axis=1))]
            u /= np.linalg.norm(u)
            v = np.array([-u[1], u[0]])
            centre = np.array(p.centroid.coords[0]) + u * rng.uniform(
                -0.1, 0.1
            ) * np.sqrt(p.area)
            parts = polys(split(p, LineString([centre - v * 1000, centre + v * 1000])))
            if len(parts) != 2 or min(q.area for q in parts) < 70:
                return [p]
            return [q for part in parts for q in lots(part, depth + 1)]

        for i, p in enumerate(block_shapes):
            if p.area < 100:
                continue
            q = p.representative_point()
            d = q.distance(Point(c))
            district = min(districts, key=lambda k: k["geometry"].distance(q))
            kind = (
                "market"
                if i == market
                else ("merchant" if district["kind"] == "market" else district["kind"])
            )
            blocks.append(
                {
                    "id": settlement["id"] + f"-block-{i}",
                    "geometry": p,
                    "kind": kind,
                    "parent": district["id"],
                }
            )
            if kind == "market":
                continue
            outer = p.buffer(-2)
            if outer.is_empty:
                continue
            if radius > 200 and kind == "military":
                keep = scale_geometry(
                    oriented_rectangle(outer), 0.45, 0.45
                ).intersection(outer)
                buildings.extend(polys(keep.union(outer.difference(outer.buffer(-5)))))
            elif (
                radius > 200
                and kind == "merchant"
                and outer.area > 1500
                and rng.random() < 0.6
            ):
                mask = outer.difference(outer.buffer(-rng.uniform(7, 11)))
                b = outer.bounds
                mask = mask.difference(box(q.x - 2, b[1] - 2, q.x + 2, q.y))
                buildings.extend(polys(mask))
            else:
                for lot in lots(outer):
                    setback = lot.buffer(-rng.uniform(1.2, 2.3))
                    if setback.area < 35:
                        continue
                    footprint = scale_geometry(
                        oriented_rectangle(setback), 0.86, 0.65
                    ).intersection(setback)
                    if radius <= 200 and rng.random() < 0.18:
                        continue
                    buildings.extend(q for q in polys(footprint) if q.area > 25)
        walls = LineString()
        if radius > 200:
            gate_mask = unary_union([r["geometry"].buffer(8) for r in roads])
            walls = envelope.buffer(-5).boundary.difference(gate_mask)
        return {
            "id": settlement["id"],
            "districts": districts,
            "envelope": envelope,
            "streets": streets.intersection(envelope),
            "blocks": blocks,
            "buildings": buildings,
            "walls": walls,
        }

    def ecology(self, hydro):
        # Regional woodland shapes originate in Voronoi domains, never raster
        # cells. Overlapping local lobes supply softer forest silhouettes.
        patches = []
        for region in self.source["planningRegions"]:
            rng = np.random.default_rng(
                purpose_seed(self.seed, region["id"], "woodland")
            )
            domain = Polygon(region["ring"])
            if region.get("biome") == "alpine":
                continue
            centre = domain.representative_point()
            if (
                rng.random() < 0.72
                and float(self.sample(np.array(centre.coords[0]))) < 65
            ):
                continue
            c = np.array(centre.coords[0])
            radius = float(rng.uniform(140, 260))
            lobes = [centre.buffer(radius, quad_segs=12)]
            for _ in range(8):
                angle = rng.uniform(0, 2 * np.pi)
                p = c + np.array([np.cos(angle), np.sin(angle)]) * radius * rng.uniform(
                    0.5, 1.1
                )
                lobes.append(
                    Point(p).buffer(radius * rng.uniform(0.25, 0.6), quad_segs=8)
                )
            patches.append(unary_union(lobes).intersection(domain))
        woodland = (
            unary_union(patches)
            .intersection(self.land.buffer(-12))
            .difference(hydro["water"])
        )
        return tuple(p for p in polys(woodland) if p.area > 6000)

    def estates(self, settlements, roads, cities, hydro, ecology):
        occupied = unary_union([c["envelope"].buffer(20) for c in cities])
        roadmask = unary_union(
            [r["geometry"].buffer(r["width"] / 2 + 4) for r in roads]
        )
        # Terrain suitability and settlement access define countryside before parcels.
        eligible = self.on_land & (self.slope < 0.20) & (self.height < 210)
        terrain = unary_union(
            [
                box(
                    x * self.spacing - self.spacing / 2,
                    z * self.spacing - self.spacing / 2,
                    x * self.spacing + self.spacing / 2,
                    z * self.spacing + self.spacing / 2,
                )
                for z, x in zip(*np.where(eligible))
            ]
        )
        catchments = unary_union(
            [
                Point(s["position"]).buffer(1300 if s["kind"] == "city" else 950)
                for s in settlements
            ]
        )
        rural = (
            self.land.buffer(-30)
            .difference(unary_union(ecology))
            .intersection(catchments)
            .intersection(terrain.buffer(35).buffer(-35))
            .difference(
                occupied.union(roadmask).union(
                    hydro["mainWater"].buffer(18).union(hydro["water"].buffer(3))
                )
            )
        )
        candidates = [
            (f"province-{c['id']}", Polygon(c["ring"]))
            for c in self.source["planningRegions"]
            if c.get("landUse") == "rural-mosaic"
            and f"province-{c['id']}" not in {s["homeRegion"] for s in settlements}
        ]
        estates = []
        for parent, p in sorted(
            candidates, key=lambda item: (item[1].centroid.x, item[1].centroid.y)
        ):
            # Reserve at least 45% of each rural parent as common, meadow or woodland.
            x0, z0, x1, z1 = p.bounds
            lo, hi = x0, x1
            for _ in range(30):
                mid = (lo + hi) / 2
                if (
                    p.intersection(box(x0 - 1, z0 - 1, mid, z1 + 1)).area
                    > p.area * 0.55
                ):
                    hi = mid
                else:
                    lo = mid
            reservation = box(x0 - 1, z0 - 1, lo, z1 + 1)
            for region in polys(p.intersection(rural).intersection(reservation)):
                if region.area < 18000:
                    continue
                centre = region.representative_point()
                road = min(roads, key=lambda r: r["geometry"].distance(centre))[
                    "geometry"
                ]
                d = road.project(centre)
                a = road.interpolate(max(0, d - 20))
                b = road.interpolate(min(road.length, d + 20))
                grain = math.atan2(b.y - a.y, b.x - a.x)
                estates.append(
                    {
                        "id": f"estate-{len(estates)}",
                        "geometry": region,
                        "parent": parent,
                        "grain": grain,
                        "strategy": "enclosed" if len(estates) % 4 else "strips",
                        "halo": 12.0,
                        "settlementScale": settlement_scale(region, cities),
                    }
                )
        return tuple(estates)

    def fields(self, estate):
        return subdivide_fields(self.seed, estate)


def canonical_edges(regions):
    # Node all boundaries once. Exact geometry is shared; directed owner uses are
    # recorded, with no independent per-field boundary noise after reconciliation.
    graph = unary_union([p["geometry"].boundary for p in regions])
    geometries = [p["geometry"] for p in regions]
    tree = sh.STRtree(geometries)
    result = []
    for line in sorted(lines(graph), key=lambda l: l.wkb_hex):
        # Machine-precision junction fragments have no boundary length or owner side.
        if line.length < 1e-7:
            continue
        coords = list(line.coords)
        if tuple(coords[-1]) < tuple(coords[0]):
            coords.reverse()
        segment = LineString(coords)
        point = segment.interpolate(0.5, normalized=True)
        owners = [
            regions[int(i)]["id"]
            for i in tree.query(point.buffer(0.001))
            if geometries[int(i)].boundary.distance(point) < 0.001
        ]
        result.append(
            {
                "id": "edge-" + hashlib.sha256(segment.wkb).hexdigest()[:16],
                "geometry": segment,
                "owners": sorted(owners),
                "kind": "hedgerow",
                "width": 2.5,
            }
        )
    return result


def export_product(path, data):
    def encode(v):
        if isinstance(v, sh.Geometry):
            return mapping(v)
        if isinstance(v, np.ndarray):
            return v.tolist()
        if isinstance(v, np.generic):
            return v.item()
        raise TypeError(type(v).__name__)

    # Publish an entire accepted map atomically, preserving the previous map on
    # serialization/write failure. Unique candidates support concurrent exporters.
    fd, candidate = tempfile.mkstemp(
        prefix=path.name + ".", suffix=".tmp", dir=path.parent
    )
    try:
        with os.fdopen(fd, "w") as stream:
            stream.write(
                json.dumps(data, default=encode, separators=(",", ":"), allow_nan=False)
                + "\n"
            )
        os.replace(candidate, path)
    finally:
        if os.path.exists(candidate):
            os.unlink(candidate)


def compose(foundation, workers=4, *, preview=False):
    island = Island(foundation, preview=preview)
    foundation = island.source
    queue = SolverQueue(workers)
    products = queue.run(
        [
            SolverJob("raw-hydrology", (), lambda _: island.hydrology()),
            SolverJob(
                "hydrology",
                ("raw-hydrology",),
                lambda d: (
                    d["raw-hydrology"]
                    if "mapHydrology" in foundation
                    else refresh_hydrology(
                        solve_lake_basins(
                            d["raw-hydrology"], foundation["terrainSites"], island.land
                        )
                    )
                ),
            ),
            SolverJob(
                "ecology", ("hydrology",), lambda d: island.ecology(d["hydrology"])
            ),
            SolverJob(
                "settlements", ("hydrology",), lambda d: island.locate(d["hydrology"])
            ),
            SolverJob(
                "roads",
                ("settlements", "hydrology"),
                lambda d: island.roads(d["settlements"], d["hydrology"]),
            ),
        ]
    )
    jobs = [
        SolverJob(
            s["id"],
            ("roads", "hydrology"),
            lambda d, s=s: island.city(s, d["roads"], d["hydrology"]),
        )
        for s in products["settlements"]
    ]
    city_products = queue.run(jobs, products)
    cities = [city_products[j.id] for j in sorted(jobs, key=lambda j: j.id)]
    inputs = dict(products, cities=tuple(cities))
    estate_products = queue.run(
        [
            SolverJob(
                "estates",
                ("settlements", "roads", "cities", "hydrology", "ecology"),
                lambda d: island.estates(
                    d["settlements"],
                    d["roads"],
                    d["cities"],
                    d["hydrology"],
                    d["ecology"],
                ),
            )
        ],
        inputs,
    )
    estates = estate_products["estates"]
    jobs = [
        SolverJob(e["id"], ("estates",), lambda d, e=e: island.fields(e))
        for e in estates
    ]
    field_products = queue.run(jobs, {"estates": estates})
    solved = {j.id: field_products[j.id] for j in jobs}
    fields = [f for e in sorted(solved) for f in solved[e]["fields"]]
    edges = canonical_edges(fields)

    def accept_access(d):
        retained = list(d["fields"])
        retired = []
        for _ in range(len(retained) + 1):
            boundary = canonical_edges(retained)
            access = build_farm_access(
                retained,
                boundary,
                d["roads"],
                d["cities"],
                d["hydrology"]["water"],
                island.land,
                island.sample,
            )
            if access["allFieldsConnected"]:
                access["retiredFields"] = retired
                return {"fields": retained, "edges": boundary, "access": access}
            missing = set(access["unservedFields"])
            retired.extend(
                dict(f, reason="no admissible road access within search budget")
                for f in retained
                if f["id"] in missing
            )
            retained = [f for f in retained if f["id"] not in missing]
        raise ValueError("farm access acceptance did not converge")

    access_product = queue.run(
        [
            SolverJob(
                "farm-access", ("fields", "roads", "cities", "hydrology"), accept_access
            )
        ],
        dict(products, fields=fields, cities=cities),
    )["farm-access"]
    fields, edges, access = (
        access_product["fields"],
        access_product["edges"],
        access_product["access"],
    )
    product = {
        "farmAccess": access,
        "compositionRevision": 7,
        "deliveryStatus": "2d-preview" if preview else "production-candidate",
        "terrainAcceptance": foundation.get("terrainAcceptance"),
        "schema": 1,
        "kind": "island-2d-map",
        "seed": island.seed,
        "extent": island.extent,
        "land": island.land,
        "source": foundation["source"],
        "drainageTruncated": foundation["drainageTruncated"],
        "drainageCoverage": foundation.get("drainageCoverage"),
        "hydrology": products["hydrology"],
        "settlements": products["settlements"],
        "roads": products["roads"],
        "landscape": dict(
            foundation["landscape"],
            hydraulicSectionsRequireRefresh=False,
            backwaterSolvePending=True,
        ),
        "terrainSites": foundation["terrainSites"],
        "planningRegions": [
            {
                "biome": c["biome"],
                "landform": c["landform"],
                "landUse": (
                    "settlement-reservation"
                    if f"province-{c['id']}"
                    in {s["homeRegion"] for s in products["settlements"]}
                    else c["landUse"]
                ),
                "id": f"province-{c['id']}",
                "geometry": Polygon(c["ring"]),
                "strategy": "voronoi",
                "parent": min(
                    products["settlements"][:2],
                    key=lambda s: Polygon(c["ring"]).distance(Point(s["position"])),
                )["id"]
                + "-realm",
            }
            for c in foundation["planningRegions"]
        ],
        "cities": cities,
        "woodlands": products["ecology"],
        "estates": estates,
        "fields": fields,
        "fieldEdges": edges,
        "kingdoms": [
            {
                "id": s["id"] + "-realm",
                "seat": s["id"],
                "members": [
                    v["id"]
                    for v in products["settlements"]
                    if v["id"] == s["id"] or v["parent"] == s["id"]
                ],
            }
            for s in products["settlements"]
            if s["kind"] == "city"
        ],
        "margins": [p for k in sorted(solved) for p in solved[k]["margins"]]
        + [f["geometry"] for f in access["retiredFields"]],
        "pending": [
            "continuous topographic route validation",
            "runtime C++ regional dispatch",
            "engineered crossing validation",
            "WFC interiors",
            "regional edge proposal negotiation",
        ],
    }
    if "terrainRaster" in foundation:
        product["terrainRaster"] = foundation["terrainRaster"]
    # Shapes remain in their semantic layers. This is not yet the final planar
    # arrangement: planning envelopes may overlap their owned detail by design.
    product["shapeProducts"] = [
        {"id": "island", "type": "land", "parent": None, "geometry": island.land}
    ]
    for family, kind in [
        ("planningRegions", "province"),
        ("estates", "estate"),
        ("fields", "field"),
    ]:
        product["shapeProducts"].extend(
            {
                "id": p["id"],
                "type": kind,
                "parent": p.get("parent"),
                "geometry": p["geometry"],
            }
            for p in product[family]
        )
    product["shapeProducts"].extend(
        {
            "id": r["id"],
            "type": "road-corridor",
            "parent": "island",
            "geometry": r["corridor"],
        }
        for r in product["roads"]
    )
    product["shapeProducts"].extend(
        {"id": l["id"], "type": "lake", "parent": "island", "geometry": l["geometry"]}
        for l in product["hydrology"]["lakes"]
    )
    product["shapeProducts"].extend(
        {
            "id": t["id"],
            "type": "farm-track-easement",
            "parent": "island",
            "geometry": t["corridor"],
        }
        for t in access["tracks"]
    )
    product["edgeInformation"] = {
        "coast": island.land.boundary,
        "waterBanks": product["hydrology"]["water"].boundary,
        "farmGates": access["gates"],
        "farmTrackSides": [t["corridor"].boundary for t in access["tracks"]],
        "roadSides": [
            {"owner": r["id"], "geometry": r["corridor"].boundary}
            for r in product["roads"]
        ],
        "cityBoundaries": [
            {"owner": c["id"], "geometry": c["envelope"].boundary} for c in cities
        ],
        "fields": edges,
        "frozenForMeshing": False,
    }
    return island, product, queue.trace


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("foundation", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--workers", type=int, default=4)
    args = parser.parse_args()
    started = time.perf_counter()
    args.output.mkdir(parents=True, exist_ok=True)
    island, product, trace = compose(
        json.loads(args.foundation.read_text()), args.workers
    )
    export_product(args.output / "island-map.json", product)
    export_product(args.output / "landscape-foundation.json", island.source)
    export_product(
        args.output / "solver-trace.json",
        {
            "workers": args.workers,
            "barriers": trace,
            "seconds": time.perf_counter() - started,
        },
    )
    print(
        f"PASS 2D composition: {len(product['settlements'])} settlements, {len(product['estates'])} estates, {len(product['fields'])} fields, {len(product['fieldEdges'])} shared edge records"
    )

    mapped = [r for r in product["hydrology"]["reaches"] if r["mainChannel"]]
    before = sum(r["corridorRouting"]["baselineCost"] for r in mapped)
    after = sum(r["corridorRouting"]["acceptedCost"] for r in mapped)
    print(
        f"PASS terrain corridors: {sum(r['corridorRouting']['status']=='lower-terrain-cost' for r in mapped)}/{len(mapped)} improved; cost reduction {100*(1-after/before):.2f}%"
    )
    access = product["farmAccess"]
    print(
        f"PASS farm access: {len(access['gates'])} connected gates, {len(access['tracks'])} tracks, {len(access['retiredFields'])} proposals retained as uncultivated margins"
    )
    sections = [
        s for r in product["hydrology"]["reaches"] for s in r["hydraulicSections"]
    ]
    print(
        f"PASS hydraulic refresh: {sum(s['kind']=='normal-flow' for s in sections)} normal-flow sections; {sum(s['kind']=='lake-backwater' for s in sections)} explicit backwater sections"
    )


if __name__ == "__main__":
    main()
