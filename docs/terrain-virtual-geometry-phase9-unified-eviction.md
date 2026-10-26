# Terrain / Virtual Geometry Convergence Phase 9: unified eviction

Phase 9 moves sparse terrain eviction behind the shared page table's
dependency-aware policy. The goal is for memory pressure to reduce detail only:
it must not remove the last resident fallback for a visible chunk or recycle
physical payload memory before parent links have been rewritten and GPU work has
finished.

## Policy

Eviction now asks `SparseVoxelSharedPageTable::canEvictPage()` before a resident
page can be uninstalled.

The decision blocks eviction when:

- the logical page is missing or not resident;
- the page is pinned, which is how terrain roots/permanent fallbacks are
  represented;
- any dependent child page is still live.

"Live" means any state other than `Unloaded`. This deliberately covers resident
children, upload-queued or copied children, and retiring children waiting on a
fence. The older `residentDependentCount` remains useful readback, but the
eviction gate scans dependent state directly so it also catches in-flight and
retiring children.

## Physical memory and rewrites

`uninstallPage()` still validates uninstall rewrites against active hierarchy
ranges, records accepted uninstall rewrite commands, and retires the page's
physical ranges behind the supplied fence. Those ranges are not returned to the
free lists until `reclaim()` observes that the fence has completed.

Tree64 node/material sidecar ranges participate in the same allocation,
retirement and reclaim path as hierarchy, brick and material ranges. Phase 9
does not choose between Tree64 LOD versions; it only ensures whatever Tree64
payload belongs to the evicted geometry page follows the same lifetime rules.
Tree64 generation/version selection is intentionally deferred to Phase 10.

## Coverage behavior

When detail is evicted, traversal can still select the pinned root fallback.
Fast camera motion or memory pressure may therefore lower detail and request the
missing refinement again, but it must not create black terrain holes by evicting
a parent while a child or child upload still depends on it.

## Tests

`SparseVoxelTerrainConvergencePhase9` covers:

- pinned root fallbacks reject eviction;
- parents reject eviction while a child is resident;
- parents remain protected while that child is retiring behind a fence;
- upload-queued/copied children block parent eviction through the same policy;
- retired Tree64 sidecar ranges are fence-protected;
- after a parent detail page evicts, LOD selection falls back to the resident
  root and requests the missing refinement.
