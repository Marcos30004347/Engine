# Terrain virtual geometry: one object, one hierarchy, per-cluster LOD

## Goal

Every brick cluster is its own LOD decision, decided by the GPU culler. The CPU
generates page payloads and hands them over; it does not choose a cut, does not
schedule objects, and does not touch the render thread for longer than an
upload.

## What already exists (verified 2026-09-04)

This is not new machinery. Almost all of it is built and tested; the CPU terrain
route is the one consumer that bypasses it.

| Piece | Where | State |
|---|---|---|
| Page-slice buffer write | `VirtualGeometryScene.cpp:649` — `bufferWrite(pagesBuffer_, pageAlloc.offset, pageSize, …)` | Done |
| Install one page into a slot of an existing object | `VirtualGeometryScene::installRuntimeObjectPage` / `installRuntimeObjectEncodedPage` | Done, used only by Phase14 tests |
| Per-cluster hierarchy rewrites | `HierarchyClusterFlagsUpdate` in `VirtualGeometryData.hpp:195` — `hierarchyNodeIndex`, `streamingLeafsBitset`, `enabledClustersBitset`, one bit per child cluster | Done |
| Rewrites produced by the builder | `VirtualGeometryHierarchyBuilder::finalizePagesAndRewrites` → `VirtualGeometryHierarchyRewrites::buildPagesAndRewrites` | Done |
| Voxel octree build calls it | `VoxelOctreeVirtualGeometryBuilder.cpp:684` | Done — **terrain chunk assets already carry rewrites** |
| Compressor carries them into encoded pages | `VirtualGeometryCompressor.cpp:306-307` | Done |
| Runtime accepts caller-supplied rewrites | `RuntimePageInstallInfo::hierarchyClusterFlagUpdates`, same on `RuntimeEncodedPageInstallInfo` | Done |
| Uninstall replays remaining pages' install rewrites | `VirtualGeometryScene.cpp:3878` | Done |
| Fixed page-slot object + slot map with parent links and `residentChildMask` | `buildTerrainGPUVGRuntimeObjectHierarchy`, `registerSparseVoxelTerrainVGRuntimeObjectWithPlacement`, `TerrainGPUVirtualGeometryPageSet` | Done, used by the generated route |
| GPU page demand readback | `collectRuntimeObjectPageRequests(objectName, …, priorityReadback, minPriority)` | Done, sample uses it for the generated route |

**The gap.** The CPU route registers **one runtime object per `(ground, level)`**
(`installRuntimeEncodedObject`, keyed by `terrainVirtualGeometryChunkObjectName`).
A hierarchy is per object, and two objects cannot cull against each other — each
root has infinite parent error, so both draw. That is why
`scheduleTerrainVirtualGeometryObjects` exists: something has to pick one level
per ground on the CPU. Per-cluster LOD is therefore already correct *inside* a
chunk and impossible *between* chunks.

**The critical warning, already in the code** (`VirtualGeometryScene.hpp:515`):
leaving `hierarchyClusterFlagUpdates` empty marks every cluster on the page a
streaming leaf, which forces its self error to zero in the culler — the cluster
draws at every distance, the parent test is skipped, and every LOD level draws
at once. Any phase that installs a page must supply correct rewrites.

## Target

One runtime object, `terrain`, with a fixed pool of page slots. Every chunk at
every level installs its pages into that pool. The DAG spans levels: a coarse
chunk's clusters are the parents of the four fine chunks' clusters, and the link
is maintained at runtime by install/uninstall rewrites, exactly as the offline
builder does within one asset.

```
install page P (chunk C, level L, quadrant q of parent chunk PC)
  1. reserve a slot block for C's pages          (page set)
  2. write each page's slice                      (installRuntimeObjectEncodedPage)
  3. rewrite PC's hierarchy node:                 (hierarchyClusterFlagUpdates)
       clear streamingLeaf bit for the clusters C now covers
       set   enabled     bit for them
  4. on uninstall, the inverse; the scene replays the remaining pages' rewrites
```

## Measured baseline to beat (2026-09-04, `--terrain-render-distance-km 5`, 1220x720)

| | value |
|---|---|
| Page generation | 2.25 ms/page |
| Transaction (worker) | 87-319 ms |
| VG refresh (render thread) | 4.1 ms worst |
| Scene apply / scheduler | 16-62 ms worst |
| Streaming + publication block | 300-720 ms on commit frames at high residency |
| Frame median / p95 | 16-19 ms / 68-294 ms (varies with streaming progress) |
| Residency | ~250 chunks by frame 300 |

---

## Phase 1 — Slot blocks for multi-page chunks — **DONE 2026-09-04**

A chunk's asset is many pages (meshlet counts run 400-3600), so the page set must
reserve a **contiguous block** of slots per chunk, not one.

**Changes**
- `TerrainGPUVirtualGeometryPageSet`: add `reserveBlock(address, pageCount)` →
  `{firstSlot, count}`; `release(address)`. Keep the existing per-address parent
  links and `residentChildMask`, now keyed on the block's first slot.
- Slot capacity: `terrainGPUVGPageCapacity` must cover
  `maximumResidentChunks × maxPagesPerChunk`. Log the high-water mark and fail
  loudly on exhaustion — silent exhaustion reads as missing terrain.

**Implemented as** `reservePageSlots(slot, pageCount)`, `slotForAssetPage(slot,
assetPageIndex)`, `reservedPageCount(slot)`, plus `TerrainGPUVGPage::extraSlots`
and `extraPage`.

Slots are **not** contiguous. A contiguous reservation would start failing while
plenty of slots remained, because the free list fragments; asset page i maps
through `slotForAssetPage` instead, and every parent, dependency and rewrite
index is remapped through that.

Only the primary slot represents the chunk: it is what `sync` assigns and
releases, what `slotByAddress_` maps to, and what carries parent, child and
bounds state. Extras are marked `extraPage` and skipped by the directory sync —
without that, `sync` swept them as if each were its own chunk and each release
erased the primary's address mapping.

**Tests** (`tests/voxel/TerrainGPUVirtualGeometryPageSetTests.cpp::testPageSlotReservation`)
- Three pages reserve three distinct slots; asset page 0 is the primary.
- A page beyond the reservation has no slot.
- Shrinking returns exactly the surplus.
- Exhaustion fails, explains itself, and changes nothing (all or nothing: a
  partially reserved chunk would publish a partial DAG the culler cannot
  distinguish from a complete one).
- Releasing the chunk returns every slot it owned, and the pool serves the next
  chunk.

## Phase 2 — Per-chunk hierarchy sub-allocation in one object — **DONE 2026-09-04**

**This phase was respecified on 2026-09-04 after measuring a real asset.** The
original text said "install each `asset.pages[i]` into slot `firstSlot + i`",
which assumed a chunk's hierarchy was one node per page. It is not.

### Measured shape of a terrain chunk asset

`SparseVoxelOctreeVirtualGeometryTests` now prints and asserts it:

```
[shape] pages=1 hierarchyNodes=49 installUpdates=40 uninstallUpdates=0 pagesWithUpdates=1
```

- **1 page, 49 hierarchy nodes.** The DAG over the chunk's brick octree lives in
  the hierarchy, not in the page split. Larger chunks (400-3600 meshlets) have
  proportionally more.
- **40 install rewrites, present.** The builder does supply them, so a slot
  install has real per-cluster flags to forward.
- `uninstallUpdates` is empty for a single-page asset, which is consistent: there
  is no other page of this asset whose flags need restoring.

### Why the flat hierarchy does not work

`buildTerrainGPUVGRuntimeObjectHierarchy` builds root → one leaf per page slot,
and installs only patch a leaf's metadata (`overrideHierarchyLeafMetadata`) and
apply rewrites. `installRuntimeObjectEncodedPage` is a thin wrapper over
`installRuntimeObjectPage`: it allocates page/cluster/decoded ranges and
forwards the rewrites. **Nothing in the install path writes hierarchy nodes.**

That is sufficient for the *generated* route, whose pages are decoded clusters
emitted by the GPU with no internal DAG — one leaf each is the whole truth.
Collapsing a CPU chunk's 49-node DAG into one leaf would destroy intra-chunk
LOD, which is the opposite of the goal.

### What the runtime is missing

Hierarchy nodes are allocated **per object**, not per page — stated outright at
`evictLeastRecentlyUsedRuntimeObjects`: "relieving pressure there means retiring
whole objects". A shared object holding many chunks needs per-chunk hierarchy
sub-allocation, which does not exist today.

The upload primitive does exist:
`VirtualGeometryStreamingManager::registerHierarchyUpload(hierarchyAllocationOffset,
hierarchyNodeOffsetInFile, hierarchyTemplate)` writes a node range at an
arbitrary offset, and `rebuildHierarchyFromInstalledPages` re-uploads it when
rewrites are applied.

### Corrected design

1. **Reserve a hierarchy sub-range per page slot.** Register the shared object
   with a hierarchy allocation of `1 + capacity × maxHierarchyNodesPerChunk`.
   Size `maxHierarchyNodesPerChunk` from a measured maximum over the terrain
   build settings and fail loudly when a chunk exceeds it — silently truncating
   a DAG reads as missing LOD levels, not as an error.
2. **New scene API**: `writeRuntimeObjectHierarchyRange(objectName, firstNode,
   nodes)`, wrapping `registerHierarchyUpload` against the object's hierarchy
   allocation. Nothing exposes this per object today.
3. **On chunk install**: copy `asset.hierarchy` into the slot's sub-range with
   every `child_start` shifted by the sub-range base, and point the shared root's
   child entry for that slot at the sub-range's root node.
4. **On chunk uninstall**: rewrite the slot's subtree root as an empty
   force-traversal node so the culler skips it, and clear the parent's covered
   bits (Phase 3).
5. Page installs then proceed as originally written — one page per slot from
   Phase 1's `slotForAssetPage` — with `hierarchyClusterFlagUpdates` taken from
   `asset.pages[i].installUpdates.hierarchyUpdates` and `hierarchyNodeIndex`
   shifted by the same sub-range base.

### Tests

`tests/voxel/SparseVoxelOctreeVirtualGeometryTests.cpp` (**done**)
- The build supplies install rewrites, and every encoded page carries them —
  the guard against the empty-rewrite failure mode.
- A chunk's DAG has more nodes than it has pages, so this phase's premise cannot
  silently regress to the flat form.

`tests/voxel/SparseVoxelTerrainVGSlotInstallTests.cpp` (**done**, 30 checks)
- Layout: node count covers root, stubs and slices; chunk roots are gathered in
  front so the shared root's children are one contiguous run; slices follow.
- **No two `(slot, chunkNode)` pairs map to the same object node** — checked
  exhaustively over the layout, because a collision silently overwrites one
  chunk's DAG with another's.
- Empty slots are inert (no children, no page).
- Remap: every child run lands inside the owning slot's slice; page indices map
  through the slot's page slots; the `PAGE_NOT_INSTALLED_BIT` survives, since it
  is what marks a page parked rather than missing.
- Rejections, all of which would otherwise publish a corrupt DAG: a chunk larger
  than its reservation, a child run beginning at the chunk root (which would not
  stay contiguous), a page the slot does not own, a slot beyond capacity.
- Rewrites: remapped into the slice, out-of-range dropped, bitsets unchanged.

### Implemented

- `VirtualGeometryScene::writeRuntimeObjectHierarchyRange(objectName, firstNode,
  nodes)` and `runtimeObjectHierarchyNodeCount` — the missing runtime primitive.
  It updates the CPU mirror the rewrites patch and uploads only that run, which
  is the same shape as the existing single-node rewrite write.
- `SparseVoxelTerrainVGSharedObjectLayout` with `nodeIndex(slot, chunkNode)` and
  `hierarchyNodeCount()`.
- `buildSparseVoxelTerrainVGSharedObjectHierarchy`,
  `remapSparseVoxelTerrainVGChunkHierarchy`,
  `remapSparseVoxelTerrainVGChunkRewrites`,
  `installSparseVoxelTerrainVGChunkIntoSlot`,
  `parkSparseVoxelTerrainVGChunkSlot`.

Pages are installed before the hierarchy that points at them: a node published
ahead of its page would name a slot the culler can already reach. Park writes
only the chunk root — the slice behind it is unreachable once nothing points at
it, which keeps an uninstall to a single node write.

## Phase 3 — Cross-chunk rewrites — **DONE 2026-09-04**

A cluster is a *streaming leaf* when the finer geometry it stands in for is not
resident: the culler may draw it but may not refine past it. While a coarse
chunk's four children are absent every one of its clusters is such a boundary,
and as each child lands the coarse clusters covering that quadrant must stop
being boundaries — otherwise the cut can never descend and the terrain sits one
level too coarse forever.

### Implemented

- `sparseVoxelTerrainVGChunkNodeQuadrants(chunkHierarchy, centerX, centerZ)` —
  the quadrant mask each hierarchy node reaches. Bits match
  `TerrainGPUVirtualGeometryPageSet::quadrantOf`: bit 0 is +x, bit 1 is +z.
- `sparseVoxelTerrainVGChunkRewritesForResidentChildren(layout, slot,
  baseUpdates, nodeQuadrants, residentChildMask)` — the chunk's own install
  rewrites adjusted for which children are resident and remapped into its slice.

### Two decisions worth keeping

**Coverage is per node, not per cluster.** Cluster bounds live in the encoded
page; node bounds are in the hierarchy already. It costs nothing in practice
because a chunk's octree splits on the same axes as the quadrants, so below the
chunk root every node lies wholly inside one quadrant; a node that does straddle
waits for every quadrant it touches, which is the conservative direction. This
does **not** coarsen LOD granularity — which cluster to draw is still decided per
cluster by the culler's self/parent error test, and these bits only bound how far
it may descend.

**`enabledClustersBitset` is untouched.** A resident child does not disable the
parent's clusters: both stay in the DAG and the error test picks. Clearing them
would delete the coarse representation instead of superseding it.

**Residency is a pure function of `residentChildMask`**, so a child leaving
restores the previous bits exactly — no accumulated state to drift. That is what
the last test pins.

### Tests (`SparseVoxelTerrainVGSlotInstallTests.cpp::testResidentChildRewrites`)

- Quadrant masks: one node per quadrant, the chunk root reaching all four, and a
  node straddling the x axis reaching two.
- Nothing resident: every node stays a boundary; rewrites are remapped into the
  slot.
- One quadrant resident: its node clears, an uncovered quadrant is untouched,
  the straddling node waits, the chunk root waits, and the enabled mask is
  unchanged.
- Three resident: the straddling node clears once both its halves are in, the
  root still waits.
- All four: everything clears.
- Returning to one resident reproduces the one-resident state bit for bit.

## Phase 4 — Demand-driven generation — **policy DONE 2026-09-04, wiring pending**

### Implemented

`planTerrainVGPageBuilds(pageSet, demand, inFlightSlots, maximumRequests)` in
`TerrainGPUVirtualGeometryPageSet.hpp`, with `TerrainVGPageDemand` and
`TerrainVGBuildRequest`. It is a pure function so the policy — which requests
survive, in what order, how many — is testable without a GPU.

Rules, each of which is a way this goes wrong when missed:
- Highest priority first, ties broken by slot, so a frame's plan is
  reproducible rather than hash-ordered.
- Demand naming one of a chunk's **extra page slots resolves to the chunk**
  (via `slotOf(page->address)`), because a chunk is built and installed whole.
  Several demands across one chunk's pages collapse to a single build.
- Already installed, or already in flight, is dropped.
- The budget caps the plan; a zero budget plans nothing; a slot holding nothing
  is ignored.

### Tests (`TerrainGPUVirtualGeometryPageSetTests.cpp::testDemandPlanning`)

Ordering and tie-breaking, the ground address carried on the request, in-flight
dedup, installed dedup, extra-page resolution, collapse of several pages of one
chunk, budget cap, zero budget, and demand for an unassigned slot.

### Still to wire

The read side. The sample already runs
`collectRuntimeObjectPageRequests(objectName, …, &priorityReadback)` for the
generated route (~line 34850); the CPU route needs the same call against the
shared object, feeding `planTerrainVGPageBuilds` and then the mesh pool. That
depends on the CPU route being registered as one shared object, which is the
wiring step below — phases 2 and 3 built the pieces but nothing calls them yet.

## Phase 4.5 — Wire the CPU route onto the shared object — **DONE 2026-09-05: draws, zero holes, frame parity**

### Outcome (2026-09-05, 5 km, 1220x720, same binary for both routes)

| | per-chunk objects (old default) | shared object |
|---|---|---|
| covered pixels once settled | 114,138 | **219,600** |
| frames with any hole / 250 | 120 (57 enclosed) | **0** |
| parent links on install | — | 253/253 |
| republishes with a live mask | — | 248/248 |
| frame median / p90, no harness | ~19 ms | **19.0 / 32.5 ms** (Untracked 0.41 ms) |
| streaming section (job launch) | 160-572 ms | **< 60 ms every frame** |

The old route was carrying a permanent non-enclosed horizon hole (~105K px)
that the shared object fills; its transient enclosed holes were commit-frame
gaps between per-object retire and install, which the shared object has no
equivalent of (a chunk's clusters are enabled by its parent's rewrite, in the
same apply).

**Correction (2026-09-05, later the same day):** the 219,600 was not terrain
coverage. A dumped frame showed the camera looking at a coarse bootstrap
chunk's clusters drawn *over* the refined ground - a slab filling most of the
view - because every chunk's finest clusters carried self error 0 (the LOD0
convention: "a cluster with nothing finer under it must never be rejected")
and so a parent kept drawing after its children arrived. Once the finest
clusters carry the chunk's voxel as error, the same view is the landscape
(sky, far ridges, refined ground) at 126,551 px, still with zero holes. The
per-chunk-object route's 114,138 and the shared route's 219,600 are not
comparable to it or to each other; the hole count is the metric that held.

### Making the parent/child transition the culler's (2026-09-05)

Demand-driven refinement never ran on this route until today: the request
block collected page requests from the GPU-generation lane's object name,
which the shared route never registers, and the oversized-leaf demand block
was keyed by per-chunk object names. Wired to the shared object (page slot ->
owning chunk slot via `TerrainGPUVirtualGeometryPageSet::ownerOf` -> record ->
refine one LOD) the cut grows 6 -> 410 chunks in 400 frames, and the culler
then showed the real defect: demand came from *parents whose four children
were all resident* - they were still drawing.

Four pieces make the transition a per-cluster culler decision:
- `VoxelOctreeVirtualGeometrySettings::leafSelfError` (the page cache sets it
  to the chunk's voxel under `absoluteOrigin`): the finest clusters carry a
  real error, so a parent's finest level is rejected where a finer chunk draws.
- `VirtualGeometryBuildSettings::finestClustersAreStreamingLeaves` (set with
  it): the finest clusters get the streaming mark, which forces their error to
  zero until residency clears it per quadrant. Without the mark the frontier
  is rejected wherever its voxel projects over budget. A mark from this rule
  must not hold a group pending in `buildPagesAndRewrites` - it never clears
  inside the build - or the page loop emits empty pages forever (the first
  attempt hung the mesh job and the run was killed at the 10-minute limit).
- The chunk root's `max_parent_lod_error` is the installed parent's finest
  error (`rootParentError` at install, `republishTerrainVGSharedChildRoots` on
  parent republish and retire): the child slice is visited only where the
  parent's finest clusters are too coarse.
- The culler publishes demand for oversized *streaming* leaves, not only true
  leaves.

Tests: `SparseVoxelOctreeVirtualGeometryTests::testFinestClustersAreStreamingLeavesWhenTheyCarryError`
(finest level fully marked with a real error, unmarked without, page count
unchanged). `SparseVoxelTerrainVGCutErrorContract` now reads its own
`refinableSelfPx`: greedy-merged flat quads are large triangles with small
error, so refinable giants are a defect only when their error exceeds the
budget; frontier giants are streaming leaves now and bounded by the generation
budget, not the install pipeline.

Frame time under continuous refinement (no harness, 200 frames): median 18.4
ms, p90 55.5, max 354; the spikes are the commit (`Terrain commit` section,
max 103 ms), the upload stage (66 ms) and GPU back-pressure after installs
(255 ms) - Phase 6's budget, now measurable per section.

Two measurement traps paid for here: (1) `--test-world-terrain-raster` adds
~115 ms/frame of untracked attachment readback on *every* route — the "shared
route is 179 ms vs 13.6 ms" gap was two runs with and without the flag; time in
a separate run without it. (2) The 330-572 ms "cacheCopied" at streaming-job
launch was the whole chunk cache deep-copied on the render thread;
`WorldVoxelChunkEntry::chunk` is now `std::shared_ptr<const PlanetVoxelChunkSet>`
(an audit found no in-place writes after creation), and the copy is a pointer
per entry.

One warning survives at ~1 in 250 frames: `slot N has installed pages outside
its record (pageSetInstalled=1 keyInstalled=0)`; the surplus retire handles it
and no hole follows, but the cause is not explained.

The sections below are the record of getting there.

### Done

- `VirtualGeometryScene::applyRuntimeObjectHierarchyClusterFlags` — the second
  missing runtime primitive. A page install carries its own asset's rewrites;
  restating a *different* chunk's masks when a child arrives has nothing to ride
  along with.
- `registerTerrainVGSharedObject`: measures `maxChunkNodes` from the bootstrap
  cut, builds the shared hierarchy, registers the object, configures the page
  set, and fills the slice free list.
- `syncTerrainVGSharedSlots`: slot assignment only. The generated route's sync
  also publishes leaves to an object this route never registers, which was the
  first failure.
- `installSharedObjectChunk` and the retire path (`park` + free the slice +
  republish the parent).
- Hierarchy buffer sized for the shared allocation.

### Four bugs found by running it, each worth keeping

1. **A hierarchy leaf's `child_start` names clusters in its page, not child
   nodes.** Shifting it pointed leaves at other chunks' geometry. Only interior
   nodes (`(flags & HIERARCHY_LEAF_FLAG) == 0`) may be remapped. My Phase 2
   remap was wrong and the unit tests did not catch it, because the synthetic
   hierarchy they used had no leaf flags set.
2. **Hierarchy slices are per chunk; page slots are per page.** Indexing slices
   by page slot reserved a slice for every page: 4096 slots by 1728 nodes, about
   450 MiB, for the ~500 chunks that own one. Slices now come from their own
   free list.
3. **The runtime object's `pageCount` must be the page-slot pool**, not the
   slice count, or installs fail with "encoded page index is out of range".
4. **The object's quantization must be the assets'.** An invented
   `unit_scale`/`padMeshlets` decodes vertices at the wrong scale.

### Five bugs found by running it (the fifth is unfixed)

1. **A hierarchy leaf's `child_start` names clusters in its page, not child
   nodes.** Shifting it pointed leaves at other chunks' geometry. Only interior
   nodes (`(flags & HIERARCHY_LEAF_FLAG) == 0`) may be remapped. The Phase 2
   unit tests missed this: the synthetic hierarchy they use sets no leaf flags,
   so every node looked interior.
2. **Hierarchy slices are per chunk; page slots are per page.** Indexing slices
   by page slot reserved one per page — 4096 slots by 1728 nodes, ~450 MiB, for
   the ~500 chunks that own one. Slices come from their own free list now.
3. **The runtime object's `pageCount` is the page-slot pool**, not the slice
   count, or installs fail with "encoded page index is out of range".
4. **The object's quantization must be the assets' own.** An invented
   `unit_scale`/`padMeshlets` decodes vertices at the wrong scale.
5. **Chunk-local geometry cannot share an object** — one object carries one
   instance transform, and chunk assets are built relative to their page origin
   (`settings.originOffset = origin`) with the per-chunk instance placed there.
   Added `SparseVoxelOctreeVirtualGeometrySettings::absoluteOrigin`, set for this
   route, so chunks are meshed in the renderer's world space. The page-relative
   default exists so a planet-scale coordinate never reaches a float; that limit
   is not reached at this world size — 64 km resolves ~4 mm in float32, forty
   times finer than the 10 cm voxel — but a genuinely planet-scale world would
   need per-chunk placement back, and one object per chunk with it.

### Earlier state (superseded 2026-09-05): installs clean, draws nothing

```
Terrain shared VG (apply): instance=0 pages=8192 installed=429 traceable=429
                           encoded=429 hierarchyNodes=884737 slices=154
coveredPixels=0
```

No install failures, a live instance, geometry in world space — and no pixels.
`meshPartTransforms` was considered and rejected as an alternative to world-space
geometry: it is the skinning bone palette (`MeshPartInfo::dominantBoneIndex`),
and routing terrain through it would move every cluster onto the skinned culling
path.

6. **`unitScaleBits` was never set.** The reference loop in
   `installRuntimeEncodedObject` copies it from
   `asset.quantizationConfig.unit_scale`; mine left the field at its default
   zero, which the decoder uses to reconstruct vertex positions. Fixed — and it
   did **not** restore coverage, so it was a real bug but not the one.

### Ranked next checks at that point (historical)

1. **Read back the shared object's hierarchy** (`writeRuntimeObjectHierarchyRange`
   has a mirror; add a debug getter) and confirm after an install that
   node `1 + slice` is the chunk root, its `child_start` lands in that slice, and
   its `pageIndex` names an installed slot. This distinguishes "the DAG is wrong"
   from "the DAG is right and the culler never reaches it" — every hypothesis
   below assumes one or the other.
2. **Instance bounds — now the leading suspect.** `objectBounds` comes from
   `chunks.minimumWorldMetres/maximumWorldMetres`. With `absoluteOrigin` the
   geometry is in *renderer* space, where Y is measured from
   `chunks.elevationOriginMetres`; if those bounds are absolute elevations, the
   instance's box does not contain its own geometry and the culler discards it
   before visiting a node. Compare the object's `localBounds` against a written
   chunk root's bounds — the debug getter
   `VirtualGeometryScene::debugReadRuntimeObjectHierarchy` was added for exactly
   this and is unused so far.
3. **Chunk root errors.** The asset's own root carries
   `max_parent_lod_error = FLT_MAX` (never choose the parent) and its
   `min_lod_error` is chunk-relative. Across chunks these should come from
   `TerrainGPUVirtualGeometryPageSet::selfError`/`parentError`, as
   `terrainGPUVGPageInfo` does for the generated route. Note this predicts *every
   level drawing at once*, not zero coverage — so it is unlikely to be the whole
   story on its own.
4. **Whether the terrain VG pass enumerates the object at all**, and whether the
   per-chunk route publishes instances through a path this one skips.

## Phase 5 — Delete the CPU cut — **5a DONE 2026-09-05, 5b open**

Deleting `scheduleTerrainVirtualGeometryObjects`,
`applyTerrainVirtualGeometryChunks` and `refreshTerrainVirtualGeometryPages`
removes the only path that currently draws terrain. It cannot precede a working
shared-object route, or the renderer draws nothing. Order: finish 4.5, verify on
a device, flip the default, then delete.

### Status (2026-09-05)

Done (5a): the shared object is the only route. `gTerrainVGSharedObject` and
the per-object branches are gone; `--terrain-vg-shared-object` is accepted as a
no-op. `scheduleTerrainVirtualGeometryObjects`, `TerrainVGScheduleObject` and
`TerrainVGScheduleResult` are deleted from `TerrainVirtualGeometrySceneSchedule`
(the footprint/key helpers stay - the shared route keys its installed set by
ground); `TerrainVirtualGeometrySceneScheduleTests` keeps only
`testGeometryHelpers`. `applyTerrainVirtualGeometryChunks` takes no pending
ground and produces only install/replace; the per-object error-demand block
(page requests by per-chunk object name) is deleted - the page-set request path
above it is the shared object's demand feed. The dead per-chunk sync of the
page set in the main loop is gone. Verified after deletion: 250 harness frames,
0 holes, coverage 219,600, no page-set warnings.

5b withdrawn (2026-09-05): the page cache's slot adoption is load-bearing on
this route too. The cache is positional (`entries_[chunkIndex]` mirrors the
streamer's record index) and adoption is what keeps a meshed chunk when the
streamer moves its record: the 400-frame slope run logged `adoptions=74` and
`adoptions=279` in single refreshes. Deleting it would re-mesh those chunks;
re-keying the cache by ground address would remove it properly but buys
nothing measurable (adoption detection is 0.1-13 ms on commit frames only).
`refreshTerrainVirtualGeometryPages` stays as the mesh-and-hand-over driver
(4 ms incremental, was 83 ms). The GPU generation lane
(`--terrain-generation gpu-v2`) is still in the tree, off by default.

Also removed (2026-09-05): the replace path's `unregisterKey`, which derived
the slot from the entry's floating-point footprint - the two-derivations trap
the install path already avoids. A replacement is now just an install; the
slot install retires what the slot holds. Suspected cause of the residual
"installed pages outside its record" warning (4-5 per run); to be confirmed
by the next long runs.

**Changes**
- Remove `scheduleTerrainVirtualGeometryObjects`,
  `applyTerrainVirtualGeometryChunks`, `refreshTerrainVirtualGeometryPages`, and
  the `(ground, level)` object naming.
- The page cache keeps only: mesh a chunk, hand over its pages. Slot identity
  comes from the page set, so the slot-adoption machinery
  (`collectStaleChunks`' adoption index, `pendingRelease_`) goes with it.
- `updateWorldVoxelChunkCache` keeps residency and eviction; it stops deciding
  which level is drawn.

**Tests**
- `TerrainVirtualGeometrySceneScheduleTests` is deleted with the scheduler; its
  coverage-invariant cases move to Phase 3's hierarchy tests.
- `SparseVoxelTerrainVGCutErrorContract` must still pass: rendered
  triangle-pixel error stays under budget with the GPU choosing.
- Frame-time gate: no render-thread section above 20 ms at 300 resident chunks.

## Phase 6 — Budgeted per-frame upload — **first half implemented 2026-09-05, unmeasured; second half open**

### Status (2026-09-05)

`applyTerrainVirtualGeometryChunks` takes an install budget: the incremental
and commit applies install at most `--terrain-vg-install-budget` chunks
(default 4; ~2 ms CPU each plus the parent republish) and report the rest as
`deferred` in the apply log; the cache keeps them resident and the next
apply's residency plan picks them up. The bootstrap apply is unlimited.
Replacements go first (a waiting re-mesh keeps drawing its old geometry; a
waiting new chunk leaves its parent standing in). The commit now has its own
CPU section (`Terrain commit`, inside `finalizePendingFrame`).

### What the commit frame was, and is (2026-09-05, 200 no-harness frames, 1220x720)

Once demand-driven refinement ran continuously, the transaction commit became
the frame's whole problem: median 17.9 ms but p95 533 ms and a 1.4 s worst
frame. Attributed with trace timers (`World terrain commit block ... detail:`,
`Terrain GPU commit detail:`, `Tree64 layer commit:`, `Terrain page table
publish:`, `Terrain Tree64 TLAS publish:`) and cut in this order:

| cause | before | fix | after |
|---|---|---|---|
| render thread starved by generation workers (SDL poll 380 ms) | p90 113 ms | worker pools inherit the render thread's user-interactive QoS on macOS; `os::hqos::setWorkerQos()` (default class) on every terrain pool and job thread, `setHighQos()` on main. Utility class throttled generation to 9 chunks/200 frames; user-initiated starved the driver (GPU back-pressure p90 123 ms) | p90 45 ms |
| `tree64DynamicModelSources = payload.models` copied every model's leaf bounds and prebuilt nodes, then cleared them | 48-147 ms | copy the five descriptor fields | 1-7 ms |
| Tree64 layer `commit()` freeing the previous model/instance sources on the render thread | 28-54 ms | `takeRetiredSources()` parked, released on a worker (`terrainAsyncReleases`) | 0 |
| two capacity-sized LOD-group index arrays rebuilt and written although empty | 17-132 ms | written once, skipped while empty | 0 |
| VG installs per commit | 12-72 ms | budget of 4 per apply | bounded |

Untraced after all of it: median 16.5 ms, p90 42.6, p95 55.6, max 205;
`Terrain commit` max 47 ms (was 360-430), GPU wait max 14 ms (was 552).

Still transaction-shaped and still on the commit frame (traced): the brick
runtime page-table publication (`prepare` 85-91 ms on commits where the page
count grew - buffer reallocation - and four `writeVector`s of ~500 pages),
the Tree64 TLAS rebuild over every instance (9-65 ms), the cache move
(5-60 ms), and the Tree64 descriptor validate/publish (2-18 ms). These are the
trace structure's publication, deferred by the brief ("first the geometry,
then the trace structure"), and the remaining second half of this phase.

The contract test runs untraced now (the trace's logging doubled frame time
at a few hundred chunks); its budget is 1800 s.

### The process is above RAM (2026-09-05)

`top -stats mem,cmprs` during any run: 15-17 GB, 11-13 GB compressed, on a
16 GB machine, with ~900K swapouts in three minutes. `vmmap`: 7.4 GB of
large heap, 5.4 GB of GPU memory. `MallocStackLogging=1` +
`malloc_history -allBySize`: the 1.52 GiB brick micro-surface arena existed
five times (the live chunk set, `terrainArenaRecycleBin`, the renderer
constructor's deep copy `brickHierarchy = *inBrickHierarchy`, the packed
first-upload mirror `brickMicroSurfaceWords`, and the host-visible GPU buffer),
the 0.19 GiB material-index arena likewise. `ENGINE_DIAGNOSE_BUFFERS=1`
prints every GPU buffer over 16 MiB (`VirtualGeometryDecoded*` 1.83 GiB, of
which the bone-weight buffer is 0.46 GiB terrain never uses; `candidates`
0.5 GiB twice; `brickMicroSurfaceData` 1.52 GiB host-visible).

Swapping is what made every CPU section - event polling included - spike
together on commit frames; thread QoS was a real but secondary cause.

Done: the renderer's two constructor copies are released right after the
bootstrap upload (only their sizes are read afterwards, kept in
`brickHierarchyCounts`; the job's `previousMaterialWords` parameter is
unused). Footprint 17.4 GB -> 13 GB, harness unchanged (0 holes).

Then (2026-09-05, Tree64 out of scope by the brief): the arena trace
(`World terrain arena capacity`) showed the outlier is the material arena -
204M entries against 144K nodes (512 slots x the largest chunk x 4, the 4x
being the documented cliff-chunk headroom that must stay) - and the render
graph dump showed this route runs only the brick culler's feedback passes,
no draw or G-buffer pass, so nothing reads the micro-surface words. Four
cuts:
- `WorldVoxelChunkStreamingSettings::packMicroSurface` (off unless the brick
  G-buffer route is requested): the world arena's micro-surface array is
  shaped to zero, the slot writer, byte accounting, upload ranges, packing
  and layout checks skip an empty array, the GPU buffer is one element. That
  is the 1.52 GiB array in the live arena, the recycle bin and GPU memory.
- `VirtualGeometryScene::Settings::decodedBoneWeightCapacity`: terrain never
  skins, so the pool is 1024 entries instead of 466 MiB derived from the pages
  buffer.
- `ensureTerrainRuntimeStorage` grows geometrically: the page-table
  publication re-created its buffers on every transaction that added pages.
- The replaced world chunk cache (last owner of evicted payloads) is released
  on a worker through the bounded page-release queue.

The cut-error contract line now carries `chunks=`; the recipe reads the
resident count from it (the last `chunks=` in an untraced log is a bootstrap
line, which failed the deepening check at 6 while every other criterion
passed at 1200 frames).

A `vmmap` after the four cuts showed the large heap down from 7.4 GB to
0.87 GB but the small heap up from 3.2 to 6.7 GB with residency: ~16-19 MB
per resident chunk, which is the per-brick micro-surface words in every
chunk *payload* (4 KB per brick, ~4096 bricks per fine chunk) - built by
`buildSparseVoxelBrickHierarchy` for every chunk and kept alive for the whole
resident cut. `SparseVoxelBrickBuildSettings::buildMicroSurface` (carried by
`PlanetVoxelChunkSettings`, set from `packMicroSurface` in the page source)
skips producing them; the slot writer and both payload serializers accept an
absent sidecar.

### The growth over time was the render graph (2026-09-05)

A long run grew from 14 GB at 25 s to 27-32 GB at 55-75 s. `MallocStackLogging`
+ `malloc_history` between two snapshots: 4 million live allocations, +3 GB
per 40 s, all from `RenderGraphRuntimeResourcesManager::resolveBindings`.
`lib::FlatMap::find` returns `std::optional<V>` - a copy - and the code pushed
each new binding-group variant into that copy, so the stored list stayed
empty, every run found no matching variant, created another binding group
(and its GPU descriptors) and leaked it. Fixed by rebuilding the list and
storing it back (`erase` + `insert`). This, not terrain, is what made the
sample "get slower as time progresses" - and what rebooted the machine: a
long run reached 32 GB on 16 GB of RAM.

**Rewind (2026-09-05, after the second crash):** the first leak fix stored
the variant list back so binding groups were reused across runs for the
first time ever - and the bootstrap frame hung the GPU (a cached group over a
frame-local resource; the pattern is already in memory), which is what took
the session down the second time. Reverted. The fix now releases the run's
created binding groups at `endRun` through the retained-resource path the
graph already uses for override groups: same per-frame creation as before,
no reuse, no leak. Root-page-first install is opt-in
(`--terrain-vg-root-page-first`) until its own bootstrap hang is understood
in short bounded runs. Rules from here: every run carries the memory guard,
a frame limit and a watchdog kill; no run longer than the contract test; one
change per build, verified by a bounded smoke before the next. Builds run
at background QoS with at most four jobs (`taskpolicy -c background nice -n
19 cmake --build ... -j 4`): the three WindowServer watchdog kills after the
reboot coincided with full-core builds, not with runs.

**Guard (2026-09-05):** the sample now checks its physical footprint every
15 frames and fails the run with `Memory budget exceeded` when it crosses
`--memory-budget-mb` / `SVR_MEMORY_BUDGET_MB` (default 70% of physical RAM).
Every run, the ctest contracts included, is bounded by it; a test that
trips it has found a memory bug, not a flaky machine.

### Pages by LOD error (2026-09-05, per the brief)

Measured with `SVR terrain VG page use` (trace, every 60 frames): at 342
resident chunks **1,250 pages installed, 39 touched by the culler per frame**
(3%). Far chunks and the coarse pages of near ones were meshed, encoded,
uploaded and decoded for nothing. Change: a chunk installs only its root page
(`SparseVoxelTerrainVGEncodedAssetRuntimeInstallSettings::installAllPages`,
`--terrain-vg-install-all-pages` restores the old behaviour); the finer
pages keep `PAGE_NOT_INSTALLED_BIT` on their leaves, the culler requests
them through the page-priority path that already existed (it was always
empty on this route), and `installTerrainVGRequestedPages` installs the
requested page plus any missing predecessor from the asset the page cache
holds (`installSparseVoxelTerrainVGChunkPage`; pages depend on their
predecessor by construction), `--terrain-vg-page-install-budget` per frame.
Chunk *refinement* demand is suppressed while a chunk still has uninstalled
pages, so a page-boundary streaming leaf never asks the streamer for a finer
chunk that its own next page would satisfy. Per-level lazy *meshing* (only
building the levels a chunk's distance needs) is the follow-up; installs are
lazy now, generation is not yet.

**Verified (2026-09-05, after the four cuts):** `SparseVoxelTerrainVGCutErrorContract`
**passes** in 147 s (it timed out at 900-1600 s before). Hole harness: 250
frames, 0 holes, 0 page-set warnings, 360 chunks. Untraced 200 frames at
1220x720 with continuous refinement: **median 15.3 ms, p90 18.6, p95 20.3,
max 22.7** (249 chunks); `Terrain commit` max 4.0 ms, streaming 6.4, refresh
3.7, GPU wait max 15, submit max 6.4. The 100-1500 ms spikes of the earlier
runs were swap pressure, not the code inside the sections. Large GPU buffers:
host-visible 4.33 -> 2.81 GiB, device-local 2.07 -> 1.62 GiB.


The second half — retiring the transaction-shaped publication so Tree64 page
retirement, field publication and page-table work become per-page — only exists
once the cut is gone. The first half (a per-frame byte and count budget on page
slices) could be done against the current path, but budgeting uploads while the
publication is still transaction-shaped moves the stall rather than removing it:
measured today, `File I/O / streaming / uploads` peaks at 300-720 ms on commit
frames, of which upload staging is only 17-60 ms.

**Changes**
- One budget for page slices per frame (bytes and count), drained round-robin by
  priority. The upload pass already batches; the budget moves from
  "per transaction" to "per frame".
- Retire the transaction-shaped publication: Tree64 page retirement, field
  publication and page-table work all become per-page.

**Tests**
- With a 4 MiB/frame budget, no frame uploads more than 4 MiB and every queued
  page eventually installs.
- Frame-time gate: `File I/O / streaming / uploads` stays under 10 ms at 300
  resident chunks (baseline: 300-720 ms).

---

## Phase 7 — Pages by LOD error — **in progress (2026-09-06)**

Brief: far geometry carries only what its LOD error needs. Measured at 342
resident chunks: 1,250 pages installed, 39 touched by the culler per frame.

### 7.0 Process and memory — **done (2026-09-06)**
The crashes were memory: the frame-1 terrain commit took the footprint to
5.95 GiB on a 16 GiB machine (VG scene sized for 512 resident chunks, a 1M
instance buffer held three times, 512 MiB of raster candidate buffers the VG
route never binds) and the GPU driver starved with it; then a binding-group
leak (override groups orphaned by a second resolve, run-created groups never
retained) grew ~4 MiB per frame. Now: scene capacities budgeted by RAM
(`--terrain-vg-resident-chunks`, `--terrain-vg-memory-mb`, shared-object slots
follow the same count), instance capacity a scene setting with bounds checks,
the leak closed (1,860 live groups flat over 1,500 frames; counters under
`ENGINE_DIAGNOSE_BUFFERS=1`), a wall-clock memory watchdog thread (exit 5), and
the launcher v2 (display on, memory level, caffeinate, memory poll). 1,500
frames at 5 km: plateau 5.35 GiB, 345 chunks, median 14.6 ms, p90 15.6 ms.
Remaining memory, in order: Tree64 page pools (550 MiB at full capacity),
the world bundle (0.78 GiB, pages could load lazily), the second CPU copy of
the material arena (194 MiB), ~9 MiB CPU per resident chunk. See the memory
notes `windowserver-hangs-memory-starvation` and `sample-footprint-on-16gib`.

### The six steps to finish streaming — **started 2026-09-06**

Measured state at the start: 6 -> 345 chunks in ~240 frames (~100 chunks/s),
one transaction = 32 pages in 60 ms on 8 workers + 4.8 ms publish + 3 MiB
upload, render thread 1.1 ms/frame on terrain, frame 14.6 ms median with
10.7 ms GPU wait (GPU-bound), footprint plateau 5.35 GiB. Generation is not
the slow part; the lifecycle is unfinished. Order, each gated by a bounded run
through `safe_run.sh`:

1. **7a verified, default flipped - DONE 2026-09-06.** All four gates through
   `safe_run.sh`: 3-frame smoke (6 root pages instead of 15; the "bootstrap
   hang" was the memory spike, not root-first); 300 traced frames: installed
   281 vs touched 41 at 354 chunks (all-pages: 1,250 installed), 25 page
   installs in total, max 8 per frame, `pagesPending=0` in steady state; the
   250-frame hole harness (`SVR_TRACE_TERRAIN_HOLES=1 --test-frame-signatures
   --test-capture-start-frame 0`) byte-identical to all-pages: interiorHoles 0
   on every frame, 244 seam-gap frames with the same mean (1,127 px) and maxima
   (span 16 px) in both; 400 frames median 14.34 ms / p90 15.95 against
   14.59 / 15.62 for all-pages. `gTerrainVGInstallAllPages` now defaults to
   false; `--terrain-vg-install-all-pages` restores the old behaviour. The
   footprint plateau did not move (5.37 GiB): the pools are still sized at
   capacity, which is step 3.
2. **7b lazy meshing - scoped, deferred behind step 5 (2026-09-06).**
   Measured: encoded assets are 64 KB (4 pages) to 820 KB (32 pages) per
   chunk, ~50 MB CPU at 345 chunks, and meshing is about a second of worker
   time for the whole ramp - second-order at 5 km, first-order only at
   thousands of far chunks. Two shapes: the minimal one (a per-chunk level cap
   from distance, deepened on request through the existing replace path)
   costs a visible transient because the replace re-installs root-first; the
   proper one (append-only page emission) needs a hierarchy child-range
   rewrite kind at runtime, since a parent's `child_start` names clusters in
   the not-yet-meshed page. Do the proper one, after the memory work.
3. **7c pool sizing + eviction - DONE 2026-09-06.** The scene now has
   `getRuntimePoolUsage()` and `evictLeastRecentlyUsedRuntimePagesForHeadroom`
   (all four pools: encoded pages, clusters, decoded vertices, decoded indices;
   deferred releases counted as free; root pages, this frame's touches and
   pages protecting a child are never evicted). The sample runs it once per
   frame right after `touchRuntimePagesFromGPUPriorities`
   (`--terrain-vg-pool-headroom`, 15%), retries an install once after evicting
   when a pool ran dry, prints pool usage and evictions in `VG page use`, and
   warns when a pool is over 90% full. Sizing on 16 GiB: decoded budget 192 MiB
   (was 512), pages buffer one chunk's worth per slot (116 MiB, was 233).
   **The sizing rule is not "touched x headroom":** with root-page-first, 256
   of the 281 installed pages are root pages, which are never evictable, and
   the rest are touched every frame; the floor is slots x one root page
   (~4k vertices, ~6.5k indices each, ~55 MiB decoded at 256 slots), touched
   pages on top. Measured: defaults, 400 frames: no eviction needed (pages 35%,
   verts 29%), plateau **4.95 GiB** (was 5.37); `--terrain-vg-memory-mb 24`
   (index pool 97.7% full): 400 static frames 1 eviction, 720 walking frames 5
   evictions, installed bounded at 272, no install failure, smoke green, frame
   median 15.0 ms; hole harness at that budget byte-identical to baseline
   (static: 244 gap frames / interiorHoles 0). **Under motion** the harness
   shows 6 single-hole frames per 250 in every configuration (all-pages,
   root-first, root-first at 24 MiB - identical spans and gap means), so they
   are a pre-existing transient of the chunk transaction, for step 5. The
   plateau is not under 4 GiB: the remaining GiB are Tree64 pools (550 MiB),
   the world bundle (780 MiB) and the arena copies, all step 5. The dolly
   harness renders no terrain (a model-scene camera); the walk harness
   (`--test-camera-walk`) is the motion test for terrain.
4. **7d coarsening feed - DONE 2026-09-06.** Why nothing coarsened: demand
   only refines; the feedback's merge votes come only from *drawn* pages, and
   the overshoot filter rightly refuses those (a chunk the camera looks at is
   at the level it needs); chunks the camera walked away from never vote. A
   720-frame walk ended at 498 resident chunks with **zero** merges. Now the
   demand tail votes from geometry: a resident chunk whose parent would still
   project under 70% of the pixel budget (nearest-approach distance, the same
   formula as `coarsenWouldOvershoot`), not demanded, holding that for
   `terrainCoarseningGraceFrames` (30), asks to merge - sibling-complete by
   construction (residents grouped by parent tile, whole groups only, oldest
   first, one priority per group, 64 requests per frame). Measured on the same
   walk with `SVR_TRACE_TERRAIN=1`: 2,405 sibling-complete merges classified,
   **2,321 coarsen rewrites applied** (1,003 with per-chunk voting), 3,206
   partial no-ops (was 10,765), resident set **375** at frame 540 (about 500
   without the feed; `SVR terrain VG lods:` prints the per-LOD histogram every
   60 traced frames), frame median 14.9 ms / p90 17.6, walk hole harness
   byte-identical to baseline (245 gap frames, 6 single-hole frames). The
   dolly harness is not a terrain camera; `--test-camera-walk` is the motion
   test, `--test-camera-travel-m` bounds it.
5. **Phase 6 second half - NOW THE GATE for the real pixel threshold.**
   Commit cost is already 1 ms; what is left is memory, and it decides
   whether the real cut fits this machine: at 1 km / `--lod-threshold 1.5`
   the culler touches ~800 pages and installs ~1,100 (against 41 / 281 under
   the 64 px test cut), the VG pools saturate (pages 182/244 MiB, decoded
   vertices 4.4M/5.0M), the resident set reaches 420-453 chunks, and the
   footprint climbs to **6.84 GiB** because capacity-sized buffers that the
   coarse cut never touched (Tree64 pools 550 MiB, brick arenas, decoded
   pools) get committed on write. To run the real threshold for 400 frames
   on 16 GiB: Tree64 pools from residency, the second material-arena mirror
   gone, the world bundle's pages lazy (0.78 GiB), then VG pools sized from
   the real working set (~300 MiB pages, ~600 MiB decoded). Gate unchanged
   plus: 1 km / 1.5 px / 400 frames under a 6 GB budget with zero install
   failures.
   **Progress (late 2026-09-06):** the world bundle drops its two transport
   copies after load (`releaseMapExportBundleTransportCopies`: flat field
   data and compressed page bytes, 0.38 GiB); the Tree64 pool is resident +
   25% + one transaction + tail instead of two complete cuts (poolFree never
   under 725 at 1 km / 1.5 px); the transient page store defaults to 512 MiB
   on 16 GiB (it was 4 GiB and had reached 328 MiB in 12 s). Footprint at
   1 km / 1.5 px / 300 frames: 6.84 -> 6.31 GiB, run completes. Composition
   at 12 s (footprint tool): IOAccelerator 1.9 GiB, GPU-private 0.76,
   MALLOC_LARGE 1.2, MALLOC_SMALL 2.3 (of which one 340 MiB material-arena
   mirror in `updateWorldVoxelChunkCache`, 328 MiB transient store, ~250 MiB
   in-flight chunk builds, 138 MiB VG encoded assets). Next: the arena
   mirror, then VG pools sized up for the real working set. Measured dead
   ends: halving the Tree64 node slot changed nothing (untouched capacity is
   never committed) and MoltenVK descriptor preallocation off saved 70 MB.
   The VG contract tests now run at 1 km. Remaining for this step: the
   material-arena mirror (one 340 MiB copy per transaction), and VG pools
   for ~1,100 pages once the mirror is gone.
6. **GPU generation route re-test - BLOCKED on memory (2026-09-06).**
   `--terrain-generation gpu-v2` under the launcher: at 5 km the route
   allocates 4.75 GiB of capacity-sized buffers before its first frame
   (brickOccupancy 774 MiB, brickNodes 580, `TerrainGPU.VG.AtlasPhysical` 561,
   terrainRuntimeNodes 388, brickLODGroups 387, Tree64 pools 550, decoded
   arenas 512) and the memory watchdog ends it at 6.67 GiB; with
   `--terrain-gpu-memory-mb 768` the arena refuses ("GPU page arena exceeds its
   budget"); at 2 km with defaults the frame-1 commit still jumps 2.35 -> 7.21
   GiB. So the five device timeouts were almost certainly the same memory
   starvation. Unblocking it is the same work the shared-object route got:
   RAM-aware defaults for `terrainGPUMemoryMiB` / `terrainPageMemoryMiB` /
   the atlas, and an attribution of the frame-1 CPU-side spike. Deferred behind
   steps 2-5; it does not gate them.

7. **Quality gates - settle contract DONE 2026-09-06.**
   `SparseVoxelTerrainVGSettleContract` (ctest, 15 s): 1 km, 640x360,
   `--lod-threshold 1.5`, 300 frames; asserts the cut's converging activity
   (issued refinements + page installs) stops by frame 260, at least 10
   LOD0 chunks under the camera, zero install failures, zero chunks without
   a slice, and zero interior holes on the captured final frame. The sample
   prints `Sparse voxel terrain settle:` on every bounded run. Two churn
   sources had to go first: (a) on the VG route the feedback feed's merge
   vote compared a VG culler page score / 65,536 against the 2.5 px merge
   band, so every observed page voted to merge after the grace period and
   demand refined it back (8-13 transactions per 100 static frames); those
   votes are now off on that route; (b) the geometric coarsening feed (7d)
   is **off by default**: under 1.5 px it cannot merge anything inside the
   render distance (a lod4 parent stays over budget until ~950 m) and its
   few boundary merges are demanded back; with both off the cut settles at
   frame 163 with no late demand. Coarsening inside the range needs the
   culler's own metric on the merged parent (cluster spheres, centre minus
   radius) - open. Also observed: ~25 raw demands per frame land on
   installed *ancestor* pages no chunk record owns (`noRecord`); harmless to
   the gate, worth a look when parent pages are revisited.
   **LOD profile contract DONE 2026-09-06.**
   `SparseVoxelTerrainVGLodProfileContract` (ctest, 13 s, same configuration):
   the sample prints `Sparse voxel terrain lod profile:` with the radial
   band table over every chunk the culler touched on the final frame (3D
   distance: chunk footprint horizontally, surface span vertically, both in
   the camera's local frame - surface heights are planet-absolute and need
   `elevationOriginMetres` subtracted). Voxel sizes are exact powers of two
   of 10 cm, so the assertions are exact: no touched chunk inside 90% of the
   render distance may be more than **one LOD level** (2x) coarser than the
   rendered-error budget wants (a chunk is quantised, so up to 2x is
   correct); the finest voxel per distance band may never decrease outward
   (monotonic radial profile); and no chunk inside the 10 cm radius
   (`voxel x 2 x pixelsPerMetre / budget`, 41.6 m at 1.5 px and 640x360) may
   be coarser than 10 cm. Measured at 1 km / 1.5 px: touched=76,
   violations=0, inversions=0, nearCoarse=0, worst overshoot 1.44 levels
   (12.8 m voxels at 980 m, i.e. at the render-distance edge where the
   streamer's own 4 px split threshold stops splitting - that threshold being
   coarser than the culler's 1.5 px budget is why the demand band exists).
   Bands at that configuration: 256-512 m 0.10-3.20 m, 512-1024 m
   1.60-12.80 m, 1024-2048 m 3.20-6.40 m.
   The remaining gates from the review, after the refactor,
   (a) a geometry-parity test: the VG route's final geometry against what the
   bricks path generates for the same chunks; (b) an image-parity test: the
   final VG image against the 10 cm voxel image within the LOD error; (c) a
   settle-time metric: frames until demand is zero and the ground within N m
   is at LOD0. Test configuration: 1-2 km render distance and an **explicit
   `--lod-threshold 1.5`** - bounded runs (`--frames`) otherwise take the
   sample's historical **64 px** test cut (`testLODPixelThreshold`), which
   is what every VG test and every run before this evening measured: voxels
   refine only when they would cover 64 pixels, so 20 cm stays until ~2 m,
   10 cm within ~1 m, and far chunks are enormous. That, not streaming, was
   the "huge chunks at distance, no 10 cm near the camera".

**Fixed the same evening, all measured through `safe_run.sh`:**
- **VG slices must cover the whole world arena (512).** The RAM-aware 256
  left ~90 of 345 resident chunks with no geometry (37,462 "no hierarchy
  slice available" warnings); the finest chunks arrive last, so it was the
  ground under the camera and the far ridges that went missing. Now 512 on
  every machine; decoded budget 256 MiB on 16 GiB.
  **512 is itself too few past ~2 km (measured 2026-09-07).** At 5 km /
  1.5 px / 1220x720 the shared object runs out of hierarchy slices:
  `noSlice=11101` in a 300-frame run, with 653 evictions and the cut frozen -
  which presents as **holes in the terrain, coarse chunks near the camera and
  geometry standing above the horizon**, appearing a few seconds in as the cut
  outgrows the slice pool. It is not a corruption and not phase 8: a control
  run with `--terrain-vg-no-scratch` is identical to the digit (chunks=500,
  idx=7.10M, lod0Chunks=4, settling at frame 233 against 231).
  `--terrain-vg-resident-chunks 1280 --terrain-vg-memory-mb 384` takes
  `noSlice` to **0** and evictions to 0 at 5 km, installed pages 1149 -> 1457,
  peak footprint 5.95 GiB. Measured ladder, all at 1.5 px:

  | render distance | slices wanted | noSlice at 512 |
  |---|---|---|
  | 1 km | ~370 | 0 |
  | 2 km | ~500 | 0 |
  | 5 km | > 512 | 11,101 |

  **Fixed 2026-09-07**, and the diagnosis was half wrong - worth recording
  both halves.
  - **The pool must cover the ancestor closure, not the resident set.**
    `syncTerrainVGSharedSlots` feeds the page set every resident record *plus
    every ancestor up to the root*, so live slots exceed resident chunks and
    the gap grows with the number of levels. At 5 km: 483 resident chunks but
    **571 slots holding a slice**. That is why a flat 512 survives 1-2 km and
    starves at 5.
  - **It is not a leak.** A self-healing reconciliation now runs at the end of
    `syncTerrainVGSharedSlots` - any slice whose slot is no longer live goes
    back to the free list - and it reclaims **0**, which is the measurement
    that killed the leak hypothesis. It stays as a cheap invariant (512-1280
    entries) and `reclaimed=` in the trace is the signal if a path ever does
    drop a slot without retiring it.
  - `terrainVGPlannedResidentChunks()` is now linear in render distance past
    2 km (`512 * ceil(km/2)`), clamped to 1280 on a 16 GiB machine because the
    slice count also sizes the hierarchy buffer and 16 page slots each.
    5 km at defaults: `noSlice` **18,414 -> 0**, `slices=571/1280`, peak
    footprint 5.95 GiB, both contracts green.
  - **Exhaustion is loud now**: one `errorf` naming the capacity, what it costs
    (chunks install no geometry and draw as holes) and the flag to raise, then
    counted silently. 11,101 identical warnings is how this stayed invisible.

  **What this did NOT fix, measured in the same runs: the holes.** Interior
  hole frames at 5 km are 87/300 before and **86/300 after** - `noSlice` was
  not their cause. Their distribution is the tell: frames 0-49: 11,
  frames 50-149: 75, **frames 150-299: 0**, against `lastActivityFrame=165`.
  They are streaming transients that clear the moment the cut settles, which
  is the same "pre-existing transient of the chunk transaction" phase 7c
  measured at 1 km (6 single-hole frames per 250) - just far more of it,
  because 5 km streams far more. **Under continuous camera motion the cut
  never settles, so they never clear**, which is why they read as permanent
  interactively. One hole region, ~1,032 px, 28 px max span. Open, and it is
  the next real defect on this route.

  **Investigation 2026-09-07 - the metric is unusable, and three mechanisms
  are eliminated. Not fixed.**

  **Stop using interior-hole frame counts as an A/B metric at n=1.** Two runs
  of the *identical* command (5 km, `--test-camera-walk`, 500 frames,
  `--terrain-vg-memory-mb 512`) gave **112 and 226** hole frames. That is a 2x
  swing with nothing changed, so every single-run comparison below the 2x line
  is void - including, on this evidence, "the scratch path halves holes"
  (112 against a 201 control) and "more decoded budget makes holes worse"
  (87 -> 112). The cause is that async meshing races the camera, so frame N
  has different residency every run; `svraster-yaw-stop-ab-harness` gets
  bit-reproducible runs only from matched poses, which a walking camera is
  not. **A deterministic hole harness is a prerequisite for fixing this**, and
  building it is the next step: many-run averaging with a stated interval, or
  a fixed-step camera with the mesh pool quiesced per frame (which changes
  what is measured and must be said), or a metric less noisy than frame counts
  (total hole pixels over the run).

  Eliminated, each by measurement rather than reasoning:
  - **Not LRU eviction of live pages.** Raising `--terrain-vg-memory-mb` to
    512 took evictions 384 -> **0** and holes did not improve.
  - **Not slice starvation.** `noSlice` 18,414 -> 0 changed hole frames
    87 -> 86.
  - **Not a ground left bare by a bailed replacement.** Added
    `bareAfterRetire`, which counts every path in `installSharedObjectChunk`
    that returns after retiring the slot's previous tenant - the reservation
    and slice paths `installFailures` never covered. It is **0** over a
    500-frame walk. The counter stays: it is the right alarm for that class
    and it was genuinely uncounted before.
  - Also observed, not the cause: `removed=0` across all 245 applies in a
    walk, so nothing is retired out from under the camera.

  **Resolved 2026-09-07 — it is meshing throughput, and the metric was wrong.**

  1. **`interiorHoles` was the wrong field.** It counts a single uncovered
     pixel with three covered neighbours - a pinhole at an LOD seam, and it
     reads 1. The visible artifact is `interiorGapPixels`, the uncovered
     terrain area, which reaches **11,883 px (a ~109x109 hole)**. Every count
     above this line is pinholes; use `interiorGapPixels` (mean and p90) from
     here on.
  2. **`--terrain-vg-synchronous-streaming` makes the run deterministic.** It
     quiesces the mesh pool, installs, and repeats until nothing is stale, with
     the apply and page-install budgets unbounded. Two runs of a 5 km walk give
     **identical** hole-frame lists, against the 112/226 swing without it. Far
     too slow to ship; it exists so a coverage change can be attributed at all,
     and it is the floor any fix is measured against.
  3. **With residency converged in-frame, coverage is essentially complete**:
     mean 13 px, p90 23, max 76 - against mean 59-135, p90 55-343, max 11,883
     asynchronous. So the artifact *is* streaming latency, and the headroom is
     three orders of magnitude on the peak.
  4. **The lever is `terrainVirtualGeometryMeshJobsPerFrame`** (4), which
     bounds how many stale chunks reach the pool each frame. At 16:
     **mean 8-21, p90 17-18** over two 500-frame walks - a 5-10x improvement on
     both robust statistics, beating even the synchronous floor on the mean,
     **at no frame-time cost** (worker time, not main-thread; the median frame
     measured faster, 127 ms against 141 ms). Exposed as
     `--terrain-vg-mesh-jobs-per-frame`.
     **Not yet the default:** at 16 the 1 km LOD profile contract fails its
     sample-size guard (`touched=27`, needs 30) while every quality assertion
     still passes. A cut that converges faster settling on a third of the
     touched chunks is a real behavioural change; understand it, then flip the
     default. That is the next step here.

  **FOUND AND FIXED 2026-09-07: a parent yielded coverage before the finer
  geometry could draw. Coverage is now additive and independent of meshing
  throughput.**

  The instrument that closed it: per frame, count chunk records that have
  geometry but no installed VG slot, and for each of those ask whether any
  *ancestor* slot is installed. Result over a 500-frame 5 km walk - frames with
  a gap over 200 px average **88.5** records with no installed geometry against
  28.5 on quiet frames, `noSlot` 2.3 against 1.0, and **`noAncestor=0.0` on
  every frame**. So the coarse geometry that should cover the waiting ground is
  always installed and present; it simply was not drawing. That is a coverage
  bug, not a residency or throughput one, and it pointed straight at the
  streaming-leaf rewrites.

  `republishTerrainVGSharedParent` cleared a parent's streaming-leaf marks for
  every quadrant in `residentChildMask`, and that mask is set by `markInstalled`
  the moment a child *chunk* installs - which under root-page-first means one
  root page and a hierarchy full of not-installed leaves the culler stops at
  while drawing nothing. Measured, same walk, uncovered terrain pixels
  (vertically enclosed, so genuinely missing terrain rather than sky between
  ridges):

  | yield rule | mean | p90 | max |
  |---|---|---|---|
  | on install (old default) | 59-135 | 55-343 | **11,883** |
  | once the child is fully resident | 59 | 195 | 380 |
  | **never (new default)** | **0** | **1** | **3** |

  **Why no yield rule works here, which is the real finding.** Inside one
  asset the builder's rule is exact and is already implemented:
  `streamingBitsetForGroup` marks cluster *i* of a group a streaming leaf iff
  any DAG child of that cluster is not yet on an installed page, and each
  page's `installUpdates` re-emit those bits for the pages resident up to it -
  so a group stops being a streaming leaf exactly when all its children are
  installed, and only then may the culler pick them.

  The terrain shared object has **no such DAG edge between chunks**. A chunk's
  finest clusters have no children in its own DAG, so the builder marks them
  permanently finest (`finestBits`), and the parent/child relation is terrain
  machinery layered on top that approximates the rule with quadrant masks.
  There is no builder-verified "all children installed" condition to appeal to,
  and every approximation measured leaks:

  | yield rule | jobs=1 | jobs=16 |
  |---|---|---|
  | fully-installed parent + fully-resident child | mean 136.6 | mean 20.8 |
  | additive (never yield) | **mean 0.20** | **mean 0.20** |

  Throughput-dependence is the signature: a coverage rule that held would be
  flat in that column. So coverage is additive - a chunk keeps drawing and its
  children draw over it where their error is better.

  **Two real defects were fixed on the way, both worth keeping regardless of
  the yield rule:**
  - **Rewrites were applied for pages that were not installed.** The cached
    per-chunk rewrites were one flat concatenation of *every* page's updates,
    republished wholesale. A page's rewrite is only valid once that page is
    resident - the builder computes it assuming every page up to it is - so
    republishing cleared intra-chunk streaming marks for pages that had never
    arrived. Rewrites are now stored per page (`updatesByPage`, with per-page
    counts carried in the scratch header) and a republish applies only the
    installed pages, latest page winning per node, in node order so two frames
    with the same residency emit the same writes.
  - The cross-chunk clear then wiped whatever survived, `streamingLeafsBitset`
    to 0, taking the intra-chunk pending bits with it.

  **Additive coverage is not acceptable as the design** (user, 2026-09-07):
  the cut must pick exactly one surface per instance, overlapping chunks are
  not allowed - that is *why* streaming leaves exist - and the installed,
  resident hierarchy must always be a valid cut. So mode 2 stays the default
  and the holes stay until phase 10 lands. `SVR_TERRAIN_VG_YIELD_MODE=0` is a
  stopgap for anyone who would rather have overdraw than holes today, not a
  design.

  **A hard invariant is already violated, measured 2026-09-07.** Pages depend
  on their predecessor by construction, so an installed chunk must hold a
  *contiguous prefix* of its pages. Counting chunks that do not, on a 5 km
  walk: **8.9 per frame on gap frames and 24.0 on quiet ones**, with
  `noRoot` tracking `pageGap` almost exactly - so these are chunks the page
  set calls installed **whose root page is not installed**. Nodes of the
  missing pages are reachable with no geometry behind them. Suspected cause:
  a re-mesh that changes a chunk's page count shifts `slotForAssetPage`, so
  previously installed pages sit at the wrong indices; not yet confirmed.
  `pageGap=`/`noRoot=` are in the `SVR terrain coverage:` trace line.

  **Fixed 2026-09-07 by enforcing the streaming rule the user stated:** the
  streaming manager owns the page hierarchy, a chunk's root page is resident
  for as long as the chunk is installed, and a requested page is installed only
  behind every page it depends on, from the root down. A repair pass at the
  head of `installTerrainVGRequestedPages` walks every installed chunk, finds
  its deepest installed page, and installs every missing page below it - root
  first - before any new request is served, because a missing ancestor
  invalidates everything above it. `installTerrainVGChunkPageByIndex` is the
  shared one-page install (scratch file, else the asset) so the repair and the
  request path cannot diverge.

  Measured, 5 km walk, 300 frames, **yield mode 2 - no overlap**:

  | | before | after |
  |---|---|---|
  | chunks with a page gap | 23.98 /frame | **0.17** |
  | chunks missing their root page | 23.98 /frame | **0.17** |
  | peak uncovered terrain | 11,883 px | **349 px** |
  | mean uncovered terrain | 168.6 px | 108.9 px |

  So the invariant violation was most of the *catastrophic* holes - a 34x cut
  in the peak - and it is fixed without overlap and without touching the yield
  policy. `rootRepairs=` in the settle summary counts the repairs (85 over 300
  frames); it should trend to zero once whatever drops those pages is found,
  and the pass is the safety net either way. The residual 108.9 px mean is
  the approximate cross-chunk yield, which is phase 10.

## Phase 10 — A real DAG from the chunk octree — **CLOSED 2026-09-08**

**What phase 10 set out to do is done: the cross-chunk relation is a real error
partition, and it needs no coverage test.** A chunk's clusters carry
`self.error = cell size` and `parent.error = 2 x cell size`, clamped to the
parent chunk's finest error, so a chunk owns exactly the octave below its parent
chunk and the two draw ranges are disjoint by construction (10c). The vertical
containment test that stood in for that is deleted - a parent brick is the OR
reduction of its eight children and the mesher emits all six faces, so the
replacement is exact and the question it asked cannot arise (10f). The yield
predicate is now the builder's own group rule: are this group's DAG children
installed (10f).

**Shipping default, 5 km walk, 300 frames, synchronous streaming:**

| | |
|---|---|
| uncovered terrain | interior gap **0.18** px mean, max 3 |
| LOD profile | `touched=205 violations=0 worstPx=2.89 worstLevels=0.95 inversions=0` |
| residency | 495 chunks, 40 at LOD0, 0 install failures, 0 bare, 0 evictions |
| root repairs | 20 |
| frame time | median 144.5 ms |

19 of 20 terrain/VG tests pass. `SparseVoxelTerrainVirtualGeometryPhase17ComputeRasterContract`
fails and is **pre-existing**: it asserts source literals
(`requestedPage->parentSlot`, `parentRecord->clipmapLOD - 1u`) that do not exist
in HEAD either, for a generated-route feature this tree does not have. It only
became visible because its binary was built for the first time.

**The default route is already virtual geometry only.** The startup line reads
`resolver request=auto selected=terrain virtual geometry route=virtual-geometry`,
and under that resolver `registerBrickResolver(prefix, /*feedbackOnly=*/true)`
registers `BrickPhase*.Reset/.Cull/.DrawArgs/.CommitLODHistory` and **no draw
pass at all** - verified by listing the registered pass names in three
configurations, zero `.Draw`, `.MergedMicroDraw` or `.ResidualDraw` in any of
them. The brick cull runs only to produce terrain page requests, which are the
streamer's LOD feedback. Nothing but virtual geometry rasterizes terrain.

*(Correction: an earlier revision of this section said the missing terrain under
the group rule was "drawn by the brick raster", citing `visibleBricks`
2,598 -> 26,981. That counter is the **cull** statistic from the feedback-only
pass; those bricks are never drawn. The pixels really are uncovered.)*

**So the group rule loses coverage, and that is the open item.** With
`SVR_TERRAIN_VG_YIELD_MODE=1` terrain pixels fall **219,600 -> 98,824**: more
than half the terrain is not drawn. `interiorCoverageGapPixels` reads only 12.98
because it counts pixels enclosed by terrain above *and* below in the same
column, and terrain replaced by sky is not enclosed - the metric understates
this failure by two orders of magnitude. `--terrain-vg-install-all-pages` and
synchronous streaming do not change it (99,111), so it is not page residency.

The mechanism is the one the additive marks exist to hide. A streaming-leaf
cluster has its self error forced to zero, so the finest resident representation
draws at *every* budget and coverage is total. Yield the mark and that chunk
draws only its own octave `[v, 2v)`; its children cover `[v/2, v)`. Wherever the
budget falls outside that union - ground the cut has refined past, which is
common in the far field - nothing draws at all. Drawn clusters fall only 22%
(66,661 -> 52,229) while covered pixels fall 55%, which is exactly the signature
of the coarse, large-area clusters dropping out.

Making one-surface-per-pixel safe therefore needs a chunk with no resident
coarser chunk to keep an unbounded upper band, decided by residency at runtime
rather than baked by the build-time clamp. `rootParentError` already does this
at the hierarchy node; the cluster-level equivalent is the remaining work.

**Phase 10 is closed on the DAG**: the cross-chunk partition is exact and needs
no coverage test, and the shipping default draws terrain with no holes.

### 10k. Tests for the error partition — **2026-09-08**

Phase 10 found roughly ten defects in how cluster errors are assigned and not
one of them had a CPU test, because every existing cut test runs with the
default settings where `parentChunkError` is infinite. Four checks added to
`SparseVoxelOctreeVirtualGeometryTests`, all passing:

- **`testGroupBoundsAreSharedByChildrenAndParent`** - a group's coarse cluster's
  `self` and each child's `parent` must be the *same bound*, sphere and error,
  which is what the triangle builder gets from assigning one `groupBounds`
  object to both. Drift there is a gap or an overlap exactly one group wide.
  Also pins monotonicity along every DAG edge.
- **`testEveryClusterIsConsumedByAtMostOneGroup`** - the octree builder has no
  equivalent of the triangle builder's
  `assert(clusters[ci].parent.error == max())`, and a twice-consumed cluster
  would silently carry the second group's bound.
- **`testParentChunkErrorClampsTheChunkToOneOctave`** - the 10c contract, and
  the important one: no cluster's parent error survives above the clamp; every
  finest cluster owns `[leafError, parentChunkError)` **exactly**; at the parent
  chunk's own error this chunk selects **nothing** (disjoint); just under it
  this chunk still draws (no gap). That is the two-chunk partition asserted
  directly, without a GPU.
- **`testRootParentErrorFollowsItsFlag`** - the flag governs exactly the root
  clusters and never changes which clusters are roots.

## The generated route's page-priority feedback — **DONE 2026-09-08**

Implemented as planned below; `SparseVoxelTerrainVirtualGeometryPhase17ComputeRasterContract`
is green. The generated route published leaves and flag updates but never read
the culler's page priorities back, so its cut had no rendered-error signal and
advanced only on the distance-scheduled feed.

The CPU route answers a page request by installing that page from an asset it
holds. The generated route holds no asset, so a page the culler asks for exists
only once the chunk owning that ground has been generated one level finer - and
the page set already names it: a requested slot's `parentSlot` is the chunk that
must refine. The parent *page* is fetched from the page set rather than assumed,
because a slot can be retired between the culler writing the priority and the
readback, and candidates are collected at 16x the frame budget so the budget is
spent on requests that survive the filters rather than on candidates. That last
point is not incidental: a cap counted in candidates is exactly what starved the
CPU route's demand queue, where every slot went to resident ancestors that hold
no record and cannot refine.

**Two caveats, both standing.** The route is not the default and
`--terrain-generation gpu-v2` ends in `kIOGPUCommandBufferCallbackErrorTimeout`
on this machine, so this is verified to build, to be internally consistent and
to satisfy the contract - not to run. And the contract is a source-text
assertion: it pins call shapes and indentation, so it cannot tell working wiring
from wiring that merely reads right. The route still needs a settle/profile gate
of its own, because the failure mode here is a cut that silently stops refining
and only a profile assertion catches that. The CPU route is unaffected: interior
gap 0.18 (max 3), 495 chunks, 40 at LOD0, 0 install failures.

## Original plan: the generated route's page-priority feedback (the red phase 17 check)

`SparseVoxelTerrainVirtualGeometryPhase17ComputeRasterContract` fails on one
check, *"VG-generated terrain consumes normal VG page-priority feedback and
schedules parent-owned refinement"*. It is a source-text assertion for three
literals - a `collectRuntimeObjectPageRequests` call on
`terrainVGGeneratedRuntimeObjectName`, `requestedPage->parentSlot`, and
`parentRecord->clipmapLOD - 1u` - **none of which have ever existed in this
tree**, on either side of the phase 10 work. It specifies unimplemented
behaviour on the GPU-generated route, which is not the default and which
device-timeouts on this machine, so the feature cannot be verified here even if
written. Writing code to satisfy a grep is worse than leaving the check red.

What it asks for, and why it is the right shape: the generated route publishes
leaves and flag updates (`publishTerrainGPUVGLeaves`,
`queueTerrainGPUVGFlagUpdate`) but never reads the culler's page priorities
back, so its cut has no rendered-error feedback at all - the same defect the CPU
route had until the demand queue was fixed. The work:

1. **Read the priorities.** Call `collectRuntimeObjectPageRequests` on
   `terrainVGGeneratedRuntimeObjectName` with the geometry-demand band, exactly
   as the CPU route does. Collect far more candidates than the frame budget and
   spend the budget on requests that survive the filters - the CPU route's
   demand queue was starved by ancestors precisely because it capped candidates.
2. **Map a requested page to the chunk that owns it.** A page the culler asks
   for is one the page set has a slot for but no geometry behind;
   `terrainGPUVGPageSet.page(slot)->parentSlot` names the chunk that must refine
   to produce it.
3. **Schedule the refinement.** Take the parent's record and request
   `clipmapLOD - 1`, through the same `frameRequests` path the CPU route uses,
   with the projected voxel size in the priority word so the queue drains
   worst-error-first.
4. **Gate it.** The generated route has no settle or LOD-profile contract of its
   own; it needs one before this can be called done, because the failure mode is
   a cut that silently stops refining and only a profile assertion catches that.

Do it on a machine where `--terrain-generation gpu-v2` completes a run.

## Confirmation: what changed in the shared virtual-geometry path — **2026-09-08**

Asked directly, and worth recording because phase 10 touched code every virtual
geometry instance runs.

**Payload generation is deterministic, and it is gated.**
`VirtualGeometryPackerDeterminismTests` builds the same fixture twice and
asserts the page count, the root page index, the hierarchy and *every page's
encoded words* are byte-identical. Green.

**Terrain residency is deterministic run to run.** Three 200-frame runs of the
same command produce the same settle line (495 chunks, 40 at LOD0, 0 install
failures, 0 root repairs, same settle frame) and the *same LOD profile* -
`touched=204 violations=0 worstPx=1.89 worstLevels=0.33 overBudget=15`, same
bands.

**The rendered output is not, and that is new.** Three runs each:

| | drawn clusters | final image hash |
|---|---|---|
| phase 10 off | 59351 / 59351 / 59348 | identical all three |
| phase 10 on | 70206 / 70938 / 70938 | two distinct values |

A few clusters of jitter exist either way and predate this work, but phase 10
widens it by two orders of magnitude and destabilises the final hash. The cut it
produces is much busier (70k drawn against 59k, `touched=204`), so more clusters
sit near a band boundary and the two-phase occlusion feedback loop carries the
difference forward. **Stated as an observation, not a diagnosis** - it is
unexplained, and it means `--test-frame-signatures` is no longer a reliable A/B
lever on this configuration.

**What the shared path actually inherited.**
- `VirtualGeometryBuildSettings::maxPageBytes` defaults to 0: the packer is
  byte-for-byte the old one unless a caller opts in (the determinism gate builds
  with defaults).
- `applyRuntimeObjectHierarchyClusterFlags` gained a defaulted
  `persistIntoPageUpdates = false`, so every existing caller is unchanged;
  `RuntimePageStatus::externalUpdates` is replayed only when non-empty.
- The install-order check and the page-prefix install live in the *terrain
  bridge*, not the scene: generic install and uninstall are untouched.
- The one real change to shared behaviour is the culler's refinement-demand
  metric, `projParentError * (rawSelfError / parentError)` where it used to be
  `projParentError * 0.5`. A cluster with `rawSelfError == 0` - the LOD0
  convention every non-terrain asset uses for its finest level - takes the old
  branch unchanged. A *streaming leaf with a real self error* takes the new one,
  which is the same value wherever the parent is one octave coarser and a
  truer one where it is not. Terrain's pre-phase-10 baseline reproduces exactly
  through it (`touched=84 violations=2`), which is the evidence that the octave
  case is untouched.

## Ancestor closure at page granularity — **2026-09-08**

The user's invariant, and it is the one the packer already guarantees inside an
asset: **all of a cluster's parent groups must be resident when it is, pages
included; the CPU checks that, installs in order, and applies the install
rewrites in that order.**

The packer emits it - `pages[childPage].dependencies.insert(parentPage)`, with
parent pages placed earlier - and the scene *reference-counts* those
dependencies so eviction cannot break a chain (`dependedOnBy`, plus the
parent/fallback guard). What it never did was **check them at install**. Terrain
passed `parentLocalPageIndex = UINT32_MAX`, so nothing stopped a page landing
before the pages holding its clusters' DAG parents, and the culler could then
reach hierarchy nodes with no geometry behind them.

`installTerrainVGRequestedPages` repaired that state after the fact, from the
root down - which is what `rootRepairs` counted, 20 per 300-frame walk, and the
task list called it a safety net rather than the cause. It was.

**Two changes make it a precondition.**
- `installSparseVoxelTerrainVGChunkPageBytes` refuses a page whose declared
  dependencies are not installed, naming the page and the slot.
- A chunk installs the page *prefix* up to and including its root page, not the
  root page alone. `rootPageIndex` is not always 0, and installing it by itself
  left its dependencies missing - the refusal above fired 2,278 times per walk
  before this, which is the same state the repair pass had been cleaning up.

Measured, 5 km walk, 300 frames, synchronous streaming:

| | before | after |
|---|---|---|
| `rootRepairs` | 20 | **0** |
| out-of-order installs refused | (unchecked) | **0** |
| `installFailures` | 0 | 0 |
| interior gap / terrain pixels | 0.18 / 218,410 | 0.18 / 218,410 |
| chunks / LOD0 | 495 / 40 | 495 / 40 |

The repair pass stays as a backstop, but it now has nothing to repair.

## Dead code retired with the vertical test — **2026-09-08**

Deleting the vertical containment test (10f) left its plumbing behind, all of it
written and never read: `SVR_TERRAIN_VG_VERTICAL_TEST` and its stale comment
(which still stated the retracted conclusion), `SparseVoxelTerrainVGChildCoverage`'s
`boundsValid`/`minimumY`/`maximumY`, `SparseVoxelTerrainVGClusterBlock`'s
`minimumY`/`maximumY`, the cluster surface's `slack`, and the cluster source's
`cellSize`. Removed. The quadrant mask keeps the tight XZ bounds, which are what
it actually needs.

**Kept, because it is load-bearing for a different reason:** the page set's
`boundsMin[1]` now comes from the chunk's elevation origin rather than its
minimum *surface*. That was added for the vertical test, but
`TerrainGPUV2GeneratedVGEmitPass` and `terrainGPUVGLeafInfo` use those bounds as
the hierarchy leaf's, and a bound that does not contain its geometry is a
culling defect on its own - a chunk's border columns are solid down to that
origin.

## What is still missing

1. **One surface per pixel — and the mechanism already exists, one level up.**
   Inside an asset this is solved and always has been: a page holds whole
   groups, a page declares the pages it depends on, and `streamingBitsetForGroup`
   marks a cluster a streaming leaf exactly while any DAG child sits on a page
   that is not resident. So a streaming leaf is a *valid cut* by construction -
   now asserted directly by `testEveryInstallPrefixIsAValidCut`, which walks
   every install prefix at a range of budgets and finds 0 holes and 0 overlaps.

   Terrain's cross-chunk substitution is the one edge not expressed that way:
   chunk A's clusters have their DAG children in chunk B, in another slot of the
   same runtime object, with **no page dependency between them**. Everything the
   yield machinery does - quadrant masks, drawable-child masks, the vertical
   test that turned out to be a non-question - is an attempt to reproduce by
   hand what page dependencies give for free. That is why
   `SVR_TERRAIN_VG_YIELD_MODE=1` costs 219,600 -> 98,824 covered pixels while
   the same rule inside an asset costs nothing.

   The fix is therefore not a new error rule but wiring the existing one: make a
   child chunk's pages dependencies of the parent chunk's substitution, so the
   scene clears the parent's streaming marks on install and restores them on
   uninstall, exactly as it does within an asset.
2. **`interiorCoverageGapPixels` understates missing terrain by two orders of
   magnitude** when the failure is terrain replaced by sky, because it counts
   only vertically enclosed pixels. It misled several rounds of this work. Any
   coverage judgement should read `terrainPixels` beside it.
3. ~~The `externalUpdates` replay is unproven.~~ **Covered 2026-09-08** by
   `testExternalClusterMasksSurviveTheUninstallReplay` in the phase 14 tests:
   two pages install, a cross-chunk style mask is applied with `persist`, the
   second page is confirmed installed and then uninstalled - which is what runs
   `rebuildRuntimeObjectHierarchyMasks` - and the mask must still be in the
   hierarchy. Verified to fail with `persist=false`, so it catches the defect it
   is named for. The harness could not reach this: `evicted=0` in every
   configuration that keeps the cut healthy.
4. ~~`SparseVoxelBrickDirectory`~~ **removed 2026-09-08.** It decided at *mesh
   time* whether to emit a face, and whether the neighbour that would cover it
   is *drawn* is a property of the runtime cut - so a TLAS listing every chunk
   still would not answer it. Inside an asset the DAG does: a group is drawn as
   a unit and its pages carry dependencies, which is why intra-chunk face
   culling is sound. Across chunks nothing does. With the sound half worth 1.7%
   of the triangles and the whole thing off by default, it was code that differs
   from the virtual-geometry path for no gain, so it is deleted rather than
   carried. 10d keeps the measurements; re-derive it if the cross-chunk
   substitution ever becomes a page dependency, which is the same prerequisite
   as item 1.
5. ~~**Phase 9**~~ done 2026-09-08: the seam is `VirtualGeometryPacker`, the
   build is measured per stage, and the fan-outs (mesh parts, groups, pages,
   compression) are byte-identical under the golden gate. Sponza 1.2 s → 0.5 s.
6. **Phase 8e reverted 2026-09-08**: retiring the brick culler held on the
   scripted walk and cost a third of the resident and LOD0 chunks with a free
   camera, which is what the renderer is. It needs a view-following demand
   feed, not a better global cut target. The brick *payload* was never the
   removable part: the CPU hierarchy is the VG mesher's input geometry. 8d, 8f
   and 8g are done.
7. **`SparseVoxelTerrainVirtualGeometryPhase17ComputeRasterContract`** fails,
   and it is a specification rather than a regression - see the plan below.

## Phase 10 — history — **10a/10b DONE 2026-09-07, 10c is the fix that matters**

The cross-chunk relation has to become a genuine DAG edge, so the builder's own
rule (`streamingBitsetForGroup`: a cluster is a streaming leaf iff any DAG child
of it is not on an installed page) governs the whole hierarchy instead of being
approximated by terrain-side quadrant masks. This is what makes the yield exact,
and it is the only way to have a valid cut with no overlap and no holes.

The user's shape, and it is the right one: **model the DAG on the octree, with a
group per brick.**

- **A group is one brick.** The octree already is the DAG
  (`voxel-vg-octree-is-the-dag`): a parent brick's eight children are its DAG
  children, and a brick's LOD error is its voxel size. Nothing has to be derived
  by partitioning or simplification - the front half of phase 9 for the voxel
  route is a direct read of the octree.
- **A brick must fit the cluster budget**, and there are three ways out if it
  does not, in preference order:
  1. Pick a brick size guaranteed to fit under `MaxVertices` / `ClusterSize`.
     Note `virtualgeometry-cluster-budgets`: MaxVertices binds before
     ClusterSize, and `vertex_count` is a uint8 that wraps to an empty meshlet -
     so the bound has to be checked against vertices, not triangles.
  2. Split one brick across several clusters that **share the brick's self and
     parent error and its DAG children**. They are one LOD decision made by
     several clusters, which is exactly what a group is, so the cut stays valid.
  3. Only if neither works, a real clustering pass over the brick's faces.
- **The cross-chunk edge falls out of it.** A chunk's root brick is a child of
  the brick it refines in the parent chunk, so a parent chunk's finest clusters
  stop being streaming leaves under the builder's rule - all children installed -
  rather than under a quadrant mask. `residentChildMask`,
  `sparseVoxelTerrainVGChunkRewritesForResidentChildren` and the quadrant
  bookkeeping all retire.
- **Gate:** gap pixels flat against `--terrain-vg-mesh-jobs-per-frame` (1 and 16
  identical) *with* no overlap - both at once, which no configuration measured
  so far achieves; plus `pageGap`/`noRoot` at zero; plus the two contracts, and
  the LOD profile contract passing without needing its touched-chunk assumption
  relaxed, since coarse stand-ins stop being drawn.

Sequencing: it needs phase 9a's seam (the front half producing clusters + DAG
for the voxel route) and phase 9a.0's determinism gate, both of which exist or
are specified. Do the page-prefix fix first - it is small, it is a stated rule,
and it is a defect under every policy.

### What already exists, found 2026-09-07 — **the intra-chunk DAG is already this design**
`VoxelOctreeVirtualGeometryBuilder` already builds exactly the proposed
structure and its header says so: octree-aligned *blocks* of `8 >> blockDepth`
cells, **one block is one cluster**, `blockDepth` raised globally until every
block fits `maxTrianglesPerCluster` / `maxVerticesPerCluster` (depth 3 is a
single cell, so the search always terminates), and
`group(parent block) = the eight child blocks under it`. Volume correspondence
is exact, so an intra-chunk DAG cut cannot open a hole. Coplanar merging and
in-cluster vertex welding are what keep `blockDepth` at zero; welding matters
because face-local corners cost four vertices per quad and bust `MaxVertices`
long before the triangle budget.

**So phase 10 is not "build the DAG" - it is "extend it across chunks".** The
missing edge is stated plainly in the builder's own contract: *"a page builds
its own self-contained DAG whose top level becomes the page roots"*, with child
links leaving the node range dropped. Each chunk is built in isolation, so a
chunk's finest clusters have no DAG children and its root clusters have no DAG
parent, and terrain approximates the relation with quadrant masks.

### 10a. Per-cluster cross-chunk boundaries — **implemented, opt-in 2026-09-07**
A first step toward the exact relation, using the block boxes the builder
already records (`clusterSources`, indexed by linearized cluster index):
`sparseVoxelTerrainVGChunkClusterQuadrantsFromBlocks` and
`...RewritesForResidentChildrenPerCluster` decide the boundary **per cluster**
(`streamingLeafsBitset` bit i is cluster i) instead of per hierarchy node. A
cluster whose block has no recorded coverage keeps its bit - never clear a
boundary whose coverage is unknown.

**Measured, and it does not pay yet**: on a 5 km walk the mean gap is within
noise of the node form (82.2 and 162.1 against 108.9) while the worst frame is
**consistently larger - 1,079 px in both runs against 349**. The reason is
structural: the node test is conservative because a straddling node waits for
every quadrant it reaches, and per-cluster releases the non-straddling clusters
sooner. Spatial precision without a sound yield *condition* only widens the
window. Off by default (`SVR_TERRAIN_VG_PER_CLUSTER_BOUNDARY=1`), kept because
it is the right granularity once 10b makes the condition exact.

### 10b. Exact cross-chunk coverage — **implemented 2026-09-07, and it is not the end state**
Two conditions had to hold together; each alone measured *worse* than the node
form:
- the boundary is decided per **cluster**, on that cluster's octree block;
- a cluster yields only when every quadrant its block reaches holds a child
  that is installed **and** whose bounds contain the block's vertical span.

The second is the one that mattered, and it is there because **cross-chunk
volume correspondence is not exact the way it is inside a chunk**: a child tile
covers a quarter of its parent in XZ, but its vertical extent comes from its own
surface range, so a parent block can reach heights the child never built.

Measured, 5 km walk, 300 frames, uncovered terrain pixels:

| rule | mean | max | streaming bits yielded |
|---|---|---|---|
| node-level | 108.9 | 349 | 205,669 / 641,414 (32%) |
| per-cluster, no vertical test | 82.2 / 162.1 | 1,079 / 1,079 | - |
| **per-cluster + vertical** | **0.18** | **3** | 18,283 / 624,342 (2.9%) |

and flat against throughput: jobs 1 and 16 both give mean 0.18, p90 1, max 3.

**But read the last column.** 2.9% yielded means 97% of boundaries are
*retained*, and a retained streaming leaf is force-drawn (self error 0) under
the child that also draws it - so this buys zero holes with substantial
**overlap**, which is the rule it was supposed to honour. It passes the hole
metric by being conservative, not by being right. It also keeps the LOD profile
contract red (`violations=2`, `touched=84`): coarse parents that never yield
stay *touched*, and the contract asserts every touched chunk meets the budget.

### 10c. Share the error across the four children — **DONE 2026-09-07; the LOD profile contract is green again**

Implemented as the user framed it, and it is emitted at build time.

**The change, in three parts.**
1. `SparseVoxelOctreeVirtualGeometrySettings::parentChunkError` (default
   infinite). Every cluster's parent error is clamped to it, and root clusters
   take it instead of infinity. The terrain page cache passes
   `2 x record.voxelSizeMetres`: the spatial quadtree keeps
   `horizontalVoxelCount` constant, so the parent chunk is exactly one octave
   coarser and no cross-chunk lookup is needed. `SVR_VG_PARENT_CHUNK_ERROR_SCALE`
   is the A/B (0 restores the old infinite roots),
   `SVR_VG_PARENT_CHUNK_ERROR_ROOTS=0` clamps the duplicate levels but leaves
   the roots infinite.
2. The refinement demand metric had `leafVoxelPx = projParentError * 0.5`, which
   assumes the parent error is always exactly one octave over the cluster's own.
   That is what 1 breaks. It is now
   `projParentError * (rawSelfError / parentError)` — the same value wherever the
   octave holds, so the pre-clamp behaviour is bit-identical (verified: the A/B
   with the clamp off reproduces `touched=84 violations=2` exactly).
3. **The demand budget was being spent on chunks that cannot refine.** The
   demand queue is ranked by projected voxel size, and the top of it is the
   resident *ancestors*: a coarse chunk whose ground is already split four ways
   still draws its own geometry with enormous triangles and saturates the
   priority word (`0100ffff`), while holding no chunk record at all — the
   streamer keeps records for leaves only. `collectRuntimeObjectPageRequests`
   was called with exactly `terrainPagesPerUpdate` as its cap, so every slot in
   the queue went to an ancestor and **zero refinements were published**:
   `vgDemand=0/32 noRecord=30 finest=2` on every frame at settle, on a 300-frame
   run, with the cut frozen a level and a half coarse. It now collects 16x the
   budget and spends the budget on requests that survive the filters.

**Measured.** 1 km / 640x360 / 1.5 px, the LOD profile contract's own
configuration:

| | touched | violations | worstPx | worstLevels | resident |
|---|---|---|---|---|---|
| before (recorded baseline) | 84 | **2** | 4.07 | 1.44 | 159 |
| clamp only | 42 | **7** | 4.78 | 1.67 | 120 |
| clamp + demand budget | 92 | **0** | 2.01 | 0.42 | 360 |
| demand budget only | 27 | 0 | 0.00 | 0.00 | 504 |

5 km walk, 300 frames, 1220x720, synchronous streaming:

| | gap mean / p90 / max | cutClusters | lod0 | rootRepairs | frame median |
|---|---|---|---|---|---|
| clamp off | 0.18 / 1 / 3 | 56,106 | 20 | **281** | 154.4 ms |
| clamp on | 0.18 / 1 / 3 | 65,462 | **40** | **20** | 149.9 ms |

Coverage is unchanged, the cut carries twice the finest chunks, and
`rootRepairs` — root pages dropping out from under installed chunks, the open
item at the top of the list — falls 14x. Before the demand-budget fix, with the
cut frozen, the clamp alone cut drawn clusters 59,219 -> 45,001 at 5 km and
54,401 -> 21,717 at 1 km: that is how much of the frame was a chunk's coarsest
level drawing unconditionally on top of its own children.

All five gates pass, the LOD profile contract unmodified:
`SparseVoxelTerrainVGSettleContract`, `SparseVoxelTerrainVGLodProfileContract`,
`SparseVoxelTerrainVGScratchFile`, `VirtualGeometryPackerDeterminism`,
`SparseVoxelTerrainVGSlotInstall`.

**What 10c does *not* subsume, measured, against the plan's own expectation.**
The vertical-containment test and the per-cluster quadrant masks have to stay.
Re-measured on the 5 km walk *after* the partition landed, uncovered enclosed
terrain pixels: with the test 0.18 / 1 / 3; with `SVR_TERRAIN_VG_VERTICAL_TEST=0`
14.4 / 25 / 76; with `SVR_TERRAIN_VG_NODE_BOUNDARY=1` the same 14.4 / 25 / 76.
An earlier attempt at yield mode 1 (fully drawable child) plus a doubled clamp
band measured 16.4, so it is not a band mismatch between the two spheres either.

The reason is that the partition and the mark answer different questions. The
partition decides which of two *resident* representations owns a budget. The
streaming-leaf mark is what keeps the finest resident representation drawable
*below its own error at all* — and whether something finer is resident is not a
fact the builder can put in a page. So `residentChildMask` and the quadrant
bookkeeping stay; what did retire is the pretence that the cross-chunk overlap
was a coverage problem. It was an error-range problem, and it is fixed.

### 10d. Crack-free chunk seams — **step 1 done 2026-09-07 and it is a negative result**

The seam, not the band, is what is visible. `interiorGapPixels` — the counter
every phase-10 measurement above uses — counts only gaps with terrain *above and
below in the same column*, so a chunk-boundary crack lands in `horizontalGap`
instead (mean 1.4, max 133 on the same runs reporting 0.18).

Two structural properties the old triangle builder has and the voxel one does
not:
- **Locked group boundaries.** `VirtualGeometryPartitioner::lockBoundaryVertices`
  locks every vertex shared by two groups so simplification cannot move a seam,
  and groups are **re-partitioned at each level**, so a boundary locked at level
  N is interior at level N+1 and is eventually simplified away. The octree
  builder's groups are octree blocks, and because the octree is nested those
  block faces lie on the same planes at every level: a seam never becomes
  interior, so it can never be resolved.
- **A neighbourhood.** `VoxelOctreeVirtualGeometryBuilder` looks neighbours up in
  `levelNodes[node.level]` — same level, page-local — and
  `sparseVoxelBrickExposedFaces` turns a null neighbour into *empty space*. Every
  chunk therefore emits a skirt of interior wall faces around its border, and at
  a LOD boundary the two sides' walls sit on different lattices.

**Step 1, built:** `SparseVoxelBrickDirectory` indexes every node of a hierarchy
by canonical scale and brick key (`canonicalGridOriginScale` is global, so bricks
in different pages and at different LODs are directly comparable), and
`sparseVoxelBrickResolveNeighbours` resamples a neighbour to the node's own cell
size — coarse upsampled, fine reduced with AND so a partly open fine neighbour
still leaves the face emitted. `sparseVoxelBrickExposedFacesResolved` is the
existing rule against that neighbourhood.

**And it is off by default, because it is unsound on its own.** Dropping a border
face because the neighbouring brick is solid assumes that brick is *drawn*.
Occupancy adjacency is a property of the voxel data; drawability is a property of
the cut; and two chunks are independent draw units today. Measured, 5 km walk,
300 frames, synchronous streaming, all three with the cross-chunk yield enabled
so the additive overlap stops hiding the result:

| | terrain pixels | interiorGap mean / max | horizGap mean |
|---|---|---|---|
| directory off | 105,739 | **14.4** / 76 | 558 |
| same scale + cross-LOD | 105,639 | 149.3 / 289 | 689 |
| same scale only | 105,549 | **182.2** / 830 | 694 |

The directory makes enclosed gaps **twelve times worse** and adds a quarter to
the horizontal seam count, while the terrain-pixel count is flat across all
three — so it is the gap metrics, not coverage attribution, that condemn it.
Same-scale-only is no better than cross-LOD, so it is not the resampling: it is
the premise.

*(Correction, same day: an earlier version of this table compared the two
directory rows against the **default** configuration's 219,070 terrain pixels
and read the difference as "half the terrain disappears". That halving is what
enabling the yield does on its own — it hands those pixels to the brick raster,
`visibleBricks` 2,598 -> 26,921 — and it is present in the directory-off
baseline too. The directory's own cost is the gap columns.)*

Under the shipping additive coverage none of this shows (0.18 either way) — a
coarse chunk is still drawing underneath, which is precisely the overlap 10c
exists to remove. The saving was 4687 -> 4215 triangles per chunk with cross-LOD,
and only 4687 -> 4607 without it, so 1.7% is all the sound part was ever worth.

**What this reorders.** Cross-chunk face culling is not an optimization that can
land ahead of the DAG work — it is only *correct* once one DAG spans the chunks
and a group is drawn as a unit. So the group/DAG restructure is a prerequisite,
not a follow-on, and the directory is the piece it will need on day one.
`SVR_VG_BRICK_DIRECTORY=1` and `SVR_VG_BRICK_DIRECTORY_CROSS_SCALE=1` keep both
halves available for that work.

### 10e. Attributing the cross-chunk yield — **instrumented 2026-09-07, partly fixed**

`yieldBits` says only 1.4% of offered streaming bits clear, but it does not say
why. `SparseVoxelTerrainVGYieldBlockStats` now splits it, printed in the settle
summary as `yieldBlocked(...)`. On a 5 km walk, 300 frames, synchronous
streaming, at the shipping default:

    offered=541408 cleared=7341 notInstalled=374660 vertical=159407

**69% is residency** — the parent's four children are simply not all resident,
and a parent that keeps drawing there is correct, not overlap. Of the 166,748
where the children *are* all resident, **96% were refused by the vertical
containment test**. That is the number worth attacking, and it is not streaming
latency: it does not drain.

Four defects found in that comparison, each real, each measured, none dominant:

| fix | cleared | vertical blocked |
|---|---|---|
| baseline | 7,341 | 159,407 |
| cluster geometry bounds instead of the octree block cube | 9,692 | 157,056 |
| chunk bounded by its geometry floor, not its minimum *surface* | 13,261 | 153,487 |
| one cell of slack for the coarse OR reduction | 14,811 | 152,285 |
| one box for both halves (XZ mask was the cube, Y was the geometry) | 14,578 | 150,515 |
| two cells of slack: cell *top* vs *surface height* | 15,440 | 150,001 |

Cleared doubled and the drawn cut fell 65,462 -> 62,873 clusters with coverage
unchanged (interior gap 0.18 -> 0.19, inside noise), so all six are worth
keeping. But the blocker moved only 5.9%, which is what a wrong model looks
like. The individual defects are real and were each confirmed against logged
comparisons - `boundsMin[1]` really was the minimum *surface* while a chunk's
border columns are solid down to its elevation origin, and the block span really
is measured to a cell face while the child reports a surface height.

**Next, and do this before proposing another fix:** the sampler logs the first
24 blocked comparisons, and they are dominated by a handful of repeated rows
from one chunk. Histogram `needAbove` / `needBelow` over *all* blocked
comparisons instead. A single mode says one more units error; a long tail says
the test is asking a question tight AABBs cannot answer, and then the answer is
the DAG - a group that spans the chunk boundary is drawn as a unit and needs no
coverage test at all, which is where 10d already points.

### 10f. The group is the unit — **vertical test deleted 2026-09-07; group rule opt-in, one hole from being the default**

The user's correction, and it retires 10e's premise: *the old cut and build were
correct - a group of clusters is replaced by its children or its parent without
cracks, and the octree structure already gives us that.* A parent brick **is**
the OR reduction of its eight children and the mesher emits all six faces of
every brick, so the parent's cluster and the child bricks under it cover the
same volume exactly. The four child chunks tile the parent's volume the same
way. There is no height a parent reaches that its children do not.

So the vertical containment test was answering a question that cannot arise, and
**it is deleted**. The evidence that it was the wrong object all along is in
10e: six separate, individually-correct corrections to how its two boxes were
derived moved its blocking rate by 5.9%. The yield predicate is now exactly the
builder's own group rule - *are this group's DAG children installed* - which
cross-chunk means the child chunk covering the cluster's quadrant is drawable.

Measured, 5 km walk, 300 frames, synchronous streaming:

| | yielded bits | drawn clusters | giant clusters | maxTriPx | interiorGap |
|---|---|---|---|---|---|
| additive (mode 0, default) | 0 | 65,462 | 2,921 | 117.2 | **0.18** |
| old mode 2 + vertical test | 1.4% | 62,873 | 2,744 | 117.2 | 0.19 |
| **group rule (mode 1)** | **30%** | **52,229** | **730** | **39.4** | 14.43 |

The group rule removes almost all of the overlap: three quarters of the giant
clusters, a third of the peak triangle size, 17% of the drawn cut. It is
**opt-in** (`SVR_TERRAIN_VG_YIELD_MODE=1`) and not the default, because
`SparseVoxelTerrainVGSettleContract` reports `interiorHoles=1` on its final
frame under it. The gate stays green; the design does not ship half-done.

**The residual, and where to look.** The gaps are thin - 1 to 4 row spans, about
one per frame - and the holes telemetry already names what borders them
(`above(instance,t)` / `below(instance,t)`). Two families: most sit at
**8-13 km of surface distance on a 5 km render distance**, i.e. at the outer
edge of the cut where the coarsest chunk has no coarser ancestor to fall back
on; a few sit at 52-82 m. On the culler side the difference is entirely the
**self** test - rejections 18,209 -> 41,767 with `parent=0` in both - which is
what removing the overlap is supposed to look like, with a small subset that
should have been replaced and was not. Chase the far family first: it is one
band, at the top of the cut, and 10c's clamp is what makes a chunk stop covering
above its own octave.

### 10g. What the triangle builder does that we did not — **2026-09-07**

Read `VirtualGeometryBuilder::build` against
`buildSparseVoxelOctreeVirtualGeometry`, ignoring geometry generation and
simplification. Four invariants hold the triangle builder's cut together:

1. **Every cluster is consumed by exactly one group, ever** -
   `assert(clusters[ci].parent.error == std::numeric_limits<float>::max())`.
2. **`child.parent` and `parent.self` are the same object** -
   `clusters[ci].parent = groupBounds` and `nc.self = groupBounds`, so the two
   bands abut exactly, sphere and error.
3. **A group it cannot replace is never consumed** - `retry` dissolves the
   group and its clusters re-enter `pending` for the next level.
4. **Leftovers become roots with an infinite parent error**, so every
   leaf-to-root chain ends in `[selfRoot, infinity)` and the chain is a total
   partition of the error axis.

The voxel builder already satisfies 1 and 2. It violates 3 and 4, and each was
measured on the 5 km walk:

**(4) is the real one, and it is fixed.** 10c clamped the chunk's *root*
clusters to `2 x chunkVoxel`, truncating the chain at a finite band whose cover
depends on an ancestor chunk being resident. Leaving the roots infinite - the
triangle builder's rule - is better on both axes at once:

| | drawn clusters | interiorGap |
|---|---|---|
| additive, roots clamped | 65,462 | 0.18 |
| **additive, roots infinite** | **54,564** | **0.18** |
| group rule, roots clamped | 52,229 | 14.43 |
| **group rule, roots infinite** | **41,662** | **12.98** |

17% less overdraw at unchanged coverage, and it is now the default. The clamp on
the *non-root* levels stays - those are genuine duplicates of the parent chunk's
bands, and killing them costs nothing (removing it entirely: gaps 12.99 against
12.98, drawn clusters 44,433 against 41,662).

**(3) is a real defect and fixing it faithfully is a loss.** A "vanishing group"
- parent block with no surface, children with plenty - consumed its children
and handed them a finite parent error with no cluster to replace them, which is
a dead end in the DAG. Making it retry-like (leave them unconsumed, let the root
pass give them an infinite parent error) costs 3,176 drawn clusters and takes
peak triangle size 117 px -> 331 for 0.27 px of holes. The original comment was
right: the coarse neighbour does cover a block that vanishes. Reverted, kept
behind `SVR_VG_VANISHING_CONSUME`.

**Not the cause, each measured:** the cluster-level clamp's dead bands (covered
by ancestors); the vertical containment test (deleted in 10f, it was answering a
question the octree already answers); cross-chunk face culling (10d).

**Where the residual sits.** Under the group rule, with roots infinite and every
page installed, 9.52 gap pixels remain with everything resident and streaming
synchronous - so it is structural, not latency. Page-level streaming accounts
for the rest (12.98 -> 9.52 with `--terrain-vg-install-all-pages`). The
group rule stays opt-in (`SVR_TERRAIN_VG_YIELD_MODE=1`) until that 9.52 is
closed; `SparseVoxelTerrainVGSettleContract` still reports `interiorHoles=1`
under it.

### 10h. Terrain masks through the streaming manager — **2026-09-07**

The user's question, and it is the right frame: the virtual-geometry streaming
manager already runs an ordinary asset like Sponza with no holes and no
overlap. Why does terrain need machinery of its own?

It should not, and the difference is one line of ownership.
`rebuildRuntimeObjectHierarchyMasks` reconstructs **the whole object's** cluster
masks from `record.pages[].installUpdates` alone - each installed page's leaves
and its own install rewrites, replayed in page order. Sponza never notices,
because every mask it has comes from exactly there. Terrain's cross-chunk yield
was written with `applyRuntimeObjectHierarchyClusterFlags`, which patched the
hierarchy mirror and uploaded it but recorded nothing against a page - so the
next uninstall of *any* page anywhere in the shared terrain object silently
reverted every yield the terrain had applied.

Fixed: `applyRuntimeObjectHierarchyClusterFlags` takes `persistIntoPageUpdates`
and records each mask against the page that owns its node, so the replay
reproduces it. Recording it against the *owning* page rather than routing it
through the child chunk's install is deliberate - the replay walks pages in
slot order, and a parent and child chunk hold unrelated slots, so a mask entered
against the child's page could be overwritten by the parent's own page defaults
depending on which slot happened to be lower.

**Measured: no change**, 12.98 gap pixels and 41,662 drawn clusters either way,
because `evicted=0` on this harness - nothing uninstalls, so the replay never
runs and the defect is latent. Forcing it with `--terrain-vg-memory-mb 160`
evicts 785 pages but starves the cut instead (`installFailures=440`,
`lod0Chunks` 40 -> 12), so it is not a clean A/B either. The fix is correct by
construction and unproven by measurement; say so rather than claim it.

**The remaining divergence, and the next step.** `republishTerrainVGSharedParent`
still rebuilds its `effective` mask set by filtering the chunk's cached
`updatesByPage` against which pages are installed - which is precisely the loop
`rebuildRuntimeObjectHierarchyMasks` already runs inside the scene. That shadow
copy exists only because terrain used to write masks outside the scene's model;
with masks now persisted it can go, leaving terrain to supply only the
cross-chunk delta and the scene to own every mask, exactly as for Sponza.

### 10i. External masks, and where the eviction holes really come from — **2026-09-08**

**External masks are now first class.** `RuntimePageStatus::externalUpdates`
holds masks a caller applied to a page's nodes after the install, and
`rebuildRuntimeObjectHierarchyMasks` replays them after every page's own
updates. `applyRuntimeObjectHierarchyClusterFlags(..., persistIntoPageUpdates)`
writes them there. This is what makes the terrain cross-chunk yield survive an
uninstall the way an ordinary asset's masks do.

They are deliberately **not** folded into `installUpdates`: both the install path
and the replay read `installUpdates.empty()` to choose a node's *default* mask,
so putting external masks there silently flips that default for any page whose
builder emitted no rewrites. The first attempt did exactly that and cost the cut
half its finest chunks.

**Measured: no change** on this harness, because nothing uninstalls (`evicted=0`
at 512 MB). Correct by construction, unproven by measurement.

**Eviction: the holes are not where I assumed.** At `--terrain-vg-memory-mb 160`
a run reports 469 install failures and 469 chunks left bare, and every failure
message is the same transient one - *"buffers cannot fit the encoded page
(N bytes await deferred release; retry next frame)"*. Freed page bytes are not
reusable until the frame's GPU work retires, so an install against them fails,
and by then the chunk being replaced has already been uninstalled.

I gated the destructive retire on "can the scene take this chunk's bytes". It
**never fired once** in 300 frames (`deferredSkips=0`) while the 469 failures
continued, so the gate was removed rather than left in as dead code. The reason
it never fired is the finding: the destructive retire is not the replacement
path at all. It is the recovery block below it - *"slot N has installed pages
outside its record"* - which retires pages the scene reports installed on a slot
the page set does not consider installed. **The page set and the scene disagree
about residency**, and the recovery for that disagreement is what strands the
ground.

That disagreement is the thing to fix next, and it is a different bug from the
one this section set out to fix: not a capacity problem, a bookkeeping one. Two
records of the same fact, maintained separately - which is the same shape as
every other defect in phase 10.

### 10j. Memory ceiling, and atomic chunk install — **2026-09-08**

**An 11 GiB virtual-geometry pool is not reachable on this machine, and the
reason is not the pool.** Measured with `--terrain-vg-memory-mb 1024`: the
process footprint reaches 5.86 GiB and the in-process watchdog exits at
**frame 6**. The footprint ramp is 0.15 -> 0.82 -> 2.65 -> 5.85 GiB within the
first seconds, all of it world generation, before the VG pools are of any size;
`gpuPages` is 229 MiB actual. Cutting the Tree64 cap four-fold
(`--terrain-gpu-memory-mb 512`) only moves the exit from frame 6 to frame 72,
because that budget is a cap and not a reservation. Micro-carving is already
off, so the 5x micro-surface arena duplication is not this.

For context: 16 GiB unified, and `terrainVGDecodedBudgetMiB()` already encodes
the limit - **256 MiB on a machine with <= 16 GiB RAM, 2048 MiB above it**.
`sample-run-discipline` records that a 5.95 GiB frame-1 footprint left 0.1 GiB
free and took WindowServer down. So 11 GiB needs a >= 32 GiB machine, where the
default is already 2048 MiB and `--terrain-vg-memory-mb 11264` is accepted by
the flag; here the useful ceiling is roughly the 512 MiB these runs used. The
work that would buy headroom is the ~5.4 GiB generation-side baseline, which is
a separate investigation.

**Atomic chunk install - fixed, and worth keeping regardless.**
`installSparseVoxelTerrainVGChunkIntoSlotFromBytes`
returned on the first failing page and left every page it had already installed
behind. The page set then does not call the slot installed while the scene
reports its pages installed, and the recovery for that disagreement - *"slot N
has installed pages outside its record"* - retires them after the chunk has been
dropped. The install now rolls back. Measured at `--terrain-vg-memory-mb 160`,
where the pressure is: that message drops from many to **1** occurrence.

**And there was never any bare ground: `bareAfterRetire` was measuring the wrong
thing.** It is set from

    retiredForReplace = page->installed == false && sliceBySlot.count(slot) == 0

which is **false** after a successful replacement retire - the slice survives it
- and **true** for a brand-new slot that never held anything. So the counter
missed the case it is named for and counted fresh chunks whose install failed,
where no ground is lost at all. Corrected to `retiredForReplace = replacing`, it
reads **0 at both 160 MiB and 512 MiB**.

That is consistent with what the gap metrics said all along and what should have
been believed over the counter: 160 MiB and 512 MiB give *identical* coverage
(interior gap 0.18, max 3), while 160 MiB costs refinement (lod0 chunks 12
against 20) and 468 transient install failures. A starved page budget costs
detail, not holes.

Two gates on "can the scene take this chunk" were written and both were deleted
for never firing once in 300 frames - the first sized on `encodedPageBytes`
alone, the second on all four pools. There is nothing for them to catch.

### 10c (original sketch, kept for the reasoning) — the actual end state, not started
The user's framing, and it dissolves both problems at once: **the four children
cover the parent's volume exactly, so if they share the parent's error and their
own error, they replace the parent exactly and the culler discards the parent**.
A cluster draws iff `self.error <= budget < parent.error`, so with
`child.parent.error == parent.self.error` the two ranges are disjoint by
construction - no overlap and no gap, with no coverage test at all. The
streaming leaf then goes back to meaning only what it should: do not refine into
geometry that is not resident.

What to check when implementing it:
- the four siblings are one LOD level so their self error already matches
  (`octree.leafSelfError = record.voxelSizeMetres`), and `rootParentError` /
  `republishTerrainVGSharedChildRoots` already push the parent's leaf error onto
  the child's chunk-root node. So the *hierarchy* gate may already be right and
  the gap is one level down;
- **a chunk's root clusters carry `parent.error = infinity`** from the builder,
  because a page builds a self-contained DAG whose top level becomes the page
  roots. Those cluster-level bounds live in the encoded page, not the hierarchy,
  so making them carry the parent chunk's error means either emitting them that
  way at build time (the builder would need the parent's error as an input) or
  rewriting them at install. Emitting at build time is the clean one, and it is
  the same seam phase 9a defines.
- Once the partition is exact, the vertical containment test and the per-cluster
  masks can go, and the LOD profile contract should pass unmodified because
  coarse parents stop being touched.


  **The gate the user asked for holds:** gap pixels are now flat against
  meshing throughput. `--terrain-vg-mesh-jobs-per-frame 1` and `16` both give
  mean 0.2, p90 1, max 3 - identical. The throughput lever is no longer a
  coverage lever, which is the point.

  Cost: the predicted overdraw does not show. Frame time is within noise
  (median 147.6 ms against 149.6), and the cut settles *earlier* (frame 233
  against 499) with more fine chunks (lod0 56 against 40) and a better LOD
  profile (worstLevels 0.78 against 1.44) - coverage no longer collapses and
  has to be rebuilt. `SVR_TERRAIN_VG_YIELD_MODE` keeps all three rules for
  A/B: 0 never (default), 1 fully-resident child, 2 the old yield-on-install.

  **One gate is red and it is this change's doing.**
  `SparseVoxelTerrainVGLodProfileContract` now reports `violations=2`
  (`touched=84`, was 76 with 0 violations; `worstLevels` unchanged at 1.44).
  Nothing got coarser - the contract asserts that *every touched chunk* meets
  the rendered-error budget, and under additive coverage a coarse stand-in
  being touched is now expected and correct, because a finer chunk draws over
  it. The contract's assumption, not the renderer, is what no longer holds:
  it should judge the finest chunk covering each region rather than every
  touched chunk. **Deliberately not changed here** - weakening a gate to pass
  one's own change is how these contracts stop meaning anything, and the call
  belongs to whoever owns the LOD contract.

  **Superseded by the above (kept because the eliminations are still true):** "A cluster/group is available only once fully resident, otherwise the
  cut stops earlier in the hierarchy" is the correct rule, and coverage must
  not depend on `--terrain-vg-mesh-jobs-per-frame`. It currently does.

  The metric is now trustworthy: `interiorCoverageGapPixels` counts only
  pixels that are uncovered with terrain **above and below in the same screen
  column**, which the code deliberately separates from horizontal gaps because
  those can be legitimate sky between two ridges. A vertical discontinuity is
  missing terrain, full stop.

  Eliminated as the cause, each measured on a 500-frame 5 km walk:
  - **Not partial residency / root-page-first.** `--terrain-vg-install-all-pages`
    at the default throughput: mean **173**, p90 343 - no better than
    root-first (mean 59-135, p90 55-343). So it is not "the parent yielded to a
    child that only has its root page".
  - **Not camera speed alone.** `--test-camera-travel-m 200`: mean 209. (Weak
    evidence - that flag bounds total travel rather than rate, so the tail of
    the run is static; treat it as inconclusive rather than as a refutation.)
  - **Not eviction, slices, or a bailed replacement** (above).

  What every effective lever has in common is **how many chunks get meshed per
  frame**: jobs 4 -> 16 gives 10x, and full in-frame convergence gives the 13 px
  floor. So the gap is the meshing backlog during refinement - and the defect is
  that the cut yields coverage while that backlog is outstanding.

  **The fix to build (not done):** audit every path that can stop a coarse
  chunk drawing before its finer replacement is installed, and make coverage
  the precondition rather than a side effect. `retireTerrainVGSharedSlot`
  already carries the right rule - a chunk may leave only when its parent is
  installed or all four children are - but it is applied **only to slots the
  page set still calls live, and only when `page->installed`**, and
  `syncTerrainVGSharedSlots` retires outside that guard. The suspect sequence
  is a *refinement*: the streamer replaces one coarse record with four finer
  ones, the coarse chunk's entry goes stale and its slot leaves `incoming`, so
  it is retired and parked while its four children are still queued for
  meshing - a leaf cut has no resident ancestor to fall back to. Instrument
  that transition first (count retires whose covering replacement is not yet
  installed - the `bareAfterRetire` counter is the right shape but it only
  watches the install path, not the sync path), then make it a hard
  precondition.

  **The likely end-state fix, and it is the user's own words:** the terrain cut
  is a *leaf* cut - only leaf chunks carry geometry, and ancestors sit in the
  page-set directory with `hasRecord=false` purely to link parents to children.
  So ground entering the cut for the first time has **no coarser geometry to
  fall back to at all**, and no streaming-leaf rule can conjure it. The fix is a
  permanently resident coarse shell: keep the ancestor levels meshed and
  installed, their clusters marked streaming leaves, so the cut always has
  something drawable above any missing leaf and refinement only ever *adds*
  detail. That is "the streaming leafs should be early in the hierarchy", and
  it makes coverage structurally independent of meshing throughput. It is
  affordable - the ancestor closure of N leaves is about N/3 chunks and the
  coarse ones are the cheapest - but it is a residency-policy change, since the
  streamer currently publishes leaves only. When it is enforced, gap pixels must be flat against
  `--terrain-vg-mesh-jobs-per-frame`, and that insensitivity is the gate.

  Also eliminated at this stage, on the deterministic harness: root-page-first
  (`--terrain-vg-install-all-pages`: 34 against 40 pinhole frames) and the
  parent-error stub gate (`SVR_VG_STUB_GATE=0`: 40 against 40).

  Still standing, in the order worth testing once the harness exists: a chunk
  whose hierarchy is published while its root page install is still pending;
  the parent's streaming-leaf republish racing the child's first drawable page
  (`SVR_SHARED_REPUBLISH_DELAY` exists to probe exactly this, and 3 made
  enclosed holes 5x worse, so the interaction is real); and page requests
  outrunning `--terrain-vg-page-install-budget` under motion.
- **Root-first request deadlock.** An installed page's finest clusters carry
  the streaming-leaf mark until the next page lands, so the culler never
  descends into the uninstalled page's nodes and never writes a priority for
  it (`vgRequests=0` on every frame), while the demand those leaves publish
  was dropped because the chunk "had pages pending". The demand is now the
  request: it installs the chunk's next page (`pendingPageInstalls`, within
  the page budget). Near-field policy (`--terrain-vg-prefetch-fraction`,
  0.5): a chunk whose finest voxel projects at or above that fraction of the
  budget takes every page at install and a requested page pulls the rest of
  its chain.
- **Coarsening feed aligned with the spatial cut:** the vote also requires
  the parent tile's projected error, in the cut's own metric and in world
  space (tile indices are anchored off the origin), to be under
  `mergeErrorPixels`; otherwise the cut re-splits what the vote merged - 10
  transactions per 100 frames on a static camera. After: 23 commits per 400
  static frames (22 without the feed), none after frame 300.
- **Saturation back-off:** with every pool full and nothing evictable, page
  and chunk installs skip the frame instead of failing by the thousands
  (11,500 failures took p90 from 17 to 134 ms; now 31.7 ms at the same
  configuration). Real-threshold results at 1 km / 1.5 px, 300 frames:
  **76 LOD0 chunks** (4 under the coarse cut), 1,110 installed / 806 touched
  pages, 295 evictions, frame median 28 ms, footprint 6.84 GiB (killed by
  the 7 GB test budget at the end), holes: 10 single-hole frames per 250.

Milestone 2 (native RT) waits on 1-3 and 5: a BLAS per resident node at
~109 B per triangle is ~3.3 GiB at full-level residency and a few hundred
MiB once residency is touched pages x headroom - measured now at the real
threshold as ~1,100 resident pages.

### 7a. Lazy install - **DONE, default (2026-09-06)**
`--terrain-vg-root-page-first`: a chunk installs its root page; finer pages
keep `PAGE_NOT_INSTALLED_BIT`, the culler requests them, and
`installTerrainVGRequestedPages` installs the requested page plus its missing
predecessors (pages depend on their predecessor by construction), budgeted per
frame. Chunk-refinement demand is suppressed while a chunk has uninstalled
pages. Verification, in order, each through `safe_run.sh`: a 3-frame smoke
with the flag; 60 frames with the trace (`pageInstalls` per frame,
`VG page use` installed vs touched); the 250-frame hole harness (0 holes is
the gate: a page that arrives late shows as a hole, since the parent's
clusters draw meanwhile only if its streaming marks are right); then timing.
Expected: installed pages track touched pages within a small multiple, decoded
pool and page buffer sized accordingly (Phase 7c).

**Tests:** extend `SparseVoxelTerrainVGSlotInstallTests` only where no scene is
needed (`assetPageOf` inverse of `slotForAssetPage`, budget arithmetic). The
install itself is GPU-only: the harness is its test.

### 7b. Lazy meshing (design)
The mesher builds every level of a chunk's octree. Build the coarse levels
first (root page = coarsest groups) and finer levels on request. The builder
must then emit pages incrementally: rebuilding with more levels must leave the
already-installed coarse pages byte-identical and only append pages (plus a
rewrite of the previously-last page's streaming marks). Test: build a fixture
chunk with levels <= L and with all levels; the first k pages' encoded words
are identical, and the appended pages' dependencies name only earlier pages.
Until then the CPU meshes all levels asynchronously; the saving is CPU time
and asset memory, second-order next to 7a.

### 7c. Pool sizing from touched pages
With 7a the decoded vertex/index pools (1.4 GiB) and the page buffer (466 MiB)
are sized for every page of every chunk. Size them from a resident-page
budget (touched pages x a headroom factor), and let the scene's LRU eviction
(`evictLeastRecentlyUsedRuntimePages`, touched through GPU priorities) drop
pages the culler stopped requesting, so a chunk's fine pages leave when the
camera moves away. Test: a run that walks toward and away from a ridge keeps
installed pages bounded and shows no hole on the approach.

### 7d. Coarsening feed
Demand only refines. When the camera moves away, fine chunks stay fine and
their fine pages stay installed (7c evicts pages, not chunks). The brick
raster's merge votes are the only coarsen signal today. A VG-side signal: a
chunk whose root page's clusters were drawn and whose finer pages were not
requested for N frames is a coarsen candidate for the streamer. Test: the
dolly harness (`svraster-dolly-camera-harness`) with resident chunk count as
the metric.

### 7e. Occupancy vertex path (design, the end-state)
Vertices synthesized in the vertex stage from brick occupancy (a per-brick
exposed-face prefix table from a compute pass over the culler's visible
clusters; clusters sized by face count; DAG = the brick octree; materials from
the material-index arena). Removes CPU meshing, page encoding, page decode and
the decoded pools; costs 5-20x more triangles near the camera (no greedy
merge), bounded by cluster LOD and the software rasterizer. Prototype next to
the decoded path and A/B on the harness: triangle count, frame time, memory.

## Phase 9 — Split the packer, and parallelise the back half — **DONE 2026-09-08 (9a seam, 9b measured, 9c both halves); 9d/9e are notes**

The user's framing, and it is the right decomposition: the builder today is one
monolith from source geometry to encoded pages, and the *front* half is the part
that differs between a triangle mesh and a voxel octree. Split it, and both
routes share everything after the split.

```text
FRONT (route-specific)                 BACK (shared, and parallel)
  triangle mesh: partition ->            page packing  -> rewrites
  simplify -> group -> DAG               -> encode -> compress -> write
  voxel octree: the octree IS
  the DAG (levels are LODs)
                        \__ clusters + LOD DAG __/
```

**Why the split is worth doing on its own**, before any parallelism: the voxel
route already has its DAG for free (`VoxelOctreeVirtualGeometryBuilder` -
levels are LOD levels, see `voxel-vg-octree-is-the-dag`), so everything it
currently shares with the mesh route is the back half anyway. Making the seam
explicit means the voxel path stops carrying mesh-shaped machinery it does not
use, and the mesh path (`VirtualGeometryPartitioner` / `Simplifier` /
`HierarchyBuilder`, ~1,350 lines) stops being on the terrain critical path.

### 9a.0 The determinism gate — **DONE 2026-09-07**
`VirtualGeometryPackerDeterminism` (ctest, CPU, 0.02 s). The refactor is gated
on "the encoded asset is byte-identical before and after", and that gate means
nothing unless the builder is deterministic to begin with. This pins it:
- the same source mesh built twice gives byte-identical page words, hierarchy,
  dependencies and root page index;
- **encoding the pages back-to-front gives the same bytes**, and re-encoding a
  page is stable. That is the property 9c's per-page fan-out needs, and the
  one a completion-order accumulator would break while still passing the first
  check.
The fixture sets `maxGroupsPerPage = 2` to force multi-page packing - at the
default 64 its 6 groups land on one page and every multi-page path (page
dependencies, per-page rewrites, root page choice) goes untested. Worth
remembering for any packer fixture.

### What was done, and what the measurement changed — **2026-09-08**

**The gate first.** `VirtualGeometryPackerDeterminism` now pins a 64-bit
fingerprint of everything the back half emits (page words, dependencies,
hierarchy, root page) for a triangle grid fixture and for a voxel terrain
chunk built through the octree builder, recorded from the builder as it stood
before any of this: `0x798d7f109865a19e` / `0x202ab9ef4bc544c4`. Every step
below left both unchanged. It also builds a 26-page fixture and a twelve-shape
fixture serially and on four threads (front half through
`VirtualGeometryBuildSettings::frontThreads`, back half through the packer's
`threads`) and requires byte-identical bytes, and writes two `.vg` files with
one and four compression threads that must be byte-identical.

**9a, the seam: `VirtualGeometryPacker`** (`src/editor/virtualgeometry/`).
Two shared back-half stages, `packPages` (pages + rewrites, serial by design)
and `encode` (quantise + encode, fanned out per page, then the serial bounds
rebuild), plus `pack` for both. `VirtualGeometryHierarchyBuilder::finalizePagesAndRewrites`
and `VirtualGeometryCompressor::encode` still exist and forward to it, so no
caller changed; the terrain bridge calls `encode(..., threads = 1)` because it
already runs on one of the page cache's workers. The DAG validation the
finalize step ran is now a free function in the header. The input contract is
documented on the class: the back half reads clusters, the LOD hierarchy, the
group infos and the cluster DAG plus the asset tables it copies through;
`groupDAG` and `buildToLinearizedClusterIndex` are front-half bookkeeping it
never reads, and the voxel fixture builds with none of the mesh-only tables.

**9b, measured before parallelising.** `VirtualGeometryBuildTimings` rides on
the build data and the encoded asset; the OBJ importer prints it. Sponza
(23.8 MB OBJ, 393 shapes, 5,035 clusters, 22 pages), serial, this machine:

| stage | ms |
|---|---|
| LOD0 clusterise (393 parts) | 597 |
| partition | 21 |
| simplify | 140 |
| group (bounds, re-clusterise, cones, bones) | 343 → 278 after the remap fix |
| hierarchy (BVH) | 68 |
| merge | 3 |
| packPages | 6 |
| encode | 7 |
| compress | 6 |
| write | 3 |
| fallback meshes (importer) | 34 |

**The back half is 22 ms of a 1.2 s build.** 9c's first bullet - per-page
encode and compress - was done (7 → 2.7 ms and 6.3 → 1.4 ms on 8 threads) and
is worth nothing on its own; the honest prior in 9b held. Two things in the
front half were worth everything:
- `rebuildPositionRemap` ran **once per group**, over every vertex and every
  cluster index, when nothing inside a level reads it. Once per level is the
  same bytes (gate) and 65 ms less.
- The per-part front half (LOD0 clusterise then the LOD chain) reads the
  shared vertex array and nothing that changes, so **mesh parts fan out** into
  indexed slots and the merges run over them in order. Inside a part, the
  groups of a level fan out the same way when parts are few (both
  `VirtualGeometryPacker::forEachIndex`, indexed results, serial in-order
  apply).

**9c, the trap that the gate caught.** The first eight-thread Sponza build
came out with 5,034 clusters, then 5,033, against 5,035 serial: **METIS keeps
its random state in globals** (GKlib), so concurrent `METIS_PartGraphRecursive`
calls interleave and the partition changes run to run. The gate's original
fixtures never crossed the fan-out threshold in the front half, which is why
the twelve-shape and 26-page fixtures exist now. The fix is a mutex around the
METIS calls: each call reseeds from `METIS_OPTION_SEED`, so serialised calls
are deterministic in any order, and meshoptimizer (everything else in the
per-group work) is stateless. After it, four Sponza builds (two serial, two
eight-thread) hash identically and count 5,035 clusters.

Sponza after, eight threads: **build 1,166 → 496 ms, total 1,217 → 538 ms**
(2.3x); LOD0 1,805 ms CPU across threads, simplify 171 CPU, group 1,181 CPU.
`VG_PACK_THREADS=1` forces every fan-out serial for an A/B.

**9d/9e, as notes.** The indexed-result-array pattern is the only concurrency
structure any of this needed; the `ConcurrentQueue` job feed the plan
considered is the atomic counter in `forEachIndex`. Page size (9e) is the
knob 8f delivered; nothing here changes the argument.

**Known-red, pre-existing:** `FileEncodingTests` (never in the gate list; its
executable was not built before this session) dies with a bus error at a
varying page of its 66,000-page suzanne fixture. It crashes at the baseline
too (rebuilt with this work stashed: SIGSEGV on two runs before its first page
compare), so it is not this phase's; it is not fixed here either.

### 9a. Name the seam
One type between the halves: clusters, their group assignment, and the LOD DAG
(parent/child ranges plus self/parent error). Both builders produce it; the
back half consumes only it. `VirtualGeometryHierarchyRewrites::buildPagesAndRewrites`
is already the first back-half stage and already takes `VirtualGeometryBuildData`,
so the seam is close to where the code already divides - the work is narrowing
that struct to what the back half actually reads, and proving it by building
the voxel route without ever populating the mesh-only fields.
**Gate:** both contracts, and a byte-identical encoded asset for one fixture
chunk before and after the split. That equality test is the whole safety net
for this refactor; write it first.

### 9b. Measure before parallelising
Nothing here is measured yet. What is known: one terrain transaction is
**32 pages in 60 ms across 8 workers**, meshing the whole 5 km ramp is about a
second of worker time, and phase 8 added an encode+LZ4 that costs **~179 KB and
a few ms per chunk**. So chunk-level parallelism already exists and already
works. The open question is whether *intra*-chunk parallelism is worth
anything, and the honest prior is that for terrain it is not - chunks are small
and there are hundreds of them. It matters for a **single large asset** (one
OBJ, one bake), which is exactly where the current builder is single-threaded.
**Do not parallelise anything before a per-stage breakdown of one large asset's
build.** Add stage timers first (partition, simplify, group, DAG, pack,
rewrite, encode, compress) and publish them the way `SVR_TRACE_TERRAIN` does.

### 9c. The parallelism that is actually available
Ranked by (expected win / risk), to be re-ranked once 9b measures:
- **Per-page encode and compress.** Embarrassingly parallel: a page's
  `PageBuffer::encode` plus its LZ4 block reads only that page and writes only
  its own buffer. Phase 8 already proved the shape - `encodeChunk` runs on the
  worker that meshed the chunk. Fan out over pages with the existing pool,
  collect into a `ConcurrentVector`, then write in page order. **This is the
  one to do first** and it needs no lock-free machinery beyond a results array
  indexed by page.
- **Per-group simplification and per-cluster quantisation.** Independent per
  group; the existing single-threaded loop is the obvious fan-out.
- **The file write itself stays serial and that is correct.** Appending is one
  `fwrite` of already-compressed bytes under a mutex (measured: ~20 ms over a
  300-frame run). Parallel writes would need a file-space allocator taken
  concurrently, which buys nothing at that cost.
- **Rewrites (`buildPagesAndRewrites`) are the wrong target.** They are a
  dependency-ordered walk over the DAG assigning pages, and the page a group
  lands on depends on what was placed before it. Parallelising it changes the
  page layout, which changes every dependency and every install rewrite - and
  the trap the whole system is built around is that a wrong streaming-leaf bit
  draws every LOD level at once (`vg-streaming-leaf-parent-test`). Leave it
  serial unless 9b shows it is a real cost.

### 9d. Lock-free structures: where they fit, and where they do not
`src/runtime/datastructure/` has `ConcurrentVector`, `ConcurrentQueue`,
`ConcurrentHashMap`, `ConcurrentPriorityQueue` and friends. The useful pattern
here is the **bounded fan-out with an indexed result array**, not a shared
mutable structure: page `i` writes slot `i`, nothing contends, and the result
is deterministic. A `ConcurrentQueue` earns its place for the *job feed* (pages
to encode) when the per-page cost is uneven, which it is - pages range 25 KB to
several hundred. **Determinism is a hard requirement**: the encoded asset must
not depend on thread scheduling, or the 9a equality test is meaningless and
every future "byte-identical" gate is too. That rules out anything that
accumulates into a shared container in completion order.

### 9f. Groups split across pages — **DONE 2026-09-08, opt-in**

`VirtualGeometryBuildSettings::splitGroupsAcrossPages`. A page is packed until
the next group does not fit and the tail it leaves is up to one whole group -
eight clusters at roughly 800 bytes each, so about 4% of a 128 KiB page and
more where clusters are large. With this on, the group's clusters split at a
cluster boundary: a prefix lands on page A and the remainder at the start of
page B. A *cluster* is never split; its encoded offsets are page-local word
offsets into that page's own blocks.

**Why it is safe, which is the whole design.** An existing test
(`testPagesHoldWholeGroups`) asserted the opposite, and its reasoning was
right: a group substitutes for its DAG children *as a unit*, its clusters share
one parent bound, and the coarse cluster they replace keeps its streaming mark
until every one of them is resident. Enabling a half-landed group would draw it
over that coarse cluster - half a substitution taken. The enabled mask says it
exactly: **a group's spans carry a zero enabled mask until its last span
lands**, which also keeps it in `pendingInstalls`, so the page that completes it
re-emits every span with the real mask. With splitting off a group is always
complete on its own page and this is the mask it always had.

**Shape:**
- The packer places *spans* (`PageGroupSpan`) rather than groups, and residency
  becomes a per-cluster question - `streamingBitsetForGroup` is keyed by
  cluster page now, not by group page.
- A leaf addresses one page and one contiguous run of it (`node.pageIndex` plus
  a page-local `child_start`, and the culler derives each mask bit as
  `local - child_start`), so a group spanning N pages becomes **N leaf nodes**,
  appended at the end of the hierarchy - appending never shifts an existing
  index, which an insert would - with the group's original node becoming a
  synthetic internal node over that contiguous run. Its bounds and errors
  already describe the whole group, so traversal reaches the halves through
  exactly the test the leaf used to make. **No page format and no shader
  change.**
- Rewrites are recorded against (group, span) while packing and resolved to
  nodes after the split, with the group-local bitsets shifted down to each
  span.

**Gates:** `SparseVoxelOctreeVirtualGeometryBuild` gained
`testSplitGroupsAreDisabledUntilComplete` (a budget search finds a straddling
configuration, clusters run in page order, no span is enabled before the
group's last page and all are after) and `testSplitPreservesTheClusterSet`;
`testEveryInstallPrefixIsAValidCut` now runs for both whole and split
configurations and reports **0 holes and 0 overlaps at every prefix and every
budget** for each. `VirtualGeometryPackerDeterminism` gained a split build that
must be byte-identical when repeated and on four threads, must place every
group's clusters exactly once across its spans, and must keep the topological
page order. Both golden fingerprints are unchanged, which is the proof that the
default path did not move; the 5 km terrain walk is unchanged too (510 chunks,
0 violations, 219,600 px).

**Off by default.** It pays where pages are packed to a byte budget
(`maxPageBytes`, itself off by default) or where clusters are large - a single
large asset, or streaming over a network where the page is the transfer unit.
Leaving it off keeps every existing asset byte-identical.

### 9e. Page size, jointly with 8f
`MAX_GROUPS_PER_PAGE` is 64 (~25 KB per page; chunks are 4-32 pages). Fewer,
larger pages mean fewer compressions, fewer dependencies and fewer reads - and
they make per-page parallelism *less* useful. Decide 8f and 9c together rather
than tuning one against the other.

## Traps, all previously paid for

- **Empty rewrites disable the cut** (`VirtualGeometryScene.hpp:515`). Phase 2's
  guard test exists for this.
- **Never truncate a software triangle's bounding box** — see
  `vg-unified-visibility-path`.
- **A streaming-leaf mark means "cannot refine past", not "always draw"** — see
  `vg-streaming-leaf-parent-test`; skipping the parent test rasterised every page
  boundary twice.
- **Leaf self error 0, root parent error infinite**, and the streaming-leaf mark
  must come from residency, not the build-time cut — `vg-cut-always-renderable`.
- **Slot identity**: keying anything by clipmap slot re-does work when the
  streamer moves chunks — `terrain-vg-page-cache-slot-adoption`.
- **Publish leaves in batches**, not one write each — 9.5 s on a 2000-page commit.
- **A sizing pass that measures a cache by walking payloads it does not own**
  silently falls back to its floors when those payloads are released. Phase 8b:
  the VG page buffer became 32 MiB instead of 244, saturated at 183 pages by
  frame 60, and froze the cut at 195 resident chunks against 366 — half the
  terrain, one warning, no error. Record the totals at build time instead.
- **Column-major matrices** into pass uniforms — `vg-column-major-matrix-uniforms`.

## Diagnostics

- `SVR_TRACE_TERRAIN=1` — cache-update stage timers (`cutPlan`, `generate`,
  `arenaShape`, `publishEntries`), job phases, upload plan summary, commit block,
  `Terrain VG stale` / `apply` / `refresh`, `Terrain upload stage`.
- `SVR_LOG_CPU_SECTIONS=N` — per-frame CPU section breakdown with resident and
  request counts.
- `RG_DUMP_ORDER=1` — render graph node order.
- `SVR_WAIT_IDLE_EACH_FRAME=1` — drain per frame when chasing ordering bugs.

## Tests that are samples, not tests (2026-09-06)

`TriangleExample`, `TriangleReverseZ` and `VirtualGeometryRenderer` are
interactive windowed programs whose frame loop ends only when the window is
closed, so ctest hangs on them forever. Their `add_test` registrations are
removed; the targets still build and are run by hand. (`AnimationTests`, despite
the name, is a pure CPU round-trip check and terminates — it stays registered.)

## Environment caveats

- The generated route (`--terrain-generation gpu-v2`) has ended in
  `kIOGPUCommandBufferCallbackErrorTimeout` on five consecutive runs on this
  machine, four of them on the first presented frame, while 900-frame CPU-route
  runs complete. Re-verify device health before trusting any GPU measurement —
  see `svraster-gpu-runs-degrade-the-device`.
- Pre-existing failures, all proven independent of this work by stashing every
  touched file and rebuilding: `SparseVoxelTerrainGBufferContract` (37 pixels of
  invalid motion), `SparseVoxelWorldTerrainRasterRegression`,
  `SparseVoxelWorldTerrainLightingResidencyRegression`,
  `SparseVoxelWorldTerrainGraphRecompile` (GPU budget: 2340 MiB needed against a
  2048 MiB cap). `SparseVoxelTerrainVirtualGeometryGBufferSmoke`,
  `SparseVoxelTerrainVGCutErrorContract`, `SparseVoxelTerrainGPUVGContract` and
  `SparseVoxelTerrainPhase7GPUAtlasDifferential` fail on the device timeout.

## Phase 8 — Terrain as a file-backed virtual-geometry instance — **8a/8b/8c DONE 2026-09-06; 8d/8e/8g DONE 2026-09-08 (8e: culler retired, payload stays and the item was wrong about why); 8f done, off by default**

The design the user proposed, and it is the right one: stop treating terrain
as a live CPU-side arena republished every transaction, and make it an
ordinary streamed virtual-geometry asset backed by a scratch file on disk.

```text
generate a chunk (all levels)  ->  encode with the normal VG machinery
                                   (pages, dependencies, LZ4)
                               ->  append to a scratch file, record its
                                   page directory entry
culler asks for a page         ->  CPU reads that page's byte range,
                                   installs it (already budgeted)
chunk no longer needed         ->  its file region is free for reuse; the
                                   file is a cache, not a save format
```

**Why this is the best design available here, not just a simplification**

- Every remaining Phase 6 memory item disappears by construction, rather than
  being optimised: the per-transaction whole-arena mirrors (one 340 MiB
  material-arena copy per transaction, measured), the transient page store
  (the file *is* the cache), the brick arenas as a runtime structure, and the
  coupling that makes generation and publication one transaction.
- The machinery exists. `SparseVoxelFileRuntime` + `SparseVoxelSceneFile`
  (the milestone-2 VSVO writer/reader) already run "root pages resident,
  GPU-driven refinement from a file", `VirtualGeometryFileCreation` already
  LZ4-compresses pages and records their dependencies, and the scene's
  install/uninstall path already takes encoded pages one at a time with a
  per-frame budget (7a) and evicts them LRU (7c).
- It answers a page request with a bounded disk read instead of a re-mesh,
  which is what 7b (lazy meshing) was trying to approximate: levels are
  generated once, ever, and the "level cap by distance" becomes "which byte
  ranges are resident".
- It makes the demand path uniform: the same request, install, evict cycle
  serves near-field prefetch, far-field root-only residency and the
  bad-memory-settings case the user described (root pages only, everything
  else on disk).

**Staging** (each step measured through `safe_run.sh`, both new contracts as
the gate)

- **8a. Write path.** After a chunk is meshed, encode and append it to the
  scratch file; keep the in-memory asset as today. The file's directory maps
  (ground, level) -> page byte ranges + dependencies. Gate: byte-identical
  re-read of every page, and the two contracts unchanged.

  **Implemented 2026-09-06**, `TerrainVirtualGeometryScratchFile`
  (`src/editor/virtualgeometry/`). Shape, and why:
  - **Two-phase, encode then append.** `encodeChunk` is static and touches no
    file state, so it runs on the terrain worker that meshed the chunk -
    `PageBuffer::encode` plus LZ4 over a few hundred KB is the whole cost.
    `appendEncodedChunk` is one `fwrite` of bytes that are already final, so it
    is cheap enough for the thread that publishes the chunk. This also means a
    build dropped at publication (its chunk moved on while it was meshing)
    never reaches the file: only published geometry is written.
  - **The framing is the one `VirtualGeometryFile::write` uses:** a page is
    `PageBuffer::encode` output in an LZ4 block, addressed by
    (offset, storedBytes, uncompressedBytes). What is added is per-page
    sidecars - dependencies and install/uninstall rewrites live *inside* the
    page's block, so installing a page is exactly one read.
  - **The chunk header is block 0** and carries the hierarchy slice, the root
    page index, the quantization (a zero `unit_scale` collapses every vertex to
    the origin) and the concatenation of every page's install rewrites. The
    parent-republish path needs all of those rewrites the moment the chunk
    installs, so keeping them in the header is what stops a chunk install from
    costing one read per page.
  - **Fail-closed on both ends.** `open` refuses a path that already exists
    (it truncates, and a mistyped path would be gone before anything reported
    it); `close` removes the file, since nothing outlives the process. A read
    cursor refuses to run past its block rather than trusting the counts it
    just read, and a released handle stops resolving - a stale handle that
    still read would be one chunk drawing another chunk's geometry, which
    reads as a culler bug for days.
  - **Handles die with their entry.** The page cache retires a chunk's scratch
    bytes at every point an entry dies (tail drop, adoption displacement,
    eviction, failed rebuild, re-mesh, `clear`). An *adoption* moves the handle
    with the entry: same ground, same geometry, different slot - see
    `terrain-vg-page-cache-slot-adoption`.
  - **On by default since 2026-09-06, with the path chosen for the run**
    (`$TMPDIR/svraster-terrain-vg-<pid>.vgcache`, removed on close): the file
    is a cache nobody names or keeps, and making the caller invent a path only
    invited a mistyped one. `--terrain-vg-no-scratch` turns it off,
    `--terrain-vg-scratch-file <path>` names one, `--terrain-scratch-mb`
    bounds it (2048 MiB default), `--terrain-vg-scratch-verify` reads every
    page back and compares it against the asset it was encoded from.
    `SVR terrain VG scratch:` prints with `SVR terrain VG page use`, and
    `Sparse voxel terrain scratch:` on every bounded run.
    `TerrainVirtualGeometryScratchFile::open` still refuses a path that already
    exists; the sample clears a leftover only for a path it chose itself, where
    ownership is known, rather than weakening that guarantee.
  - **Two ways this silently does nothing, both paid for:** the write switch
    read the *path* rather than the enable flag, so with an auto-selected path
    the file opened and every chunk went unwritten (`chunks=0` with terrain
    drawing normally from memory); and a scratch file that fails to open only
    warns. Check the `Terrain VG scratch file:` line at startup and a non-zero
    `chunks=` in the summary before believing a measurement.
  - Unit gate: `SparseVoxelTerrainVGScratchFile` (ctest, CPU only).

  **Measured 2026-09-06**, 1 km / `--lod-threshold 1.5` / 300 frames /
  640x360, through `safe_run.sh`, with `--terrain-vg-scratch-verify`:
  `chunks=493 live=369 pages=2416 file=86.1MiB live=62.4MiB writeMs=18.9
  readMs=98.6 verified=493 mismatches=0 writeFailures=0 readFailures=0`.
  - **The gate holds: zero mismatches over 2,416 pages.** Every page read back
    is the words `PageBuffer::encode` produced, with its dependencies and its
    rewrites.
  - **86.1 MiB written, 62.4 MiB live** - 493 chunks written, 124 retired and
    their bytes accounted dead, so the release path fires. About **179 KB per
    chunk compressed**, against the ~9 MiB of CPU per resident chunk the arena
    costs today. That ratio is the whole argument for 8b.
  - **Writing is free: 18.9 ms over the entire run** on the publishing thread
    (the encode is on the worker that meshed the chunk). The 98.6 ms of reads
    is the verify pass, off in a shipping run - but it prices 8b: 2,909 reads
    in 98.6 ms is **~34 us per page**, LZ4 decompress included, so the 8-page
    install budget is ~0.3 ms a frame. 8b can start synchronous and only move
    to a prefetch queue if that shows up.
  - Both contracts green in the same run (`lastActivityFrame=161`,
    `lod0Chunks=48`, `installFailures=0`, `noSlice=0`; profile
    `violations=0 inversions=0 nearCoarse=0`) and green again as ctest against
    the same binary.
- **8b. Read path.** Install from the file rather than from the in-memory
  asset; drop the asset once written. Reads happen on the existing terrain
  worker pool, never on the render thread. Gate: contracts green, plateau
  under 4 GiB at 1 km / 1.5 px.

  **Implemented 2026-09-06. Reads work and the assets are gone; the 4 GiB
  target is not met, and the reason is that the VG asset was never the big
  term.**
  - **The install path no longer knows where the bytes came from.** The bridge
    grew `SparseVoxelTerrainVGChunkPageBytes` (encoded words, dependencies,
    install rewrites) plus `installSparseVoxelTerrainVGChunkPageBytes` and
    `installSparseVoxelTerrainVGChunkIntoSlotFromBytes`, the latter fetching
    each page through a callback so the hierarchy-before-pages order stays
    inside the bridge where it was paid for. The asset-taking entry points are
    now thin wrappers over these, so the in-memory path and the file path run
    the same code.
  - **A chunk install is one header read plus one read per page it writes**;
    a requested page is one read. Reads are synchronous on the thread that
    installs: measured at **62 ms over a 300-frame run** (0.2 ms a frame) for
    every chunk and page install in it, which is not worth a prefetch queue
    yet. `readFallbacks` counts installs that had a handle and had to use the
    asset instead; it is 0 in every run so far, and a non-zero value is the
    signal that the read path is not actually serving.
  - **Entries keep the chunk's shape, not its geometry**: page count, root
    page, hierarchy node count, quantization, and the two byte totals the pool
    sizing needs. `--terrain-vg-scratch-keep-assets` holds the geometry as
    well, for the A/B.
  - **`TerrainVirtualGeometryAssetSet` held a full second copy of every
    chunk's encoded geometry and nothing ever read it** - the renderer takes a
    pointer to the set and never touches it, and the only readers of `assets`
    were `.empty()` and `.size()` in a log line. It is a count now.

  **Measured 2026-09-06**, A/B on one binary, 1 km / 1.5 px / 300 frames /
  640x360, `SVR_TRACE_TERRAIN=1`, through `safe_run.sh`:

  | | assets kept | assets dropped |
  |---|---|---|
  | peak footprint | 5.94 GiB | **5.52 GiB** |
  | resident chunks | 366 | 360 |
  | settle frame | 165 | 165 |
  | install failures | 0 | 0 |
  | scratch read fallbacks | 0 | 0 |

  **0.42 GiB, and that number is the point.** It is almost exactly the file's
  live size (63.5 MiB compressed, roughly 400-500 MiB expanded), which says
  the encoded VG asset is *all* that came out - and that the "~9 MiB of CPU
  per resident chunk" in phase 7.0 was never the VG asset, it was the brick
  payload and the arena beside it. The remaining 5.5 GiB is the list phase 6
  already names (Tree64 pools, the world bundle, the material-arena mirror)
  plus the VG runtime pools themselves (182 MiB of pages, 4.4M decoded
  vertices). **So the 4 GiB gate is not 8b's to meet**; it is 8e's, and the
  honest statement is that file-backing removed the asset term completely and
  the term was 0.42 GiB.

  **The trap, paid for once (2026-09-06):** the scene's runtime pools are
  budgeted by walking every cache entry's asset for hierarchy and encoded
  page bytes. With the assets released that walk finds nothing and falls back
  to its floors (8 KiB hierarchy, 64 KiB pages per chunk), which gave the page
  buffer **32 MiB instead of 244**. Nothing failed loudly: the pool saturated
  at 183 installed pages by frame 60, the demand band starved, and the cut
  froze at **195 resident chunks against 366** with `lastActivityFrame=62`
  and one install warning. Both byte totals are recorded on the entry at build
  time now. **Any sizing pass that measures a cache by walking payloads it
  does not own is this bug waiting to happen** - and note the shape of the
  symptom, since it is what made it hard: half the terrain, no error.
- **8c. Region reuse.** Free-list the file by region; a ground that leaves the
  cut releases its ranges. Bound the file (`--terrain-scratch-mb`). Gate: a
  720-frame walk keeps the file bounded with no install failure.

  **Implemented and measured 2026-09-06. Gate met.** A chunk is written as one
  contiguous blob, so a region is the unit: release puts it on a free list kept
  sorted by offset and coalesced with either neighbour, append takes first fit
  and otherwise extends the cursor. A region at the very end gives its bytes
  back to the cursor, so a file that empties drops its high-water mark instead
  of holding the peak of the run. First fit, not best fit: chunks of one LOD
  are near enough the same size that freed regions are reused by their own
  kind, and the scan is not worth a placement quality this workload cannot use.
  **Handles are never reissued** - a dead record keeps its slot with its page
  table cleared (a few dozen bytes), because reusing an index would let a stale
  handle resolve into whatever took its place.

  720-frame walk (`--test-camera-walk`), 1 km / 1.5 px, through `safe_run.sh`:
  - **Unbounded:** 546 chunks written, 410 live, **file 71.3 MiB against
    69.7 MiB live** - 98% occupancy, 1.6 MiB free in 66 regions - with
    `reuse=208 extend=338`. Without reuse the same walk writes ~97 MiB and
    keeps growing; with it the file tracks the resident set.
    `installFailures=0`, `readFallbacks=0`.
  - **`--terrain-scratch-mb 32`, deliberately under the 70 MiB working set:**
    the file stops **exactly at 32.0 MiB**, 200 appends are refused, and
    **`installFailures=0` with 399 resident chunks and `lod0Chunks=40`** - a
    refused write leaves the chunk's asset in memory, so a budget below the
    working set costs RAM and never geometry. That is the degradation the
    design wants, and it is the answer to "the file can be huge": it does not
    have to be.
- **8d. Incremental generation, not startup-complete.** Generate a chunk's
  levels on first demand and append; the file grows with what has been seen.
  A whole-world bake at startup is minutes and is the wrong default; keep it
  as an offline option later.

  **Implemented and measured 2026-09-08 - as a retained directory of every
  (ground, level, payload) the run has seen, not as levels appended to a
  chunk.** Two readings of this item were possible, and the second is the one
  that holds up:
  - *Append a chunk's finer levels later* (7b's shape) needs a coarse-only
    build whose pages are a byte-identical prefix of the full build's. They
    are not, and cannot be made so cheaply: every cluster's `self` bound is
    the join of the whole subtree under it (`groupBounds` in the octree
    builder joins the children's `self`, recursively) and the finest included
    level's clusters would carry `tightBounds` instead. Producing the full
    build's bounds without meshing the finer levels means walking every
    finer node's exposed faces anyway, which is most of the meshing. And on
    this route a chunk draws only the band `[v, 2v)` below its parent chunk
    (10c), so its coarse levels are never the ones wanted first.
  - *Generate each ground once, ever* is what the phase 8 design bullet
    actually says ("levels are generated once, ever, and the level cap by
    distance becomes which byte ranges are resident"), and it is bounded and
    measurable. That is what was built.

  The shape:
  - **Identity is content, so a revisit is recognisable.** `brickPayloadRevision`
    is an FNV hash of the generated chunk's bytes mixed with a hash of its
    tile address (`PlanetVoxelChunk.cpp`, `sourceRevision` / `mixRevision`),
    and `regionMappingId` is a hash of the tile. A ground regenerated later
    with the same content reproduces both, so the page cache's existing
    adoption key `(regionMappingId, clipmapLOD, brickPayloadRevision)` is the
    file directory's key too. Nothing in the file format changed.
  - **A dead entry retains its scratch chunk** (`retireEntry`): at every
    point an entry died - tail drop, adoption displacement, no-geometry
    eviction, failed rebuild, re-mesh under a new payload - the entry's shape,
    build stats (the parent republish needs the cluster boxes) and handle move
    into `retained_` under the identity, instead of the region being released.
    The asset, if one is still held, goes to `pendingRelease_` as before. A
    second retirement under the same identity keeps the newer copy.
  - **A stale slot is served from the directory** (`reviveFromRetained`,
    checked after slot adoption and before a build is queued): the record's
    identity is looked up, the retained entry moves back into the slot with
    the slot-derived fields (chunk index, clipmap slot, node revision, page
    origin, world centre) taken from the live record, and the update counts a
    `revivedChunks` instead of a `rebuiltChunks`. The scene sees an ordinary
    entry with a scratch handle, so the 8g install path reads its pages with
    no code of its own.
  - **The file budget is enforced against the retained set**, never the live
    one: before an append, `canAppend` is asked and retained chunks are
    released least-recently-retained first until it fits. A budget below the
    live working set still degrades exactly as 8c described - a refused append
    and an asset kept in memory - because live chunks are never touched.
  - Gate: `SparseVoxelTerrainVirtualGeometryRuntimeRefinement`
    (`testRetiredChunksAreRevivedFromTheScratchFile`): evict, retain, revive
    with the same handle and shape and no build; a changed payload at the same
    ground is a separate chunk and both coexist; retention off releases at
    once; `clear` releases retained chunks; under a file bounded to exactly the
    working set a new payload's append is made room for by retiring the
    retained chunk, with no refused append.
  - `--terrain-vg-no-scratch-retain` for the A/B; the scratch summary line
    prints `retained=<count>/<MiB> retainedTotal= revived= retainedEvicted=`.
  - A harness that comes back: `--test-camera-out-and-back` with
    `--test-camera-travel-m <m>` is ground-forward travel that turns around at
    the run's midpoint. `--test-camera-walk` oscillates over 12 m and never
    leaves ground it has seen, which is why it shows `revived=0`.

  Measured, 5 km / 1220x720 / 1.5 px / 300 frames / 512 MiB pool, synchronous
  streaming, through `safe_run.sh`:

  | | reference walk | out-and-back 400 m, retain on | retain off |
  |---|---|---|---|
  | chunks meshed | 658 | 843 | 1,009 |
  | revived | 0 | **166** | 0 |
  | retained at end | 163 / 14.4 MiB | 362 / 17.8 MiB | 0 |
  | scratch file | 54.6 MiB | 43.8 MiB | 26.9 MiB |
  | worker meshing ms | - | **971** | 1,151 |
  | resident / LOD0 | 495 / 40 | 481 / 128 | 481 / 128 |
  | install failures | 0 | 0 | 0 |
  | final frame | 219,600 / 0 holes | 219,600 / 0 holes | 219,600 / 0 holes |

  The reference walk is unchanged in every number and in its final hash. On
  the walk that returns, one chunk arrival in six is a read instead of a mesh,
  worker meshing drops 16%, and the price is 17 MiB of file. The remaining
  cost of a revisit is the terrain generator regenerating the bricks the
  chunk record is made of - the VG route no longer needs them to draw, which
  is 8e's subject, and until then a revive still waits on that generation.

  What it does not do: retained entries keep their build stats in memory
  (`clusterSources` is ~60 bytes a cluster, tens of KB a chunk), which is what
  the parent republish's quadrant masks read; moving them into the chunk
  header is the obvious next step if the retained set ever matters for RAM.
- **8e. Retire what terrain no longer needs on this route.** With geometry
  served from the file, the VG route does not need the brick arenas, the
  Tree64 payloads, or the lighting payloads to draw. Gate each removal
  separately: the *brick culler* currently produces the feedback that drives
  the spatial cut (`readWorldTerrainPageRequests`) and the merge votes the
  GPU route needs, so removing bricks means the VG demand band becomes the
  only cut feed - measure the cut and the two contracts before and after,
  and keep a flag to restore them.

  **REVERTED 2026-09-08. The culler is back on by default; the removal held
  only for the harness it was measured on.** Everything below was measured and
  is true of the scripted walk at 5 km with synchronous streaming. It is not
  true with a free camera, and the free camera is what the renderer is:

  | 5 km, no walk, no synchronous streaming | resident | LOD0 | worst px |
  |---|---|---|---|
  | brick culler on (baseline) | **489** | **32** | 2.37 |
  | culler off, cut coupled | 429 | 12 | 0.00 |
  | 25 km default, culler on (baseline) | **504** | 4 | 2.30 |
  | 25 km default, culler off | 327 | 4 | 0.00 |

  The profile numbers look *better* without the culler and the picture is
  worse, which is the trap: the profile scores the chunks the culler touched,
  so a cut that holds fewer chunks scores well on the ones it kept. Resident
  chunks and LOD0 chunks are the numbers that track what the screen shows, and
  both fall by a third. The reason is that the brick culler's requests refine
  *where the camera is looking*, and the coupled cut - a better global target -
  coarsens uniformly to fit the arena, so the near field pays for the horizon.
  The scripted walk hides it because it oscillates over 12 m of ground the cut
  has already resolved.

  So retiring the culler needs a demand feed that follows the view, not a
  better global target. `--terrain-vg-no-brick-feedback` and
  `--terrain-vg-lod-coupled-cut` keep both experiments available, and the
  budget-fit bisection that served the coupled cut is reverted with it.

  **What survives:** the publication gate fix, which is a real defect
  independent of any of this - the frame's refine requests were handed to the
  streamer only when the *brick* feedback had been read, so a route without it
  dropped them entirely.

  **Historical, and measured on the walk harness at 5 km:** The first measurement said the culler's requests were
  load-bearing (495 -> 274 chunks, two profile violations). That was true and
  the diagnosis was wrong: three separate defects were doing the work the
  brick feed appeared to do, and with them fixed the culler leaves and the cut
  is *better* than it ever was with it.

  **The three defects, each of which alone froze the cut without bricks:**
  1. **The publication gate.** Handing the frame's refine requests to the
     streamer was gated on the *brick* feedback read
     (`if (readTerrainFeedback || vgFeedbackRequests != 0u)`), so with the
     culler off the demand band's requests were computed, logged and dropped -
     16 a frame reaching an empty queue, `World terrain take: taken=1 cap=8`,
     a cut advancing one chunk per transaction and settling at 30 of 500. This
     is the one that made the brick culler look load-bearing: not its requests,
     the gate they shared.
  2. **The cut target.** The streamer's geometric cut is pinned at 4 px
     (`kGeneratedWorldTerrainSplitErrorPixels`) while the renderer draws to
     `--lod-threshold`; the brick requests were closing that gap one at a time.
     The cut is now coupled to the render budget on this route
     (`gTerrainVGLodCoupledCut`), with the distance-growth term zeroed so the
     streamer's projected error is exactly the renderer's.
  3. **The budget-fit hysteresis.** The fit-to-arena search raised the *merge*
     threshold to equal the split threshold, so a chunk that had just refined
     was immediately a merge candidate. Invisible while the path almost never
     ran; with a 1.5 px budget it runs every update and the cut thrashed -
     `built=32 refined=24 coarsened=8 evicted=38` per transaction, pinned near
     320 against a 510 target for 600 frames. It now carries merge in
     proportion, and bisects for the finest fitting cut instead of taking
     wherever a 1.5x escalation first crossed the budget (which left 35% of the
     arena unused).

  **Measured, brick culler off by default, 5 km / 1220x720 / 1.5 px / 300
  frames / 512 MiB pool:**

  | | brick culler on (before) | off (now) |
  |---|---|---|
  | resident chunks | 495 | **510** |
  | profile violations | 0 | **0** |
  | worst px | 2.89 | **0.00** |
  | chunks over budget | 28 | **0** |
  | settled at frame | 299 | **229** |
  | covered pixels | 219,600 | 219,600 |
  | install failures | 0 | 0 |
  | `visibleBricks` | 2,598 | **0** |

  The LOD profile is *exact* now - every chunk the culler touched is at or
  under the pixel budget - because the cut is built from the same metric the
  contract judges instead of being nudged toward it by feedback. The 1 km
  contract configuration settles at frame 164 with 68 LOD0 chunks at the same
  2.25 px worst case the brick path reached, using 27% fewer chunks. All nine
  gates green.

  `--terrain-vg-brick-feedback` restores the culler and its requests;
  `--terrain-vg-no-lod-coupled-cut` restores the fixed 4 px target;
  `--terrain-vg-no-spatial-demand` disables the projected-voxel demand feed
  added alongside (which measures inert - the rendered-error band already
  claims every over-budget chunk it would name - and is kept because it is the
  feed that is correct by construction rather than by coincidence).

  **The payload stays, and the item was wrong about why.** Three layers, found
  in order:
  - not `prepareTerrainBrickUploadPlan` as predicted, but the *geometry field
    cache*: `voxelGeometryFieldCacheCapacity` is sized from the brick hierarchy
    at graph build and then refuses every publication whose real table differs.
    Now gated (capacity zero means "no geometry fields this run").
  - behind that, the terrain **publication rides on the upload pass**, which is
    registered only when the payload is: without it every publication is
    refused with "a terrain GPU publication is already pending" (99 refusals,
    cut frozen at 6 chunks). The material field atlas and the page states
    travel in the same batch as the brick arenas.
  - and decisively, **the CPU brick hierarchy is the input geometry the VG
    mesher meshes from** (`buildSparseVoxelOctreeVirtualGeometry(chunks.bricks, ...)`).
    It can never leave this route; terrain would have nothing to mesh.

  So the memory item is narrower than stated: the *GPU* arenas and the CPU
  mirrors kept for upload, not the hierarchy. Freeing those means teaching the
  terrain publication to complete with no brick ranges in its batch - the same
  pipeline the lighting rewrite will touch, and the reason it is left to it.
  `--terrain-vg-no-brick-payload` reproduces the refusals.

  **Historical: the original measurement, before the three defects were
  found.** Kept because the shape of the failure is worth remembering - the
  cut settled coarse with coverage *unchanged* at 219,600 px, so nothing in
  the gap counters moved and only the LOD profile showed it.

  The staging the plan asks for, in order:
  - **The brick raster is already gone** on this route and has been:
    `registerBrickResolver(prefix, /*feedbackOnly=*/true)` registers
    `BrickPhase*.Reset/.Cull/.DrawArgs` and no draw pass at all. `visibleBricks`
    in the summary is that culler's statistic, never a draw (see the metrics
    section above).
  - **The merge votes are already off** on this route
    (`feedbackMergeVotes = gbufferResolver != TerrainVirtualGeometry`), so what
    the brick culler still contributes is *refine* requests alone.
  - **What the brick passes cost on the GPU: nothing measurable.** A 300-frame
    walk with `--gpu-timings` lists no brick pass in the top twelve; the
    twelfth is 0.293 ms and the frame is dominated by the VG meshlet draw
    (16.3 ms) and the scene ray schedule (12.5 ms). So the case for removing
    them was never GPU time - it is the brick payload's memory, which the
    culler needs resident (`brickPayloadResident` covers both routes for
    exactly this reason).
  - **`--terrain-vg-no-brick-feedback`** silences `readWorldTerrainPageRequests`
    on this route and leaves everything else running, so the VG demand band is
    the only feed. Reference walk (5 km, 1220x720, 1.5 px, 300 frames, walk,
    512 MiB pool, synchronous streaming):

    | | brick feedback on | off | off, demand budget 256 |
    |---|---|---|---|
    | resident chunks | **495** | 274 | 265 |
    | chunks at LOD0 | 40 | 48 | 48 |
    | profile violations | **0** | 2 | 2 |
    | worst px | **2.89** | 4.53 | 4.53 |
    | worst levels | 0.95 | 1.60 | 1.60 |
    | chunks meshed | 658 | 371 | 371 |
    | covered pixels | 219,600 | 219,600 | 219,600 |
    | install failures | 0 | 0 | 0 |

    **The LOD profile contract fails without it**, which is the gate this
    phase was told to hold. Note that coverage is *unaffected*: the
    streaming-leaf marks keep the frontier drawing, so the failure is not a
    hole - it is terrain that is permanently two levels too coarse, which is
    the failure mode that reads as "the cut silently stopped refining" and is
    exactly what the metrics section warns about.
  - **It is not a budget.** Raising `--terrain-pages-per-update` from 64 to
    256 moves the settled cut by nine chunks (274 → 265, the wrong way, inside
    run-to-run noise) and does not move the profile at all. The demand queue is
    not full; the demand band simply never asks about that ground.

  **Why, and what would unblock it.** The VG demand band is a *rendered-error*
  feed: it asks to refine a chunk whose drawn triangles are too large on
  screen. The brick culler's requests are *spatial* - they name ground the cut
  should hold at a level, whether or not anything is currently drawing there
  too coarsely. A chunk that is coarse enough for its own triangles to be
  small, but whose ground deserves a finer chunk, is invisible to the first
  feed and named by the second. That is the same asymmetry `7d` records from
  the other side (demand only refines; the merge votes were the only coarsen
  signal). So retiring the brick culler needs a **spatial feed on the VG
  route** first - the projected-voxel demand band widened to score resident
  ground rather than only drawn triangles - and that is a phase of its own,
  not a removal. Until it exists the brick payload stays resident, and the
  0.42 GiB that phase 8b removed remains the whole of what file-backing saves.

  Not attempted, and why: the Tree64 payload is the shadow backend
  (`ShadowBackend::Tree64`) and the lighting payloads are what the lighting
  passes read, so neither is terrain-draw scaffolding that this route made
  redundant - they are separate consumers with their own gates.
- **8f. Page size — byte budget implemented 2026-09-08, off by default.**
  `VirtualGeometryBuildSettings::maxPageBytes` packs a page until an estimated
  byte budget is reached instead of to a group count alone; a group larger than
  the budget is still placed, because a page must hold at least one.
  `SVR_VG_PAGE_KIB` sets it for terrain. The estimate runs before the encoder,
  so it is a target rather than a guarantee - it only has to be monotone in the
  geometry and deterministic, and it is calibrated against the measured page
  buffer (178 clusters and 142 KiB per page, about 800 bytes a cluster).

  Measured, 5 km / 150 frames / 246 chunks, coverage unchanged in all three:

  | budget | installed pages | page buffer | scratch file |
  |---|---|---|---|
  | off (count only) | 857 | 119.1 MiB | 888 pages, 22.4 MiB |
  | 128 KiB | 872 (+1.8%) | 121.5 MiB | 891 pages, 22.9 MiB |
  | 64 KiB | 966 (+12.7%) | 121.3 MiB | 994 pages, 23.0 MiB |

  **The premise of the note below was wrong**: pages are not "about 25 KB". That
  is the LZ4-compressed size in the scratch file; the page the buffer holds and
  the install allocates is ~142 KiB, already at the 128 KB target. So a byte
  budget makes page size *even* rather than *larger*, and raising
  `MAX_GROUPS_PER_PAGE` would move it the wrong way. Left off by default until
  there is a measurement that wants a specific size - the knob is the
  deliverable, not a new default.

  Original note: **8f. Page size.** With pages read from disk, larger pages are pure win
  (fewer reads, fewer dependencies): raise `MAX_GROUPS_PER_PAGE` (64 today,
  about 25 KB per page, chunks of 4-32 pages) toward a 128 KB page, and
  consider one page per chunk where a chunk's every level fits. Check the
  encoded format's per-page index widths before raising, and re-measure the
  install budget: one page per chunk makes an install atomic and root-first
  moot for near chunks.
- **8g. Push the file range down into the scene (the user's framing).** After
  8b the terrain route reads a page's bytes and hands them to
  `installRuntimeObjectEncodedPage`, which still takes a `vector<uint32_t>`.
  The end state is that a runtime object's page *is* a (file, offset, size),
  so the scene reads it itself - which makes the eviction the scene already
  does reversible without a CPU-side asset anywhere, and serves every virtual
  geometry instance, not only terrain. `VirtualGeometryFile` already carries
  exactly this descriptor per page (`file_offset`, `compressed_size`,
  `uncompressed_size`), so this is a scene-side API change, not a format one.
  Do it after 8b measures what the read actually costs.

  **Implemented and measured 2026-09-08.** The shape, and what each part is for:
  - **`VirtualGeometryPageSource` / `VirtualGeometryPageLocation`**
    (`src/runtime/virtualgeometry/VirtualGeometryPageSource.hpp`). A location is
    (source, key, index, offset, storedBytes, uncompressedBytes); the source
    reads `PageBuffer::encode` words at it. `key`/`index` are the source's own
    identity for the page and exist so a source can *refuse* a stale location
    rather than read what now sits at the offset - the scratch file reuses
    regions, and a stale read there is one chunk drawing another chunk's
    geometry. Both `TerrainVirtualGeometryScratchFile` (key = chunk handle) and
    `VirtualGeometryFile` (index = page table entry) implement it, so any
    file-backed instance is served the same way as terrain.
  - **`RuntimeEncodedPageInstallInfo::location`.** With no words and a valid
    location the scene reads the page itself (after the generation and
    protection checks, so a rejected install costs no read). A valid location is
    recorded on the page (`RuntimePageStatus::reinstall`, with the scalar
    install parameters and the scan ranges) and **survives an uninstall** like
    `installUpdates` does, which is what makes the scene's LRU eviction
    reversible with no CPU-side asset anywhere: `reinstallRuntimeObjectPageFromSource`
    rebuilds the install from the record, dependencies first (an eviction may
    have taken them too), and `reinstallRequestedRuntimePagesFromSource` runs
    that over the culler's requests.
  - **Priming.** `primeRuntimeObjectPageSource` records a location on a page that
    was never installed, so a first-time request is the scene's to serve as
    well. The bridge's chunk install primes every page it leaves for the culler,
    and the scratch chunk header now carries every page's dependencies beside
    its rewrites - so a page install, first or repeat, touches **no page block
    in the caller at all**: the header is read once per chunk, the scene reads
    each page's words once per install. `readPage` on the scratch file is now a
    verify-only path.
  - **Forgetting.** A retired chunk's page slots and a failed chunk install's
    rollback forget their locations (`forgetRuntimeObjectPageSource`), and a
    source about to close can drop every location that reads from it
    (`forgetRuntimePageSource`). Without this a later request could reinstall a
    dead chunk's page into a slot another chunk now owns - the scratch file
    would refuse the read, but the failure belongs at the retire, not at the
    read.
  - **Gates.** `SparseVoxelTerrainVGScratchFile` (locations read the same words
    as `readPage`; a tampered range, a released chunk - even after its region is
    reused - and a page past the chunk are refused; a `.vg` file's locations are
    its page table); the phase 14 suite (`testRuntimePagesAreLocationsTheSceneReadsItself`:
    install from locations alone, evict, reinstall with the original
    generation/revision and the dependency restored, evicted dependencies come
    back first, requests, a refusing source, priming, forgetting, words plus
    location, words alone). All eight gates green.

  Measured, reference run (5 km, 1220x720, 1.5 px, 300 frames, walk,
  512 MiB pool), same numbers as before: 495 chunks / 40 at LOD0, 0 install
  failures, 0 root repairs, 0 read fallbacks, coveredPixels 219,600 and holes 0
  on the final frame, LOD profile unchanged (`violations=0 worstPx=2.89`).
  `sourceReads=1746` against `pages=1771`: the scene read almost every page it
  installed, 49 ms of reads over the run. `sceneServed=7`: at 512 MiB nothing
  is evicted (`evicted=0`) and near chunks install whole, so the request path
  is rare here.

  With the pool forced under the working set (`--terrain-vg-memory-mb 128`):
  `evicted=198`, `sceneServed=48`, `sceneServeFailures=0`, `readFallbacks=0`,
  final frame still 219,600 covered / 0 holes, 495 chunks / 36 at LOD0. The 164
  install failures are all `scene buffers cannot fit the encoded page ... await
  deferred release` - the documented degradation of a pool below its working
  set, and none from the new path.

  **What it does not change:** the terrain route still reads the chunk header
  at chunk install (hierarchy, rewrites, and now dependencies), and the
  `--terrain-vg-no-scratch` / `--terrain-vg-scratch-keep-assets` A/B paths still
  install from the in-memory asset and record no locations, so they fall back
  to the caller-served request loop exactly as before.

