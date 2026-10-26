# Voxel material Phase 0 baselines

These references are executable assertions, not screenshots. They deliberately
hash integer occupancy or quantized shading outputs so results do not depend on
camera jitter, colour-space conversions, or host byte order.

## Deterministic signatures

| Fixture | Signature |
|---|---:|
| Hash columns | `1a6f766ebda33013` |
| Clumped columns | `9f29a5f35bc32f9e` |
| Volume density | `781086ea7135f066` |
| Slope staircase + coverage patch | `c105f266c7064607` |
| Grass/soil IR output grid | `23c61a8695e05ce7` |

The geometry fixtures evaluate four signed canonical parent keys and every
microcell at subdivision five. The grass fixture evaluates depth, stable
random, quantitative slope, and riser combinations. The coverage agreement
test additionally compares the CPU evaluator with an independent f32 mirror of
the WGSL formula for subdivisions two through eight (917,504 samples).

Run them with:

```sh
ctest --test-dir build --output-on-failure -L svraster.phase0
```

## Sidecar memory contract

The logical uncompressed cost is exactly:

```text
occupied voxel count * 4 bytes (microSurfaceData)
+ palette entry count * 2 bytes (paletteDescriptorIds)
```

`sparseVoxelMaterialSidecarMemory()` reports logical and vector-resident bytes.
The planet transient round-trip test verifies both arrays survive spill/restore
and uses the same logical formula for its uncompressed payload baseline.

## GPU timing reference

Reference machine: Apple M4 MacBook Air, 16 GB, Vulkan/MoltenVK, Release,
1280x720 output and 640x360 internal resolution. Four-frame disabled/legacy
brick smoke run on 2026-08-10:

| Stage | Mean GPU time |
|---|---:|
| Brick resolve | 0.421 ms |
| Material G-buffer compute | 0.104 ms |
| G-buffer identity raster | 0.037 ms |
| Whole timed-pass sum | 7.918 ms |

Command:

```sh
./build-release/tests/SparseVoxelRendererSample \
  --frames 4 --no-terrain --gbuffer-resolver bricks --gpu-timings \
  --disable-vsync --skip-output-signature
```

Terrain timing cases use the same camera/package and only change the material
mode, avoiding comparisons between different visible workloads:

```sh
# Disabled
... --terrain-package "$PLANET" --disable-terrain-micro-materials
# Slope staircase
... --terrain-package "$PLANET" --terrain-micro-carving
# Heightfield profiles
... --terrain-package "$PLANET" --terrain-micro-carving
# Volume profile
... --terrain-package "$PLANET" --terrain-micro-volume
```

Append `--gpu-timings --test-static-scene --frames 64` to each capture command.
The exported planet package is intentionally not checked into the repository;
CI/performance hosts must use one identical package hash for all four rows.
