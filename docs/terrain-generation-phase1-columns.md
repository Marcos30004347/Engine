# GPU terrain generation Phase 1 canonical columns

Phase 1 introduces the immutable CPU reference payload that the hybrid GPU
builders will consume in Phases 3 and 4. Generated-world terrain now follows
this sequence:

`map sampling -> canonical columns -> CPU raster leaves -> brick hierarchy`

The map sampler is no longer entangled with leaf emission. The exact Tree64
reference adapter also consumes the same columns directly rather than decoding
the brick hierarchy.

## ABI

The current ABI is `kWorldMapTerrainColumnABIVersion = 1` and its semantic
revision is `kWorldMapTerrainColumnSemanticRevision = 1`. Both values are
validated before a builder reads a payload and participate in the generated
page content revision.

`WorldMapTerrainColumnGPU` is a standard-layout 16-byte record:

| Word | Meaning |
| --- | --- |
| 0 | inclusive `topY` |
| 1 | inclusive `firstSolidY` |
| 2 | material ID in bits 0..7; surface context in bits 8..15 |
| 3 | column flags |

`WorldMapTerrainColumnWallGPU` is a parallel 16-byte sidecar. It stores the
run floor before boundary stitching, the lowest-neighbour threshold, and the
deterministically selected ordinary and boundary wall contexts. The fixed face
tie order is `-X, +X, -Z, +Z`; equal neighbour heights retain the first face.

Canonical corner and boundary heights are signed 64-bit integers in global
height units. One finest voxel contains exactly
`kWorldMapTerrainHeightUnitsPerVoxel` units, matching the UNorm14 terrain
surface denominator. Ownership quantizes into this integer domain before
`ceil(height) - 1`, floor, closure, or stitch decisions.

Adjacent LODs share exact canonical corners at every aligned coordinate. The
existing conservative surface-envelope sidecar remains responsible for
whole-footprint coarse/fine coverage; Phase 1 does not add finest-grid map
resampling to coarse page generation, which would change its streaming cost.

## Validation

Build and run the reference freeze and canonical-column suites:

```sh
cmake --build build-release \
  --target SparseVoxelTerrainPhase0Tests SparseVoxelTerrainPhase1Tests -j 8
ctest --test-dir build-release \
  -R 'SparseVoxelTerrainPhase(0CPU|1CanonicalColumns)' \
  --output-on-failure
```

The Phase 1 suite covers all nine Phase 0 fixtures at LODs 0, 1, and 2. It
requires exact decoded raster occupancy, material, and context from the
column-fed leaf adapter; exact Tree64 occupancy from the direct column adapter;
exact adjacent-page corners and boundary ownership; conservative coarse/fine
envelope containment;
randomized representable ranges; deterministic wall ownership; and fail-closed
ABI/range validation.

`SparseVoxelWorldTerrainStreamingRoundTrip` also passes. The broader
`SparseVoxelWorldTerrainLODStability` test remains red independently of column
parity: its moving-camera run still reports 111 pending asynchronous chunks at
frame 358, so the test's assumed settled window never begins. Phase 1 does not
weaken that check or treat an unsettled suffix as stable.
