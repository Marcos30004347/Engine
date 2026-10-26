# Terrain generation Phase 3: GPU raster hierarchy

**Status:** implemented and qualified in inactive-page storage  
**Date:** 2026-08-27

Phase 3 converts the versioned CPU canonical-column payload from Phase 1 into
the existing sparse raster-brick ABI on the GPU. It does not publish that
payload to a live terrain slot. This preserves the Phase 2 retain-complete
failure rule while Tree64 and the joint publication transaction are still
being implemented.

## Implemented contract

- A fixed-capacity planner creates at most eight dense brick level ranges in
  signed canonical coordinates. Candidate compaction order is X, then Y, then
  Z, matching the CPU `std::map<BrickKey, ...>` oracle.
- The fixed render-graph chain registers reset, eight possible level stages,
  and validation once. Indirect dispatch masks allow the same build to run in
  one frame or one hierarchy level per frame without changing output order.
- Level zero emits the exterior shell directly from `WorldMapTerrainColumnGPU`
  and `WorldMapTerrainColumnWallGPU`. It reproduces surface, boundary-wall,
  neighbour-wall, material, and context selection without rebuilding leaves.
- Parent levels OR-reduce 2x2x2 child cells, use the CPU context-rank tie break,
  emit octant-ordered child links, set reciprocal parents, and reproduce the
  measured monotonic LOD error.
- Occupancy-prefix records, packed 4-cubed coarse masks, occupied bounds,
  byte-packed palette indices, parallel 64-bit micro-context values, roots,
  canonical origins, and renderer-visible node fields use the existing ABI.
- The material palette remains an immutable exact upload in this hybrid phase.
  A 256-entry material-ID table maps canonical terrain materials into it; the
  GPU generates the per-cell material and context streams.
- The terminal GPU validator checks occupancy prefixes/counts and ranges,
  root/parent/child structure, root reachability by parent ownership, parent
  error monotonicity, and child-to-parent occupancy containment. Only a result
  with no validation bits becomes `Ready`.
- A two-word semantic hash covers every renderer-visible raster word in
  canonical order. The CPU mirror uses the same hash for differential tests.

All output buffers owned by `TerrainGPURasterBuildChain` are inactive private
storage. There is deliberately no terrain page-state, raster root directory,
Tree64 descriptor, or publication-CAS binding in this chain.

## Failure behavior

Node, child, material, and root allocations are checked before the relevant
level is emitted. Exact-fit capacities succeed. Reducing any output capacity
by one produces `TerrainGPURasterStatus::Overflow` with the precise overflow
stage and never produces `Ready`. Dense candidate exhaustion is rejected by
the CPU planner before dispatch.

Because publication is absent, an overflow, invalid result, cancelled stage
sequence, or device failure cannot replace a complete resident page. Phase 5
will connect only matching validated raster and Tree64 results to the existing
generation-checked publication transaction.

## Qualification

From the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target \
  SparseVoxelTerrainPhase3Tests \
  SparseVoxelTerrainPhase3GPUTests -j4
ctest --test-dir build --output-on-failure \
  -R 'SparseVoxelTerrainPhase3(RasterContract|RasterGPU)'
```

The CPU contract test covers planner layout, material-table construction,
independent validator corruption cases, semantic-hash determinism, and every
Phase 0 fixture at internal LOD 0, 1, and 2. The Vulkan test compares complete
GPU readback against the CPU hierarchy byte-for-byte, repeats it with a
different frame slicing, validates the GPU/CPU semantic hash, and exercises
all one-element output overflows.

## Relation to the reported ground-level artifacts

This phase hardens the raster half of the failure mode: occupancy prefixes,
material offsets, contexts, and links cannot disagree between bricks without
failing validation. It does not yet change the live image. The flickering light
field and shaking shadow boundaries also consume Tree64 and can only switch to
the new data after Phase 4 builds the matching hierarchy and Phase 5 publishes
both atomically. Enabling raster-only publication here would recreate the
one-frame geometry/shadow disagreement the phased design is intended to
eliminate.
