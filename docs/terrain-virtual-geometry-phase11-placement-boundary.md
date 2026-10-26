# Terrain / virtual geometry Phase 11: placement and asset streamer boundary

Phase 11 turns the Phase 8 rock/vegetation maps into a runtime placement input
contract without making terrain generation responsible for trees, rocks or
groundcover rendering.

The new boundary is split in two layers:

- `MapPlacement` remains the renderer-neutral candidate generator. It now
  prefers the Phase 8 `vegetation_potential`, `rock_potential`,
  `exposed_bedrock`, `canopy_density`, `ground_density` and `riparian_mask`
  fields when they are present, while still accepting the legacy
  `placement_potential` channel contract.
- `SparseVoxelTerrainPlacement` bridges those candidates into runtime
  streaming records. Candidate buffers are keyed by world-data page, terrain
  page, world seed, palette version/signature and terrain-output revision.

The bridge deliberately emits requests, not loaded assets. A placement instance
contains opaque asset keys, an instance transform and one of two residency
domains:

- sparse voxel assets become ordinary `SparseVoxelStreamedInstance` records
  with their own producer/root/desired pages;
- mesh assets stay in the virtual-geometry object/page-table domain.

Terrain pages therefore expose placement inputs and identity, but they do not
own mesh payloads, voxel payloads, Tree64 payloads or per-asset page cuts.

Footprint feedback is also explicit. Already accepted objects, authored
blockers or gameplay systems may feed circular exclusion footprints back into a
candidate-buffer build. The placement bridge rejects overlapping candidates
without asking terrain to punch holes or inspect asset internals.

The disable gate is intentionally boring: disabled placement returns an empty
candidate buffer before validating/streaming anything. The Phase 11 regression
builds terrain before and after this disabled pass and checks the terrain
payload is byte/semantic identical.
