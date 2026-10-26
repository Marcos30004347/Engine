# GPU terrain V2 Phase 1: 3D chunk contract

**Status:** complete

**Date:** 2026-08-27

## Outcome

Phase 1 defines the logical and GPU-facing contract used by the bounded V2
terrain generator. It does not wire the new jobs into runtime streaming; that
starts after the Phase 2 queues, scans, and fenced arenas exist.

The implementation is in `TerrainGPUChunkContract.hpp` and
`TerrainGPUChunkContract.cpp`.

## Logical identity and shape

`TerrainGPUChunkKey` is a 32-byte logical content identity containing:

- world bundle revision;
- signed X, Y, and Z chunk coordinates;
- clipmap LOD and generator revision;
- terrain slot; and
- expected publication generation.

Physical scratch, raster, Tree64, atlas, and publication allocations are not
part of the key. Rebuilding evicted content into different physical ranges
therefore preserves logical identity.

`TerrainGPUChunkShape` is an 8-byte brick shape. Production raster jobs use an
8 x 8 x 8 brick side. The validation API accepts other power-of-two sides for
tests and device experiments, provided every derived shader index fits in 32
bits.

## Coordinate contract

World positions are quantized once into signed 64-bit coordinates on the
finest voxel lattice. Every later conversion is integer-only:

```text
world metres
  -> canonical finest voxel (int64)
  -> clipmap voxel (signed int32, floor division by 2^LOD)
  -> column tile / 3D chunk / brick
  -> half-open voxel bounds
```

Floor division is explicit. For example, voxel -1 belongs to tile/chunk -1,
not zero. This removes the truncation-toward-zero disagreement that otherwise
creates overlaps west, below, or north of renderer zero.

Reverse conversions expose exact half-open bounds for clipmap voxels, column
tiles, chunks, and bricks. Adjacent owners consequently derive their shared
plane from the same integer, rather than independently rounded floating-point
translations.

## Half-open ownership

Chunk bounds use `[minimum, maximum)` on every axis. A cell on one chunk's
maximum boundary belongs to the adjacent chunk whose minimum is that plane.
The same rule extends through edges and corners, yielding exactly one owner.

Boundary samples use finest-grid canonical coordinates. Their signature hashes:

- canonical axis and plane;
- canonical tangential range; and
- sorted coordinate, value, and flag records.

Face orientation and clipmap LOD are not hashed. Positive-X on one chunk and
negative-X on its neighbour therefore compare equal; a fine and coarse chunk
also compare equal when they represent the same canonical plane and samples.
Samples outside the selected face are rejected before hashing.

## Vertical chunks

Columns use inclusive signed runs `[firstSolidY, topY]`. Stack enumeration
converts the aggregate run to a half-open interval and floor-divides it by the
chunk's voxel height. This supports chunks below the page elevation datum and
allows the same horizontal column tile to feed multiple independently
scheduled vertical slabs.

Invalid/empty runs schedule nothing. Runs whose inclusive top cannot be
represented as an exclusive bound are rejected.

## Dense brick oracle

The CPU reference enumerator produces every dense brick candidate in stable
X-fastest order:

```text
linear = (z * bricksY + y) * bricksX + x
```

Each record carries local brick coordinates, signed LOD-voxel minimum, and the
canonical brick coordinate. This is the differential oracle for the Phase 3
GPU classifier and emitter; it is not a runtime geometry builder.

## GPU stage ABI

`TerrainGPUChunkJobGPU` is a 128-byte, 16-byte-aligned, standard-layout,
trivially-copyable upload record. It contains only 32/64-bit shader fields and
includes:

- packed logical key and expected generation;
- signed half-open bounds and brick shape;
- monotonic `TerrainGPUJobStage` value;
- bounded stage item count;
- estimated and measured GPU microseconds;
- flags;
- input/output leases; and
- completion fence.

The stage values cover atlas wait, columns, fine classification/scan/emission,
optional raster parents, Tree64 masks/scan/link, validation, publication, and
terminal states.

## Validation

Run:

```sh
ctest --test-dir build-release --output-on-failure \
  -R '^SparseVoxelTerrainV2Phase1ChunkContract$'
```

The suite verifies:

- frozen logical and GPU ABI size/alignment/offsets;
- negative floor division and all coordinate conversions;
- disjoint and complete union across all six adjacent chunk faces;
- unique half-open ownership on faces, edges, and corners;
- orientation-independent same-LOD boundary signatures;
- identical finest-grid signatures across an LOD0/LOD1 boundary;
- rejection of displaced boundary samples;
- signed vertical surface/foundation stacks;
- deterministic dense brick reference enumeration;
- exact rejection at the `2^32` dense-cell boundary;
- checked multiplication and signed-bound overflow; and
- 4,000 randomized shapes against an independent 64-bit indexing oracle.

