# GPU terrain generation Phase 6: world-map atlas

Date: 2026-08-27

## Outcome

Phase 6 connects the existing streamed `MapExportBundle` GPU representation to
terrain build jobs. Map pages can now be read and decompressed on the worker,
uploaded under an independent byte budget, published through a field/page
directory, pinned for every consuming job, and released without blocking the
terrain scheduler.

The live CPU terrain generator remains the fallback. Phase 7 will bind these
atlas resources to the elevation/shell kernel and remove CPU column uploads for
atlas-resident jobs.

## Existing atlas contract

The repository already contained the core generic map infrastructure, which
Phase 6 preserves:

- `MapExportStreamingCache` selects deterministic root, detail, ancestor, and
  guard-page units under a decoded-residency budget.
- `MapExportPageCache` performs page-file I/O, hash validation, and
  decompression on a worker. Sampling groups such as biome IDs and weights are
  prepared and installed together.
- `MapExportGpuPageLayout` encodes typed field metadata, a field-major quadtree
  page directory, fixed-stride physical slots, scale/offset transforms, and
  missing-page entries.
- `MapExportGpuPageCache` uploads physical bytes before replacing the page
  table, so a shader cannot resolve a slot before its contents exist.
- `map_export_pages.wgsl` implements ancestor resolution, gutter-aware nearest
  and linear filtering, exact `R8Uint`/`R32Uint`/`RGBA8Uint` access, and paired
  sparse-mixture filtering.

Physical storage is word addressed, but remains typed through each field
descriptor. This avoids descriptor-per-field limits while preserving exact
categorical values.

## Terrain job residency

`TerrainGPUWorldMapAtlas` adds the terrain-facing lease and readiness layer.

Each tracked job owns the union of:

- permanently pinned roots;
- the requested page rectangle and guard pages;
- automatically expanded sampling-group companions;
- any ancestor pages selected by the residency planner.

The union of all active jobs is the physical-cache target. Therefore a page
cannot be evicted while any queued or running terrain job still references it.
Targets larger than the physical slot count fail immediately instead of
remaining permanently starved.

Map readiness uses a two-step safe-frame transaction:

```cpp
MapExportPagePublication publication;
atlas.publish(maximumUploadBytes, publication, &error);
gpuPageCache.apply(publication, &error); // physical bytes, then page table
atlas.commit(publication, scheduler, &error);
```

Only `commit` calls `TerrainGPUBuildScheduler::markMapsReady`. Decoded pages or
CPU-assigned slots are not sufficient. An atlas-resident job enters
`UploadColumns` with a zero-byte map dependency; Phase 7 will consume the GPU
atlas directly during column generation.

Jobs remain pinned until explicitly released or observed terminal during a
later commit. Cancellation before readiness never starts terrain GPU work.

## Terrain field contract

`terrainGPUWorldMapFields` selects refined elevation when present, otherwise
base elevation, and requires the terrain-critical water, river, substrate,
soil, and paired biome fields. It validates exact formats and filters.

Missing optional base elevation, uplift, relief/landform, erosion, coast,
climate, placement, soil-layer, road, settlement, biome fallback, or object
exclusion fields do not block residency. An available optional field with an
incompatible format fails closed rather than being sampled with the wrong
semantics.

## Validation

Completed locally:

```text
cmake --build build --target SparseVoxelTerrainPhase6Tests
./build/tests/SparseVoxelTerrainPhase6Tests
cmake --build build --target WorldMapTests WorldMapGpuTests
./build/tests/WorldMapTests
spirv-val map-export-pages-test.spirv
```

Coverage includes:

- worker-thread page preparation and manifest-only disk streaming;
- independent upload throttling and later completion;
- scheduler jobs remaining blocked until GPU publication commit;
- job pinning, root retention, release, and eviction;
- physical target overflow, missing required fields, invalid field semantics,
  missing optional fields, sampling-group integrity, truncated uploads, and
  aliased slots;
- CPU sampler gutters, seams, map edges, mip/ancestor fallback, categorical
  identity, continuous interpolation, and sparse biome mixtures;
- GPU sampler compilation and SPIR-V validation for every supported format.

Runtime execution of `WorldMapGpuTests` remains pending a clean device run. The
current Vulkan/Metal device also stops completing unchanged baseline terrain
GPU tests, so Phase 6 remains under the existing GPU-generation feature gate.

## Phase 7 handoff

The Phase 7 sampling kernel should bind the metadata, page-table, and physical
buffers from `MapExportGpuPageCache`, use the field indices from the validated
layout, and retain the atlas job lease until Phase 5 publication or
cancellation has completed. No page data needs to return to CPU memory.
