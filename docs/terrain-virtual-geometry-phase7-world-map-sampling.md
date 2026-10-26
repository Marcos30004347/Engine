# Terrain / virtual geometry Phase 7: world-map sampling bridge

Phase 7 wires terrain generation back to the semantic map export contract by
adding a CPU-visible oracle for the values the V2 terrain column shader must
consume from `MapExportGpuPageCache`.

The new `sampleTerrainGPUV2SemanticInputs` report records, per fixed world-map
sample:

- decoded field values after map-export unit scale/offset;
- requested and resolved page addresses;
- ancestor-page fallback when requested detail is not resident;
- explicit unavailable-field fallbacks for planned-but-absent semantic maps;
- missing required-page faults suitable for atlas retry requests;
- normalized paired biome mixtures and the terrain material selected from those
  inputs.

This keeps the shader binding ABI stable for Phase 7. The existing 20 V2 column
field slots still carry the fields already consumed by generated terrain
columns. Source-region, rock, and vegetation probes are surfaced as semantic
diagnostics without claiming shader slots until Phase 8 extends the world export
and material model.

The important convergence rule is now testable: terrain generation must match
`MapExportSampler` for gutter-aware linear sampling, nearest categorical
sampling, physical units, paired biome-mixture normalization, and ancestor
fallback. Missing fine semantic pages can therefore request streaming while the
current frame still builds from the best resident ancestor rather than inventing
zero-valued terrain materials.

`SparseVoxelTerrainConvergencePhase7` verifies the bridge with fixed samples,
root-only page residency, required-page fault reporting, terrain-column material
histograms, and explicit rock/vegetation unavailable diagnostics.
