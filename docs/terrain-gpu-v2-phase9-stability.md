# Terrain GPU V2 Phase 9: Visual Stability Qualification

Phase 9 adds the qualification boundary that prevents a fast but visually
unstable terrain implementation from becoming the automatic runtime path. It
analyzes raw geometry before lighting, then lighting before temporal
presentation. A temporal upscaler is never allowed to conceal a raster defect.

The capture/analyzer is renderer-independent so Vulkan, Metal, and future
backends feed the same exact contract. The Phase 10 shipping integration owns
recording the renderer targets into this contract and switching qualified
devices to V2 by default.

## Ground camera path

`buildTerrainGPUV2GroundPath()` resolves every camera sample against the
finest available terrain height field. It does not infer the floor from the
camera, a coarse LOD bounding box, or the top of a placeholder voxel.

The deterministic path contains three segments:

1. a fine-surface-following travel segment;
2. a stationary projection-jitter segment at normal eye clearance; and
3. a stationary projection-jitter segment exactly 1 cm above the fine surface.

Each sample records the resolved surface height and actual clearance. Leaving
the fine-height footprint fails path construction instead of silently placing
the camera in the sky.

## Capture contract

Every captured pixel records:

- primitive, brick, cell, and face identity;
- raw depth;
- linear color and light visibility;
- expected terrain coverage and actual owner count;
- canonical boundary consistency;
- CPU-oracle occupancy and GPU occupancy; and
- whether all values are finite.

Capture metadata records layer, frame, comparison group, camera motion,
projection jitter, temporal-upscaler state, light motion, camera clearance,
and primary/direct-shadow ray-origin offsets.

The required layer set is:

- raw G-buffer;
- unlit albedo;
- direct-light-only;
- full lighting; and
- upscaled output.

Raw G-buffer captures are required with temporal upscaling both disabled and
enabled for the same frames. At least one stationary jitter pair and one
stationary 1 cm pair are mandatory.

## Failure attribution

`analyzeTerrainGPUV2Stability()` reports independent counters for:

- expected terrain pixels with zero coverage (black cracks);
- pixels with more than one owner (overlap);
- inconsistent canonical boundaries and T-junctions;
- primitive/brick/cell/face identity changes;
- raw depth changes;
- unlit or lit image changes;
- stationary-light visibility changes;
- temporal-on geometry mismatches;
- defects present natively but hidden by temporal output;
- nonzero primary or direct-shadow ray-origin offsets;
- CPU/GPU occupancy disagreement;
- non-finite pixels; and
- unstable 1 cm samples.

Only stationary-camera frames in the same comparison group are compared.
Moving path samples still exercise streaming and capture coverage, but they do
not create false identity failures from legitimate parallax.

Qualification requires every counter to remain zero and every required layer,
temporal variant, stationary jitter pair, close pair, and occupancy comparison
to be present. Raw failure counters are evaluated before shadow stability.

## Runtime gate

Phase 8's `TerrainGPUV2Runtime::qualificationReady()` now requires a recorded,
passing Phase 9 report in addition to its timing, performance, capture-matrix,
device-limit, and no-drain gates. The overlay exposes `visual=pending`,
`visual=pass`, or `visual=fail`. A device therefore cannot enter the automatic
V2 path from timestamp performance alone.

The shader configuration gate also freezes the exact-receiver rule already
used by terrain direct shadows:

- direct shadow creation uses `rayOrigin = surfacePosition`; and
- compacted world traversal starts at distance zero.

A direction-dependent cell-exit or normal offset fails configuration.

## Backend/device table

The qualification table stores backend, device, driver, stable device profile,
status, counters, and reason. Duplicate records are rejected and Markdown
output is deterministic.

The current automated runner matrix is:

| Gate | Backend | Status | Coverage |
|---|---|---|---|
| Visual analyzer | CPU/reference | Qualified | All five layers, temporal on/off, stationary jitter, 1 cm |
| Fine occupancy | Vulkan | Qualified when test runs | Every GPU occupancy word and material byte-compared with CPU oracle |
| Tree64 | Vulkan | Qualified when test runs | GPU masks, links, and roots byte-compared with CPU oracle |
| Metal renderer capture | Metal | Requires execution on target device | Same capture contract; no backend-specific thresholds |

Unsupported or unexecuted backends remain explicit table entries and cannot be
treated as qualified by `auto` mode.

## Tests

`SparseVoxelTerrainV2Phase9GroundStability` verifies the fine-ground path, a
passing complete capture matrix, deliberate defect attribution, qualification
table behavior, and the 1 cm finite/stable rule.

`SparseVoxelTerrainV2Phase9GPUOccupancyQualification` re-runs the Phase 4
Vulkan differential. It compares the entire generated occupancy payload, which
is stronger than checking only cells adjacent to reported screen-space
discontinuities.

`SparseVoxelTerrainV2Phase9GPUTree64Qualification` re-runs the Phase 5 Vulkan
differential for the hierarchy consumed by renderer DDA traversal.
