# Terrain / Virtual Geometry Phase 5 Terrain Instances

Phase 5 makes generated terrain chunks enter the virtual-geometry runtime as
ordinary streamed instances.

The terrain producer now has a small adapter from `TerrainGPUChunkKey` to:

- a stable `SparseVoxelPageProducer` namespace for the world/generator pair;
- a per-chunk root page for the coarse fallback;
- a per-chunk desired page for the generated/finer LOD payload;
- a normal `SparseVoxelStreamedInstance` rooted at the fallback page.

The terrain slot is intentionally not part of the logical streamed page id. A
slot identifies one physical installation path; the streamed page identity is
the reproducible content identity: world bundle, chunk coordinates, clipmap LOD,
generator revision and page flavor.

## Runtime selection contract

`selectSparseVoxelStreamedInstanceLOD` is the shared traversal oracle:

1. the instance must be valid;
2. its root fallback page must exist and be resident;
3. if the desired page is resident, traversal selects it;
4. if the desired page is missing or unloaded, traversal selects the resident
   root and requests the desired page for streaming.

That means disabling fine terrain generation after roots are installed still
renders coarse fallback terrain. Missing fine pages become page requests, not
holes.

## Test coverage

`SparseVoxelTerrainConvergencePhase5` verifies that:

- neighboring terrain chunks become ordinary streamed instances;
- chunks share a producer namespace but own independent root/desired pages;
- coarse fallback roots are pinned and installed through the shared buffers;
- desired/fine terrain pages cannot become selectable before their fallback
  roots are resident;
- one chunk can refine to a resident fine page while a neighbor remains on its
  fallback without a global terrain cut update.
