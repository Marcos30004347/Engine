# Terrain / virtual geometry Phase 12: shared cut authority

Phase 12 narrows the remaining terrain-only visibility authority.

Generated terrain still has producer-specific scheduling, raster generation,
Tree64 generation, diagnostics and compatibility publication records. The
renderer-facing identity, however, is now the same sparse-voxel page identity
used by prebuilt and streamed assets:

- `terrainGPUV2StreamedChunkPages()` gives each generated chunk a
  `SparseVoxelPageProducer`, root page and desired page.
- `terrainGPUV2ProducedPagePayload()` converts a validated GPU raster/Tree64
  result into an ordinary shared-page payload with install/uninstall rewrite
  counts.
- `TerrainGPUV2ResidencyManager::sharedPageResidency()` projects compatibility
  hidden/active chunks into `SparseVoxelPageState` records. Hidden generated
  output appears as `Decoded`; legacy-published output appears as `Resident`.

The old `TerrainGPUV2ResidencyManager::publish()` owner-group path is now
explicitly compatibility-only. `TerrainGPUV2GroupPublication` defaults to
`allowLegacyVisibleCutPublication=false`, so a caller cannot accidentally make
the terrain owner group the renderable cut. Existing Phase 7 compatibility
coverage opts in deliberately.

The Phase 12 convergence test proves the intended handoff:

1. a generated terrain chunk reserves a hidden payload and projects to a
   decoded shared page;
2. legacy visible-cut publication is rejected by default and leaves the hidden
   payload intact;
3. the same generated result installs through `SparseVoxelSharedPageTable`;
4. `selectSparseVoxelStreamedInstanceLOD()` chooses the generated terrain page
   from the shared hierarchy; and
5. the explicit legacy publication path, when enabled, projects back to the
   same shared page identity.

That leaves terrain-specific code limited to producing and describing terrain
payloads. The cut authority is the shared page table and its dependency-aware
install/eviction rules.
