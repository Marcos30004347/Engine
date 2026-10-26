"""2D geographic features and conservative regional climate transport.

The SIGGRAPH hydrology terrain remains the mountain source. These geology and
climate operators are separate extensions; they do not create a render mesh.
"""

import numpy as np
import shapely as sh
from shapely.geometry import Polygon, LineString, Point, mapping, shape
from scipy.ndimage import gaussian_filter1d, distance_transform_edt
from scipy.interpolate import CubicSpline
from terrain_landforms import sample, smooth, cover_polygon, coherent_noise

BIOMES = ("desert", "dry-scrub", "grassland", "woodland", "rainforest", "cold-heath")
COLOURS = np.array(
    [
        [201, 164, 104],
        [170, 164, 105],
        [137, 164, 100],
        [84, 132, 87],
        [40, 103, 78],
        [152, 160, 139],
    ]
)
PRESETS = {
    "temperate": dict(
        temperature=13.0, moisture=9000.0, evaporation=650.0, pet=800.0, stylized=False
    ),
    "humid-tropical": dict(
        temperature=26.0,
        moisture=24000.0,
        evaporation=1200.0,
        pet=1250.0,
        stylized=False,
    ),
    "arid": dict(
        temperature=25.0, moisture=1500.0, evaporation=50.0, pet=1600.0, stylized=False
    ),
    "mixed-showcase": dict(
        temperature=25.0, moisture=4800.0, evaporation=180.0, pet=1300.0, stylized=True
    ),
}


def transport(
    centres, areas, heights, edges, wind=(1.0, 0.0), moisture=2000.0, evaporation=100.0
):
    """DAG advection; each outgoing split shares, rather than copies, water.

    Volume units are annual mm*m². This is a configurable climate proxy, not a
    weather model. Incoming air is supplied only at upwind graph boundaries.
    Edge conductance uses shared interface length and its wind projection.
    """
    centres, areas, heights, wind = map(np.asarray, (centres, areas, heights, wind))
    if (
        len(centres) != len(areas)
        or len(areas) != len(heights)
        or np.any(areas <= 0)
        or not all(np.isfinite(a).all() for a in (centres, areas, heights, wind))
        or np.linalg.norm(wind) <= 0
        or min(moisture, evaporation) < 0
    ):
        raise ValueError("invalid climate graph")
    wind = wind / np.linalg.norm(wind)
    projection = centres @ wind
    order = np.lexsort((np.arange(len(areas)), projection))
    outgoing = [[] for _ in areas]
    indegree = np.zeros(len(areas), int)
    for a, b, length in edges:
        a, b = int(a), int(b)
        if (
            not (0 <= a < len(areas) and 0 <= b < len(areas))
            or a == b
            or not np.isfinite(length)
            or length <= 0
        ):
            raise ValueError("invalid climate edge")
        delta = projection[b] - projection[a]
        if abs(delta) < 1e-9:
            continue
        if delta < 0:
            a, b = b, a
        conductance = length * abs(delta) / np.linalg.norm(centres[b] - centres[a])
        outgoing[a].append((b, conductance))
        indegree[b] += 1
    incoming = np.zeros(len(areas))
    rain = np.zeros(len(areas))
    carried_height = np.zeros(len(areas))
    injected = evaporated = exported = 0.0
    ledger = []
    for i in order:
        supply = moisture * areas[i] if indegree[i] == 0 else 0.0
        evap = evaporation * areas[i]
        upstream_h = carried_height[i] / incoming[i] if incoming[i] > 0 else 0.0
        available = incoming[i] + supply + evap
        fraction = np.clip(0.20 + max(0.0, heights[i] - upstream_h) / 250.0, 0.20, 0.85)
        rain[i] = available * fraction
        remaining = available - rain[i]
        total = sum(c for _, c in outgoing[i])
        for b, conductance in outgoing[i]:
            flux = remaining * conductance / total
            incoming[b] += flux
            carried_height[b] += flux * heights[i]
        out = remaining if not outgoing[i] else 0.0
        injected += supply
        evaporated += evap
        exported += out
        ledger.append(
            dict(
                region=int(i),
                incoming=float(incoming[i]),
                boundaryInput=float(supply),
                evaporation=float(evap),
                precipitation=float(rain[i]),
                downstream=float(remaining - out),
                boundaryExport=float(out),
            )
        )
    residual = injected + evaporated - rain.sum() - exported
    return rain / areas, dict(
        units="annual mm*m2",
        input=injected,
        evaporation=evaporated,
        precipitation=float(rain.sum()),
        export=exported,
        residual=float(residual),
        regions=ledger,
    )


def regional_graph(f, land, height, cell):
    regions = sorted(f["planningRegions"], key=lambda p: p["id"])
    polygons = [sh.make_valid(Polygon(r["ring"])).intersection(land) for r in regions]
    centres = np.array(
        [[p.representative_point().x, p.representative_point().y] for p in polygons]
    )
    areas = np.array([p.area for p in polygons])
    # Sample ridge-sensitive regional elevation, not just a centroid in a valley.
    offsets = np.array([[0, 0], [75, 0], [-75, 0], [0, 75], [0, -75]])
    heights = (
        sample(height, (centres[:, None, :] + offsets).reshape(-1, 2), cell)
        .reshape(-1, 5)
        .max(axis=1)
    )
    edges = []
    for a, p in enumerate(polygons):
        for b in range(a + 1, len(polygons)):
            shared = p.boundary.intersection(polygons[b].boundary).length
            if shared > 1:
                edges.append((a, b, float(shared)))
    return regions, polygons, centres, areas, heights, edges


def climate_fields(graph, height, cell, mask, preset, seed):
    if preset not in PRESETS:
        raise ValueError("unknown climate preset")
    config = PRESETS[preset]
    _, _, centres, areas, heights, edges = graph
    rain, ledger = transport(
        centres,
        areas,
        heights,
        edges,
        moisture=config["moisture"],
        evaporation=config["evaporation"],
    )
    x, z = np.meshgrid(np.arange(len(height)) * cell, np.arange(len(height)) * cell)
    # Continuous normalized Gaussian blend across all regional owners. No cell
    # seam, nearest-neighbour switch or per-region independent edge noise.
    numerator = np.zeros_like(height)
    denominator = np.zeros_like(height)
    for centre, value in zip(centres, rain):
        w = np.exp(-((x - centre[0]) ** 2 + (z - centre[1]) ** 2) / (2 * 360.0**2))
        numerator += value * w
        denominator += w
    precipitation = numerator / np.maximum(denominator, 1e-30)
    volume = float(np.sum(rain * areas))
    normalization = volume / float(precipitation[mask].sum() * cell**2)
    precipitation *= normalization
    if config["stylized"]:
        # Explicit biome art direction at an intentionally compressed island scale.
        boundary_warp = 650 * coherent_noise(x, z, 1100, seed + 411)
        boundary_warp += 300 * np.sin((z - 1800) / 850)
        west = 1 - smooth((x + boundary_warp - 1900) / 2400)
        precipitation = 0.35 * precipitation + 3500 * west + 80 * (1 - west)
    raster_volume = float(precipitation[mask].sum() * cell**2)
    temperature = config["temperature"] - 0.0065 * height
    pet = np.maximum(100.0, config["pet"] + 20 * (temperature - config["temperature"]))
    aridity = precipitation / pet
    # Smooth partition of unity, ordered by climatic moisture, with thermal split.
    thresholds = (0.20, 0.50, 0.90, 1.60)
    transitions = [smooth((aridity - t) / 0.24 + 0.5) for t in thresholds]
    weights = np.stack(
        [
            1 - transitions[0],
            transitions[0] - transitions[1],
            transitions[1] - transitions[2],
            transitions[2] - transitions[3],
            transitions[3],
            np.zeros_like(height),
        ]
    )
    warm = smooth((temperature - 16) / 4)
    weights[3] += weights[4] * (1 - warm)
    weights[4] *= warm
    cold = 1 - smooth((temperature - 2) / 5)
    weights[:-1] *= 1 - cold
    weights[-1] = cold
    # Quintic evaluation can overshoot one by machine epsilon. Clamp and
    # renormalize before publishing fractions to subsequent region solvers.
    weights = np.clip(weights, 0, 1)
    weights /= weights.sum(axis=0)
    weights *= mask
    return dict(
        precipitation=precipitation,
        temperature=temperature,
        pet=pet,
        aridity=aridity,
        biome_weights=weights,
    ), dict(
        preset=preset,
        parameters=config,
        regionalRainfallMm=rain.tolist(),
        atmosphericLedger=ledger,
        rasterPrecipitationVolumeMmMetres2=raster_volume,
        authoredRainfallVolumeAdjustmentMmMetres2=(
            raster_volume - volume if config["stylized"] else 0.0
        ),
        interpolationMassNormalization=normalization,
        interpolation="normalized Gaussian regional blend, sigma 360 m",
        annualClimateProxy=True,
        measuredClimate=False,
        hydrologyRecoupled=False,
        override=(
            "warped west-to-east annual rainfall blend" if config["stylized"] else None
        ),
    )


def plateau_surface(
    height, cell, polygon, level, centre, grade=(0.012, -0.006), face=35.0, apron=85.0
):
    if not polygon.is_valid or polygon.is_empty or min(face, apron) <= 0:
        raise ValueError("invalid plateau")
    x, z = np.meshgrid(np.arange(len(height)) * cell, np.arange(len(height)) * cell)
    points = sh.points(x.ravel(), z.ravel())
    d = sh.distance(points, polygon).reshape(height.shape)
    plane = level + grade[0] * (x - centre[0]) + grade[1] * (z - centre[1])
    # 75% free face then a gentler talus collar; compact support outside cap.
    factor = np.where(
        d < face, 1 - 0.75 * smooth(d / face), 0.25 * (1 - smooth((d - face) / apron))
    )
    return height * (1 - factor) + plane * factor


def noisy_plateau_outline(polygon, seed, region_id):
    """One canonical, periodic contour with broad recesses and fine rock detail.

    Random lattice octaves replace the former two repeating sine lobes. Noise
    displaces a well-resolved reference normal; topology and cap width constrain
    amplitude before the same contour is used by the height and edge solvers.
    """
    if (
        polygon.geom_type != "Polygon"
        or not polygon.is_valid
        or polygon.is_empty
        or polygon.interiors
    ):
        raise ValueError("invalid plateau outline")
    ring = polygon.exterior
    count = max(64, int(np.ceil(ring.length / 8)))
    s = np.linspace(0, ring.length, count, endpoint=False)
    reference = sh.get_coordinates(sh.line_interpolate_point(ring, s))
    tangent = np.roll(reference, -2, axis=0) - np.roll(reference, 2, axis=0)
    normal = np.column_stack([-tangent[:, 1], tangent[:, 0]])
    lengths = np.linalg.norm(normal, axis=1)
    if lengths.min() < 1e-8:
        raise ValueError("degenerate plateau perimeter")
    normal /= lengths[:, None]
    octaves = [(280.0, 42.0), (125.0, 24.0), (58.0, 15.0), (30.0, 6.0)]
    displacement = np.zeros(count)
    for octave, (wavelength, amplitude) in enumerate(octaves):
        rng = np.random.default_rng(
            np.random.SeedSequence([seed, region_id, 712, octave])
        )
        n = max(4, int(round(ring.length / wavelength)))
        values = rng.uniform(-1, 1, n)
        values -= values.mean()
        values /= max(np.abs(values).max(), 1e-8)
        lattice = CubicSpline(
            np.linspace(0, ring.length, n + 1),
            np.r_[values, values[0]],
            bc_type="periodic",
        )
        displacement += amplitude * np.clip(lattice(s), -1, 1)
    parameters = np.linspace(0, ring.length, count * 2, endpoint=False)
    for gain in (1.0, 0.8, 0.6, 0.4):
        controls = reference + gain * displacement[:, None] * normal
        # Only remove sub-sample zigzags; the old 40m controls erased fine edges.
        controls = gaussian_filter1d(controls, 0.35, axis=0, mode="wrap")
        spline = CubicSpline(
            np.r_[s, ring.length],
            np.vstack([controls, controls[0]]),
            bc_type="periodic",
        )
        cap = Polygon(spline(parameters))
        core = cap.buffer(-18)
        if (
            cap.is_valid
            and 0.75 * polygon.area < cap.area < 1.25 * polygon.area
            and not core.is_empty
            and core.geom_type == "Polygon"
            and cap.boundary.hausdorff_distance(ring) < 75
        ):
            return (
                cap,
                dict(
                    type="periodic-cubic",
                    controls=controls.tolist(),
                    knotsMetres=s.tolist(),
                    periodMetres=ring.length,
                    sampleParametersMetres=parameters.tolist(),
                ),
                dict(
                    version=2,
                    method="periodic lattice octaves along canonical perimeter",
                    seedNamespace=[seed, region_id, 712],
                    wavelengthsMetres=[a for a, b in octaves],
                    amplitudesMetres=[b for a, b in octaves],
                    acceptedAmplitudeScale=gain,
                    referenceGeometry=mapping(polygon),
                    referencePerimeterMetres=ring.length,
                    perimeterRatio=cap.length / ring.length,
                    areaRatio=cap.area / polygon.area,
                    maximumEdgeDisplacementMetres=cap.boundary.hausdorff_distance(ring),
                ),
            )
    raise ValueError("plateau edge noise cannot preserve a valid broad cap")


def build_geography(base, cell, land, weight, rivers, lakes, graph, seed):
    """Choose landforms in planning domains; preserve river/lake reservations."""
    h = base.copy()
    x, z = np.meshgrid(np.arange(len(h)) * cell, np.arange(len(h)) * cell)
    pts = sh.points(x.ravel(), z.ravel())
    mask = sh.contains_xy(land, x, z)
    water = sh.union_all(
        [LineString(r["points"]).buffer(max(110, r["width"] * 4)) for r in rivers]
        + [shape(l["geometry"]).buffer(130) for l in lakes]
    )
    mountain = cover_polygon(weight, cell, land, 0.1).buffer(80)
    available = land.buffer(-160).difference(water.union(mountain))
    candidates = []
    for r, p in zip(graph[0], graph[1]):
        clipped = p.buffer(-85).buffer(28).intersection(available)
        for part in sh.get_parts(clipped):
            if part.geom_type == "Polygon" and part.area > 45000:
                candidates.append((part.area, r["id"], part))
    features = []
    occupied = []
    for _, rid, p in sorted(candidates, reverse=True, key=lambda a: (a[0], -a[1])):
        # Only select caps whose full influence collar is clear of water/coasts/mountains.
        cap = p.buffer(-45).buffer(45)
        if cap.is_empty or cap.geom_type != "Polygon" or cap.area < 40000:
            continue
        try:
            cap, spline_record, edge_noise = noisy_plateau_outline(cap, seed, rid)
        except ValueError:
            continue
        support = cap.buffer(120)
        if (
            not land.buffer(-30).covers(support)
            or support.intersects(water)
            or support.intersects(mountain)
        ):
            continue
        if any(support.distance(o) < 200 for o in occupied):
            continue
        centre = np.array([cap.centroid.x, cap.centroid.y])
        level = float(sample(h, [centre], cell)[0] + 42)
        old = h.copy()
        h = plateau_surface(h, cell, cap, level, centre)
        crest = sh.get_coordinates(
            sh.line_interpolate_point(
                cap.exterior, np.linspace(0, cap.length, 180, endpoint=False)
            )
        )
        # Nearest point on the outer parallel curve supplies paired toe geometry.
        toe = sh.get_coordinates(
            sh.get_point(sh.shortest_line(sh.points(crest), cap.buffer(35).boundary), 1)
        )
        feature = dict(
            id=f"plateau-{rid}",
            type="plateau",
            parentRegion=rid,
            geometry=mapping(cap),
            support=mapping(support),
            spline=spline_record,
            edgeNoise=edge_noise,
            centre=centre.tolist(),
            topLevelMetres=level,
            grade=[0.012, -0.006],
            faceWidthMetres=35,
            apronWidthMetres=85,
            crest=crest.tolist(),
            toe=toe.tolist(),
            crestHeightMetres=sample(h, crest, cell).tolist(),
            toeHeightMetres=sample(h, toe, cell).tolist(),
            topDrainage="tilted plane toward +z/-x; open escarpment edge",
            maximumRaiseMetres=float((h - old).max()),
        )
        features.append(feature)
        occupied.append(support)
        if len(features) == 2:
            break
    if len(features) < 2:
        raise ValueError("not enough safe plateau domains")
    # Small resistant outcrops, selected on upland spurs, not random lowland balls.
    dz, dx = np.gradient(h, cell)
    slope = np.hypot(dx, dz)
    eligible = (weight > 0.6) & (slope > 0.2) & (slope < 0.65) & (h > 55) & mask
    waterdist = sh.distance(pts, water).reshape(h.shape)
    eligible &= waterdist > 120
    rows, cols = np.where(eligible)
    rng = np.random.default_rng(np.random.SeedSequence([seed, 1010]))
    picks = rng.permutation(len(rows))
    used = []
    for k in picks:
        c = np.array([cols[k] * cell, rows[k] * cell])
        if any(np.linalg.norm(c - q) < 250 for q in used):
            continue
        tangent = np.array([-dz[rows[k], cols[k]], dx[rows[k], cols[k]]])
        tangent /= np.linalg.norm(tangent)
        normal = np.array([-tangent[1], tangent[0]])
        along = (x - c[0]) * tangent[0] + (z - c[1]) * tangent[1]
        across = (x - c[0]) * normal[0] + (z - c[1]) * normal[1]
        bump = 9 * (1 - smooth(np.abs(along) / 65)) * (1 - smooth(np.abs(across) / 24))
        h += bump
        poly = LineString([c - 45 * tangent, c + 45 * tangent]).buffer(15)
        features.append(
            dict(
                id=f"outcrop-{len(used)}",
                type="rock-outcrop",
                geometry=mapping(poly),
                crest=[(c - 45 * tangent).tolist(), (c + 45 * tangent).tolist()],
                heightAddedMetres=9,
                orientation=tangent.tolist(),
                supportMetres=[130, 48],
            )
        )
        used.append(c)
        if len(used) == 8:
            break
    return h, features


def geographic_regions(h, cell, land, weight, rivers, lakes, features, cliffs):
    x, z = np.meshgrid(np.arange(len(h)) * cell, np.arange(len(h)) * cell)
    pts = sh.points(x.ravel(), z.ravel())
    mask = sh.contains_xy(land, x, z)
    lines = sh.union_all([LineString(r["points"]) for r in rivers])
    distance = sh.distance(pts, lines).reshape(h.shape)
    slope = np.hypot(*np.gradient(h, cell))
    # Relative elevation to the existing carved channel distinguishes valley
    # slopes from unrelated uplands near a river in plan view.
    from terrain_water_realization import ChannelSurface

    channel = ChannelSurface(h, cell, land, rivers)
    bed = np.empty(h.size)
    q = np.column_stack([x.ravel(), z.ravel()])
    for i in range(0, len(q), 16000):
        bed[i : i + 16000] = channel.project(q[i : i + 16000])[1]
    relative = h - bed.reshape(h.shape)
    valley = (
        (1 - smooth((distance - 35) / 200)) * (1 - smooth((relative - 12) / 40)) * mask
    )
    floodplain = (
        valley * (1 - smooth((slope - 0.03) / 0.1)) * (1 - smooth((relative - 3) / 8))
    )
    scree = (
        smooth((slope - 0.20) / 0.35)
        * (1 - smooth((slope - 0.8) / 0.4))
        * smooth((h - 35) / 35)
        * mask
    )
    wetlands = floodplain * (1 - smooth((h - 10) / 20))
    fields = dict(valley=valley, floodplain=floodplain, scree=scree, wetland=wetlands)
    regions = [
        dict(id=name, type=name, geometry=mapping(cover_polygon(field, cell, land)))
        for name, field in fields.items()
    ]
    return fields, regions


def coastal_bluffs(h, cell, land, rivers, lakes, coast=None):
    """Finite coastal rock bluffs with open, sea-level toes and tapered ends."""
    x, z = np.meshgrid(np.arange(len(h)) * cell, np.arange(len(h)) * cell)
    pts = sh.points(x.ravel(), z.ravel())
    mask = sh.contains_xy(land, x, z)
    if coast is None:
        coast = sh.distance(pts, land.boundary).reshape(h.shape)
    protected = sh.union_all(
        [LineString(r["points"]).buffer(180) for r in rivers]
        + [shape(l["geometry"]).buffer(160) for l in lakes]
    )
    water_clear = smooth(sh.distance(pts, protected).reshape(h.shape) / 100)
    coast_ring = max(sh.get_parts(land), key=lambda p: p.area).exterior
    sites = np.linspace(0, coast_ring.length, 36, endpoint=False)
    selected = []
    features = []
    for s in sites:
        centre = coast_ring.interpolate(s)
        if centre.distance(protected) < 500 or any(
            centre.distance(p) < 1800 for p in selected
        ):
            continue
        stations = np.linspace(s - 230, s + 230, 101) % coast_ring.length
        toe = sh.get_coordinates(sh.line_interpolate_point(coast_ring, stations))
        line = LineString(toe)
        along = sh.distance(pts, line).reshape(h.shape)
        # End fading also follows curve length, not a rectangular patch mask.
        # Radial support is continuous even across the medial axis of a bay.
        # Using nearest arc station here caused steps when two shore segments
        # were equally close to an inland point.
        radius = np.hypot(x - centre.x, z - centre.y)
        ends = 1 - smooth((radius - 80) / 330)
        support = (
            (1 - smooth((coast - 100) / 260))
            * (1 - smooth((along - 100) / 260))
            * ends
            * water_clear
            * mask
        )
        raised = 32 * smooth(coast / 28) * support
        h = h + raised
        inner = land.buffer(-28).boundary
        crest = sh.get_coordinates(
            sh.get_point(sh.shortest_line(sh.points(toe), inner), 1)
        )
        poly = cover_polygon(support, cell, land, 0.35)
        features.append(
            dict(
                id=f"coastal-cliff-{len(features)}",
                type="coastal-cliff",
                geometry=mapping(poly),
                crest=crest.tolist(),
                toe=toe.tolist(),
                crestHeightMetres=sample(h, crest, cell).tolist(),
                toeHeightMetres=sample(h, toe, cell).tolist(),
                faceWidthMetres=28,
                maximumRaiseMetres=float(raised.max()),
                coastlineUnchanged=True,
            )
        )
        selected.append(centre)
        if len(features) == 2:
            break
    if len(features) != 2:
        raise ValueError("not enough separated coastal bluff sites")
    return h, features


def biome_partition(weights, cell, land, water):
    """Canonical, mutually exclusive contours; water gets its own ownership.

    Tiny contour fragments go to the largest biome through the final complement.
    This barrier publishes one partition, which all later solvers must reuse.
    """
    labels = np.argmax(weights, axis=0)
    # Outside-land weights are all zero. Extend the nearest inland label for
    # contouring, otherwise argmax invents a narrow desert strip on every coast.
    outside = weights.sum(axis=0) == 0
    nearest = distance_transform_edt(
        outside, return_distances=False, return_indices=True
    )
    labels[outside] = labels[tuple(nearest[:, outside])]
    order = sorted(range(len(BIOMES)), key=lambda i: (np.count_nonzero(labels == i), i))
    remaining = land.difference(water)
    regions = []
    for i in order[:-1]:
        p = cover_polygon((labels == i).astype(float), cell, land, 0.5).intersection(
            remaining
        )
        remaining = remaining.difference(p)
        regions.append(
            dict(id=BIOMES[i], type=BIOMES[i], geometry=mapping(p), areaMetres2=p.area)
        )
    i = order[-1]
    regions.append(
        dict(
            id=BIOMES[i],
            type=BIOMES[i],
            geometry=mapping(remaining),
            areaMetres2=remaining.area,
        )
    )
    regions.append(
        dict(
            id="freshwater",
            type="freshwater",
            geometry=mapping(water.intersection(land)),
            areaMetres2=water.intersection(land).area,
        )
    )
    return regions
