# Terrain / Virtual Geometry Phase 4 Prebuilt Producer

Phase 4 adds the first non-terrain producer path to the convergence runtime.
A decoded CPU/prebuilt sparse-voxel package now translates into the same
`SparseVoxelProducedPagePayload` ticket used by generated terrain.

The shared flow remains unchanged:

1. decode a `SparseVoxelPrebuiltPagePackage`;
2. convert it with `sparseVoxelProducedPagePayloadFromPrebuilt`;
3. reserve shared physical ranges with `reserveProducedPagePayload`;
4. copy the decoded bytes into those ranges and call `markPayloadCopied`;
5. expose the page only through `installCopiedPagePayload` and install rewrites.

## What changed

`SparseVoxelPrebuiltPagePackage` represents a tiny decoded page package for
tests and future CPU asset streaming. It carries:

- the target page id and expected page generation;
- producer and payload revisions;
- decoded `VirtualSparseVoxelPageData`;
- install/uninstall rewrite records;
- optional Tree64 sidecar counts for fixture variants.

`sparseVoxelProducedPagePayloadFromPrebuilt` validates the decoded package and
emits neutral produced-payload metadata. It does not allocate storage, mutate a
scene tree, or publish a render-visible cut.

## Tree64 fixture policy

Tree64 payloads remain sidecars of the page payload. A prebuilt package can
carry different Tree64 node/material counts for different fixture or LOD
variants, but installation still happens through the shared page-table ticket.
That keeps the CPU path compatible with later LOD-specific Tree64 streaming
without requiring a realtime DAG-cut-to-Tree64 mutation path yet.

## Test coverage

`SparseVoxelTerrainConvergencePhase4` verifies that:

- decoded CPU packages become neutral produced payloads;
- invalid or mismatched packages are rejected before physical allocation;
- CPU-loaded and equivalent synthetic/generated pages install through the same
  reserve/copy/install rewrite path;
- multiple object instances keep independent root pages and stream without a
  global terrain cut;
- Tree64 fixture ranges are reserved per instance as optional sidecars.
