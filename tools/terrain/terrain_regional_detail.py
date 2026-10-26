"""Regional mountain branches, landform erosion masks, and aeolian detail."""

import numpy as np
import shapely as sh
from shapely.geometry import shape, LineString, Point, mapping, Polygon
from scipy.interpolate import CubicSpline
from terrain_noise_mountains import gradient_noise, ridged_field
from terrain_landforms import smooth, sample


def coordinates(h, cell):
    x, z = np.meshgrid(np.arange(len(h)) * cell, np.arange(len(h)) * cell)
    return x, z, np.column_stack([x.ravel(), z.ravel()])


def geology_clearance(features, points, pad=80):
    domains = [
        shape(f.get("support", f["geometry"])).buffer(pad)
        for f in features
        if f["type"] in ("plateau", "coastal-cliff")
    ]
    if not domains:
        return np.ones(len(points))
    return smooth(sh.distance(sh.points(points), sh.union_all(domains)) / 100)


def expand_ranges(base, cell, weight, coast, land, rivers, features, foundation, seed):
    """Select spaced inland planning provinces; emit two branches and two massifs."""
    x, z, points = coordinates(base, cell)
    primary = LineString(foundation["mountainRangeGuide"])
    from terrain_water_realization import ChannelSurface

    dr, bed, rw, rd = ChannelSurface(base, cell, land, rivers).project(points)
    dr, bed, rw, rd = (a.reshape(base.shape) for a in (dr, bed, rw, rd))
    feature_clear = geology_clearance(features, points).reshape(base.shape)
    candidates = []
    for region in foundation["planningRegions"]:
        polygon = Polygon(region["ring"]).intersection(land)
        if polygon.is_empty:
            continue
        c = np.array(polygon.representative_point().coords[0])
        if land.boundary.distance(Point(c)) < 480:
            continue
        if sample(weight, [c], cell)[0] > 0.15:
            continue
        if sample(feature_clear, [c], cell)[0] < 0.99:
            continue
        if sample(dr, [c], cell)[0] < 230:
            continue
        distance = primary.distance(Point(c))
        if distance < 500:
            continue
        candidates.append((region["id"], c, distance))
    # Maximize separation, with stable IDs breaking ties. No search-order RNG.
    chosen = []
    while candidates and len(chosen) < 4:
        score = lambda item: (
            min([item[2]] + [np.linalg.norm(item[1] - o[1]) for o in chosen]),
            -item[0],
        )
        item = max(candidates, key=score)
        candidates.remove(item)
        if chosen and min(np.linalg.norm(item[1] - o[1]) for o in chosen) < 850:
            continue
        chosen.append(item)
    if len(chosen) < 3:
        raise ValueError("insufficient inland provinces for additional mountain ranges")
    out = base.copy()
    new_weight = weight.copy()
    records = []
    for k, (rid, c, _) in enumerate(chosen):
        nearest = np.array(primary.interpolate(primary.project(Point(c))).coords[0])
        tangent = c - nearest
        tangent /= np.linalg.norm(tangent)
        normal = np.array([-tangent[1], tangent[0]])
        kind = "branch-range" if k < 2 else "separate-massif"
        controls = (
            np.array([nearest, (nearest + c) / 2 + normal * 100, c])
            if k < 2
            else np.array([c - normal * 310, c + tangent * 90, c + normal * 310])
        )
        arc = np.r_[0, np.cumsum(np.linalg.norm(np.diff(controls, axis=0), axis=1))]
        path = LineString(
            CubicSpline(arc, controls, bc_type="natural")(np.linspace(0, arc[-1], 181))
        )
        distance = sh.distance(sh.points(points), path).reshape(base.shape)
        width = 310 + 45 * gradient_noise(x, z, 650, seed + 300 + k)
        support = 1 - smooth(distance / (width * 1.7))
        support *= smooth((coast - 130) / 300) * feature_clear
        # Avoid damming existing water and taper new tributary valleys into it.
        support *= smooth((dr - rw / 2 - 40) / 180)
        ridge = ridged_field(x, z, seed + 400 + k * 17)
        uplift = support * (55 + 140 * ridge**1.5)
        out = np.maximum(out, base + uplift * (1 - smooth(weight)))
        new_weight = np.maximum(new_weight, support)
        records.append(
            dict(
                id=f"range-{rid}",
                type=kind,
                parentRegion=rid,
                controlsMetres=controls.tolist(),
                widthMetres=310,
                supportRadiusMetres=527,
                geometry=mapping(path),
                peakAddedMetres=float(uplift.max()),
            )
        )
    return out, new_weight, records, dr


def regional_weathering(base, cell, weight, coast, land, rivers, lakes, features, seed):
    """Lowland roughness and spatial erodibility, including plateau caps and cliffs."""
    x, z, points = coordinates(base, cell)
    water = sh.union_all(
        [LineString(r["points"]).buffer(r["width"] / 2) for r in rivers]
        + [shape(l["geometry"]) for l in lakes]
    )
    water_fade = smooth(
        (sh.distance(sh.points(points), water).reshape(base.shape) - 35) / 70
    )
    land_fade = smooth(coast / 24) * sh.contains_xy(land, x, z)
    clear = geology_clearance(features, points, pad=0).reshape(base.shape)
    detail = 4 * gradient_noise(x, z, 290, seed + 700) + 1.6 * gradient_noise(
        x, z, 95, seed + 701
    )
    base = base + detail * water_fade * land_fade * clear * (1 - smooth(weight))
    dz, dx = np.gradient(base, cell)
    slope = np.hypot(dx, dz)
    erodibility = np.maximum(0.20 + 0.25 * smooth(slope / 0.22), weight)
    classes = {"mountains": weight > 0.5, "lowlands": (weight < 0.1) & (clear > 0.99)}
    for kind in ["plateau", "coastal-cliff"]:
        members = [f for f in features if f["type"] == kind]
        combined = np.zeros_like(base, dtype=bool)
        for feature in members:
            domain = shape(feature.get("support", feature["geometry"]))
            mask = sh.covers(domain, sh.points(points)).reshape(base.shape)
            combined |= mask
            if kind == "plateau":
                cap = sh.covers(shape(feature["geometry"]), sh.points(points)).reshape(
                    base.shape
                )
                # Weather the cap gently while cutting runoff into rim and talus.
                erodibility[mask] = 0.65
                erodibility[cap] = 0.12
                base += cap * (0.6 * gradient_noise(x, z, 90, seed + 800)) * water_fade
                feature["surfaceModel"] = (
                    "design plane plus bounded noise and hydraulic weathering"
                )
                feature["weatheredHeightField"] = "heightmap-metres.npy"
            else:
                erodibility[mask] = 0.6
        classes[kind] = combined
    erodibility *= water_fade * land_fade
    return base, erodibility, classes


def desert_detail(
    h, cell, desert_weight, wind, coast, water_distance, feature_clear, seed
):
    """Procedural asymmetric dunes; wind direction is a coherent regional forcing."""
    v = np.asarray(wind, dtype=float)
    if v.shape != (2,) or not np.isfinite(v).all() or np.linalg.norm(v) < 1e-8:
        raise ValueError("dune wind must be a finite nonzero 2D vector")
    v = v / np.linalg.norm(v)
    x, z, _ = coordinates(h, cell)
    dz, dx = np.gradient(h, cell)
    slope = np.hypot(dx, dz)
    eligibility = (
        np.clip(desert_weight, 0, 1)
        * smooth((coast - 65) / 110)
        * smooth((water_distance - 55) / 100)
        * feature_clear
    )
    eligibility *= 1 - smooth((slope - 0.06) / 0.20)
    from terrain_feature_scale import dune_raster, SMALL_DUNES

    delta = dune_raster(x, z, cell, v, seed) * eligibility
    return (
        delta,
        eligibility,
        dict(
            method="area-averaged small transverse dunes; no aeolian transport simulation",
            windDirection=v.tolist(),
            wavelengthMetres=SMALL_DUNES.wavelength_metres,
            maximumDesignHeightMetres=SMALL_DUNES.maximum_height_metres,
            phaseWarpWavelengthsMetres=[SMALL_DUNES.warp_wavelength_metres],
            phaseWarpAmplitudesMetres=[SMALL_DUNES.warp_amplitude_metres],
            sandPatchWavelengthMetres=SMALL_DUNES.patch_wavelength_metres,
            stossFraction=SMALL_DUNES.stoss_fraction,
            samplesPerWavelength=SMALL_DUNES.wavelength_metres / cell,
            representation=(
                "area-averaged" if cell <= 10 else "detail deferred to finer sampling"
            ),
            maximumAddedMetres=float(delta.max()),
            coveredAreaMetres2=float(np.count_nonzero(delta > 0.5) * cell**2),
        ),
    )
