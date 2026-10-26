# Terrain / Virtual Geometry Phase 0 Baseline

Status: phase 0 snapshot

This freezes the terrain/virtual-geometry convergence contract before ownership
moves from terrain publication state to shared streamed hierarchy state.

## Captured regressions kept in scope

- `SparseVoxelCapturedTerrainRegression.cmake`: fixed captured world-data camera
  coverage.
- `SparseVoxelCapturedTerrainAngleCoverageRegression.cmake`: close-ground angle
  coverage and LOD0 hole capture.
- `SparseVoxelCapturedTerrainMotionRegression.cmake`: fast camera motion with a
  complete LOD0 owner requirement.
- `SparseVoxelCapturedTerrainStreamingMotionRegression.cmake`: streamed terrain
  motion coverage.
- `SparseVoxelWorldTerrainShadowParity.cmake`: shadow leak parity around the
  published terrain cut.
- `SparseVoxelWorldTerrainLightingResidencyRegression.cmake`: lighting
  residency agreement.

## Phase 0 diagnostics

The shared sparse-voxel residency path now exposes `SparseVoxelCutDiagnostics`
with counters for:

- page install;
- page uninstall;
- parent fallback;
- child selection;
- invalid generation fallback;
- HZB rejection;
- frustum rejection;
- raster proxy coverage misses;
- Tree64 lighting misses;
- page residency misses;
- selected neither parent nor child.

`classifySparseVoxelMissingRegion` maps a fixed-camera diagnostic snapshot to
the Phase 0 buckets: cut selection, HZB/frustum, raster proxy coverage, Tree64
lighting, or page residency.

The current terrain resident-page cache and GPU V2 residency manager also
report explicit page install and page uninstall counts so terrain publication
telemetry can be compared against the shared streamed-page path during the
convergence work.

## Synthetic streamed hierarchy fixture

`SparseVoxelTerrainConvergencePhase0Tests` builds a tiny terrain-shaped sparse
voxel scene as a `.vsvo` page package. It does not call the terrain generator.
The fixture installs a pinned root page, then installs eight child pages through
the shared page-residency path.

The test verifies:

- missing roots are reported as selected neither parent nor child;
- root-only residency reports parent fallback rather than a hole;
- complete child residency reports child selection with no missing region;
- stale install generations become invalid-generation fallback diagnostics;
- existing HZB culling counters bridge into the convergence diagnostic names.

## Tree64 sidecar fixture

Tree64 payloads are treated as immutable sidecar versions for the same logical
page. Phase 0 freezes the simple versioned path first:

- LOD0, LOD1 and coarser Tree64 payloads can coexist for one geometry revision;
- each version carries its own model payload revision and requested cell scale;
- runtime selection uses the current projected chunk error to choose the
  coarsest resident sidecar that still satisfies the error threshold;
- if the finer desired sidecar is missing, the current frame falls back to a
  resident conservative sidecar and requests the missing version from the CPU
  or GPU producer;
- realtime Tree64 updates derived from the current runtime DAG cut are deferred
  until this simpler sidecar path proves insufficient.

## Current terrain-only publication/cut points to remove later

These are intentionally not deleted in Phase 0. They keep the current terrain
renderer running while the shared contracts become measurable.

- `src/voxel/svraster/PlanetVoxelChunk.cpp`: `selectedWorldVoxelPageCut` and
  the generated-world page directory still compute a CPU-visible terrain page
  cut.
- `src/voxel/svraster/PlanetVoxelChunk.cpp`: terrain page install/uninstall
  batches update generated-world residency flags before the shared hierarchy is
  the only cut source.
- `src/voxel/svraster/TerrainGPUJointPublication.hpp` and
  `src/voxel/svraster/rendering/TerrainGPUJointPublicationChain.hpp`: joint
  raster/Tree64 publication remains a terrain-specific visibility transaction.
- `src/voxel/svraster/TerrainGPUV2Publication.hpp` and
  `src/voxel/svraster/rendering/TerrainGPUV2PublicationChain.hpp`: grouped
  terrain owner publication remains the current GPU-v2 cut publication path.
- `src/voxel/svraster/TerrainGPUV2Runtime.cpp`: accepted/rejected publication
  totals still describe terrain renderer ownership, not a source-neutral page
  install path.
- `src/voxel/svraster/rendering/SparseVoxelTerrainUploadPass.hpp`: terrain
  publication records and freeze controls still exist as renderer-facing state.

The later convergence phases should shrink these to page production,
scheduling, and diagnostics. The renderer-visible decision should come from
resident hierarchy traversal.
