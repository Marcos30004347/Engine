# Terrain generation Phase 4: GPU Tree64 hierarchy

**Status:** implemented in inactive-page storage; runtime GPU qualification pending
**Date:** 2026-08-27

Phase 4 converts the same canonical column payload used by the Phase 3 raster
builder into the renderer's existing packed Tree64 ABI. It removes the GPU
path's dependency on reconstructing column runs from raster bricks, supports
power-of-four model sides through a fixed-capacity dense mask pyramid, and
cannot make its output visible before Phase 5 performs a joint publication.

## Implemented contract

- The CPU planner selects the smallest supported power-of-four side containing
  the horizontal page and its highest occupied cell. It lays out a dense mask
  pyramid from terminal 4-cubed cells through a single root. The planner
  rejects any side whose pyramid cannot fit the supplied 32-bit capacity.
- The fixed render-graph chain contains reset, terminal-mask construction,
  seven possible upper reductions, compaction, and validation. Indirect stage
  masks allow a whole build or one pass per frame without changing output.
- Terminal masks are produced directly from each column's inclusive
  `[firstSolidY, topY]` run. Upper masks are exact OR reductions; this phase
  does not introduce conservative coarsening.
- Stack-free parent-major compaction assigns one immutable 12-byte Tree64 node
  to every reachable nonempty dense mask. A scratch mapping connects dense
  masks to output node indices and preserves contiguous popcount-ranked child
  runs required by the existing traversal ABI.
- The terminal validator checks every mapped node's mask, terminal/internal
  flag, child range, popcount rank, dense-child mapping, reachability count,
  and unique output-index coverage. Only a zero-error result becomes `Ready`.
- The ready record carries the terrain generation, geometry revision, model
  side, cell size, node-page identity, semantic hash, and existing terrain
  Tree64 descriptor layout.
- Reset, mask, compaction, and validation entry points compile into separate
  SPIR-V modules. This keeps backend compilation bounded and prevents serial
  validation logic from inflating unrelated stage pipelines.

GPU node order is deterministic but is not required to match the CPU builder's
depth-first physical order for trees deeper than side 64. The public contract
is the valid Tree64 topology, exact decoded occupancy, ray behavior, and stable
GPU output across dispatch slicing. The semantic hash covers the actual
canonical GPU node stream and is independently recomputed after readback.

## Failure and publication boundary

The planner rejects an insufficient dense-mask allocation before dispatch.
Compaction counts the exact required node population before writing output; an
undersized node page returns `Overflow/Nodes`. Invalid topology returns a
non-ready result with validation bits. In all cases, buffers remain private to
`TerrainGPUTree64BuildChain`.

The chain has no terrain directory, live node-page state, raster root, or
publication compare-and-swap binding. Therefore an incomplete, stale,
overflowed, or invalid build cannot replace a resident page. Phase 5 will
publish only a matching ready raster result and ready Tree64 result in one
generation-checked transaction.

## Qualification

From the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target \
  terrain_gpu_tree64_build \
  SparseVoxelTerrainPhase4Tests \
  SparseVoxelTerrainPhase4GPUTests -j4
ctest --test-dir build --output-on-failure \
  -R 'SparseVoxelTerrainPhase4Tree64(Contract|GPU)'
```

The CPU contract suite covers sides 4, 16, 64, 256, and 1024, exact column and
padded-cell occupancy, planner underflow, validation/hash behavior, and every
Phase 0 fixture at internal LOD 0 through 2. The Vulkan differential suite
builds sides 64 and 256, validates complete readback, checks all column cells
plus deterministic padded samples, compares 512 page-entry rays with the dense
DDA oracle, verifies whole-frame versus sliced byte determinism, and exercises
a one-node-short overflow.

Existing Tree64 CPU and GPU traversal suites remain the ABI regression gate.

## Relation to the reported lighting artifacts

Phase 4 eliminates a potential disagreement source by deriving shadow
occupancy from the exact same canonical columns as raster geometry and by
validating every cross-node link. It still does not alter the live light field
or shadow input. The reported ground-level flicker, brick-boundary shaking,
and close voxel pattern cannot change until Phase 5 atomically publishes the
matching Phase 3 raster page and Phase 4 Tree64 page.
