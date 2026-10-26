#!/usr/bin/env python3
"""Terrain-first SIGGRAPH 2013 field + separately measured hydraulic erosion.

Consumes the full C++ compact field and watershed dual. Does not create roads,
settlements, farmland, or meshes. Raster is a scalar simulation domain only.
"""

import argparse
import hashlib
import heapq
import json
import subprocess
import tempfile
from pathlib import Path
import numpy as np
import shapely as sh
from shapely.geometry import Polygon, LineString, mapping
from scipy.interpolate import CubicHermiteSpline
from scipy.ndimage import distance_transform_edt, label, map_coordinates
from scipy.spatial import cKDTree
from PIL import Image, ImageDraw, ImageFont

METHOD = "siggraph2013-watershed-primitives-v4"


def land_shape(f):
    land = sh.union_all([Polygon(c["ring"]) for c in f["coasts"] if c["area"] > 0])
    for c in f["coasts"]:
        if c["area"] < 0:
            land = land.difference(Polygon(c["ring"]))
    return land


def channel_curves(f, land):
    nodes = np.array([n["position"] for n in f["nodes"]])
    receivers = [n["receiver"] for n in f["nodes"]]
    incoming = [[] for _ in nodes]
    flow = {int(r["from"]) - 1: r for r in f["reaches"]}
    for i, j in enumerate(receivers):
        if j < len(nodes):
            incoming[j].append(i)
    node_depth = np.full(len(nodes), 0.35)
    for i, reach in flow.items():
        node_depth[i] = max(node_depth[i], reach["depth"])
    for i in range(len(nodes) - 1, -1, -1):
        j = receivers[i]
        if j < len(nodes):
            node_depth[j] = max(node_depth[j], node_depth[i])
    tangent = []
    for i, j in enumerate(receivers):
        down = nodes[j, [0, 2]] - nodes[i, [0, 2]] if j < len(nodes) else np.zeros(2)
        ups = sorted(incoming[i], key=lambda k: -flow[k]["discharge"])
        up = nodes[i, [0, 2]] - nodes[ups[0], [0, 2]] if ups else down
        v = up / max(np.linalg.norm(up), 1e-12) + down / max(
            np.linalg.norm(down), 1e-12
        )
        tangent.append(v / max(np.linalg.norm(v), 1e-12))
    curves = []
    for r in f["reaches"]:
        i, j = int(r["from"]) - 1, int(r["to"]) - 1
        a, b = nodes[i, [0, 2]], nodes[j, [0, 2]]
        length = np.linalg.norm(b - a)
        t = np.linspace(0, 1, max(12, int(length / 2) + 1))
        xy = CubicHermiteSpline(
            [0, 1], [a, b], [tangent[i] * length * 0.7, tangent[j] * length * 0.7]
        )(t)
        line = LineString(xy)
        # Stop smoothing at a crossing or coast violation, never alter the graph.
        if not land.buffer(0.02).covers(line) or not line.is_simple:
            xy = a[None, :] * (1 - t[:, None]) + b[None, :] * t[:, None]
        for old in curves:
            shared = {i, j} & {old["from"] - 1, old["to"] - 1}
            if not shared and LineString(xy).intersects(LineString(old["points"])):
                xy = a[None, :] * (1 - t[:, None]) + b[None, :] * t[:, None]
                break
        arc = np.r_[0, np.cumsum(np.linalg.norm(np.diff(xy, axis=0), axis=1))]
        u = arc / arc[-1]
        # Discharge is additive, unlike the paper's non-conservative area power law.
        depth_a, depth_b = node_depth[i], node_depth[j]
        bed = (nodes[i, 1] - depth_a) * (1 - u) + (nodes[j, 1] - depth_b) * u
        curves.append(
            {
                "from": i + 1,
                "to": j + 1,
                "points": xy.tolist(),
                "bed": bed.tolist(),
                "discharge": r["discharge"],
                "width": r["width"],
                "depth": depth_a,
            }
        )
    return curves


def fit_profiles_to_regional_terrain(curves, height, cell):
    """Lower control profiles into the blended terrain, keeping junctions shared.

    Reverse node order is upstream-to-downstream. No hill blending can leave a
    river floating above the lowlands. Original graph elevations remain the
    paper construction controls; these are the realized terrain bed profiles.
    """
    node_bed = {}
    available = {}
    original = {}
    for r in curves:
        xy = np.array(r["points"])
        original[r["from"]] = np.array(r["bed"])
        a = (
            map_coordinates(
                height, [xy[:, 1] / cell, xy[:, 0] / cell], order=1, mode="nearest"
            )
            - r["depth"]
        )
        available[r["from"]] = a
        for node, value in (
            (r["from"], min(a[0], r["bed"][0])),
            (r["to"], min(a[-1], r["bed"][-1])),
        ):
            node_bed[node] = min(node_bed.get(node, float("inf")), value)
    for r in sorted(curves, key=lambda r: -r["from"]):
        p = np.array(r["points"])
        arc = np.r_[0, np.cumsum(np.linalg.norm(np.diff(p, axis=0), axis=1))]
        h = np.minimum(original[r["from"]], available[r["from"]])
        h[0] = node_bed[r["from"]]
        # Strictly descending minimum envelope, including small lake-like flats.
        h = np.minimum.accumulate(h + arc * 1e-5) - arc * 1e-5
        node_bed[r["to"]] = min(node_bed[r["to"]], float(h[-1]))
        r["bed"] = h.tolist()
    maximum = 0.0
    for r in curves:
        p = np.array(r["points"])
        arc = np.r_[0, np.cumsum(np.linalg.norm(np.diff(p, axis=0), axis=1))]
        line = node_bed[r["from"]] + arc / arc[-1] * (
            node_bed[r["to"]] - node_bed[r["from"]]
        )
        bed = np.minimum(r["bed"], line)
        maximum = max(maximum, float(np.max(original[r["from"]] - bed)))
        r["bed"] = bed.tolist()
    return maximum


def flood_analysis(height, mask, cell):
    """Priority flood: expose real retained basins and ocean-connected receivers.

    Filled surface is routing metadata; it does not silently fill the terrain.
    The first settled neighbor is a receiver with smaller visit rank, so flats
    and lake interiors cannot form cycles or terminate in the middle of land.
    """
    rows, cols = height.shape
    filled = height.copy()
    seen = ~mask.copy()
    parent = np.full(height.shape, -1, dtype=np.int32)
    rank = np.full(height.shape, -1, dtype=np.int32)
    heap = []
    for z, x in np.argwhere(~mask):
        heap.append((float(height[z, x]), int(z * cols + x)))
    heapq.heapify(heap)
    order = 0
    while heap:
        h, i = heapq.heappop(heap)
        z, x = divmod(i, cols)
        rank[z, x] = order
        order += 1
        for dz, dx in (
            (-1, 0),
            (1, 0),
            (0, -1),
            (0, 1),
            (-1, -1),
            (-1, 1),
            (1, -1),
            (1, 1),
        ):
            zz, xx = z + dz, x + dx
            if 0 <= zz < rows and 0 <= xx < cols and not seen[zz, xx]:
                seen[zz, xx] = True
                parent[zz, xx] = i
                filled[zz, xx] = max(h, float(height[zz, xx]))
                heapq.heappush(heap, (filled[zz, xx], zz * cols + xx))
    depth = np.where(mask, filled - height, 0)
    lakes, count = label(depth > 0.25, np.ones((3, 3)))
    basins = []
    for i in range(1, count + 1):
        at = lakes == i
        basins.append(
            {
                "id": i,
                "areaMetres2": int(at.sum()) * cell * cell,
                "storageCubicMetres": float(depth[at].sum() * cell * cell),
                "maximumDepthMetres": float(depth[at].max()),
                "surfaceRangeMetres": [
                    float(filled[at].min()),
                    float(filled[at].max()),
                ],
            }
        )
    return filled, depth, parent, rank, basins


def regional_relief(f, base, mask, gx, gz, coast):
    """Blend river-derived mountain relief through a finite regional collar.

    Region choices precede drainage; this operator only attenuates the relief.
    Bilateral filtering suppresses small blotches while retaining steep ridges.
    """
    selected = set(f.get("mountainRegions", []))
    if not selected:
        raise ValueError(
            "Terrain revision 5 requires explicit mountain region ownership"
        )
    core = sh.union_all(
        [Polygon(r["ring"]) for r in f["planningRegions"] if r["id"] in selected]
    )
    if "mountainRangeGuide" in f:
        # Shared high-level guide: controls the slope-map support rather than
        # painting a height spine. The paper still supplies all river/crest heights.
        core = LineString(f["mountainRangeGuide"]).buffer(320)
    core = core.buffer(100).buffer(-200).buffer(100)
    pts = sh.points(gx.ravel(), gz.ravel())
    dist = sh.distance(pts, core.boundary).reshape(base.shape)
    signed = np.where(sh.contains_xy(core, gx, gz), dist, -dist)
    t = np.clip((signed + 300) / 480, 0, 1)
    weight = t * t * t * (t * (t * 6 - 15) + 10)
    lowland = np.minimum(base, 8 * (1 - np.exp(-coast / 180)) + 0.012 * coast)
    filtered = base.copy()
    for _ in range(3):
        pad = np.pad(filtered, 1, mode="edge")
        num = np.zeros_like(base)
        den = np.zeros_like(base)
        for dz in range(-1, 2):
            for dx in range(-1, 2):
                neighbor = pad[1 + dz : 1 + dz + len(base), 1 + dx : 1 + dx + len(base)]
                w = np.exp(
                    -0.5 * (dx * dx + dz * dz) / 1.2**2
                    - 0.5 * ((neighbor - filtered) / 6) ** 2
                )
                num += neighbor * w
                den += w
        filtered = num / den
    blended = np.where(mask, lowland * (1 - weight) + filtered * weight, 0)
    return (
        blended,
        weight,
        {
            "mountainRegionIds": sorted(selected),
            "coreGeometry": mapping(core),
            "transitionOutsideMetres": 300,
            "transitionInsideMetres": 180,
            "filter": "3 bilateral passes, spatial 1.2 samples, elevation sigma 6 m",
            "mountainCoreLandFraction": float(
                ((weight >= 0.5) & mask).sum() / mask.sum()
            ),
            "lowlandFraction": float(((weight < 0.1) & mask).sum() / mask.sum()),
            "heightScale": "metres; no vertical exaggeration",
        },
    )


def render(height, mask, cell, path, title):
    # Same scale/light before and after, no geometry or AI-generated illustration.
    dz, dx = np.gradient(height, cell)
    normal = np.sqrt(1 + dx * dx + dz * dz)
    light = 0.40 + 0.60 * np.clip((0.82 - 0.38 * dx + 0.42 * dz) / normal, 0, 1)
    stops = [0, 70, 150, 220, 320, 450, 700]
    colors = np.array(
        [
            [173, 182, 132],
            [148, 162, 113],
            [126, 141, 104],
            [137, 139, 116],
            [157, 151, 133],
            [188, 182, 166],
            [219, 215, 205],
        ]
    )
    rgb = (
        np.stack([np.interp(height, stops, colors[:, i]) for i in range(3)], axis=-1)
        * light[:, :, None]
    )
    slope = np.hypot(dx, dz)
    rock = np.clip((slope - 0.45) / 0.6, 0, 0.65)[..., None]
    rgb = rgb * (1 - rock) + np.array([150, 145, 132]) * light[..., None] * rock
    rgb[~mask] = [48, 79, 91]
    im = Image.fromarray(np.uint8(np.clip(rgb[::-1], 0, 255))).resize(
        (1539, 1539), Image.Resampling.LANCZOS
    )
    d = ImageDraw.Draw(im)
    d.rectangle((0, 0, 1539, 68), fill="#192e35")
    d.text(
        (22, 18),
        title,
        fill="white",
        font=ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 24),
    )
    # Metric legend and bar are part of every terrain comparison.
    d.rectangle((20, 1400, 510, 1520), fill="#192e35")
    font = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 22)
    d.line(
        (45, 1442, 45 + int(1000 / (cell * (height.shape[1] - 1)) * 1539), 1442),
        fill="white",
        width=5,
    )
    d.text((45, 1458), "1 km | elevations in metres", fill="white", font=font)
    for i in range(351):
        color = tuple(int(np.interp(i, stops, colors[:, k])) for k in range(3))
        d.line((1140 + i, 1440, 1140 + i, 1460), fill=color, width=1)
    d.text((1130, 1470), "0          175          350 m", fill="white", font=font)
    im.save(path)


def build(f, directory, erosion_binary):
    if f.get("terrainMethod") != METHOD or f.get("landscapeVersion") == 1:
        raise ValueError(
            "Regenerate the C++ paper foundation; artistic uplift is not terrain input"
        )
    out = Path(directory)
    out.mkdir(parents=True, exist_ok=True)
    n = f["scalarField"]["side"]
    extent = f["extent"]
    cell = extent / (n - 1)
    height = np.array(f["scalarField"]["height"]).reshape(n, n)
    gx, gz = np.meshgrid(np.linspace(0, extent, n), np.linspace(0, extent, n))
    land = land_shape(f)
    mask = sh.contains_xy(land, gx, gz)
    support = np.array(f["scalarField"]["supported"]).reshape(n, n)
    if not np.all(support[mask]):
        raise ValueError("uncovered terrain primitives")
    coast = sh.distance(sh.points(gx.ravel(), gz.ravel()), land.boundary).reshape(n, n)
    taper = np.clip(coast / 100, 0, 1)
    taper = taper * taper * (3 - 2 * taper)
    base = np.where(mask, np.maximum(0, height) * taper, 0)
    primitive_base = base.copy()
    base, mountain_weight, regional = regional_relief(f, base, mask, gx, gz, coast)
    curves = channel_curves(f, land)
    regional["maximumBedLoweringMetres"] = fit_profiles_to_regional_terrain(
        curves, base, cell
    )
    points = np.vstack([r["points"] for r in curves])
    beds = np.concatenate([r["bed"] for r in curves])
    widths = np.concatenate(
        [np.full(len(r["points"]), max(3, r["width"])) for r in curves]
    )
    distance, near = cKDTree(points).query(np.column_stack([gx.ravel(), gz.ravel()]))
    distance = distance.reshape(n, n)
    bed = beds[near].reshape(n, n)
    width = widths[near].reshape(n, n)
    # Art direction acts on river-to-ridge relief, never on an independent spine.
    # Keep valleys fixed and increase local ridge prominence only inside mountains.
    base += mountain_weight * 0.6 * np.maximum(base - bed - 2, 0)
    regional["ridgeReliefGain"] = 1.6
    radius = np.maximum(35, width * 3)
    w = np.maximum(0, 1 - (distance / radius) ** 2) ** 2
    profile = bed + np.maximum(0, distance - width * 0.5) * 0.22
    carved = np.where(mask, base * (1 - w) + np.minimum(base, profile) * w, 0)
    # Preserve longitudinal channel beds; particles erode slopes and deliver
    # sediment to the protected corridor. Export means leaving the active mask,
    # not a claim that sediment has already reached the ocean.
    erosion_mask = (
        mask.astype(float)
        * mountain_weight
        * np.clip((distance - width * 0.5 - 2 * cell) / (3 * cell), 0, 1)
        * np.clip(coast / 70, 0, 1)
        * np.clip(carved / 40, 0, 1)
    )
    with tempfile.TemporaryDirectory(prefix="terrain-erosion-") as tmp:
        ip = Path(tmp) / "input.json"
        op = Path(tmp) / "output.json"
        ip.write_text(
            json.dumps(
                {
                    "side": n,
                    "cellMetres": cell,
                    "seed": f["seed"],
                    "height": carved.ravel().tolist(),
                    "mask": erosion_mask.ravel().tolist(),
                }
            )
        )
        subprocess.run(
            [str(Path(erosion_binary).resolve()), str(ip), str(op)], check=True
        )
        erosion = json.loads(op.read_text())
    final = np.array(erosion["height"]).reshape(n, n)
    raster_bed_errors = []
    for r in curves:
        xy = np.array(r["points"])
        raster_bed_errors.extend(
            map_coordinates(final, [xy[:, 1] / cell, xy[:, 0] / cell], order=1)
            - r["bed"]
        )
    regional["rasterMinusBedRangeMetres"] = [
        float(np.min(raster_bed_errors)),
        float(np.max(raster_bed_errors)),
    ]
    filled, depth, parent, rank, basins = flood_analysis(final, mask, cell)
    # Quantitative gate is a prerequisite, not a claim of visual sign-off.
    checks = {
        "finiteTerrain": bool(np.isfinite(final).all()),
        "boundedMountainFootprint": regional["mountainCoreLandFraction"] <= 0.3,
        "broadLowlands": regional["lowlandFraction"] >= 0.5,
        "noLargeFloatingChannels": bool(min(raster_bed_errors) >= -0.1 * cell),
        "primitiveCoverage": bool(support[mask].all()),
        "erosionMeasured": erosion["ledger"]["erodedCubicMetres"] > 0,
        "sedimentConserved": abs(erosion["ledger"]["massResidualCubicMetres"])
        < max(1, erosion["ledger"]["erodedCubicMetres"]) * 1e-7,
        "oceanUnchanged": bool((final[~mask] == 0).all()),
        "directedDrainage": bool((rank.ravel()[parent[mask]] < rank[mask]).all()),
        "riverProfilesDownhill": all(np.all(np.diff(r["bed"]) <= 1e-9) for r in curves),
        "runoffConserved": all(
            abs(l["local"] + l["incoming"] - l["loss"] - l["outgoing"]) < 1e-8
            for l in f["ledger"]
        ),
    }
    if not all(checks.values()):
        raise ValueError(f"terrain acceptance failed: {checks}")
    arrays = {
        "heightmap-metres": final,
        "terrain-before-erosion": carved,
        "terrain-primitive-field": primitive_base,
        "mountain-weight": mountain_weight,
        "erosion-delta": carved - final,
        "basin-depth": depth,
        "routing-surface": filled,
        "flow-receiver": parent,
        "flow-rank": rank,
        "land-mask": mask,
    }
    for name, array in arrays.items():
        np.save(out / f"{name}.npy", array)
    Image.fromarray(np.uint16(np.clip(final[::-1] * 64, 0, 65535))).save(
        out / "heightmap-16bit.png"
    )
    render(
        carved,
        mask,
        cell,
        out / "terrain-before-erosion.png",
        "WATERSHED TERRAIN | compact primitives + river profiles | before erosion",
    )
    render(
        final,
        mask,
        cell,
        out / "terrain-after-erosion.png",
        f"REGIONAL TERRAIN | restrained erosion | 6 km island | summit {final.max():.0f} m",
    )
    delta = carved - final
    scale = max(1, float(np.abs(delta).max()))
    rgb = np.full((*delta.shape, 3), 235.0, dtype=float)
    rgb[:, :, 1] -= np.maximum(delta, 0) / scale * 190
    rgb[:, :, 2] -= np.maximum(delta, 0) / scale * 190
    rgb[:, :, 0] -= np.maximum(-delta, 0) / scale * 190
    rgb[~mask] = [48, 79, 91]
    Image.fromarray(np.uint8(rgb[::-1])).resize((1026, 1026)).save(
        out / "erosion-change.png"
    )
    product = {
        "schema": 5,
        "regionalRelief": regional,
        "terrainMethod": METHOD,
        "seed": f["seed"],
        "extent": extent,
        "land": mapping(land),
        "heightmap": {
            "path": "heightmap-metres.npy",
            "side": n,
            "cellMetres": cell,
            "rowOrder": "south-to-north",
            "units": "metres",
            "pngRowOrder": "north-to-south",
            "pngMetresPerInteger": 1 / 64,
            "oceanValue": 0,
        },
        "sourceFoundationSha256": hashlib.sha256(
            json.dumps(f, sort_keys=True).encode()
        ).hexdigest(),
        "terrainAcceptance": {
            "checks": checks,
            "numericalPass": True,
            "settlementReady": False,
            "reason": "Terrain-only revision: river raster agreement, basin realization and visual acceptance precede settlements",
        },
        "erosion": erosion["ledger"],
        "basins": basins,
        "rivers": curves,
        "metrics": {
            "peakMetres": float(final.max()),
            "heightQuantilesMetres": np.quantile(
                final[mask], [0, 0.25, 0.5, 0.75, 0.95, 1]
            ).tolist(),
            "maximumBasinDepthMetres": float(depth.max()),
            "basinStorageCubicMetres": float(depth.sum() * cell * cell),
            "meanAbsoluteErosionMetres": float(np.abs(delta[mask]).mean()),
            "crestEdges": sum(not e["riverCrossing"] for e in f["watershedEdges"]),
            "riverNodes": len(f["nodes"]),
            "terrainPrimitives": len(f["terrainPrimitives"]),
        },
        "meshing": False,
        "socialSolversRun": [],
    }
    (out / "terrain-map.json").write_text(json.dumps(product, indent=2) + "\n")
    for name, value in checks.items():
        print(f"PASS {name}: {value}")
    print("PASS terrain-only publication:", json.dumps(product["metrics"]))
    return product


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("foundation")
    ap.add_argument("output")
    ap.add_argument("--erosion-binary", default="build/tests/WorldTerrainErosion")
    args = ap.parse_args()
    build(
        json.loads(Path(args.foundation).read_text()), args.output, args.erosion_binary
    )
