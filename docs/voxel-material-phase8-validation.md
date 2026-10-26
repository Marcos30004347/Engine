# Voxel material Phase 8 validation

Phase 8 keeps texture access outside both interpreters. Shading resolves at
most two statically declared scalar fields before executing Shading IR.
Geometry fields are sampled once while materializing the per-parent occupancy
cache; brick and Tree64 traversal only query that cache.

## Bounded cost contract

| Resource or operation | Implemented bound |
| --- | ---: |
| Logical fields per material | 4 |
| Fields read by one shading program | 2 |
| Fields read by one geometry program | 1 |
| Field mapping probes per field | 8 |
| Atlas loads per shading field | 1 nearest or 4 bilinear |
| Palette colours | 4 |
| Material field defaults | 16 bytes per stable slot |
| Field resolver controls | 48 bytes per stable slot |
| Auxiliary palette | 16 bytes per stable slot |
| Stable resolved sample address | 16 bytes per shaded pixel |
| Mapping record | 48 bytes, 16,384-entry bounded table |
| Atlas tile | 32 x 32 RGBA8Uint |
| Physical atlas tiles | 2,048 |
| Logical material/region mappings | 4,096 |
| Geometry-cache probes per traversal query | 8 |
| Geometry-cache record | 80 bytes per table entry |
| Geometry-cache records | 2^18 materialized parents |

The disabled and legacy G-buffer paths do not resolve fields. A programmable
program with a zero required-field mask performs no mapping-table or atlas
loads. Programs with auxiliary palette weights opt into the two material-owned
palette entries; all other programs retain the exact two-colour blend and do
not load those entries.

Geometry lookup is fixed work at traversal time: at most eight hash-table
probes followed by one occupancy-bit test. Sampling, fixed-point Geometry IR,
and conservative residency handling occur when the cache is rebuilt, not for
each traversing fragment or ray. The table is allocated at four times the
materialized-record count, rounded to a power of two, and rejects data that
cannot be inserted within the probe limit.

## Correctness coverage

`VoxelMaterialStableSlotTests` covers field descriptor validation, asset
round-trips, stable slot revisions, CPU Shading/Geometry IR field loads,
bounded mapping lookup, nearest and bilinear atlas sampling, authored fallback,
cached geometry, conservative non-residency, cache invalidation, four-colour
normalization, the legacy two-colour path, and version-two asset migration.

`SparseVoxelPlanetChunkTests` covers deterministic planet-region field
generation, shared region borders, parent/child LOD agreement, and streaming
replacement. `SparseVoxelMicroGeometryCPUReferenceTests` and
`SparseVoxelBinningG9Tests` cover the shared brick/Tree64 occupancy path and
the G-buffer ABI/binding integration. `SparseVoxelRendererSampleSmoke` creates
the pipelines through Vulkan/MoltenVK, covering backend translation of the
storage-buffer cache ABI in addition to static WGSL-to-SPIR-V compilation.

Run the focused validation with:

```sh
ctest --test-dir build --output-on-failure -L svraster.phase8
ctest --test-dir build --output-on-failure -R \
  'SparseVoxelPlanetChunkTests|SparseVoxelMicroGeometryCPUReferenceTests|SparseVoxelBinningG9Tests'
ctest --test-dir build --output-on-failure -R SparseVoxelRendererSampleSmoke
```

Compile the GPU consumers with:

```sh
cmake --build build --target \
  sparse_voxel_material_gbuffer SparseVoxelRendererSample
```

GPU timing remains scene- and machine-dependent. Use the fixed capture setup
and comparison rules in `voxel-material-phase0-baselines.md`; compare the same
terrain package with fields disabled and enabled. The structural limits above
ensure a field-enabled capture cannot introduce unbounded interpreter work,
texture indirection, or traversal-time texture sampling.
