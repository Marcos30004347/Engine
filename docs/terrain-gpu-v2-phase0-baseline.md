# GPU terrain V2 Phase 0 baseline

**Status:** historical failure baseline; GPU V2 replacement qualified

**Date:** 2026-08-27

## Qualified replacement result (2026-08-28)

The failure figures below are retained as the before-state. The shipping
`auto` path now selects `gpu-v2-runtime`, emits solid 8 x 8 x 8 leaf bricks,
publishes each four-child ownership rewrite atomically, and does not register
the obsolete raster-parent hierarchy in its render graph.

An Xcode `Metal System Trace` of the 16 km command (117 retained frames) now
attributes the terrain generator as follows:

| Family | GPU ms/frame |
|---|---:|
| Fine raster | 0.084 |
| Publication | 0.060 |
| Columns | 0.055 |
| Tree64 | 0.037 |
| Raster hierarchy | not registered |
| **Total** | **0.236** |

The comparable pre-removal capture was 0.527 ms/frame, of which 0.355
ms/frame was the hierarchy alone. The authoritative Metal result is therefore
a 55% reduction in terrain-generation encoder time. A normal 90-frame,
default-resolution, no-vsync smoke run reported 10.717 ms average CPU frame
time and 16.502 ms p95 while continuously refining and atomically replacing
four-page ownership groups. Runtime terrain allocation is 871.70 MiB under
the 2048 MiB budget.

The post-change qualification includes GPU/CPU world-map column parity for
LODs 0 through 15, exact solid-brick/Tree64 tests, G-buffer material/normal/
depth contracts, a ground-anchored one-centimetre multi-frame light-field
test, and the moving-camera world terrain raster regression. All pass.

## Purpose

Phase 0 freezes the current full-GPU runtime failure before the V2 generator
changes job shape, scheduling, allocation, or hierarchy construction. The
capture is deliberately diagnostic: a known rendering or synchronization
failure is written to JSON rather than preventing the report from being
created.

The report schema is `terrain-gpu-v2-phase0-v1`.

## Automated ground-level capture

Run the focused CTest with:

```sh
ctest --test-dir build-release --output-on-failure \
  -R '^TerrainGPUV2Phase0GroundBaseline$'
```

The test launches the current `gpu-full` runtime at a source-map-resolved
ground position, walks 16 metres, disables lighting and temporal/upscaling
paths, captures per-pass GPU timestamps, and writes:

```text
build-release/tests/terrain-gpu-v2-phase0-smoke.json
```

The camera Y contract uses the canonical LOD-0 source-map cell top even while
the renderer has only a coarse resident owner. This prevents the old failure
mode where the test started in the sky or on top of a huge coarse voxel. The
report records resident LOD separately, so failure to make LOD 0 resident is
visible rather than hidden by camera placement.

## First captured result

The first automated capture used a 1 km render radius, 640 x 360 output, and
24 frames on the current development machine:

| Measurement | Result |
|---|---:|
| CPU frame average / p50 / p95 / p99 | 1121.87 / 1829.38 / 3630.60 / 3639.63 ms |
| GPU generation average / p50 / p95 / p99 | 1173.98 / 1739.45 / 1775.69 / 1866.83 ms |
| Slow generation pass | `TerrainGPUBuild.FullGPU.Columns.Stage0`, about 1.74-1.87 seconds |
| Submitted / completed pages | 16 / 16 |
| Forced drain events / frames drained | 16 / 18 |
| Maximum queued pages / pending frames | 4 / 2 |
| Status readback | 6912 bytes, 432 bytes per completed page |
| Build scratch / hidden output | 9.89 / 2.84 MiB |
| Resident terrain pages / Tree64 | 862.50 / 226.26 MiB |
| Accounted total including scratch and hidden output | about 1101.49 MiB |
| Ground camera clearance | 1.80 m above the canonical LOD-0 source surface |
| Finest resident owner after the trace | LOD 4, 1.60 m voxels |
| Visible bricks / covered pixels | 0 / 0 |

This is already a decisive reproduction of the reported unusable frame rate.
The normal renderer passes in the same frames were generally a few
milliseconds; the generation dispatch and the CPU-side forced drains dominate
the trace.

## Current invariants

These contracts must remain true while replacing the generator:

- terrain addresses, voxel bounds, and shared boundaries use canonical integer
  coordinates after quantization;
- a raster brick is an exact 8 x 8 x 8 occupancy and material payload;
- terrain microvoxel geometry and displaced primary-hit geometry are disabled;
- primary DDA depth, face ID, G-buffer position, and albedo come from the solid
  voxel boundary without a terrain ray offset;
- `gpu-full` does not retain generated page volume or geometry in the CPU page
  cache;
- new page payloads build in hidden storage and ownership is published only as
  a complete generation;
- eviction retires physical storage only after the last GPU consumer fence;
- hierarchy occupancy/error reduction is fine-to-coarse even though streaming
  may publish coarse coverage before fine ownership; and
- the 2048 MiB configuration must include resident output, hidden output, and
  scratch in admission decisions instead of budgeting each dispatch in
  isolation.

## Known failures frozen by the capture

- The current column Stage 0 dispatch takes roughly 1.8 seconds for a page.
- Each generated page forces the pending-frame queue to drain because all jobs
  share one mutable scratch/uniform lane.
- The runtime still reads 432 bytes of generation/publication status back for
  every page completion.
- Four queued pages do not constitute a time budget; a single page can exceed
  the frame budget by two orders of magnitude.
- After 24 frames the ground camera still has only a 1.6 m LOD-4 owner instead
  of 10 cm LOD 0.
- The trace has no visible bricks and no rasterized terrain pixels, so current
  GPU generation is not merely slow; it has not produced usable runtime
  geometry at the camera.
- Current memory accounting exposes principal scratch and output allocations,
  but the scheduler does not yet reserve a single atomic device-memory budget
  across resident, hidden, and in-flight work.

## Authoritative traces

### 16 km churn

This is the authoritative performance trace. It is intentionally not part of
the default CTest suite because the frozen implementation is already spending
seconds per generated page.

```sh
./build-release/tests/SparseVoxelRendererSample \
  --world-map world-data \
  --world-map-center 0.5,0.5 \
  --terrain-render-distance-km 16 \
  --terrain-builder gpu-full \
  --terrain-v2-phase0-report build-release/tests/terrain-gpu-v2-phase0-16km.json \
  --test-camera-travel-m 512 \
  --frames 180 \
  --resolution 1920x1080 \
  --disable-lighting \
  --no-taa \
  --disable-metalfx-upscaler \
  --disable-frame-generation
```

### Close-ground seam trace

```sh
./build-release/tests/SparseVoxelRendererSample \
  --world-map world-data \
  --world-map-center 0.5,0.5 \
  --terrain-render-distance-km 1 \
  --terrain-builder gpu-full \
  --terrain-v2-phase0-report build-release/tests/terrain-gpu-v2-phase0-close.json \
  --test-camera-travel-m 16 \
  --frames 120 \
  --resolution 1280x720 \
  --disable-lighting \
  --no-taa \
  --disable-metalfx-upscaler \
  --disable-frame-generation
```

The same close trace should be repeated with lighting enabled after raw raster
coverage is nonzero. Until then, a lighting comparison cannot distinguish a
lighting defect from the generator producing no visible terrain.

## Remaining Phase 0 captures

- run the 16 km trace to completion after preserving enough time for the known
  multi-minute/hour failure;
- capture the CPU builder with the identical camera and render settings;
- capture rendering-only after prewarming and freezing terrain ownership;
- add peak atlas residency, atlas faults/uploads/evictions, and per-stage
  submitted workgroup counts to the JSON report; and
- store representative raw G-buffer, ownership, and light-visibility images
  once GPU output reaches the camera.
