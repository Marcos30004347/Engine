# Terrain / Virtual Geometry Phase 3 Production Split

Phase 3 separates payload production from render-visible page installation.

The shared flow is now:

1. producer emits `SparseVoxelProducedPagePayload` metadata;
2. residency reserves physical ranges with `reserveProducedPagePayload`;
3. copy/finalize completion marks the reservation copied with
   `markPayloadCopied`;
4. `installCopiedPagePayload` applies install rewrites and only then makes the
   page resident.

## What changed

`SparseVoxelStreaming.hpp` now exposes:

- `SparseVoxelProducedPagePayload`: source-neutral metadata for a complete
  producer result, including expected page generation, required producer
  revision, payload revision, physical range requirements and rewrite counts.
- `SparseVoxelReservedPagePayload`: the produced metadata plus the actual
  reserved physical allocation and copy/finalize fence.

`SparseVoxelSharedPageTable` now provides:

- `reserveProducedPagePayload`;
- `markPayloadCopied`;
- `installCopiedPagePayload`.

Terrain GPU V2 now has a small adapter:

- `terrainGPUV2ProducedPagePayload`, which converts
  `TerrainGPUV2PayloadResultGPU` into the neutral produced-payload ticket.

This adapter does not publish terrain ownership, change a render cut, or install
hierarchy rewrites. It only translates validated V2 output counts/revisions into
the shared staging contract.

## Safety rules encoded here

- Produced metadata alone does not allocate storage.
- Reserving storage moves the page to `UploadQueued`, not `Resident`.
- Copied payloads remain hidden until install rewrites run.
- Install before copy completion is rejected.
- Stale producer revisions are rejected before allocation.
- Stale page generations are rejected before allocation/install.
- The install rewrite count must match the produced metadata.

This gives later phases a clean seam:

```text
produce payload metadata
  -> reserve/copy payload bytes
  -> install hierarchy rewrites
  -> traversal may select resident page
```

## Test coverage

`SparseVoxelTerrainConvergencePhase3` verifies that:

- Terrain GPU V2 payload results convert into neutral produced metadata;
- metadata-only production does not reserve hierarchy storage or record install
  rewrites;
- reserve and copy steps do not make the page resident;
- only `installCopiedPagePayload` exposes the terrain page to traversal;
- stale producer results and incomplete payload metadata are rejected before
  physical storage is reserved.
