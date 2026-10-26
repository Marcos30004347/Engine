# GPU terrain generation Phase 5: atomic publication

Date: 2026-08-27

## Outcome

Phase 5 provides the inactive-page-to-live-page bridge for the Phase 3 raster
hierarchy and Phase 4 Tree64 hierarchy. The bridge validates matching build
results, relocates both payloads into reserved global arenas, creates the
existing `SparseVoxelTerrainPublicationRecordGPU`, and lets
`SparseVoxelTerrainUploadPass` install the raster page state and Tree64
descriptor with its existing all-or-nothing compare-and-swap.

CPU generation remains the default live path. The GPU path is additive and
can be selected per scheduled job after clean-device GPU qualification; a
rejected or stale GPU result leaves the previous complete page visible.

## Publication sequence

1. Phase 3 and Phase 4 finish in inactive source arenas.
2. `validateTerrainGPUJointPublicationRequest` rejects malformed CPU allocation
   metadata before submission.
3. `validateTerrainGPUJointPublication` checks that both GPU results are ready,
   error-free, and agree on terrain generation, geometry revision, and occupied
   voxel count. It also checks every source and destination range and the
   replacement Tree64 descriptor.
4. `relocateTerrainGPUJointPublication` copies the renderer-native raster and
   Tree64 payloads. It patches global raster indices, page translation,
   canonical coordinates, terrain slot, palette base, region mapping, clipmap
   LOD, and optional force-leaf compatibility state.
5. A full compute barrier completes all payload writes.
6. `finalizeTerrainGPUJointPublication` emits a publication record only for an
   approved lane and fills the separately reserved Tree64 material-page lease.
7. `SparseVoxelTerrainUploadPass` reads that GPU-owned record buffer. Its
   validation dispatch compares both expected live identities and all physical
   page generations; its publication dispatch writes both replacements or
   neither.

Publication requests must be ordered by strictly increasing terrain slot,
matching the existing batch CAS contract. Explicit source offsets allow jobs
to share fixed-capacity Phase 3/4 arenas without reading another job's lane.

## Ownership and cancellation

`TerrainGPUBuildScheduler` marks a returned publication as pending, so it is
not submitted twice. Cancellation and generation changes during publication
remain pending until the publication fence completes. `completePublication`
then resolves the job as published, failed, or cancelled and defers scratch
release through that fence.

`TerrainGPUJointPublicationRetirementQueue` mirrors physical-page ownership:

- accepted CAS: retire the previous live lease;
- rejected CAS: retire the unreachable replacement lease;
- either result: reclaim only after the completion fence.

This keeps the old raster and Tree64 pages paired during failures and prevents
a frames-in-flight reader from observing recycled storage.

## Integration points

- `TerrainGPUJointPublicationChain` owns request, result, and publication-record
  buffers and registers validate, relocate, and finalize passes.
- Its `records()` buffer is passed as
  `SparseVoxelTerrainUploadPass::Buffers::terrainPublicationRecords`.
- `SparseVoxelTerrainUploadPass::update` accepts a nonzero publication count
  with no CPU record pointer when that external GPU buffer is configured.
- The upload pass must be destroyed before the chain that owns its external
  record buffer.

## Validation

Completed locally:

```text
cmake --build build --target terrain_gpu_joint_publication
spirv-val terrain-gpu-joint-publication-{validate,relocate,finalize}.spirv
cmake --build build --target SparseVoxelTerrainPhase5Tests
./build/tests/SparseVoxelTerrainPhase5Tests
./build/tests/SparseVoxelTerrainPhase2Tests
cmake --build build --target SparseVoxelTree64PageUploadGPUTests
```

The three shader variants compile and pass SPIR-V validation. CPU contract
tests cover request bounds, accepted/rejected lease ownership, duplicate
submission prevention, stale work while publication is in flight, and exact
fence-delayed scratch reclamation. Phase 2 scheduler regressions and the
existing terrain-upload consumer compile cleanly.

Runtime GPU execution remains pending a clean device run. During Phase 4
qualification, unchanged baseline Tree64 GPU binaries also stopped completing
on the current Vulkan/Metal device, so another run on that wedged device would
not distinguish Phase 5 failures from the existing device state. Until the
clean-device run passes, GPU publication should remain feature-gated and CPU
generation remains the fallback.

## Clean-device qualification gate

Before enabling GPU publication by default:

- run an end-to-end ready/relocate/CAS/readback test for accepted, partial-ready,
  stale generation, and revision/occupancy mismatch cases;
- freeze publication while builds complete, then release the batch;
- exercise terrain-slot recycling and frames-in-flight retirement;
- walk, rotate, teleport, move/return, and refine/coarsen in the live sample;
- confirm there are no transient raster/Tree64 disagreements, page seams,
  shaking shadows, or early physical-page reuse.
