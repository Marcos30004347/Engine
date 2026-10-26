"""Regenerate measurable channel and basin terrain products from v5 evidence."""

import argparse
import hashlib
import json
import time
from pathlib import Path

import numpy as np
import shapely as sh
from scipy.ndimage import map_coordinates
from shapely.geometry import shape

from terrain_water_realization import (
    ChannelSurface,
    read_source,
    realize_basins,
    qualify_lakes,
    add_lake_primitives,
)
from build_hydrology_terrain import render


def generate(source, output, side=1025):
    start = time.perf_counter()
    source, output = Path(source), Path(output)
    old, h, mask = read_source(source)
    output.mkdir(parents=True, exist_ok=True)
    cell = old["heightmap"]["cellMetres"]
    land = shape(old["land"])
    original_height = h.copy()
    h, basin_primitives = add_lake_primitives(h, cell, land, old["rivers"])
    surface = ChannelSurface(h, cell, land, old["rivers"])
    probes = np.vstack([r["points"] for r in old["rivers"]])
    beds = np.concatenate([r["bed"] for r in old["rivers"]])
    inside = sh.covers(land, sh.points(probes))
    exact = surface.sample(probes)
    original = map_coordinates(h, [probes[:, 1] / cell, probes[:, 0] / cell], order=1)
    error = np.abs(exact[inside] - beds[inside])
    if error.max() > 1e-6:
        raise ValueError(f"channel bed disagreement {error.max()}")
    print(
        f"PASS continuous channel beds: max error {error.max():.9f} m; {inside.sum()} probes",
        flush=True,
    )
    detailed = surface.raster(side)
    step = old["extent"] / (side - 1)
    gx, gz = np.meshgrid(np.arange(side) * step, np.arange(side) * step)
    detailed_mask = sh.contains_xy(land, gx, gz)
    detailed[~detailed_mask] = 0
    print(f"PASS sampled heightfield: {side} x {side}; cell {step:.6f} m", flush=True)
    lakes, filled, depth, parent, rank = realize_basins(detailed, detailed_mask, step)
    candidates = lakes
    lakes, rejected = qualify_lakes(candidates, surface, step)
    if not lakes:
        raise ValueError("no lake candidates passed physical shore checks")
    raster_error = (
        map_coordinates(detailed, [probes[:, 1] / step, probes[:, 0] / step], order=1)
        - beds
    )
    product = {
        "schema": 1,
        "stage": "terrain-water-realization-v8",
        "terrainMethod": old["terrainMethod"],
        "source": str(source),
        "sourceTerrainSha256": hashlib.sha256(
            (source / "terrain-map.json").read_bytes()
        ).hexdigest(),
        "extent": old["extent"],
        "land": old["land"],
        "rivers": old["rivers"],
        "lakes": lakes,
        "basinCandidates": candidates,
        "basinPrimitives": basin_primitives,
        "basinRejections": rejected,
        "surface": {
            "type": "bilinear-watershed-plus-exact-channel-profile",
            "baseHeight": "base-height.npy",
            "baseCellMetres": cell,
            "channelSource": "rivers",
            "raster": "heightmap-metres.npy",
            "side": side,
            "cellMetres": step,
            "rowOrder": "south-to-north",
            "rasterIsApproximation": True,
        },
        "acceptance": {
            "continuousChannelBedAgreement": True,
            "sourceRiversPreserved": True,
            "lakeStorageMeasured": True,
            "coarseBasinRoutesToOcean": True,
            "refinedSpillConnectionReady": False,
            "confluenceProfileBlend": True,
            "settlementReady": False,
        },
        "pending": [
            "dynamic lake outlets/backwater and shared discharge coupling",
            "hydraulic sections and confluence water-surface validation",
            "climate/biomes and rocky/coastal landforms",
            "multi-seed visual qualification",
        ],
        "metrics": {
            "peakMetres": float(detailed.max()),
            "sourceMaximumBedErrorMetres": float(
                np.abs(original[inside] - beds[inside]).max()
            ),
            "continuousMaximumBedErrorMetres": float(error.max()),
            "sampledRasterMaximumBedErrorMetres": float(
                np.abs(raster_error[inside]).max()
            ),
            "lakes": len(lakes),
            "basinCandidates": len(candidates),
            "rejectedBasins": len(rejected),
            "maximumShoreErrorMetres": max(
                (l["maximumShoreErrorMetres"] for l in lakes), default=0
            ),
            "lakeAreaMetres2": sum(l["areaMetres2"] for l in lakes),
            "lakeStorageCubicMetres": sum(l["storageCubicMetres"] for l in lakes),
            "seconds": time.perf_counter() - start,
        },
        "meshing": False,
        "socialSolversRun": [],
    }
    for name, a in {
        "base-height": h,
        "source-height": original_height,
        "heightmap-metres": detailed,
        "land-mask": detailed_mask,
        "spill-surface": filled,
        "basin-depth": depth,
        "flow-receiver": parent,
        "flow-rank": rank,
    }.items():
        np.save(output / f"{name}.npy", a)
    (output / "terrain-water.json").write_text(json.dumps(product, indent=2) + "\n")
    render(
        detailed,
        detailed_mask,
        step,
        output / "terrain-height.png",
        "TERRAIN WATER | watershed terrain + exact channel profiles",
    )
    print("PASS measured basin geometry:", json.dumps(product["metrics"]), flush=True)
    return product


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("source")
    parser.add_argument("output")
    parser.add_argument("--side", type=int, default=1025)
    a = parser.parse_args()
    generate(a.source, a.output, a.side)
