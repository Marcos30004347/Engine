#!/usr/bin/env python3

import pathlib
import sys


def fail(message: str) -> None:
    raise SystemExit(f"Tree64 page-streaming cutover contract failed: {message}")


if len(sys.argv) != 2:
    fail("expected the sparse voxel renderer source path")

source = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")

for legacy in (
    "uploadPlanetTree64",
    "uploadPlanetBrickHierarchy",
    "overwritePlanetTerrainArena",
    "lastPlanetUploadPartial",
    "payload.materialCellTable",
    "Destination::Tree64MaterialCells",
    "appendTerrainChangedPaddedNodeRanges",
):
    if legacy in source:
        fail(f"legacy whole-arena entry point remains: {legacy}")

for required in (
    "stagePlanetTerrainUpload",
    "terrainTree64StreamingDiagnostics",
    "SparseVoxelTree64PageAllocatorStats",
    "rewriteMode=page-streamed",
    "previous complete raster/Tree64 cut remains resident",
    "sparseTerrainUpdate",
    "appendTerrainSparseUploadRanges",
):
    if required not in source:
        fail(f"missing streamed-terrain contract marker: {required}")

print("Tree64 generated-terrain page-streaming cutover contract passed")
