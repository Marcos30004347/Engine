# Terrain / virtual geometry Phase 8: placement and substrate map contract

Phase 8 extends the world-map export with the terrain-adjacent semantic fields
future placement and asset streaming need, without making terrain generation
own tree or rock rendering.

The exporter now writes continuous, page-streamable maps when their inputs are
already available:

- `rock_potential` (`RGBA8Unorm`, linear): exposed bedrock, boulders, scree and
  cliff potential;
- `exposed_bedrock` (`R16Float`, linear): single-channel normalized bedrock
  exposure;
- `vegetation_potential` (`RGBA8Unorm`, linear): trees, shrubs, groundcover and
  riparian potential;
- `canopy_density` (`R16Float`, linear);
- `ground_density` (`R16Float`, linear);
- `riparian_mask` (`R16Float`, linear).

The categorical maps whose producers are not stable yet remain explicit
manifest entries with `available=false` and no payload:

- `rock_type`;
- `vegetation_type`;
- existing future fields such as `roads`, `settlement_activity` and
  `object_exclusion`.

This keeps the streaming contract honest: unavailable maps are visible in
diagnostics and do not allocate page-table slots, while available continuous
maps build the same planar quadtree pages, gutters, hashes, compression records
and ancestor fallback as the rest of the map export.

The V2 terrain semantic report samples these fields opportunistically. If a
continuous Phase 8 map is present, `sampleTerrainGPUV2SemanticInputs` records
its decoded value and page fallback. If a type map is still unavailable, the
report records an `UnavailableField` fallback instead of silently reading zero.

`WorldMapTests` covers deterministic export, read/write/hash roundtrip,
metadata-only manifest loading and runtime sampling for the new fields.
`SparseVoxelTerrainConvergencePhase8` verifies that available Phase 8 maps bind
through `MapExportGpuPageLayout` and are visible to the terrain semantic report.
