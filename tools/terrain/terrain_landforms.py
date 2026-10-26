"""2D watershed refinement, canonical coasts, connected water and land covers.

The SIGGRAPH 2013 graph/dual crests are source constraints. Profile sharpening,
particle erosion, spline scarps and seasonal cover are explicit extensions.
No mesh is constructed by this module.
"""

import copy
import json
import subprocess
import tempfile
from pathlib import Path

import contourpy
import numpy as np
import shapely as sh
from scipy.ndimage import gaussian_filter1d, map_coordinates, convolve
from scipy.spatial import cKDTree
from shapely.geometry import LineString, Point, Polygon, mapping, shape
from terrain_water_realization import ChannelSurface


def smooth(t):
    t = np.clip(t, 0, 1)
    return t**3 * (10 + t * (-15 + 6 * t))


def sample(a, points, cell):
    p = np.atleast_2d(points)
    return map_coordinates(a, [p[:, 1] / cell, p[:, 0] / cell], order=1, mode="nearest")


def canonical_coast(land, spacing=6.0, smoothing=36.0):
    """Uniform arclength low-pass; preserve coves larger than the 36 m filter.

    Filtering the supplied contour, rather than independent edge jitter, removes
    spikes without inventing new disconnected fragments or changing island scale.
    """

    def ring(r):
        line = LineString(r)
        n = max(32, int(np.ceil(line.length / spacing)))
        p = sh.get_coordinates(
            sh.line_interpolate_point(line, np.arange(n) * line.length / n)
        )
        p = gaussian_filter1d(p, smoothing / (line.length / n), axis=0, mode="wrap")
        return np.vstack([p, p[0]])

    parts = [
        Polygon(ring(p.exterior), [ring(h) for h in p.interiors])
        for p in sh.get_parts(land)
    ]
    result = sh.union_all(parts)
    if not result.is_valid or len(sh.get_parts(result)) != len(parts):
        raise ValueError("coast filtering changed topology")
    if abs(result.area / land.area - 1) > 0.03:
        raise ValueError("coast filtering exceeded 3% area budget")
    return result


def coherent_noise(x, z, wavelength, seed):
    """Cubic lattice noise in world metres; shared by both sides of boundaries."""
    rng = np.random.default_rng(seed)
    a = rng.uniform(
        -1, 1, (int(np.max(z) / wavelength) + 5, int(np.max(x) / wavelength) + 5)
    )
    return map_coordinates(
        a, [z / wavelength + 1, x / wavelength + 1], order=3, mode="nearest"
    )


def sharpen_watersheds(
    f, old_height, weight, rivers, land, side=1025, angular=False, crest_noise=False
):
    extent = f["extent"]
    cell = extent / (side - 1)
    x, z = np.meshgrid(np.linspace(0, extent, side), np.linspace(0, extent, side))
    pts = np.column_stack([x.ravel(), z.ravel()])
    oldcell = extent / (len(old_height) - 1)
    base = sample(old_height, pts, oldcell).reshape(x.shape)
    w = sample(weight, pts, oldcell).reshape(x.shape)
    river = ChannelSurface(old_height, oldcell, land, rivers)
    dr, bed, _, _ = river.project(pts)
    # The paper includes mouth POINT primitives even when growth from that
    # mouth has no accepted edge. Omitting them invents an elevated coastal
    # plateau by borrowing the bed of a distant highland stream.
    for node in f["nodes"]:
        if node["receiver"] < len(f["nodes"]):
            continue
        mouth = np.array(node["position"])[[0, 2]]
        distance = np.linalg.norm(pts - mouth, axis=1)
        nearer = distance < dr
        dr[nearer] = distance[nearer]
        bed[nearer] = 0.0

    vertices = np.array([v["position"] for v in f["crestVertices"]])
    # Apply the regional slope budget to CREST controls before interpolation;
    # multiplying an entire finished mountain by a province mask creates domes
    # around that mask's perimeter, unrelated to watershed geometry.
    cw = sample(weight, vertices[:, [0, 2]], oldcell)
    cd = sh.distance(sh.points(vertices[:, [0, 2]]), land.boundary)
    low = 8 * (1 - np.exp(-cd / 180)) + 0.012 * cd
    node_beds = {}
    for reach in rivers:
        node_beds[reach["from"] - 1] = reach["bed"][0] + 1.5
        node_beds[reach["to"] - 1] = reach["bed"][-1] + 1.5
    for i, vertex in enumerate(f["crestVertices"]):
        if vertex["coastal"]:
            vertices[i, 1] = 0.0
            continue
        floor = max((node_beds.get(n, 0) for n in vertex["sites"]), default=0.0)
        vertices[i, 1] = max(floor, low[i] * (1 - cw[i]) + vertices[i, 1] * cw[i])
    # Section 5.1/6.2: project only onto crest edges belonging to the
    # containing river-node Voronoi cell, not an unrelated nearby watershed.
    nodes = np.array([n["position"] for n in f["nodes"]])[:, [0, 2]]
    owners = cKDTree(nodes).query(pts)[1]
    dc = np.full(len(pts), np.inf)
    crest = np.zeros(len(pts))
    for owner in np.unique(owners):
        ids = np.flatnonzero(owners == owner)
        queries = pts[ids]
        for edge in f["watershedEdges"]:
            if edge["riverCrossing"] or owner not in (edge["left"], edge["right"]):
                continue
            a, b = vertices[edge["a"]], vertices[edge["b"]]
            av = a[[0, 2]]
            bv = b[[0, 2]]
            v = bv - av
            u = np.clip((queries - av) @ v / max(v @ v, 1e-12), 0, 1)
            distance = np.linalg.norm(queries - av - u[:, None] * v, axis=1)
            nearer = distance < dc[ids]
            chosen = ids[nearer]
            dc[chosen] = distance[nearer]
            crest[chosen] = a[1] + u[nearer] * (b[1] - a[1])
    unresolved = ~np.isfinite(dc)
    dc[unresolved] = sh.distance(sh.points(pts[unresolved]), land.boundary)
    if angular:
        # Share the same crest projection on both sides of a divide. Owner-only
        # nearest-edge switches can introduce a height step when the compact
        # cross-crest filter is removed. Only real, non-river-crossing edges are
        # eligible; source vertices remain canonical at branch junctions.
        edges = [e for e in f["watershedEdges"] if not e["riverCrossing"]]
        a = vertices[[e["a"] for e in edges]]
        b = vertices[[e["b"] for e in edges]]
        if crest_noise:
            from terrain_crest_noise import crest_segments

            a, b = crest_segments(vertices, edges, f["seed"])
        av, bv = a[:, [0, 2]], b[:, [0, 2]]
        tree = sh.STRtree(sh.linestrings(np.stack([av, bv], axis=1)))
        matches = tree.query_nearest(sh.points(pts), all_matches=False)
        index = np.empty(len(pts), dtype=int)
        index[matches[0]] = matches[1]
        v = bv[index] - av[index]
        u = np.clip(
            np.sum((pts - av[index]) * v, axis=1)
            / np.maximum(np.sum(v * v, axis=1), 1e-12),
            0,
            1,
        )
        dc = np.linalg.norm(pts - av[index] - u[:, None] * v, axis=1)
        crest = a[index, 1] * (1 - u) + b[index, 1] * u
    # Evaluate the river-to-crest profile at scalar queries, instead of blending
    # only heights at sparse disk centres. This retains angular watershed crests.
    t = dr / np.maximum(dr + dc, 1e-6)
    relief = np.maximum(crest - bed, 0)
    profile = (
        bed + np.maximum(1.15 * crest - bed, 0) * t**1.35
        if angular
        else bed + 1.2 * relief * t**1.65
    )
    # Only the selected mountain provinces change. Valley anchors are untouched.
    blend = (0.98 if angular else 0.8) * w.ravel() * smooth(dr / 35)
    h = (1 - blend) * base.ravel() + blend * profile
    residual = sum(
        amplitude * coherent_noise(x, z, wave, f["seed"] + offset)
        for wave, amplitude, offset in [(160, 1.0, 11), (65, 0.45, 12), (28, 0.18, 13)]
    )
    amplitude = np.minimum(8, 0.08 * relief).reshape(x.shape) * w
    protect = smooth(dr.reshape(x.shape) / 35)
    h = h.reshape(x.shape)
    # Section 7 compact blend: dense scalar fragments of radius 38 m keep
    # cell interfaces continuous. This is a deterministic quadrature of the
    # compact kernel, not a mesh or an independent mountain noise field.
    offsets = np.arange(-int(np.ceil(38 / cell)), int(np.ceil(38 / cell)) + 1) * cell
    kx, kz = np.meshgrid(offsets, offsets)
    kernel = np.maximum(0, 1 - (kx * kx + kz * kz) / 38**2) ** 2
    filtered = convolve(h, kernel / kernel.sum(), mode="nearest")
    if angular:
        # A symmetric convolution across a divide turns its cusp into a tube.
        # Keep the one-sided river-to-crest profiles at the divide, and blend
        # toward the compact filter only on the flanks. Crest noise is likewise
        # attenuated so the tip is an actual height constraint, not a soft bump.
        flank = smooth(dc.reshape(x.shape) / 55)
        h = (1 - flank) * h + flank * filtered
        protect = protect * smooth(dc.reshape(x.shape) / 14)
    else:
        h = filtered
    h = h + amplitude * protect * residual
    inside = sh.contains_xy(land, x, z)
    coast = sh.distance(sh.points(pts), land.boundary).reshape(x.shape)
    # Regenerate the coastal slope from the canonical contour. Do not preserve
    # the old triangular height taper along the replaced jagged boundary.
    shore = 0.045 * coast + 0.00015 * coast**2
    k = smooth(coast / 150)
    h = np.where(
        inside, np.maximum(0, (1 - k) * shore + k * np.minimum(h, 0.35 * coast)), 0
    )
    return h, w, dr.reshape(x.shape), dc.reshape(x.shape), coast, inside


def erode_slopes(
    h,
    cell,
    weight,
    river_distance,
    crest_distance,
    mask,
    seed,
    binary,
    crest_protection=12,
    erosion_settings=None,
):
    resistance = (
        weight
        * smooth((river_distance - 24) / 35)
        * smooth((crest_distance - crest_protection) / 35)
        * mask
    )
    initial = h.copy()
    ledgers = []
    with tempfile.TemporaryDirectory(prefix="terrain-landforms-") as tmp:
        ip, op = Path(tmp) / "input.json", Path(tmp) / "output.json"
        for i in range(2):
            ip.write_text(
                json.dumps(
                    dict(
                        side=len(h),
                        cellMetres=cell,
                        seed=seed + i,
                        height=h.ravel().tolist(),
                        mask=resistance.ravel().tolist(),
                        **(
                            {"erosionSettings": erosion_settings}
                            if erosion_settings is not None
                            else {}
                        ),
                    )
                )
            )
            subprocess.run([str(Path(binary).resolve()), str(ip), str(op)], check=True)
            result = json.loads(op.read_text())
            h = np.array(result["height"]).reshape(h.shape)
            ledgers.append(result["ledger"])
    return h, dict(
        passes=ledgers,
        addedPasses=2,
        settings=erosion_settings,
        protectedCrestMetres=crest_protection,
        totalHeightVolumeLossCubicMetres=float((initial - h).sum() * cell**2),
    )


def spline_cliffs(h, cell, weight, river_distance):
    """Select rock slopes and cut supported scarps with paired crest/toe curves."""
    dz, dx = np.gradient(h, cell)
    slope = np.hypot(dx, dz)
    x, z = np.meshgrid(np.arange(len(h)) * cell, np.arange(len(h)) * cell)
    eligible = (
        (h > 65)
        & (h < 195)
        & (weight > 0.85)
        & (river_distance > 140)
        & (slope > 0.22)
        & (slope < 0.65)
    )
    score = np.where(eligible, slope, -1.0)
    features = []
    for i in range(3):
        iz, ix = np.unravel_index(score.argmax(), score.shape)
        if score[iz, ix] < 0:
            break
        c = np.array([x[iz, ix], z[iz, ix]])
        # Normal points downhill; a gentle curved contour follows the slope.
        normal = -np.array([dx[iz, ix], dz[iz, ix]]) / slope[iz, ix]
        tangent = np.array([-normal[1], normal[0]])
        along = (x - c[0]) * tangent[0] + (z - c[1]) * tangent[1]
        across = (
            (x - c[0]) * normal[0] + (z - c[1]) * normal[1] - 13 * np.sin(along / 95)
        )
        envelope = smooth((145 - np.abs(along)) / 50)
        cut = 20 * envelope * smooth(across / 12) * (1 - smooth((across - 24) / 90))
        # Water corridors remain authoritative and immutable.
        cut *= smooth((river_distance - 35) / 65)
        h = np.maximum(0, h - cut)
        u = np.linspace(-135, 135, 91)
        crest = c + u[:, None] * tangent + 13 * np.sin(u / 95)[:, None] * normal
        toe = crest + 12 * normal
        features.append(
            dict(
                id=f"cliff-{i+1}",
                type="spline-scarp",
                crest=crest.tolist(),
                toe=toe.tolist(),
                crestHeightMetres=sample(h, crest, cell).tolist(),
                toeHeightMetres=sample(h, toe, cell).tolist(),
                dropMetres=20.0,
                transitionMetres=12.0,
                supportLengthMetres=290.0,
                supportWidthMetres=114.0,
                geometry=mapping(LineString(crest).buffer(18)),
            )
        )
        score[(x - c[0]) ** 2 + (z - c[1]) ** 2 < 650**2] = -1
    if len(features) < 2:
        raise ValueError("not enough suitable rock slopes for cliff fixture")
    return h, features


def river_network(source, height, weight, cell, land, minimum_source=70.0):
    """Mapped rivers are highland source-to-ocean chains; other runoff is lateral.

    Retain the paper graph's flow at each surviving reach. The difference between
    outgoing flow and surviving tributaries is recorded as local/diffuse runoff.
    """
    allr = {r["from"]: r for r in source}
    has_incoming = {r["to"] for r in source}
    heads = [
        i
        for i, r in allr.items()
        if i not in has_incoming
        and r["bed"][0] >= minimum_source
        and sample(weight, [r["points"][0]], cell)[0] > 0.5
        and sample(height, [r["points"][0]], cell)[0] >= minimum_source
    ]
    if not heads:
        raise ValueError("no high-ground river sources")
    keep = set()
    for head in heads:
        node = head
        seen = set()
        while node in allr:
            if node in seen:
                raise ValueError("river cycle")
            seen.add(node)
            keep.add(node)
            node = allr[node]["to"]
    rivers = [copy.deepcopy(allr[i]) for i in sorted(keep)]
    # Shared water-surface and bed controls at junctions. Width/depth laws are
    # monotone with discharge; water surface is continuous at all incident ports.
    depths = {}
    for r in rivers:
        r["width"] = 2.8 + 4.6 * np.sqrt(r["discharge"])
        r["depth"] = 0.3 + 0.42 * r["discharge"] ** 0.4
        r["velocity"] = r["discharge"] / ((2 / 3) * r["width"] * r["depth"])
        r["velocityModel"] = (
            "nominal parabolic section Q/A; lake and confluence backwater not solved"
        )
        for node in [r["from"], r["to"]]:
            depths[node] = max(depths.get(node, 0), r["depth"])
    for r in rivers:
        points = np.array(r["points"])
        line = LineString(points)
        u = (
            np.r_[0, np.cumsum(np.linalg.norm(np.diff(points, axis=0), axis=1))]
            / line.length
        )
        r["waterSurfaceMetres"] = (np.array(r["bed"]) + 1.5).tolist()
        if r["to"] not in keep:
            # Extend the last tangent through the NEW coast, then end offshore.
            tail = points[-1] - points[max(0, len(points) - 12)]
            tail /= np.linalg.norm(tail)
            extended = LineString(np.vstack([points, points[-1] + tail * 1000]))
            intersections = sh.get_coordinates(extended.intersection(land.boundary))
            stations = sorted(
                extended.project(Point(q))
                for q in intersections
                if extended.project(Point(q)) > line.length * 0.4
            )
            if not stations:
                raise ValueError("river mouth cannot reach canonical ocean")
            station = stations[0]
            arc = np.r_[0, np.cumsum(np.linalg.norm(np.diff(points, axis=0), axis=1))]
            retain = arc < station - 1e-6
            mouth = np.array(extended.interpolate(station).coords[0])
            offshore = np.array(extended.interpolate(station + 30).coords[0])
            newpoints = np.vstack([points[retain], mouth, offshore])
            # Reparameterize coastal water height to sea level at the shoreline.
            newarc = np.r_[
                0, np.cumsum(np.linalg.norm(np.diff(newpoints, axis=0), axis=1))
            ]
            level = np.interp(
                newarc, [0, station, station + 30], [r["waterSurfaceMetres"][0], 0, 0]
            )
            r.update(
                points=newpoints.tolist(),
                waterSurfaceMetres=level.tolist(),
                mouthPosition=mouth.tolist(),
                terminal="ocean",
            )
            r["bed"] = (level - depths[r["to"]]).tolist()
        else:
            r["terminal"] = "junction"
    # Coastal regrading must preserve shared node bed; recompute all profiles
    # from shared water controls, allowing different channel depths downstream.
    nodelevel = {}
    for r in rivers:
        nodelevel[r["from"]] = r["waterSurfaceMetres"][0]
        nodelevel[r["to"]] = r["waterSurfaceMetres"][-1]
    for r in rivers:
        p = np.array(r["points"])
        arc = np.r_[0, np.cumsum(np.linalg.norm(np.diff(p, axis=0), axis=1))]
        u = arc / arc[-1]
        level = np.asarray(r["waterSurfaceMetres"])
        # Water levels cannot go uphill on near-flat source reaches.
        end = min(nodelevel[r["from"]], nodelevel[r["to"]])
        level = (
            np.interp(arc, [0, arc[-1]], [nodelevel[r["from"]], end])
            if r["terminal"] != "ocean"
            else level
        )
        r["waterSurfaceMetres"] = level.tolist()
        r["bed"] = (
            level - ((1 - u) * depths[r["from"]] + u * depths[r["to"]])
        ).tolist()
        r["localRunoffCumecs"] = r["discharge"] - sum(
            q["discharge"] for q in rivers if q["to"] == r["from"]
        )
        if r["localRunoffCumecs"] < -1e-7:
            raise ValueError("negative runoff ledger")
    return rivers, heads


def connected_lakes(rivers, land, count=2, seed=27):
    """River-valley lake primitives with explicit two-port steady flow.

    Fixed design stage on a monotone reach; not an off-channel pond or a dynamic
    rainfall simulation. Water level becomes constant between the two ports.
    """
    lines = [LineString(r["points"]) for r in rivers]
    lakes = []
    candidates = sorted(range(len(rivers)), key=lambda i: -rivers[i]["discharge"])
    for index in candidates:
        r = rivers[index]
        line = lines[index]
        levels = np.array(r["waterSurfaceMetres"])
        drop = levels[0] - levels[-1]
        if line.length < 225 or drop > 8 or not 3 < levels.mean() < 120:
            continue
        c = np.array(line.interpolate(0.5, normalized=True).coords[0])
        if any(Point(c).distance(shape(l["geometry"])) < 650 for l in lakes):
            continue
        t = np.array(line.interpolate(0.6, normalized=True).coords[0]) - np.array(
            line.interpolate(0.4, normalized=True).coords[0]
        )
        t /= np.linalg.norm(t)
        normal = np.array([-t[1], t[0]])
        rng = np.random.default_rng(np.random.SeedSequence([seed, r["from"], 901]))
        long_radius = float(rng.uniform(78, 90))
        short_radius = float(rng.uniform(50, 67))
        phase = rng.uniform(0, 2 * np.pi, 2)
        theta = np.linspace(0, 2 * np.pi, 193)
        modulation = (
            1
            + 0.09 * np.cos(3 * theta + phase[0])
            + 0.035 * np.sin(5 * theta + phase[1])
        )
        p = c + (
            (long_radius * np.cos(theta) * modulation)[:, None] * t
            + (short_radius * np.sin(theta) * modulation)[:, None] * normal
        )
        polygon = Polygon(p)
        if not land.buffer(-100).covers(polygon):
            continue
        if any(i != index and q.distance(polygon) < 18 for i, q in enumerate(lines)):
            continue
        hits = sh.get_coordinates(line.intersection(polygon.boundary))
        if len(hits) != 2:
            continue
        ports = sorted([(line.project(Point(q)), q) for q in hits], key=lambda a: a[0])
        (a, inlet), (b, outlet) = ports
        if a < 20 or b > line.length - 20:
            continue
        arc = np.r_[
            0, np.cumsum(np.linalg.norm(np.diff(np.array(r["points"]), axis=0), axis=1))
        ]
        stage = float(np.interp((a + b) / 2, arc, levels))
        # Insert the exact ports into the reach, so topology is not inferred
        # from a nearest sample or a painted line passing close to the lake.
        newarc = np.unique(np.r_[arc, a, b])
        points = sh.get_coordinates(sh.line_interpolate_point(line, newarc))
        newlevels = np.interp(
            newarc, [0, a, b, line.length], [levels[0], stage, stage, levels[-1]]
        )
        depths = np.interp(
            newarc,
            [0, line.length],
            [levels[0] - r["bed"][0], levels[-1] - r["bed"][-1]],
        )
        r.update(
            points=points.tolist(),
            waterSurfaceMetres=newlevels.tolist(),
            bed=(newlevels - depths).tolist(),
        )
        lakes.append(
            dict(
                id=f"lake-{len(lakes)+1}",
                type="connected-valley-lake",
                geometry=mapping(polygon),
                riverFrom=r["from"],
                waterLevelMetres=stage,
                designDepthMetres=float(rng.uniform(5.5, 8.0)),
                inletPosition=inlet.tolist(),
                outletPosition=outlet.tolist(),
                inletStationMetres=a,
                outletStationMetres=b,
                inletDischargeCumecs=r["discharge"],
                outletDischargeCumecs=r["discharge"],
                storageChangeCumecs=0.0,
                waterModel="steady through-flow at design stage",
                dynamicBackwaterSolved=False,
                shorePortExclusionMetres=max(12, r["width"] * 2),
                areaMetres2=polygon.area,
            )
        )
        if len(lakes) == count:
            break
    if len(lakes) < count:
        raise ValueError(f"only {len(lakes)} connected lake sites; requested {count}")
    return lakes


class LandscapeSurface:
    """Canonical continuous terrain queries. Raster products are approximations."""

    def __init__(self, height, cell, land, rivers, lakes):
        # Lake beds may rise towards a spill; the channel replacement constructor
        # expects downhill river beds. Initial constructed profiles retain this.
        self.channel = ChannelSurface(height, cell, land, rivers)
        self.land = land
        self.height = height
        self.cell = cell
        self.lakes = [
            (
                shape(l["geometry"]),
                l,
                LineString(
                    next(r for r in rivers if r["from"] == l["riverFrom"])["points"]
                ),
            )
            for l in lakes
        ]

    def sample(self, points):
        pts = np.atleast_2d(points)
        queries = sh.points(pts)
        h = self.channel.sample(pts)
        for polygon, lake, line in self.lakes:
            d = sh.distance(queries, polygon.boundary)
            inside = sh.covers(polygon, queries)
            band = (d < 45) | inside
            if not band.any():
                continue
            level = lake["waterLevelMetres"]
            lakebed = level - lake["designDepthMetres"] * smooth(d / 42)
            collar = level + 0.18 * d
            proposed = np.where(inside, lakebed, collar)
            blend = np.where(inside, 1, 1 - smooth(d / 45))
            # Restore the inlet/outlet corridors through the shore collar.
            line_dist = sh.distance(queries, line)
            port = smooth(line_dist / lake["shorePortExclusionMetres"])
            outside = (1 - blend * port) * h + blend * port * proposed
            interior = port * lakebed + (1 - port) * np.minimum(h, lakebed)
            h = np.where(band, np.where(inside, interior, outside), h)
        return np.where(sh.covers(self.land, queries), h, 0.0)

    def raster(self, side):
        axis = np.linspace(0, (len(self.height) - 1) * self.cell, side)
        out = np.empty((side, side))
        for row in range(0, side, 32):
            x, z = np.meshgrid(axis, axis[row : row + 32])
            out[row : row + 32] = self.sample(
                np.column_stack([x.ravel(), z.ravel()])
            ).reshape(x.shape)
        return out


def cover_fields(h, cell, land, coast, cliffs, rivers, lakes, season="winter"):
    if season not in ("winter", "summer"):
        raise ValueError("unknown season")
    x, z = np.meshgrid(np.arange(len(h)) * cell, np.arange(len(h)) * cell)
    pts = sh.points(x.ravel(), z.ravel())
    mask = sh.contains_xy(land, x, z)
    dz, dx = np.gradient(h, cell)
    slope = np.hypot(dx, dz)
    # A maritime cold-season preset supports transient hilltop snow at this
    # 6 km / ~300 m scale. Summer explicitly melts it; this is not alpine ice.
    temperature = (0.65 if season == "winter" else 16.0) - 0.0065 * h
    snow = (
        smooth((0.15 - temperature) / 0.65) * (1 - smooth((slope - 0.55) / 0.65)) * mask
    )
    rock = smooth((slope - 0.28) / 0.5) * smooth((h - 35) / 45) * mask
    water = sh.union_all(
        [shape(l["geometry"]) for l in lakes]
        + [LineString(r["points"]).buffer(r["width"] / 2) for r in rivers]
    )
    freshwater = sh.covers(water, pts).reshape(h.shape)
    sand = (
        (1 - smooth((coast - 25) / 32))
        * (1 - smooth((h - 2) / 3))
        * (1 - smooth((slope - 0.1) / 0.18))
        * mask
    )
    sand[freshwater] = 0
    snow[freshwater] = 0
    rock[freshwater] = 0
    return dict(
        temperature=temperature,
        snow=snow,
        rock=rock,
        sand=sand,
        slope=slope,
        waterMask=freshwater,
    )


def cover_polygon(field, cell, land, threshold=0.35):
    cg = contourpy.contour_generator(
        x=np.arange(len(field)) * cell,
        y=np.arange(len(field)) * cell,
        z=field,
        fill_type="OuterOffset",
    )
    points, offsets = cg.filled(threshold, 1.01)
    polygons = []
    for p, o in zip(points, offsets):
        rings = [p[a:b] for a, b in zip(o[:-1], o[1:])]
        poly = Polygon(rings[0], rings[1:])
        if poly.area > 30:
            polygons.append(poly)
    return sh.union_all(polygons).intersection(land)


def ridge_sections(f, before, after, weight, cell):
    """Predeclared cross-sections at ALL eligible watershed-edge midpoints.

    Eligibility uses source geometry/ownership, never whether a result improved.
    Prominence is centre height minus mean height 50 m either side of the crest.
    """
    vertices = np.array([v["position"] for v in f["crestVertices"]])
    rows = []
    distances = np.linspace(-120, 120, 97)
    for index, edge in enumerate(f["watershedEdges"]):
        if edge["riverCrossing"]:
            continue
        a, b = vertices[edge["a"]][[0, 2]], vertices[edge["b"]][[0, 2]]
        v = b - a
        length = np.linalg.norm(v)
        c = (a + b) / 2
        if length < 90 or sample(weight, [c], cell)[0] < 0.9:
            continue
        normal = np.array([-v[1], v[0]]) / length
        p = c + distances[:, None] * normal
        old = sample(before, p, f["extent"] / (len(before) - 1))
        new = sample(after, p, cell)

        def prominence(values):
            return float(
                np.interp(0, distances, values)
                - 0.5
                * (np.interp(-50, distances, values) + np.interp(50, distances, values))
            )

        rows.append(
            dict(
                edge=index,
                centre=c.tolist(),
                distancesMetres=distances.tolist(),
                beforeMetres=old.tolist(),
                afterMetres=new.tolist(),
                beforeProminenceMetres=prominence(old),
                afterProminenceMetres=prominence(new),
            )
        )
    return rows
