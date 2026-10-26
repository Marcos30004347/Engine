"""Explicit map-preview handoff from validated terrain; not production admission."""

import copy, hashlib, json
from pathlib import Path
import numpy as np
import shapely as sh
from shapely.geometry import shape, mapping, Polygon, LineString, box
from scipy.ndimage import label, map_coordinates
from build_hydrology_terrain import METHOD


def decode_hydrology(data):
    h = copy.deepcopy(data)
    for key in ("water", "mainWater", "drainageFootprint"):
        h[key] = shape(h[key])
    for r in h["reaches"]:
        r["geometry"] = shape(r["geometry"])
    for lake in h["lakes"]:
        lake["geometry"] = shape(lake["geometry"])
    return h


def prepare_preview(directory):
    root = Path(directory)
    f = json.loads((root / "foundation.json").read_text())
    terrain = json.loads((root / "terrain-map.json").read_text())
    if (
        terrain["terrainMethod"] != METHOD
        or not terrain["terrainAcceptance"]["numericalPass"]
        or not all(terrain["terrainAcceptance"]["checks"].values())
    ):
        raise ValueError("terrain numerical gates have not passed")
    if (
        hashlib.sha256(json.dumps(f, sort_keys=True).encode()).hexdigest()
        != terrain["sourceFoundationSha256"]
    ):
        raise ValueError("terrain and foundation revisions do not match")
    manifest = json.loads((root / "manifest.json").read_text())
    for name in (
        "heightmap-metres.npy",
        "basin-depth.npy",
        "routing-surface.npy",
        "mountain-weight.npy",
    ):
        matches = [v for k, v in manifest.items() if Path(k).name == name]
        if (
            len(matches) != 1
            or hashlib.sha256((root / name).read_bytes()).hexdigest() != matches[0]
        ):
            raise ValueError(f"scalar source checksum failed: {name}")
    height = np.load(root / "heightmap-metres.npy")
    depth = np.load(root / "basin-depth.npy")
    filled = np.load(root / "routing-surface.npy")
    weight = np.load(root / "mountain-weight.npy")
    side = height.shape[0]
    cell = terrain["heightmap"]["cellMetres"]
    land = shape(terrain["land"])
    if height.shape != (side, side) or not np.isfinite(height).all():
        raise ValueError("invalid scalar terrain")
    # Every measured depression reserves land, even when too small to label.
    labels, count = label(depth > 0.25, np.ones((3, 3)))
    lakes = []
    reserved = []
    for basin_id in range(1, count + 1):
        coords = np.argwhere(labels == basin_id)
        polygon = sh.union_all(
            [
                box(
                    x * cell - cell / 2,
                    z * cell - cell / 2,
                    x * cell + cell / 2,
                    z * cell + cell / 2,
                )
                for z, x in coords
            ]
        ).intersection(land)
        reserved.append(polygon)
        surface = filled[labels == basin_id]
        if polygon.area >= 500 and float(surface.max()) > 0.1:
            lakes.append(
                {
                    "id": f"pond-{basin_id}",
                    "geometry": mapping(polygon),
                    "type": "measured-basin-reservation",
                    "waterLevelMetres": float(surface.max()),
                    "maximumDepthMetres": float(depth[labels == basin_id].max()),
                    "storageCubicMetres": float(
                        depth[labels == basin_id].sum() * cell * cell
                    ),
                    "outletHydraulicsSolved": False,
                }
            )
    receivers = [n["receiver"] for n in f["nodes"]]
    nonleaves = {j for j in receivers if j < len(receivers)}
    bed = {r["from"] - 1: r["bed"][0] for r in terrain["rivers"]}
    heads = [
        i for i in range(len(receivers)) if i not in nonleaves and bed.get(i, 0) > 55
    ]
    main = set()
    for i in heads:
        while receivers[i] < len(receivers):
            main.add(i + 1)
            i = receivers[i]
    if not main:
        raise ValueError("no mountain-fed mapped drainage")
    reaches = []
    for r in terrain["rivers"]:
        curve = LineString(r["points"])
        width = max(r["width"], 1.5)
        reaches.append(
            {
                "id": f'river-{r["from"]-1}',
                "from": r["from"],
                "to": r["to"],
                "geometry": mapping(curve),
                "bedProfileMetres": r["bed"],
                "discharge": r["discharge"],
                "width": width,
                "depth": r["depth"],
                "mainChannel": r["from"] in main,
                "hydraulicSections": [],
                "hydraulicSectionNeedsRefresh": True,
                "requiresBackwaterSolve": True,
            }
        )
    river_mask = sh.union_all(
        [shape(r["geometry"]).buffer(r["width"] / 2, quad_segs=4) for r in reaches]
    ).intersection(land)
    main_mask = sh.union_all(
        [
            shape(r["geometry"]).buffer(r["width"] / 2, quad_segs=4)
            for r in reaches
            if r["mainChannel"]
        ]
    ).intersection(land)
    basin_mask = sh.union_all(reserved)
    hydro = {
        "reaches": reaches,
        "lakes": lakes,
        "water": mapping(river_mask.union(basin_mask)),
        "mainWater": mapping(main_mask.union(basin_mask)),
        "drainageFootprint": mapping(river_mask),
        "mainChannelThresholdCumecs": 0.08,
        "ledger": f["ledger"],
        "basinReservations": {
            "count": count,
            "geometry": mapping(basin_mask),
            "physicalOutletSolvePending": True,
        },
        "hydraulicModel": {
            "type": "preserved terrain bed curves and discharge",
            "backwaterSolved": False,
            "mapPreviewOnly": True,
        },
    }
    for region in f["planningRegions"]:
        polygon = Polygon(region["ring"])
        x, z = polygon.representative_point().coords[0]
        h = float(map_coordinates(height, [[z / cell], [x / cell]], order=1)[0])
        w = float(map_coordinates(weight, [[z / cell], [x / cell]], order=1)[0])
        core = region["id"] in f["mountainRegions"]
        upland = core or w > 0.1
        region.update(
            meanControlElevationMetres=h,
            landform="mountain" if core else "foothill" if upland else "lowland",
            biome=(
                "alpine"
                if core
                else "montane-woodland" if upland else "temperate-grassland"
            ),
            landUse="wilderness" if upland else "rural-mosaic",
        )
    # Retain sparse probes for legacy diagnostic consumers; sampling/rendering
    # use the authoritative raster, not a re-interpolated primitive proxy.
    gx, gz = np.meshgrid(
        np.linspace(0, f["extent"], 81), np.linspace(0, f["extent"], 81)
    )
    samples = map_coordinates(height, [gz / cell, gx / cell], order=1)
    f["terrainSites"] = np.column_stack(
        [gx.ravel(), gz.ravel(), samples.ravel()]
    ).tolist()
    f["terrainRaster"] = {
        "side": side,
        "cellMetres": cell,
        "rowOrder": "south-to-north",
        "height": height.ravel().tolist(),
    }
    f["mapHydrology"] = hydro
    f["landscapeVersion"] = 5
    f["landscape"] = {
        "method": "SIGGRAPH watershed terrain with regional blending and measured erosion",
        "ridgeSpines": [],
        "rockFormations": [],
        "headwaterNodes": [i + 1 for i in heads],
        "heightRangeMetres": [float(height.min()), float(height.max())],
        "mountainRegionIds": f["mountainRegions"],
        "hydraulicSectionsRequireRefresh": True,
    }
    f["terrainAcceptance"] = copy.deepcopy(terrain["terrainAcceptance"])
    f["terrainAcceptance"].update(
        mapPreviewReady=True,
        settlementReady=False,
        reason="Validated terrain and basin reservations admitted for an explicitly requested 2D preview; physical outlet/crossing work remains.",
        previewChecks={
            "sourceHashVerified": True,
            "scalarSourceVerified": True,
            "basinReservationsComplete": len(reserved) == count,
            "riversPreserved": len(reaches) == len(terrain["rivers"]),
        },
    )
    return f
