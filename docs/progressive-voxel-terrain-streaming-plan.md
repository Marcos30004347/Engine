# Progressive voxel terrain streaming — implementation plan

Written 2026-08-21.

This plan replaces the generated-world spatial clipmap with Minecraft-style,
fixed-footprint terrain chunks whose virtual voxel hierarchies are refined on
demand. It applies to the planar generated-world path backed by
`MapExportBundle`. The packaged spherical-planet path remains unchanged until
the new path is proven.

---

## 1. Decision

The generated world will have exactly one spatial owner for each resident
terrain chunk.

- Every chunk has a fixed world-space footprint and a stable virtual root.
- A newly entering chunk begins with its coarsest admitted voxel hierarchy.
- GPU traversal computes projected error and requests finer hierarchy pages.
- CPU workers generate only the requested exterior terrain shell from the
  world maps. They do not fill hidden interior columns or a solid chunk volume.
- New nodes, bricks, materials and shadow data are uploaded before the root or
  parent pointer that exposes them is rewritten.
- The coarse parent remains the fallback until the refinement is committed.
- Distant chunks lose fine pages before their coarse root is evicted.
- Raster and Tree64 consume the same committed chunk generation.

There will be no overlapping spatial cascades, finer-owned rectangles, or
coarse terrain objects hidden behind fine terrain objects. “LOD 0” continues to
mean the finest representation; increasing LOD numbers mean coarser data.

This is a paging system, but it is not a clipmap: chunk footprints do not grow
with LOD and parent and child terrain objects do not compete for ownership.

---

## 2. Why this replaces the current system

The generated-world implementation in
`src/voxel/svraster/PlanetVoxelChunk.cpp` currently builds concentric spatial
rings. Each successive level doubles both voxel size and page footprint. The
coarse and fine pages overlap, and `finerBounds` plus
`kSparseVoxelBrickHasFinerTerrainClip` decide which representation owns a
sample.

That design creates three correctness surfaces:

1. raster ownership must clip the overlap exactly;
2. Tree64 must clip the same overlap independently;
3. a moving clipmap must retire old physical slots at the same publication
   boundary.

The observed walls changing or disappearing after camera-driven terrain
updates are consistent with ownership changing as the rings move. A fixed
chunk has no moving ownership boundary. Only its internal hierarchy changes.

The engine already has most of the required mechanisms:

- `PlanetVoxelChunkStrategy::ProgressiveLOD` provides fixed-footprint chunks,
  stable physical slots and coarse-to-fine GPU feedback for packaged terrain.
- `sparse_voxel_brick_cull.wgsl` already emits terrain page request bits and
  priorities from projected error.
- `SparseVoxelTerrainUploadPass` already uploads bounded changed ranges and
  supports a final publication record.
- the brick hierarchy already provides coarse parents that can remain visible
  while descendants are absent;
- the asynchronous sample coordinator already rejects stale terrain
  generations and keeps the previous complete terrain publication on failure.

The work is therefore a consolidation: make generated-world chunks use the
progressive contract, then replace fixed whole-slot rewrites with true virtual
descendant-page publication.

---

## 3. Scope and non-goals

### In scope

- Planar generated worlds loaded from `MapExportBundle`.
- A bounded camera-centred grid of fixed-footprint chunks.
- Coarse bootstrap generation, GPU-requested refinement and distance-driven
  coarsening/eviction.
- Fast surface-shell voxelization: top surfaces, exposed cliffs and the minimum
  closure needed for a watertight visible shell, with no hidden geology fill.
- Stable virtual hierarchy roots and versioned physical page allocation.
- Atomic raster, material and Tree64 publication.
- ImGui controls and diagnostics for the new policy.
- Removal of generated-world spatial clipmap ownership after qualification.

### Not in scope

- Changing the world generator or exported map schema.
- Generating terrain on the GPU. The GPU selects detail; deterministic CPU
  workers continue to generate it.
- Infinite voxel render distance. Generated worlds render only the bounded
  voxel chunk window; pixels beyond it resolve to the atmosphere/sky.
- Editing terrain or persisting player modifications.
- Replacing the packaged planet terrain streamer in the first migration.
- Replacing the brick renderer or Tree64 traversal algorithm.

---

## 4. Required invariants

These are correctness gates, not implementation suggestions.

### 4.1 Single ownership

For every active `WorldVoxelChunkAddress`, exactly one committed root is visible
to rasterization and exactly one matching instance is visible to Tree64. No
other chunk footprint overlaps it.

### 4.2 Stable world grid

Chunk bounds are integer multiples of `chunkEdgeMetres` from the exported map
origin. Every voxel address is expressed on the canonical finest grid. Adjacent
chunks sample shared boundary coordinates identically at every generated LOD.

### 4.3 Parent-before-child safety

A child pointer may be published only after all referenced node, occupancy,
material, palette, micro-surface and geometry-field data are resident. Until
then the parent remains the drawable fallback.

### 4.4 Unpublish-before-recycle safety

Eviction first removes the child/root reference and advances the chunk
generation. Physical pages are returned to the allocator only after every
frame that could have read the old generation has completed.

### 4.5 One terrain generation

Raster roots, material-field mappings and Tree64 instances for a chunk carry
the same generation. A mismatched Tree64 instance must be ignored rather than
allowed to cast a stale shadow.

### 4.6 Bounded work

CPU generation, GPU upload bytes, root rewrites and physical-page retirement
all have explicit per-frame or per-publication budgets. A camera teleport may
degrade to coarse parents but may not stall rendering to build the complete
target cut.

### 4.7 Determinism

`(world bundle signature, chunk address, requested LOD, generator revision)`
uniquely determines chunk payload bytes. Scheduling order must not affect the
result.

### 4.8 Surface-only occupancy

Terrain generation emits the exterior occupancy needed by raster and shadow
queries, not a filled underground volume. The shell includes the sampled top
surface, exposed side cells for cliffs and discontinuities, and enough
downward/neighbor closure that rays cannot enter through a visible opening.
Interior cells that cannot contribute a visible face or first shadow hit are
absent. Raster and Tree64 derive from this same shell occupancy; Tree64 must not
silently reconstruct a solid volume from height columns.

---

## 5. Target data model

Add a generated-world progressive streamer alongside the legacy clipmap types
while migration is active.

```cpp
struct WorldVoxelChunkAddress
{
  int32_t x = 0;
  int32_t z = 0;
};

struct WorldVoxelChunkRequest
{
  uint32_t slot = 0xffffffffu;
  uint32_t expectedGeneration = 0u;
  uint32_t currentLOD = 0u;
  uint32_t desiredLOD = 0u;
  uint32_t priority = 0u;
};

enum class WorldVoxelChunkState : uint8_t
{
  Empty,
  CoarseQueued,
  CoarseResident,
  RefinementQueued,
  RefinementBuilding,
  PublicationPending,
  Resident,
  EvictPending,
};

struct WorldVoxelChunkEntry
{
  WorldVoxelChunkAddress address{};
  uint32_t slot = 0xffffffffu;
  uint32_t generation = 0u;
  uint32_t residentLOD = 0u;
  uint32_t requestedLOD = 0u;
  WorldVoxelChunkState state = WorldVoxelChunkState::Empty;
  // Stable virtual root and owned physical hierarchy pages.
};

struct WorldVoxelChunkStreamingSettings
{
  double chunkEdgeMetres = 0.0;
  double finestVoxelSizeMetres = 0.0;
  uint32_t finestHorizontalVoxelCount = 0u;
  uint32_t residentRadius = 0u;
  uint32_t lodCount = 1u;
  float splitErrorPixels = 4.0f;
  float mergeErrorPixels = 2.5f;
  uint32_t maximumResidentChunks = 0u;
  uint32_t maximumGeneratedChunksPerUpdate = 0u;
  uint64_t maximumUploadBytesPerFrame = 0u;
};
```

The split threshold must be greater than the merge threshold. That hysteresis
prevents a stationary camera near a boundary from repeatedly generating and
evicting the same level.

The first implementation may retain one fixed-capacity physical arena per
chunk slot, as packaged `ProgressiveLOD` does today. The final representation
must separate stable virtual roots from pooled physical descendant pages so a
refinement adds pages instead of requiring maximum fine capacity for every
distant chunk.

---

## 6. Runtime flow

### 6.1 Chunk entry

1. Convert the camera position to a canonical chunk address.
2. Compute the bounded square of desired chunk addresses.
3. Retain matching entries and stable slots.
4. Assign leaving slots to entering addresses only after generation-safe
   retirement.
5. Queue entering chunks at their coarsest admitted LOD.
6. Publish each coarse root independently as it becomes ready.

The renderer does not wait for the entire camera window. Missing chunks outside
the currently committed voxel cut remain uncovered until their voxel pages are
published.

### 6.2 GPU refinement feedback

For each visible chunk hierarchy, traversal computes projected error from the
node’s world extent, distance and output projection.

- If the current hierarchy can refine locally and children are resident,
  traversal descends normally.
- If error exceeds `splitErrorPixels` and the required descendants are absent,
  the shader emits a request for the next finer admitted LOD.
- Requests are keyed by `(slot, expectedGeneration)` and atomically retain the
  maximum priority.
- The first implementation requests one complete next-finer chunk hierarchy.
  A later phase requests individual virtual node pages.

Feedback is read one or more frames later without stalling. The CPU discards a
request if its expected generation no longer matches the slot.

### 6.3 CPU scheduling

The coordinator coalesces requests per chunk, keeps the finest requested LOD,
and sorts by:

1. visible projected error;
2. camera distance;
3. age, so continuously visible requests cannot starve;
4. stable chunk address for deterministic ties.

One chunk advances by at most one LOD per generation job initially. This keeps
latency bounded and ensures every intermediate representation is usable.

### 6.4 Generation

`buildWorldMapVoxelTerrain` gains an explicit internal LOD while retaining the
same chunk bounds. At LOD `n`, horizontal resolution is the finest resolution
divided by `2^n`; the result must remain brick aligned.

All LODs sample the same continuous world fields and canonical chunk corners.
Coarse occupancy must conservatively contain the surface represented by finer
children. Coarse data must never be generated by sampling a different spatial
page footprint.

Generation is surface-driven. For each horizontal sample, determine the
surface cell and the exposed vertical interval required by lower neighbours.
Emit only:

- the top surface cell or surface micro-patch;
- side-shell cells whose face can be exposed by a lower adjacent column;
- a small configurable closure depth below the lowest exposed face;
- boundary halo data needed to make the same decision as the neighbouring
  chunk.

Do not emit cells between that shell and an arbitrary foundation plane. The
existing `surfaceShellDepthVoxels` behavior is the upper bound to reduce, not a
reason to preserve hidden interior voxels. Chunk-generation metrics must
separate map sampling, shell extraction and brick/Tree64 construction so the
cost of each requested LOD is visible.

### 6.5 Publication

Publication order is:

1. allocate destination physical pages or a scratch slot;
2. upload nodes, child indices, occupancy and roots;
3. upload materials, palette, micro-surface and geometry-field data;
4. prepare the Tree64 model/instance for the same chunk generation;
5. issue one final root/page-table rewrite exposing the new hierarchy;
6. publish the matching Tree64 instance and terrain generation in that same
   graph transaction;
7. retire the previous pages after their last reader fence.

No intermediate frame may see a new root with old payload data, or new shadow
data with an old raster root.

### 6.6 Coarsening and eviction

When error remains below `mergeErrorPixels` for a configurable grace period,
fine descendants become eviction candidates. The coarse ancestor is confirmed
resident, the child reference is removed, the generation advances, and the
child pages are retired after fences.

When a chunk leaves the resident radius, all fine descendants are removed
first. Its coarse root may remain in a short LRU cache, but its active world
slot is unpublished before that slot can be assigned to another address.

### 6.7 Camera movement and Tree64

A camera move that changes the resident chunk window is a Tree64 update even
when no chunk changes internal LOD. Retained chunks keep their model payloads,
but entering and leaving chunk instances change directory membership and the
slot reused for an entering address receives a new generation.

Movement publication must therefore stage together:

1. raster root removal for leaving addresses;
2. coarse raster root installation for ready entering addresses;
3. Tree64 instance/reference removal for leaving addresses;
4. Tree64 instance/reference installation for the exact entering address,
   transform and generation;
5. the new committed resident-window generation.

Tree64 directory scrolling may reuse unchanged model nodes, but it must rebuild
or patch memberships and references for the new camera-relative directory.
Until an entering coarse chunk is ready, neither raster nor Tree64 may expose
that address; it remains uncovered rather than using another terrain
representation.

---

## 7. Implementation phases

Each phase must ship with its tests and diagnostics. Do not delete the legacy
clipmap until phase 8 passes.

### Phase 0 — capture the current failure and add ownership diagnostics

**Status (2026-08-21): implemented and captured.** The deterministic
`--test-terrain-ownership` move/return run against `world-data` recorded 89
raster pages, 356 raster roots and 89 matching Tree64 instances at generations
1, 2 and 3, with zero ownership mismatches at every publication boundary.
Debug modes 29–33 expose owner, boundary, LOD, physical slot and publication
generation respectively.

**Work**

- Add chunk address, spatial LOD, slot and generation to raster and Tree64
  diagnostic readback.
- Add debug views for chunk boundaries, active owner, resident LOD and slot.
- Capture a deterministic camera path where a wall disappears after an update.
- Record active raster roots and Tree64 instances before and after the update.

**Files**

- `samples/sparseVoxelRenderer/SparseVoxelRendererSample.cpp`
- `assets/shaders/voxel/svraster/wgsl/sparse_voxel_brick_cull.wgsl`
- Tree64 diagnostic shaders and CPU readback structures

**Done when**

- The wall capture is reproducible.
- Every visible artifact can be associated with a chunk/slot/generation.
- The diagnostic adds no state mutation to the old path.

### Phase 1 — generated-world fixed-footprint progressive cache

**Status (2026-08-21): implemented.** Generated worlds now default to a
bounded fixed-footprint cache. Each resident address has one toroidally stable
physical slot and begins at the coarsest brick-aligned internal LOD. A one-cell
placement regression retains six of nine radius-one chunks and replaces only
the entering row. Nodes carry their stable refinement slot but no finer-region
clip rectangle or force-leaf spatial-cascade flag. Duplicate refinement
feedback is coalesced by `(slot, expectedGeneration)` and stale generations are
discarded. Phase 9 removed the temporary legacy A/B path and its switches.

**Work**

- Add `WorldVoxelChunkStreamingSettings`, address, entry and cache types.
- Build a bounded toroidal grid using one fixed `chunkEdgeMetres`.
- Port slot retention and request coalescing from
  `PlanetVoxelChunkStrategy::ProgressiveLOD`.
- Build all entering chunks at the coarsest admitted internal LOD.
- Aggregate the resulting chunks without `finerBounds` or finer-terrain flags.
- Keep the old world clipmap selectable for A/B comparison.

**Files**

- `src/voxel/svraster/PlanetVoxelChunk.hpp`
- `src/voxel/svraster/PlanetVoxelChunk.cpp`
- `tests/procedural/MapPlacementTests.cpp`

**Done when**

- A stationary coarse grid renders with one owner per chunk.
- Moving one chunk cell retains all shared slots and replaces only one entering
  row.
- No generated node carries spatial clip ownership metadata.

### Phase 2 — deterministic per-chunk internal LOD generation

**Status (2026-08-21): implemented.** `buildWorldMapVoxelTerrain` now accepts
an explicit internal LOD, canonical finest-grid resolution/voxel size, shared
elevation-anchor LOD, generator revision and exterior-shell policy. Fixed
chunks generate top cells, exposed cliff columns and only the configured
closure beneath otherwise hidden faces. Payload revisions hash the source key
and emitted hierarchy bytes. The progressive cache has an optional bounded CPU
payload LRU keyed by bundle, chunk address, LOD and generator revision, and
reports separate sampling, shell-extraction and hierarchy-build timings.

The placement regressions build every admitted LOD twice and compare payload
bytes, verify shared halo samples for adjacent chunks, stable world bounds and
elevation origin across LODs, conservative coarse coverage, bounded hidden
closure depth, elevation-independent shell cost, and zero-regeneration restore
of an evicted row.

**Work**

- Make world-map terrain generation accept a fixed footprint plus requested
  internal LOD.
- Replace hidden column/foundation filling with exterior-shell extraction.
- Use the halo to emit only top cells, exposed cliff cells and bounded closure
  cells required for a watertight visible surface.
- Validate every admitted resolution is brick aligned.
- Preserve canonical boundary samples, elevation origin translation and
  material lookup across levels.
- Give every generated payload a deterministic content revision.
- Add an optional CPU cache keyed by bundle, address, LOD and generator
  revision.

**Done when**

- Repeated builds of every LOD are byte-identical.
- Adjacent chunks share bit-identical boundary heights.
- A coarse hierarchy conservatively contains its finer hierarchy.
- Changing LOD never changes chunk world bounds or root transform.
- No generated leaf lies below the configured visible/closure shell band.
- Per-chunk timings demonstrate that generation cost follows surface area and
  exposed relief, not terrain volume or elevation above a foundation plane.

### Phase 3 — GPU desired-LOD requests

**Status:** Implemented.

**Work**

- Generalize the existing terrain request buffers from bit/priority to include
  expected slot generation and desired LOD.
- Emit requests only for progressive generated-world chunks.
- Use output resolution and projected node extent for the error calculation.
- Add split/merge hysteresis and a minimum residency age.
- Coalesce multiple node requests into one next-finer chunk request for the
  first implementation.

**Files**

- `assets/shaders/voxel/svraster/wgsl/sparse_voxel_brick_common.wgsl`
- `assets/shaders/voxel/svraster/wgsl/sparse_voxel_brick_cull.wgsl`
- `src/voxel/svraster/rendering/SparseVoxelBrickPasses.hpp`
- `samples/sparseVoxelRenderer/SparseVoxelRendererSample.cpp`

**Done when**

- A chunk above the error threshold requests exactly its next admitted LOD.
- A satisfied chunk emits no request.
- A recycled slot’s stale request is rejected by generation.
- A stationary camera does not oscillate between two LODs.

### Phase 4 — asynchronous refinement and whole-slot atomic publication

**Status (2026-08-21): implemented.** Generated-world feedback now enters the
existing asynchronous terrain worker one request at a time. The worker builds
only the requested next-finer exterior shell, retains the previous owner as a
rollback record, and prepares the existing brick-page, material-field and
Tree64 transaction. Coarse admission reserves a fixed finest-level slot for
this transitional implementation, so a refinement changes one page without
relocating unrelated owners. Camera-centre/generation mismatches, cancellation,
fixed-slot growth, GPU memory rejection and upload-packet rejection restore the
previous complete cache. The real GPU packet builder enforces the configured
per-frame byte ceiling after Tree64 and field data are appended.

**Work**

- Feed generated-world GPU requests into the existing terrain worker queue.
- Generate one requested chunk without rebuilding unrelated chunks.
- Initially use a scratch/fixed slot and publish the complete replacement at a
  terrain transaction boundary.
- Reuse `prepareTerrainBrickUploadPlan`, material-field publication and
  `SparseVoxelTerrainUploadPass`.
- Retain the previous hierarchy on cancellation, generation mismatch, budget
  failure or upload failure.

**Done when**

- Approaching one chunk refines only that chunk.
- Raster never observes a partially rewritten hierarchy.
- Teleporting while generation is active discards the stale result.
- Refinement cost is bounded by generation and upload budgets.

### Phase 5 — true virtual descendant pages

**Status (2026-08-21): in progress.** Generated-world chunks allocate
node/occupancy, child, root, material and palette ranges from a fixed physical
arena. Replacement payloads are written while unreferenced; raster state,
Tree64 descriptors and material state share the final publication packet.
Every changed stable slot now emits one explicit compare-and-swap rewrite with
its expected parent and replacement page identity (generation, LOD, raster
allocation and Tree64 geometry revision). The render thread validates both
sides against the committed raster and Tree64 state before admitting uploads.

The remaining storage step is to split a finer chunk build into independently
owned descendant ranges and keep its coarse parent physically resident.
Currently one internal-LOD request still builds a complete chunk payload and
retires the preceding allocation after publication. That is safe and bounded,
but it is not yet the memory scaling promised by this phase.

**Work**

- Introduce a physical page allocator for node, child, occupancy, material and
  palette ranges.
- Keep one stable virtual root descriptor per chunk slot.
- Generate new descendants into unreferenced pages.
- Publish parent-child pointer rewrites last.
- Track page ownership and last-reader frame for deferred reclamation.
- Permit independent branch requests once whole-chunk refinement is stable.

**Done when**

- Distant chunks consume only coarse-page memory.
- Refinement allocation grows in proportion to requested detail rather than
  maximum fine capacity times resident chunk count.
- Forced allocation failure retains the parent without a hole.
- Randomized refine/evict/recycle tests find no dangling page reference.

### Phase 6 — Tree64 generation-locked publication

**Status (2026-08-21): implemented.** Every progressive terrain instance now
carries its stable raster slot, residency generation and region mapping. Tree64
models occupy fixed per-slot node pages, so retained geometry keeps its model
payload while movement rebuilds current transforms, directory memberships and
references. Raster roots, material fields, terrain page ownership and Tree64
updates are emitted by one final upload transaction. The trace validates the
instance owner against that atomically published page state and returns unknown
on mismatch, preventing stale shadow-only hits. Replaced model slots become
visible only in the final queue-ordered publication, sharing the raster page
reader fence.

**Work**

- Build or update Tree64 per refined chunk from the exact pending hierarchy.
- Add chunk slot and generation to Tree64 terrain instances.
- Publish raster root, material state and Tree64 instance together.
- On every resident-window move, remove leaving Tree64 instances and install
  entering coarse instances in the same publication as raster root changes.
- Reuse retained Tree64 models while updating directory memberships and
  references for the camera-relative window.
- Ignore or conservatively fall back from any generation mismatch.
- Retire old Tree64 model pages under the same fence policy as raster pages.

**Done when**

- A newly refined shape cannot cast a shadow before it can rasterize.
- An evicted shape cannot remain as a shadow-only caster.
- Shadow hit-cell diagnostics always identify an active raster chunk
  generation.
- Moving the camera without changing any internal chunk LOD still produces the
  correct Tree64 instance/reference update.

### Phase 7 — movement, coarsening and memory policy

**Status (2026-08-21): implemented.** The fixed toroidal resident window
spawns entering chunks at the coarse admitted LOD before processing any detail
request and removes every leaving owner from the next raster/Tree64
transaction. GPU feedback now records each active page's maximum visible
output-space error, not only split events. Generation-checked CPU policy keeps
a fine-use epoch per slot, applies the split/merge dead band and residency/grace
intervals, and replaces least-recently-useful fine owners with one complete
parent LOD. CPU payload-cache bytes, immutable GPU brick-page bytes and Tree64
bytes have independent limits and diagnostics. The bounded payload LRU retains
recent spatial evictions for backtracking without weakening slot-generation
checks.

**Work**

- Add camera-centred spawn/despawn scheduling.
- Add merge hysteresis, grace frames and fine-page LRU eviction.
- Prioritize coarse entering chunks over refinement so movement cannot expose
  empty terrain near the camera.
- Define CPU cache, GPU page and Tree64 budgets separately.
- Measure chunk edge, radius and LOD count rather than baking guessed defaults.

**Done when**

- Continuous movement for ten resident-window widths has bounded memory.
- A back-and-forth path does not regenerate retained chunks.
- Fast travel shows coarse fallback/background, never stale old-location
  geometry.
- Returning to a recently visited area benefits from the CPU cache without
  violating generation checks.
- Every committed resident-window address has matching raster and Tree64
  ownership, and no leaving address survives in either representation.

### Phase 8 — UI, qualification and default switch

**Implementation (Phase 8):** the generated-world default became progressive
fixed chunks while the legacy path was retained temporarily for qualification.
The terrain panel now reports chunk footprint, resident radius/count, admitted
and actual resident LOD ranges, CPU/GPU/Tree64 memory, and exposes split/merge,
generation/upload budgets, scheduler/request freezes, publication freeze, and
direct LOD/slot/generation ownership views. Publication freeze holds a complete
CPU transaction before GPU staging; an already staged upload is allowed to
finish its atomic raster/Tree64 commit. The temporary A/B benchmark script was
removed with the legacy implementation in Phase 9. Benchmark JSON still
records the active terrain streaming mode.

**Work**

- Replace generated-world clipmap controls with:
  - progressive streaming enable;
  - chunk edge and resident radius diagnostics;
  - split and merge error pixels;
  - finest and coarsest admitted LOD;
  - CPU generation and GPU upload budgets;
  - freeze requests/publication;
  - colour by chunk LOD, slot and generation.
- Add side-by-side benchmark presets for legacy clipmap and progressive chunks.
- Make progressive chunks the generated-world default after qualification.

**Done when**

- The captured wall path remains clean through startup, movement, refinement,
  coarsening and return travel.
- Progressive mode meets the correctness and performance gates below.

### Phase 9 — remove generated-world spatial clipmaps

**Status (2026-08-21): implemented.** Generated-world startup, movement,
refinement, rollback and publication now have one fixed-chunk scheduler. The
legacy page-address/cache/settings API, CPU generator, CLI switches, UI
controls, tests and A/B script are removed. Projection-radius calculation is
retained under chunk-streaming terminology. The packed finer-owner encoding is
still used by packaged planets that explicitly select `ClipmapRings`; generated
worlds no longer produce that metadata.

Generated-world startup is now a bounded partial cut rather than an eager full
window. A retained priority worker pool continuously prepares the next nearby,
camera-visible, or background batch and signals completion through a callback;
only the render thread stages rewrites. Each partial cut includes matching
raster sidecars and Tree64 pages, commits atomically, and immediately permits
the next batch until the resident window is full. Rejected batches roll back
without skipping frame progression.

**Work**

- Remove `WorldMapVoxelClipmapSettings`, spatial-LOD page addresses,
  `finerBounds`, ownership shader branches and generated-world clipmap UI.
- Retain shared projection-error helpers only where still used.
- Rename generated-world cache/logging from clipmap terminology to chunk
  streaming terminology.
- Update documentation and command-line help.

The packaged planet `ClipmapRings` compatibility strategy is a separate path
and is not deleted by this phase.

---

## 8. Tests and qualification gates

### CPU unit tests

- fixed address maps to fixed world bounds at every LOD;
- adjacent chunks share exact canonical boundary samples;
- all admitted LOD resolutions are brick aligned;
- coarse occupancy conservatively contains fine occupancy;
- surface-shell extraction emits no leaves below its visible/closure band;
- tall flat terrain costs approximately the same to voxelize as low flat
  terrain with the same exposed surface area;
- same input produces the same payload hash across three runs;
- slot retention across a one-cell camera move;
- stale request rejection after slot reuse;
- cancellation leaves the committed cache unchanged;
- page allocator randomized refine/evict/recycle invariant test;
- LRU and budget behavior is deterministic.

Primary locations:

- `tests/procedural/MapPlacementTests.cpp`
- `tests/voxel/SparseVoxelPlanetChunkTests.cpp`
- `tests/voxel/SparseVoxelTerrainTree64Tests.cpp`

### GPU reference tests

- projected error emits the expected desired LOD;
- satisfied error emits no request;
- maximum priority wins for duplicate requests;
- expected generation survives readback and stale generations are rejected;
- root rewrite cannot become visible before payload writes;
- Tree64 generation mismatch produces no stale hit.
- a movement-only window update removes leaving Tree64 references and installs
  entering references without rebuilding retained model payloads.

### Integration tests

- stationary startup followed by a no-op terrain publication produces the same
  geometry and shadow signature;
- walk across chunk boundaries at oblique and grazing camera angles;
- refine and coarsen repeatedly under a moving directional light;
- teleport with generation and upload jobs in flight;
- three-generation streaming smoke test;
- memory-budget exhaustion retains coarse fallback;
- long-duration movement has stable resident page counts and no unbounded
  retired-page queue;
- compare raster occupancy and Tree64 material-cell ownership for every active
  chunk generation.
- sweep the camera through repeated slot reuse while holding internal LOD fixed
  to isolate movement publication from refinement.

### Visual gates

- no freestanding page-edge walls;
- no geometry visible from only one side unless it is a valid terrain face;
- no shadow in a region without an active raster owner;
- no holes during refinement or eviction;
- no persistent chunk-boundary line in albedo, normal, depth or shadow views;
- expected block-scale LOD stepping is allowed, but it must be attached to the
  terrain surface and move only when that chunk’s committed LOD changes.

### Performance gates

Phase 0 records the baseline before fixed numbers are chosen. Qualification
then requires:

- no render-thread wait for CPU terrain generation;
- bounded upload bytes per frame;
- p95 publication CPU time no worse than the current terrain transaction;
- GPU cull/request overhead reported separately and below its agreed budget;
- resident memory stays within configured brick, material and Tree64 budgets;
- no full render-graph rebuild during normal movement or refinement.

---

## 9. Metrics and ImGui diagnostics

The runtime panel should report:

- desired, coarse-resident and refining chunk counts;
- resident chunks by LOD;
- queued requests by LOD and oldest request age;
- generated chunks and milliseconds per update;
- sampled columns, emitted shell leaves, rejected interior cells and shell
  extraction milliseconds;
- uploaded bytes, page count and publication batches;
- active, free and retired physical hierarchy pages;
- stale requests and stale completed jobs discarded;
- raster/Tree64 generation mismatches;
- Tree64 movement updates, reused models, installed/removed instances and
  rewritten directory references;
- CPU cache hits, misses and evictions;
- current chunk address and resident window bounds.

Debug overlays must be usable without lighting so ownership can be inspected
independently of shadows.

---

## 10. Risks and explicit mitigations

### Too many fixed chunks

A fixed-footprint grid scales with visible area. Before selecting defaults,
measure chunk edges of 32, 64, 128 and 256 metres against the desired voxel
render radius. If instance/root overhead is excessive, increase chunk footprint
or reduce the voxel render distance; do not reintroduce overlapping spatial
cascades or a mesh fallback.

### Boundary steps between different chunk LODs

Use one canonical finest grid and shared boundary samples. Limit neighbouring
requested LOD difference to one during the first implementation. If a visible
step remains, request a narrow boundary refinement from the coarser chunk; do
not render both whole chunks over the same footprint.

### Refinement oscillation

Use split/merge hysteresis, minimum residency age and request aging. Expose
counters for suppressed split and merge decisions.

### Partial upload visibility

Payload-first and pointer-last publication is mandatory. Include a fault test
that aborts after every upload stage and verifies the old parent remains valid.

### Physical-page fragmentation

Use fixed-size pages per buffer class and deferred free lists, not arbitrary
suballocations. Compact only offline or at an explicitly drained maintenance
boundary.

### Tree64 rebuilding dominates refinement

Build per chunk and reuse unchanged models. If Tree64 still dominates, retain a
coarser shadow LOD for that same chunk generation; never retain a model for a
different spatial owner or generation.

---

## 11. Rollout order

Phases 1–8 kept both implementations behind a generated-world streaming
strategy switch:

```text
--world-terrain-streaming clipmap       # compatibility only
--world-terrain-streaming progressive   # new implementation
```

Progressive became the default after the captured artifact path, movement
stress test, Tree64 generation test and memory qualification passed. Phase 9
then deleted the legacy generated-world clipmap and its strategy switch. The
packaged-planet `ClipmapRings` strategy is outside this migration and remains
available.

---

## 12. Definition of done

The system is complete when a generated world can start coarse, refine visible
chunks to the requested pixel error, move and recycle its bounded chunk window,
coarsen under memory pressure, and keep raster and Tree64 synchronized without
overlapping spatial terrain owners.

The original wall/shadow capture must stay clean for the entire sequence, not
only after the first camera update.
