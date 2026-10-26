# Terrain / Virtual Geometry Phase 1 Contract

Phase 1 introduces the shared runtime names that terrain, prebuilt sparse-voxel
assets and future streamed object pages can all use before the larger shared
page-table migration.

## Shared source vocabulary

The neutral contract now lives in `SparseVoxelStreaming.hpp`:

- `SparseVoxelPageId`: stable logical page id scoped by producer namespace.
- `SparseVoxelPageProducer`: source descriptor for prebuilt, CPU-generated and
  GPU-generated page payloads.
- `SparseVoxelStreamedInstance`: render-facing instance/root/page range view.
- `SparseVoxelPageState`: shared residency state.
- `SparseVoxelPhysicalPageAllocation`: physical buffer ranges owned by one
  resident logical page, including optional Tree64 sidecar ranges.
- `SparseVoxelPageInstallRewrite`: install transaction metadata plus hierarchy
  rewrite.
- `SparseVoxelPageUninstallRewrite`: uninstall transaction metadata plus
  hierarchy fallback rewrite.
- `SparseVoxelResidencyManager`: shared name for the existing deterministic
  residency owner.

The small helper `SparseVoxelBufferRange` keeps physical allocation validity
source-neutral: an empty range is valid, but a non-empty range must name a real
first element.

## Source mapping

| Source | Producer kind | Shared description |
| --- | --- | --- |
| CPU-loaded `.vsvo` / future object asset | `PrebuiltSparseVoxel` | `SparseVoxelStreamedInstance` + `SparseVoxelPhysicalPageAllocation` + install/uninstall rewrites |
| CPU-authored/generated page | `CPUGenerated` | Same logical page id, allocation and rewrite records |
| GPU-generated terrain chunk | `GPUGeneratedTerrain` | Same page id/allocation/rewrite records; Tree64 LOD versions occupy optional sidecar ranges |
| Future GPU-built dynamic object | `GPUGeneratedObject` | Same instance and page contract |

This phase deliberately does not remove the current terrain publication cache or
packed terrain GPU page descriptor. Those are Phase 2+ migration targets. The
new contract makes the seam explicit: render/cut logic can ask the neutral page
state whether a hierarchy node is resident, while producer-specific code remains
behind page production and installation.

## Tree64 sidecar note

Tree64 payloads are represented as optional allocation ranges on the same
resident logical page. Different Tree64 LOD versions of one logical page remain
producer/version choices, selected by the sidecar LOD contract from Phase 0 and
installed through the same allocation/rewrite vocabulary.

The baseline remains immutable sidecar versions plus explicit streaming
requests for missing required versions. Realtime Tree64 updates from the current
runtime DAG cut are still intentionally outside the Phase 1 contract.

## Test coverage

`SparseVoxelTerrainConvergencePhase1` asserts that:

- local page zero is a valid page id inside a non-zero producer namespace;
- `SparseVoxelPageState::Resident` is the only render-visible residency state;
- CPU-loaded pages, GPU-generated terrain chunks and future generated object
  pages all fit the same instance/page/allocation/rewrite structs;
- Tree64 sidecar ranges fit the shared physical allocation shape;
- invalid non-empty physical ranges are rejected without caring which producer
  emitted them.
