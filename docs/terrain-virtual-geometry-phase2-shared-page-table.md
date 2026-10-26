# Terrain / Virtual Geometry Phase 2 Shared Page Table

Phase 2 adds a CPU-side shared page table and physical range allocator that can
describe pages before the renderer is migrated away from terrain-specific
publication state.

## Runtime pieces

`SparseVoxelStreaming.hpp` now exposes:

- `SparseVoxelPhysicalBufferKind`: hierarchy, child-index, brick, occupancy,
  material, material-cell and Tree64 sidecar buffer lanes.
- `SparseVoxelPhysicalRangeAllocator`: fenced range allocator for one physical
  buffer lane.
- `SparseVoxelPhysicalAllocatorStats`: debug readback for active, retiring and
  free physical ranges.
- `SparseVoxelSharedPageTableSettings`: capacities for all shared buffer lanes.
- `SparseVoxelSharedPageTableEntry`: logical page state, producer, allocation,
  generation, dependency counters, pinning and retirement fence.
- `SparseVoxelSharedPageTable`: source-neutral page directory, dependency
  validator, allocation owner, install/uninstall rewrite recorder and fence
  reclaimer.

## Safety rules encoded here

- Page id namespace must match the producer id.
- Dependencies must already exist before a page can be added.
- A page can reserve physical ranges before its dependencies are resident, but
  it cannot install until all dependencies are resident.
- A reserved page records the pending allocation as `UploadQueued`, preventing
  duplicate reservations from leaking physical ranges before install.
- A pinned page cannot be uninstalled.
- A parent with resident dependents cannot be uninstalled.
- Install/uninstall rewrites must target active physical hierarchy/child ranges.
- Retired ranges stay unavailable until their completion fence is reclaimed.
- Reclaimed pages return to `SparseVoxelPageState::Unloaded` and may allocate
  new physical ranges later.

This is still a CPU-side convergence scaffold. Terrain GPU publication and the
packed terrain page descriptor still exist; later phases can route them through
this shared table instead of letting them define a separate render-visible cut.

## Test coverage

`SparseVoxelTerrainConvergencePhase2` exercises:

- per-buffer allocation across hierarchy, child, brick, occupancy, material,
  material-cell and Tree64 sidecar lanes;
- root/coarse fallback pinning;
- root → parent → child dependency installation;
- unsafe parent eviction rejection while a child is resident;
- child uninstall with a parent fallback rewrite;
- range retirement behind a fence;
- physical range reuse after fence completion;
- rejection of install rewrites that target unallocated or fenced-retired
  ranges;
- debug readback of logical page state and physical allocations.
