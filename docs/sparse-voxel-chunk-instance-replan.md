# Voxel terrain as GPU-emitted chunk instances

A replan. The brick-cluster plan it replaces assumed the brick hierarchy could
become terrain's LOD once the streaming was incremental. Measurement says it
cannot, for a reason that plan never considered, and the reason changes the
target.

---

## Part 1 - What is established

Each of these is measured, not assumed.

**The LOD machinery works, and terrain discards all of it.** Disabling
`forceLeafRaster` on a converged 16 km world:

| | gate on | gate off |
|---|---|---|
| bricks drawn | 65,123 | **1,050** |
| covered pixels | 253,878 | 257,753 |

A 62x reduction in drawn geometry is sitting behind one flag.

**The flag is protecting against a real artifact.** Rendered both, frame 99,
same camera. The reference is clean fine-grained ground. With the gate off:
scattered dark blocks standing above the surface, long streaks following the
terrain contours, terracing. 190,323 pixels differ by more than 0.02.

**It is not a coarseness problem.** `lodPixelThreshold` is 1.5, so a selected
brick's voxel projects to about a pixel and a half. The blocks in the image are
many pixels tall. The reduction is not producing a coarse surface, it is
producing a *different* one - which is what the shader comment always said:
"invents occupied cells that neither the authored leaves nor Tree64 have".

**It is not a threshold-rule problem either.** Terrain pages store a surface
shell - `surfaceShellDepthVoxels = 1`, one top voxel per flat column - and the
first hypothesis was that OR-reducing a sparse shell promotes lone voxels. A
majority reduction requiring two occupied children per parent cell produced a
visually identical image. That hypothesis is dead.

**The mechanism that fits the evidence.** On a slope, four child sub-columns
under one parent cell have their single top voxel at different heights. Where
those heights straddle a parent-cell boundary, *both* parent cells become
occupied, and the column is two coarse voxels thick where the real surface is
one. Every slope becomes a doubled staircase, which is exactly the streak
pattern in the image and exactly why a per-cell count threshold cannot help: a
parent cell on a slope legitimately has several occupied children. Supported by
the artifacts following contours rather than scattering uniformly, and by the
majority-reduction result. Not yet proven.

**There are two LOD axes and they do not compose.** A page's voxel is
`0.1 * 2^spatialLOD`, chosen by the CPU quadtree. A brick level halves it again.
Adjacent ground can therefore resolve at three different scales at once. VG has
no equivalent, because a page there is a streaming container and every LOD
decision lives in one cluster DAG.

**Geometry is not where the frame goes.** Metal System Trace, two captures:
`SceneScreenTrace` is 22-30% and no brick pass is close. Per-pass shares are
otherwise untrustworthy here - `BrickPhase1.DrawArgs`, a `workgroup_size(1,1,1)`
kernel writing four values, reports 3-5% - so treat the ranking as ordinal only.
The renderer's own `--gpu-timings` is worse: it attributed 2.33 ms to
`resetBrickCounters`, which stores twelve atomics from a single thread.

---

## Part 2 - What already exists

Not to be rebuilt.

- **Brick cut selection**: monotonic two-test cut, no traversal, no barriers,
  crack-free by construction. Two-phase culling with an HZB, consecutive-frame
  occlusion agreement, 25% LOD dead band with double-buffered ownership.
- **Bricks are already drawn as instances**: `drawArgs.instanceCount = visible`,
  eighteen vertices per instance, three camera-facing cube faces.
- **A per-page GPU table**: `SparseVoxelTerrainPageStateGPU`, sixteen bytes
  indexed by page slot, bound to the cull and Tree64 trace shaders, and written
  by the terrain upload pass. This is both the instance table and the
  install-rewrite mechanism, already in the tree.
- **GPU page requests**: the cull shader derives a page slot and desired LOD per
  selected brick and writes `terrainPageRequestBits/Priorities/Generations/
  DesiredLODs`; `SparseVoxelTerrainFeedbackPackPass` packs them for readback.
  The request half of "GPU requests, CPU installs" is done.
- **Incremental publication**: cuts advance one ownership group at a time,
  closed under 2:1 balance and disjointness, ordered by error deficit.
- **A page directory**: every page in render distance nameable and orderable
  with no payload resident.
- **Deterministic pages**: byte-reproducible across runs, worker counts and
  generation order, with a test.

---

## Part 3 - The target

One LOD axis, chunks as GPU-emitted instances, and streaming driven by the cut.

```
   chunk DAG  (one axis: chunk -> 8 children, monotonic measured error)
       |
       |  GPU: seed roots, cull (frustum / HZB / error), descend
       |      emit surviving leaves as instances
       v
   instance buffer + indirect draw args
       |
       |  GPU: unreached-but-wanted chunks write page requests
       v
   CPU: install in dependency order, rewrite flags and pointers
```

### Runtime contract

This is the contract the refactor implements. Anything outside it is a
compatibility bridge, not the destination.

**Hierarchy metadata is independent from payload residency.** The active world
window has a complete page/node directory on the GPU for the pages the streamer
can name this frame: roots, installed descendants, the selected coverage, and
the immediate refinement frontier. A page may be addressable with no voxel
payload resident. Payload bytes live in the streaming file/transient store and
are installed into physical arenas only when requested.

**A generated page owns a complete local hierarchy.** Page generation emits all
local brick geometry and hierarchy for that page's admitted finest resolution,
preferably bottom-up like virtual geometry: leaves first, then parent error and
bounds. Physical arena offsets are not part of this payload. Installing the page
relocates those local ranges and patches the runtime page table.

**GPU node selection is error driven.** A terrain runtime node contains bounds,
a monotonic error metric, child range, owning page id, fallback/draw payload
range, and flags. The cull traversal may descend only through resident child
pages whose sibling group is complete. If the wanted child page is missing, the
GPU draws the current resident fallback node and emits a page request with an
error-derived priority.

**Install patches are the only way residency changes.** CPU installs in
parent-before-child order. A page install uploads payload ranges, patches the
page table to point at those ranges, marks the page resident, and only then
enables the parent child range when the complete child group is resident. CPU
uninstalls in child-before-parent order, clears exposed child ranges before
clearing page residency, and retires physical ranges after the reader-frame
grace period.

**The visible cut is always a partition.** The GPU may draw a parent fallback or
its complete resident child group, never both, and never a partial child group.
Missing payload, pending generation, upload failure, or eviction pressure must
fall back to an already resident ancestor rather than exposing a hole.

**The page table is the source of truth.** `WorldVoxelChunkEntry` is a physical
payload/allocation record during the migration. The authoritative logical state
is `WorldVoxelPageDirectory` on CPU and the terrain runtime page directory on
GPU. Renderer code must not infer page existence from resident payload arrays.

Three properties, and the third is the one the old plan lacked:

1. **A page is a streaming container, not a LOD level.** It holds chunks; it
   does not set their resolution.
2. **Chunks are the instances.** Already true for bricks; what changes is that
   the chunk's LOD comes from the DAG rather than from which page it landed in.
3. **The coarse representation is built for rasterization, not by reduction.**
   A coarse chunk must be a *resampling of the surface* - one coarse voxel per
   coarse column at the height the fine surface actually reaches - not a
   bitwise OR of the fine cells. The OR stays, separately, because the Tree64
   and the shadow trace read it and a caster lost there is a light leak.

Point 3 is the whole unlock. Without it the DAG is unrenderable and every other
part is wasted, which is what the last two stages demonstrated.

---

## Part 4 - Stages

### Stage A - Prove the resampled coarse chunk — **RAN, REJECTED**

Implemented the resample: a coarse cell survives if it is its column's topmost
occupied cell, or if all eight of its children are occupied. The first keeps the
surface, the second keeps solid interior and cliff walls, and together they were
meant to drop exactly the cells slope doubling invents.

Rendered frame 99 with `forceLeafRaster` off. **The blocks and contour streaks
are unchanged.** Brick count fell further, 1,050 to 745, so the rule did remove
cells - just not the ones responsible.

Then isolated the other variable, because Stage 4 had made the cut read measured
error, floored at a quarter of the voxel, which could select a brick whose voxels
are far larger than the pixel threshold. Reverting the metric to voxel size
produced **745 bricks and identical coverage** - byte-identical selection. The
metric is not involved either.

That is three reductions - OR, majority-of-two, surface resample - and two
metrics, all producing the same artifact. The cause is not how a coarse chunk is
built, so no reduction rule will fix it.

What the images point at instead: the streaks are long, straight and
grid-aligned, which reads as seams between neighbours resolving at different
levels rather than error inside any one chunk. That is consistent with the
two-axis diagnosis and inconsistent with every per-chunk rule tried.

**Per this stage's own criterion, that is the stop.** Terrain keeps the
page-only LOD it has, `forceLeafRaster` stays, and the 62x is not reachable by
making coarse chunks better. Stages B through E rested on A and are void as
written.

**What would still be worth knowing**, if this is picked up again: render a
debug view colouring each pixel by the LOD level of the chunk that produced it.
If the blocks coincide with level boundaries the diagnosis is confirmed and the
question becomes whether a voxel DAG can be made seam-free at all; if they do
not, the coarse chunks are wrong in some way none of the three rules addressed
and the search continues. Everything above was inferred from images and counts,
which has now reached its limit.

Step 9 implements this as `--debug-mode 34`: chunk-level colours still come from
the terrain ownership sidecar, while brick-level colours are carried through the
resolved micro-payload side buffer for non-exact parent hits.

### Stage A (original plan)

Smallest experiment that decides everything. Replace the OR reduction, for the
raster representation only, with a column resample: for each coarse column, take
the maximum fine surface height under it and emit one coarse voxel there, plus
the shell depth below. Render frame 99 with `forceLeafRaster` off and compare
against the reference image.

**Accept** when the blocks and contour streaks are gone at a brick count far
below 65,123. **Reject** and stop if they persist - the two axes are then not
separable and terrain keeps the page-only LOD it has, which is a legitimate
answer that costs nothing more to find out.

Vertical brick boundaries are the fiddly part: a column crosses the eight-cell
brick stack, so the resample needs the neighbouring bricks in `finer`, which the
reduction loop already holds.

### Stage B - Dual occupancy

Storage for the outcome of A. Coarse chunks carry two masks: conservative for
tracing, resampled for raster. Coarse levels are roughly a seventh of all nodes,
so the cost is about 13 MB against a 1.6M-node hierarchy.

**Accept** when shadows and the Tree64 read identical occupancy to today -
`SparseVoxelTree64` suites unchanged - while the raster path reads the resample.

### Stage C - Retire the page LOD axis

With A and B, a page no longer needs to set resolution. Pages become fixed-
resolution streaming containers and the CPU quadtree's split test stops being a
LOD decision. `forceLeafRaster` is deleted rather than worked around.

**Accept** when `buildWorldVoxelChunkLeafCut` no longer consults
`splitErrorPixels`, and the drawn brick count at matched image quality is far
below today's.

### Stage D - Close the streaming loop on the cut

The request half exists. What is missing is that requests come from the *cut*
rather than from a CPU quadtree metric: a chunk that wants to refine but whose
children are not installed marks itself a streaming leaf and writes a request.
CPU installs in dependency order and rewrites flags and pointers; installed
chunks are live the next frame.

**Accept** by porting `verifyValidCut` from
`VirtualGeometryPageInstallsTests`: enabled set closed under parents, streaming
mask a subset of enabled, asserted after every install and uninstall.

Do not port `applyInstallUpdates` - it discards the page id and rebuilds the
whole hierarchy. At voxel node counts that is the full rewrite this work exists
to remove.

### Stage E - Traversal

Only after the DAG spans pages. Seed from roots, descend, emit at leaves, with
ping-pong queues and indirect dispatch as
`VirtualGeometryCullingMultipleDispatchesPass` does. It retires the passes that
walk all 1.6M nodes every frame.

Justify it on scaling, not on frame time: the flat cull is currently cheap.

---

## Part 5 - What to delete, and what not to

**Delete**, once Stage C lands: `forceLeafRaster`, the clipmap LOD byte in
`node.level`, the terrain clip-bounds packing in `instanceIndex`, and the split
error curve as a *second* LOD metric.

**Do not delete** `canonicalGridOriginScale.xyz`. It carries an explicit warning
that it never comes from a world-space float division, and recomputing it in the
shader reintroduces exactly that.

**Node diet, deferred.** Moving the per-page constants - `regionMappingId`,
`instanceIndex`, the level bits, the canonical scale - into the page table takes
the node from 96 to 80 bytes, about 26 MB across 1.6M nodes. It is real
simplification, and Stage C deletes several of those fields outright, so doing
it first would be work done twice. It also does not help the passes that walk
the hierarchy every frame: those touch `previouslyVisible`, four bytes per node,
which the diet does not shrink.

---

## Part 6 - Where the frame actually goes

Nothing above is a frame-time plan. `SceneScreenTrace` is 22-30% of GPU time and
the brick passes are single digits. The 62x is worth having for scaling,
streaming cost and memory, but a geometry win of that size does not move a frame
dominated by lighting. If frame time is the goal, that is the pass to open.
