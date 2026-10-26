"""Continuous channel profiles and measured depression water, without meshes.

The scalar field keeps the accepted watershed terrain away from channels. Lakes
are physical sublevel sets at their spill elevation, not filled terrain pixels.
Spill storage is a static capacity, not a claim of solved discharge/backwater.
"""

import hashlib
import heapq
import json
from pathlib import Path

import numpy as np
import shapely as sh
from scipy.ndimage import label, map_coordinates
from shapely.geometry import LineString, Point, Polygon, mapping, shape


def read_source(root):
    root = Path(root)
    manifest = json.loads((root / "manifest.json").read_text())
    names = [
        "foundation.json",
        "terrain-map.json",
        "heightmap-metres.npy",
        "land-mask.npy",
    ]
    for name in names:
        expected = [v for k, v in manifest.items() if Path(k).name == name]
        actual = hashlib.sha256((root / name).read_bytes()).hexdigest()
        if len(expected) != 1 or expected[0] != actual:
            raise ValueError(f"source checksum failed: {name}")
    product = json.loads((root / "terrain-map.json").read_text())
    acceptance = product["terrainAcceptance"]
    if (
        not acceptance["numericalPass"]
        or not acceptance["checks"]
        or not all(acceptance["checks"].values())
    ):
        raise ValueError("unaccepted source terrain")
    return (
        product,
        np.load(root / "heightmap-metres.npy"),
        np.load(root / "land-mask.npy"),
    )


class ChannelSurface:
    """Exact segment projections, with a compact cross-section replacement.

    The source is bilinear away from channels. River curves/profiles are piecewise
    linear and preserve shared node beds and discharge. Display rasters are samples
    of this function, not a promise that coarse bilinear raster sampling resolves
    a 2.4 m channel. Exact geometric queries use this evaluator.
    """

    def __init__(self, height, cell, land, rivers):
        self.height = np.asarray(height, dtype=float)
        self.cell = float(cell)
        self.land = land
        if (
            self.height.ndim != 2
            or not np.isfinite(self.height).all()
            or self.cell <= 0
        ):
            raise ValueError("invalid scalar field")
        starts, ends, beds0, beds1, widths0, widths1, depths0, depths1 = (
            [] for _ in range(8)
        )
        node_bed, node_width, node_depth = {}, {}, {}
        for r in rivers:
            for node, h in [(r["from"], r["bed"][0]), (r["to"], r["bed"][-1])]:
                if node in node_bed and abs(node_bed[node] - h) > 1e-7:
                    raise ValueError("inconsistent confluence bed")
                node_bed[node] = h
                node_width[node] = max(node_width.get(node, 0), r["width"])
                node_depth[node] = max(node_depth.get(node, 0), r["depth"])
        for r in rivers:
            points, bed = np.asarray(r["points"]), np.asarray(r["bed"])
            if (
                len(points) < 2
                or len(points) != len(bed)
                or not np.isfinite(points).all()
                or not np.isfinite(bed).all()
            ):
                raise ValueError("invalid river samples")
            lengths = np.linalg.norm(np.diff(points, axis=0), axis=1)
            if (
                np.any(lengths <= 1e-10)
                or np.any(np.diff(bed) > 1e-7)
                or r["width"] <= 0
                or r["depth"] <= 0
            ):
                raise ValueError("invalid river profile")
            u = np.r_[0, np.cumsum(lengths)] / lengths.sum()
            w = (1 - u) * node_width[r["from"]] + u * node_width[r["to"]]
            d = (1 - u) * node_depth[r["from"]] + u * node_depth[r["to"]]
            starts.extend(points[:-1])
            ends.extend(points[1:])
            beds0.extend(bed[:-1])
            beds1.extend(bed[1:])
            widths0.extend(w[:-1])
            widths1.extend(w[1:])
            depths0.extend(d[:-1])
            depths1.extend(d[1:])
        if not starts:
            raise ValueError("no channel constraints")
        self.a, self.b = np.array(starts), np.array(ends)
        self.v = self.b - self.a
        self.length2 = np.sum(self.v * self.v, axis=1)
        self.bed0, self.bed1 = np.array(beds0), np.array(beds1)
        self.width0, self.width1 = np.array(widths0), np.array(widths1)
        self.depth0, self.depth1 = np.array(depths0), np.array(depths1)
        self.river_data = []
        for r in rivers:
            points = np.asarray(r["points"])
            arc = np.r_[0, np.cumsum(np.linalg.norm(np.diff(points, axis=0), axis=1))]
            self.river_data.append(
                (
                    LineString(points),
                    arc,
                    np.asarray(r["bed"]),
                    node_width[r["from"]],
                    node_width[r["to"]],
                    node_depth[r["from"]],
                    node_depth[r["to"]],
                )
            )
        self.river_tree = sh.STRtree([r[0] for r in self.river_data])
        self.maximum_radius = max(max(w / 2 + 12, w * 2) for w in node_width.values())
        self.tree = sh.STRtree(sh.linestrings(np.stack([self.a, self.b], axis=1)))

    def project(self, points):
        points = np.atleast_2d(np.asarray(points, dtype=float))
        if points.shape[1] != 2 or not np.isfinite(points).all():
            raise ValueError("invalid surface query")
        if not len(points):
            return (np.array([]),) * 4
        matches = self.tree.query_nearest(sh.points(points), all_matches=False)
        index = np.empty(len(points), dtype=np.int64)
        index[matches[0]] = matches[1]
        t = np.clip(
            np.sum((points - self.a[index]) * self.v[index], axis=1)
            / self.length2[index],
            0,
            1,
        )
        distance = np.linalg.norm(
            points - self.a[index] - t[:, None] * self.v[index], axis=1
        )

        def lerp(a, b):
            return a[index] * (1 - t) + b[index] * t

        return (
            distance,
            lerp(self.bed0, self.bed1),
            lerp(self.width0, self.width1),
            lerp(self.depth0, self.depth1),
        )

    def sample(self, points):
        points = np.atleast_2d(np.asarray(points, dtype=float))
        if points.shape[1] != 2 or not np.isfinite(points).all():
            raise ValueError("invalid surface query")
        base = map_coordinates(
            self.height,
            [points[:, 1] / self.cell, points[:, 0] / self.cell],
            order=1,
            mode="nearest",
        )
        queries = sh.points(points)
        pairs = self.river_tree.query(
            queries, predicate="dwithin", distance=self.maximum_radius
        )
        numerator = np.zeros(len(points))
        denominator = np.zeros(len(points))
        terrain_weight = np.ones(len(points))
        # Blend *distinct reach* profiles in their common support. Nearest-reach
        # replacement alone leaves a step along the bisector of confluences.
        for index in np.unique(pairs[1]):
            ids = pairs[0, pairs[1] == index]
            curve, arc, beds, wa, wb, da, db = self.river_data[index]
            station = sh.line_locate_point(curve, queries[ids])
            distance = sh.distance(
                queries[ids], sh.line_interpolate_point(curve, station)
            )
            u = station / arc[-1]
            bed = np.interp(station, arc, beds)
            half = ((1 - u) * wa + u * wb) / 2
            depth = (1 - u) * da + u * db
            radius = np.maximum(half + 12, half * 4)
            t = np.clip((distance - half) / (radius - half), 0, 1)
            kernel = 1 - t * t * t * (10 + t * (-15 + 6 * t))
            weight = kernel * (half / np.maximum(distance, 1e-6)) ** 4
            profile = bed + depth * np.minimum(distance / half, 1) ** 2
            numerator[ids] += weight * profile
            denominator[ids] += weight
            terrain_weight[ids] *= 1 - kernel
        result = (numerator + terrain_weight * base) / (denominator + terrain_weight)
        inside = sh.covers(self.land, queries)
        return np.where(inside, result, 0)

    def raster(self, side):
        if not isinstance(side, int) or not 2 <= side <= 4097:
            raise ValueError("invalid scalar sample resolution")
        if self.height.shape[0] != self.height.shape[1]:
            raise ValueError("square scalar source required")
        extent = (self.height.shape[0] - 1) * self.cell
        axis = np.linspace(0, extent, side)
        output = np.empty((side, side))
        for z in range(0, side, 32):
            xg, zg = np.meshgrid(axis, axis[z : z + 32])
            output[z : z + 32] = self.sample(
                np.column_stack([xg.ravel(), zg.ravel()])
            ).reshape(xg.shape)
        return output


def priority_spill(height, land):
    """Six-neighbour priority flood on the same diagonal as shoreline triangles.

    Routing and storage share the SW–NE cell diagonal; the opposite diagonal
    is not a physical edge of this piecewise-linear analysis field.
    Boundary ocean samples are sinks; the caller must reserve an ocean border.
    """
    h = np.asarray(height, dtype=float)
    land = np.asarray(land, dtype=bool)
    if h.shape != land.shape or h.ndim != 2 or not np.isfinite(h).all():
        raise ValueError("invalid basin field")
    if land[0].any() or land[-1].any() or land[:, 0].any() or land[:, -1].any():
        raise ValueError("ocean border required")
    n, m = h.shape
    seen = ~land.copy()
    filled = h.copy()
    parent = np.full(h.size, -1, dtype=np.int32)
    rank = np.full(h.size, -1, dtype=np.int32)
    coast = (
        np.roll(land, 1, 0)
        | np.roll(land, -1, 0)
        | np.roll(land, 1, 1)
        | np.roll(land, -1, 1)
        | np.roll(np.roll(land, 1, 0), 1, 1)
        | np.roll(np.roll(land, -1, 0), -1, 1)
    ) & ~land
    heap = [(float(h.ravel()[i]), int(i)) for i in np.flatnonzero(coast)]
    heapq.heapify(heap)
    order = 0
    while heap:
        level, i = heapq.heappop(heap)
        rank[i] = order
        order += 1
        z, x = divmod(i, m)
        for zz, xx in (
            (z - 1, x),
            (z + 1, x),
            (z, x - 1),
            (z, x + 1),
            (z - 1, x - 1),
            (z + 1, x + 1),
        ):
            if 0 <= zz < n and 0 <= xx < m and not seen[zz, xx]:
                seen[zz, xx] = True
                j = zz * m + xx
                parent[j] = i
                filled[zz, xx] = max(level, h[zz, xx])
                heapq.heappush(heap, (float(filled[zz, xx]), j))
    if np.any(rank[land.ravel()] < 0):
        raise ValueError("unreachable land")
    return filled, parent.reshape(h.shape), rank.reshape(h.shape)


def clipped_triangle(vertices, level):
    """Sublevel polygon and exact storage integral on a linear triangle."""
    result = []
    for a, b in zip(vertices, np.roll(vertices, -1, axis=0)):
        ina, inb = a[2] < level, b[2] < level
        if ina:
            result.append(a)
        if ina != inb:
            result.append(a + (b - a) * ((level - a[2]) / (b[2] - a[2])))
    if len(result) < 3:
        return None, 0.0
    q = np.array(result)
    volume = 0.0
    for k in range(1, len(q) - 1):
        tri = q[[0, k, k + 1]]
        v, w = tri[1, :2] - tri[0, :2], tri[2, :2] - tri[0, :2]
        area = abs(v[0] * w[1] - v[1] * w[0]) / 2
        volume += area * np.maximum(level - tri[:, 2], 0).mean()
    return Polygon(q[:, :2]), float(volume)


def shoreline(height, cell, level, member_indices):
    """Clip triangles adjoining wet nodes, retaining true interpolated shores."""
    n, m = height.shape
    cells = set()
    for i in member_indices:
        z, x = divmod(int(i), m)
        for zz, xx in ((z - 1, x - 1), (z - 1, x), (z, x - 1), (z, x)):
            if 0 <= zz < n - 1 and 0 <= xx < m - 1:
                cells.add((zz, xx))
    pieces = []
    volumes = []
    for z, x in sorted(cells):
        corners = np.array(
            [
                [x * cell, z * cell, height[z, x]],
                [(x + 1) * cell, z * cell, height[z, x + 1]],
                [(x + 1) * cell, (z + 1) * cell, height[z + 1, x + 1]],
                [x * cell, (z + 1) * cell, height[z + 1, x]],
            ]
        )
        for ids in ([0, 1, 2], [0, 2, 3]):
            p, v = clipped_triangle(corners[ids], level)
            if p is not None and p.area > 1e-10:
                pieces.append(p)
                volumes.append(v)
    if not pieces:
        return Polygon(), 0.0
    geom = sh.union_all(pieces, grid_size=1e-8)
    # A diagonal triangulation can expose a separate depression in the same cell;
    # retain only pieces belonging to the selected wet connected component.
    wet = sh.points(
        np.column_stack(
            [np.array(member_indices) % m * cell, np.array(member_indices) // m * cell]
        )
    )
    polygons = [
        p
        for p in sh.get_parts(geom)
        if p.geom_type == "Polygon" and np.any(sh.covers(p, wet))
    ]
    geom = sh.union_all(polygons)
    volume = sum(
        v for p, v in zip(pieces, volumes) if geom.covers(p.representative_point())
    )
    return geom, float(volume)


def realize_basins(height, mask, cell, minimum_depth=0.25):
    if (
        not np.isfinite(cell)
        or cell <= 0
        or not np.isfinite(minimum_depth)
        or minimum_depth <= 0
    ):
        raise ValueError("invalid basin scale")
    filled, parent, rank = priority_spill(height, mask)
    depth = np.where(mask, filled - height, 0)
    # Include shallow margins of every positive depression; threshold selects a
    # body by its maximum depth, not a shrunken shoreline at 0.25 m depth.
    labels, count = label(depth > 1e-8, np.array([[1, 1, 0], [1, 1, 1], [0, 1, 1]]))
    lakes = []
    flatparent = parent.ravel()
    flatfilled = filled.ravel()
    for number in range(1, count + 1):
        ids = np.flatnonzero(labels.ravel() == number)
        if depth.ravel()[ids].max() < minimum_depth or flatfilled[ids].max() <= 0:
            continue
        if np.ptp(flatfilled[ids]) > 1e-7:
            raise ValueError("merged depressions with unequal water levels")
        level = float(flatfilled[ids[0]])
        geom, volume = shoreline(height, cell, level, ids)
        if geom.is_empty or not geom.is_valid:
            raise ValueError("invalid lake shore")
        root = int(ids[np.argmin(rank.ravel()[ids])])
        path = [root]
        visited = {root}
        while mask.ravel()[path[-1]]:
            nxt = int(flatparent[path[-1]])
            if nxt < 0 or nxt in visited:
                raise ValueError("invalid lake outlet path")
            visited.add(nxt)
            path.append(nxt)
        coords = np.column_stack(
            [
                np.array(path) % height.shape[1] * cell,
                np.array(path) // height.shape[1] * cell,
            ]
        )
        spill = int(flatparent[root])
        lakes.append(
            {
                "id": f"lake-{number}",
                "geometry": mapping(geom),
                "waterLevelMetres": level,
                "spillPosition": [
                    float(spill % height.shape[1] * cell),
                    float(spill // height.shape[1] * cell),
                ],
                "maximumDepthMetres": float(depth.ravel()[ids].max()),
                "areaMetres2": float(geom.area),
                "storageCubicMetres": volume,
                "wetSamples": len(ids),
                "spillSample": spill,
                "rootSample": root,
                "outletPath": coords.tolist(),
                "outletRoutingLevelsMetres": flatfilled[path].tolist(),
                "status": "static-spill-capacity",
                "dischargeSolved": False,
            }
        )
    return lakes, filled, depth, parent, rank


def qualify_lakes(lakes, surface, cell):
    """Select resolvable off-channel basins and refine their physical shore.

    Do not mistake under-resolved channel pits for ponds. River/lake coupling is
    a separate solve; bodies intersecting channel support remain explicit pending
    candidates. Refinement uses the actual continuous height function.
    """
    from shapely.affinity import translate

    selected, rejected = [], []
    corridors = sh.union_all(
        [
            r[0].buffer(max(max(r[3], r[4]) / 2 + 12, max(r[3], r[4]) * 2) + cell)
            for r in surface.river_data
        ]
    )
    for lake in lakes:
        polygon = shape(lake["geometry"])
        if polygon.intersects(corridors):
            rejected.append(
                {"id": lake["id"], "reason": "channel-coupling-or-sampling-candidate"}
            )
            continue
        if polygon.area < 300 or lake["maximumDepthMetres"] < 0.75:
            rejected.append({"id": lake["id"], "reason": "below-resolved-lake-preset"})
            continue
        x0, z0, x1, z1 = polygon.bounds
        origin = np.array([x0 - cell * 2, z0 - cell * 2])
        step = cell / 8
        nx = int(np.ceil((x1 - x0 + cell * 4) / step)) + 1
        nz = int(np.ceil((z1 - z0 + cell * 4) / step)) + 1
        gx, gz = np.meshgrid(np.arange(nx) * step, np.arange(nz) * step)
        h = surface.sample(np.column_stack([gx.ravel(), gz.ravel()]) + origin).reshape(
            nz, nx
        )
        root = polygon.representative_point()
        px, pz = np.rint((np.array(root.coords[0]) - origin) / step).astype(int)
        local_mask = np.ones_like(h, dtype=bool)
        local_mask[[0, -1]] = False
        local_mask[:, [0, -1]] = False
        local_spill, local_parent, local_rank = priority_spill(h, local_mask)
        spill_level = float(local_spill[pz, px])
        level = spill_level - 0.05
        components, _ = label(
            h < level - 1e-8, np.array([[1, 1, 0], [1, 1, 1], [0, 1, 1]])
        )
        number = components[pz, px]
        if number == 0:
            rejected.append({"id": lake["id"], "reason": "refined-basin-not-found"})
            continue
        wet = components == number
        if wet[0].any() or wet[-1].any() or wet[:, 0].any() or wet[:, -1].any():
            rejected.append(
                {"id": lake["id"], "reason": "refined-shore-exceeds-reservation"}
            )
            continue
        refined, storage = shoreline(h, step, level, np.flatnonzero(wet))
        refined = translate(refined, xoff=origin[0], yoff=origin[1])
        # Union arithmetic can leave nanometre-wide sliver holes along shared
        # triangle edges. Remove only sub-micrometre-area holes whose sampled
        # terrain is within the 5 mm shoreline tolerance; preserve resolved islands and all larger holes.
        cleaned = []
        removed_hole_area = 0.0
        for part in sh.get_parts(refined):
            holes = []
            for hole in part.interiors:
                area = Polygon(hole).area
                if (
                    area < 1e-6
                    and np.max(surface.sample(np.asarray(hole.coords))) < level + 0.005
                ):
                    removed_hole_area += area
                else:
                    holes.append(hole)
            cleaned.append(Polygon(part.exterior, holes))
        refined = sh.union_all(cleaned)
        linear_shore = refined
        refined = refined.simplify(step * 0.05, preserve_topology=True)
        try:
            refined = fit_shore_level(refined, surface, level, step)
        except ValueError as exc:
            rejected.append({"id": lake["id"], "reason": str(exc)})
            continue
        rings = [np.asarray(p.exterior.coords) for p in sh.get_parts(refined)]
        rings += [
            np.asarray(r.coords) for p in sh.get_parts(refined) for r in p.interiors
        ]
        error = float(np.max(np.abs(surface.sample(np.vstack(rings)) - level)))
        if error > 0.05:
            rejected.append(
                {"id": lake["id"], "reason": "shore-height-error", "errorMetres": error}
            )
            continue
        spill_components, _ = label(
            ((local_spill - h) > 1e-8) & (np.abs(local_spill - spill_level) < 1e-7),
            np.array([[1, 1, 0], [1, 1, 1], [0, 1, 1]]),
        )
        ids = np.flatnonzero(spill_components == spill_components[pz, px])
        spill_root = int(ids[np.argmin(local_rank.ravel()[ids])])
        spill = int(local_parent.ravel()[spill_root])
        record = dict(lake)
        record.update(
            coarseSpillSample=record.pop("spillSample"),
            coarseRootSample=record.pop("rootSample"),
            numericalSliverHoleAreaMetres2=removed_hole_area,
            waterLevelMetres=level,
            spillLevelMetres=spill_level,
            status="static-water-stage-below-spill",
            coarseWaterLevelMetres=lake["waterLevelMetres"],
            spillPosition=(
                np.array([spill % nx * step, spill // nx * step]) + origin
            ).tolist(),
            outletPathStatus="coarse-routing-reference; refined spill connection pending",
            geometry=mapping(refined),
            areaMetres2=float(refined.area),
            storageCubicMetres=storage,
            storageModel="integrated fine piecewise-linear samples; continuous-field estimate",
            contourAdjustmentAreaMetres2=float(
                refined.symmetric_difference(linear_shore).area
            ),
            maximumDepthMetres=float((level - h[wet]).max()),
            shorelineSampleMetres=step,
            maximumShoreErrorMetres=error,
            basinClassification="resolved-off-channel-static-capacity",
        )
        if refined.area < 300 or record["maximumDepthMetres"] < 0.75:
            rejected.append({"id": lake["id"], "reason": "refined-basin-below-preset"})
            continue
        selected.append(record)
    return selected, rejected


def add_lake_primitives(height, cell, land, rivers, count=2):
    """Bounded basin primitives on broad, low-gradient sites outside rivers.

    This is declared procedural landform construction, not simulated erosion.
    The compact depression is C1 at its boundary; the spill solver, not the
    primitive's requested depth, subsequently determines water level and shore.
    """
    h = np.array(height, dtype=float, copy=True)
    n, m = h.shape
    gx, gz = np.meshgrid(np.arange(m) * cell, np.arange(n) * cell)
    points = np.column_stack([gx.ravel(), gz.ravel()])
    water = sh.union_all([LineString(r["points"]).buffer(190) for r in rivers])
    safe = land.buffer(-300).difference(water)
    gradient = np.gradient(h, cell)
    slope = np.hypot(*gradient)
    eligible = sh.contains_xy(safe, gx, gz) & (h > 5) & (h < 45) & (slope < 0.06)
    score = np.where(eligible, np.exp(-(((h - 20) / 16) ** 2)) / (1 + 40 * slope), -1.0)
    controls = []
    for attempt in range(64):
        number = len(controls)
        if number >= count or score.max() < 0:
            break
        z, x = np.unravel_index(np.argmax(score), h.shape)
        centre = np.array([x * cell, z * cell])
        grad = np.array([gradient[1][z, x], gradient[0][z, x]])
        angle = float(np.arctan2(grad[1], grad[0]) + np.pi / 2)
        c, s = np.cos(angle), np.sin(angle)
        dx, dz = gx - centre[0], gz - centre[1]
        u = (c * dx + s * dz) / 125
        v = (-s * dx + c * dz) / 75
        theta = np.arctan2(v, u)
        # Small coherent contour modulation; no independent per-edge jitter.
        r = np.hypot(u, v) / (1 + 0.08 * np.cos(3 * theta + 0.6))
        support = r < 1
        positions = points[support.ravel()]
        if not np.all(sh.covers(safe, sh.points(positions))):
            score[max(0, z - 8) : z + 9, max(0, x - 8) : x + 9] = -1
            continue
        falloff = np.maximum(0, 1 - r * r) ** 2
        cut = 7 * falloff
        h -= cut
        controls.append(
            {
                "id": f"basin-primitive-{number}",
                "centre": centre.tolist(),
                "radiiMetres": [125, 75],
                "angleRadians": angle,
                "maximumCutMetres": float(cut.max()),
                "removedVolumeCubicMetres": float(cut.sum() * cell * cell),
                "type": "compact-procedural-depression",
                "erosionSimulation": False,
            }
        )
        score[np.hypot(dx, dz) < 1400] = -1
    return h, controls


def fit_shore_level(geometry, surface, level, step):
    """Project contour vertices to the continuous level set, then refine edges.

    Bounded Newton steps preserve the selected contour branch. Invalid polygons
    are rejected by the caller, never repaired into a different lake topology.
    """

    def project(points):
        points = np.array(points, copy=True)
        origin = points.copy()
        for _ in range(12):
            error = surface.sample(points) - level
            if np.abs(error).max() < 1e-7:
                break
            epsilon = 0.001
            dx = (
                surface.sample(points + [epsilon, 0])
                - surface.sample(points - [epsilon, 0])
            ) / (2 * epsilon)
            dz = (
                surface.sample(points + [0, epsilon])
                - surface.sample(points - [0, epsilon])
            ) / (2 * epsilon)
            gradient = np.column_stack([dx, dz])
            delta = (
                error[:, None]
                * gradient
                / np.maximum(np.sum(gradient * gradient, axis=1)[:, None], 1e-12)
            )
            length = np.linalg.norm(delta, axis=1)
            delta *= np.minimum(1, step * 0.5 / np.maximum(length, 1e-12))[:, None]
            points -= delta
            if np.linalg.norm(points - origin, axis=1).max() > step * 2:
                raise ValueError("shore projection exceeded collar")
        return points

    def ring(coords):
        points = project(np.array(coords)[:-1])
        for _ in range(8):
            mid = (points + np.roll(points, -1, axis=0)) / 2
            residual = np.abs(surface.sample(mid) - level)
            split = residual > 0.015
            if not split.any():
                break
            inserted = project(mid[split])
            result = []
            j = 0
            for i, p in enumerate(points):
                result.append(p)
                if split[i]:
                    result.append(inserted[j])
                    j += 1
            points = np.array(result)
        return np.vstack([points, points[0]])

    polygons = []
    for polygon in sh.get_parts(geometry):
        polygons.append(
            Polygon(
                ring(polygon.exterior.coords),
                [ring(r.coords) for r in polygon.interiors],
            )
        )
    result = sh.union_all(polygons)
    if not result.is_valid:
        raise ValueError("invalid projected shore")
    return result
