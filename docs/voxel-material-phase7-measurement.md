# Voxel material Phase 7 measurement contract

Phase 7 keeps two Tree64 pipeline families. A trace without a material sidecar
loads the original Phase 0 SPIR-V with the original binding layout. A trace
with the sidecar loads the `.material` variant. This makes the disabled result
stronger than a one-percent timing tolerance: it adds zero shader instructions,
zero material bindings, zero context reads, and zero material-pass bytes to the
baseline path.

## Material-aware counters

`SparseVoxelTree64MaterialCountersGPU` contains eight 32-bit counters:

| Counter | Meaning |
|---|---|
| `candidateParents` | Exact Tree64 leaves offered to material refinement |
| `sidecarProbes` | Open-addressed material-record probes |
| `contextLoads` | Matching sidecar records read |
| `interpreterInstructions` | Geometry IR instructions scheduled at candidates |
| `microCellSteps` | Exact micro-DDA cells visited |
| `carvedParents` | Parents rejected before traversal continued |
| `acceptedParents` | Parents producing a hit, including conservative defaults |

The CPU reference additionally reports transitions between different
instruction counts as `instructionDivergence`. Incremental material-pass
bandwidth is zero for the packed common context and four bytes per internal
pixel only when the optional auxiliary context target is active.

## Current optimization decisions

- Keep separate material-aware shader variants. This is required to preserve
  the disabled baseline exactly.
- Keep direct IR for the verified small-program budget. The reference
  heightfield fixture executes two instructions per tested cell and remains
  inside the existing subdivision-eight, `3*N+3` micro-DDA bound.
- Do not add material binning, cached masks, or Tree64 node summaries yet. The
  counters provide the end-to-end evidence needed to reconsider them on a
  representative terrain capture without changing deterministic occupancy.

The CPU parity fixture checks raster, Tree64 primary, shadow, and probe
consumers against one ordered candidate stream, including continuation from a
completely carved parent and stable identity at the later retained hit. All
material-aware WGSL variants are compiled as part of
`SparseVoxelRasterShaders`.

The sidecar is intentionally exact-only. A producer inserts a record when a
Tree64 base cell has one unambiguous material and context. A coarse cell that
combines multiple source voxels or materials omits the record and remains
conservatively occupied. Cached masks or node summaries can replace that
fallback later if representative captures justify their memory and build cost.
The sample now extracts grid-aligned finest brick parents and enables the
material-aware static Tree64 variants only when this produces at least one
record. Cell-size mismatches, grid misalignment, canonical-key mismatches,
disabled materials, and overlapping conflicting definitions are counted and
omitted. Dynamic terrain remains on the baseline variant because its
independently transformed coarse model cells are not the same parent grid.
Supplying conflicting records directly to the table builder is a hard error.
Automatic sample activation is capped at a one-million-source upper bound,
which limits the 50%-loaded 32-byte hash table to 64 MiB. Larger scenes retain
the zero-overhead baseline until a representative capture justifies an
explicitly budgeted sidecar or a denser summary representation.
