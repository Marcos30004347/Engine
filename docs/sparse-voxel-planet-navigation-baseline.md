# Sparse voxel planet navigation baseline

Captured on 2026-08-11 from the Debug Vulkan build at 1280x720 output
(640x360 internal resolution), using
`build/tests/polygon-terrain-small/planet.planetpkg`, one clipmap level, and a
one-page chunk radius. Rendered hashes are backend-specific observations; the
CPU mesh signatures are portable regression contracts.

## Overview visual baseline

Command:

```sh
cd build/tests
./SparseVoxelRendererSample \
  --planet-package polygon-terrain-small/planet.planetpkg \
  --terrain-lod-levels 1 \
  --terrain-chunk-radius 1 \
  --test-planet-overview \
  --frames 3
```

Captured results:

- Package overview mesh: 14,406 vertices and 27,648 triangles.
- Package mesh signature: `2fd64bb5c57eb9d8`.
- Covered pixels: 30,737; the automated minimum is 10,000 pixels.
- Rendered output hash: `1cca85238ab50383`.
- CPU frame time over the three startup frames: 70.053 ms average, 53.895 ms
  median, and 123.168 ms p95. These Debug startup numbers are diagnostic, not
  a release-performance gate.
- Initial nine-page terrain and overview build: 7.13 seconds with two voxel
  workers.

The deterministic fixture contracts are:

- Graph-native overview: `1b8d75bfb639d8f5`.
- Raster fallback overview: `236b9c6a28c38992`.

The visual review checks that the entire globe fits at the default orbit,
mountain and forest regions separate at globe scale, thin authored rivers stay
visible, oceans retain a distinct limb/specular response, and no label is drawn
for a target on the hidden hemisphere.

## Terrain round-trip baseline

Command:

```sh
cd build/tests
./SparseVoxelRendererSample \
  --planet-package polygon-terrain-small/planet.planetpkg \
  --terrain-lod-levels 1 \
  --terrain-chunk-radius 1 \
  --test-terrain-streaming \
  --frames 3 \
  --skip-output-signature
```

Captured results:

- Outbound publication: nine generated pages.
- Return publication: nine memory-restored pages and zero generated pages.
- Each direction published one 35.14 MiB graph-upload packet, below the 64 MiB
  transaction limit.
- The scripted round trip completed in 171 rendered frames; its post-startup CPU
  frame summary was 34.019 ms average, 33.332 ms median, and 33.475 ms p95.
- The round trip completed without a render-graph rebuild, navigation queue
  drain, or synchronous terrain overwrite.

Use the CPU Phase 7 tests for rapid latest-request-wins validation and exact
cached-page accounting; use these two GPU commands for shader, frame-loop, and
publication validation.
