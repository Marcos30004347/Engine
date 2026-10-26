# Terrain / Virtual Geometry Convergence Phase 10: Tree64 generations

Phase 10 makes Tree64 lighting sidecars follow the same logical page generation
contract as the geometry cut. Lighting may use a different spatial
representation, but it must not silently trace a different version of the world.

## Sidecar identity

A Tree64 sidecar version is identified by:

- logical terrain page id;
- terrain slot/generation used by the current install path;
- geometry revision;
- internal Tree64 LOD;
- projected-error threshold for which that sidecar is valid.

The async `SparseVoxelTree64PageScheduler` now allows multiple internal LOD
jobs for the same logical page and geometry revision to be current at the same
time. A newer terrain generation or geometry revision supersedes older jobs for
that logical page, so stale sidecars cannot remain the active answer to a new
geometry cut.

Older callers that do not yet pass a logical page id continue to use
`terrainSlot` as the effective logical id.

## Lighting selection

`selectSparseVoxelTerrainTree64LightingSidecar()` is the CPU oracle for the
future shader-side decision:

1. Find the desired sidecar version for the selected geometry page,
   geometry revision, and projected error.
2. If that exact desired version is resident, trace it.
3. If it is missing/producing, request it and trace the coarsest resident
   conservative sidecar for the same geometry revision.
4. If the selected page has no conservative resident sidecar, explicitly fall
   back to a provided parent/ancestor sidecar.
5. If only stale sidecars exist, report a generation mismatch instead of
   tracing them.

The result reports whether lighting is traceable, whether a fallback was used,
whether the desired sidecar should be requested, and whether the miss was a
generation mismatch. It also increments the existing cut diagnostics:
`tree64LightingMissCount` for missing sidecars and
`invalidGenerationFallbackCount` for stale-generation rejection.

## Tree64 LOD versions

Generated terrain can now stream LOD0, LOD1, and coarser Tree64 payloads as
separate immutable sidecar versions for the same logical page. This matches the
planned direction from the convergence notes:

- GPU traversal still selects geometry from the resident hierarchy cut.
- The Tree64 view chooses a sidecar version by projected error.
- If the desired version is absent, CPU/GPU production can build that version
  while the current frame traces a resident conservative ancestor or coarser
  sidecar.
- Rebuilding Tree64 directly from the transient runtime DAG cut remains a later
  experiment, not the baseline.

Phase 10 intentionally does not replace the shader trace implementation in one
step. It defines and tests the shared generation/LOD/fallback contract that the
shader descriptor path should consume next.

## Tests

`SparseVoxelTerrainConvergencePhase10` covers:

- matching-generation LOD0 selection for close terrain;
- missing desired coarse LOD requests with conservative same-page fallback;
- explicit parent fallback when no same-page conservative sidecar is resident;
- stale sidecar rejection as a generation mismatch;
- refusal to use non-conservative sidecars as implicit lighting fallback;
- concurrent scheduler jobs for LOD0 and LOD1 sidecars of one logical page;
- superseding older sidecar jobs when the geometry revision changes.
