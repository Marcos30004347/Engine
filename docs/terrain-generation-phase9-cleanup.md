# GPU terrain generation Phase 9: cleanup

Phase 9 removes the generic scene hierarchy adapter from generated terrain's
default CPU path. Canonical terrain columns now feed a terrain-specific brick
builder directly, without allocating a center/length, material, and index
record for every occupied voxel and without using the generic ordered-map
hierarchy construction.

Generated terrain now requests the strict GPU-v2 builder by default.
Qualification or session failures are visible failures in the default path, not
silent CPU payload generation. `auto` continues to provide the conservative
profile-certified compatibility behavior when an explicit fallback is wanted.
The CPU reference fallback consumes the same canonical column ABI as the GPU
path and emits the existing renderer hierarchy ABI, but it is an oracle/debug
path rather than the default renderer path.

## Direct column hierarchy

`buildWorldMapTerrainColumnHierarchy` expands each canonical solid run straight
into fixed-capacity brick cells. It restores canonical order by sorting brick
coordinates before emitting every level, applies the existing material/context
tie-breaks and error reduction, and emits the existing nodes, occupancy
prefixes, material streams, child links, roots, and terrain palette.

The previous `buildWorldMapTerrainColumnLeafPayload` plus
`buildSparseVoxelBrickHierarchy` route remains independently callable as a test
oracle and emergency compatibility reference. It is no longer called by
generated-world production terrain. Generic scene and imported-voxel callers
continue to use the generic hierarchy builder.

## Regression contract

`SparseVoxelTerrainPhase9Cleanup` covers every Phase 0 fixture at internal LOD
0 through 2. For every case it requires:

- byte-identical renderer payloads and equal semantic hashes versus the CPU
  leaf/map oracle;
- equal surface materials and occupied fine-voxel counts;
- deterministic output when rebuilt at the exact fine-brick capacity;
- fail-closed behavior with a one-brick-short capacity; and
- aggregate temporary brick storage below the three per-voxel leaf arrays it
  replaces.

Across that matrix, retained temporary brick records total 1,461,696 bytes,
versus 13,727,584 bytes of capacity in the reference leaf/material/index
arrays: an 89.4 percent reduction for the representation measured by this
gate. The figure deliberately excludes allocator bookkeeping on both routes.

The generated page is still relocated and packed by the existing publication
path, so residency ownership, cache keys, generation revisions, and public
renderer layout are unchanged by this adapter substitution.

## Deferred optional work

No pooled scratch allocator, GPU payload disk cache, direct physical-page size
class, or terrain DAG was added. The local device did not complete Phase 8 GPU
runtime qualification, and the Phase 0 benchmark did not provide evidence that
these additional lifetime and invalidation mechanisms are warranted. They
remain follow-ups that require captures from a responsive minimum-tier device
and a discrete backend. The existing transient CPU page cache and CPU fallback
remain enabled.

## Validation

Build and run the Phase 9 cleanup test:

```sh
cmake --build build --target SparseVoxelTerrainPhase9Tests
ctest --test-dir build --output-on-failure \
  -R '^SparseVoxelTerrainPhase9Cleanup$'
```

Run the complete CPU terrain sequence:

```sh
ctest --test-dir build --output-on-failure -L cpu \
  -R '^SparseVoxelTerrainPhase[0-9]'
```

GPU-linked test executables and shaders may be built on the current profile,
but GPU-labelled tests must only be executed on a responsive qualification
device.
