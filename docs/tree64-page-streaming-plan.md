# Asynchronous Tree64 Page Streaming Plan

**Status:** implementation complete through Phase 9, 2026-08-21

**Post-qualification amendment:** generated-world residency now uses a
non-overlapping spatial-LOD quadtree leaf cut for long render distances rather
than a full window of finest fixed-footprint owners. Tree64 pages retain stable
physical slots and immutable publication semantics; raster and Tree64 consume
the same leaf set, and a parent-to-children ownership change publishes only
after the complete replacement cut is ready. Zero-filled absent stable slots
trace as empty space, while genuinely stale nonzero generations remain invalid.

## 1. Objective

Replace generated-terrain Tree64 whole-snapshot rebuilds with immutable,
independently streamed Tree64 pages. A fixed-footprint terrain chunk keeps one
stable logical owner while its current Tree64 page is replaced as its internal
LOD changes.

Tree64 generation, staging and upload must happen asynchronously. The renderer
continues tracing the last complete Tree64 page until the replacement node
payload and its matching raster chunk are ready. Publication is one atomic
descriptor rewrite; the old page is reclaimed only after every frame that may
still read it has completed.

The target transaction is:

```text
committed chunk descriptor -> old Tree64 page

worker builds replacement page
        |
        v
upload replacement into an unreachable physical slot
        |
        v
publish raster generation + Tree64 descriptor together
        |
        v
committed chunk descriptor -> new Tree64 page
        |
        v
retire old page after the reader completion fence
```

## 2. Scope

This migration initially applies to generated-world terrain using progressive
fixed chunks.

It includes:

- one independently addressable Tree64 BLAS page per resident terrain chunk;
- stable logical chunk descriptors and replaceable physical node pages;
- per-chunk CPU Tree64 generation from the same committed voxel geometry;
- asynchronous worker generation and bounded multi-frame GPU upload;
- generation-checked atomic raster/Tree64 publication;
- deferred node-page and material-page reclamation;
- incremental membership/reference updates for entering and leaving chunks;
- rollback, cancellation, backpressure and memory budgets;
- CPU, GPU and renderer qualification tests;
- removal of the generated-terrain whole-snapshot/diff path after qualification.

It does not initially include:

- modifying nodes that are currently reachable by a trace;
- child pointers that cross Tree64 page boundaries;
- partial subtree refinement inside one terrain chunk;
- replacing the ordinary dynamic-object Tree64 snapshot path;
- changing packaged-planet `ClipmapRings` ownership;
- changing Tree64's conservative occupancy semantics.

## 3. Current implementation

The code already contains most of the required primitives:

- `SparseVoxelTree64ChunkGPU` identifies a physical `nodeSlot`, root offset,
  node count and content generation;
- `SparseVoxelTree64Directory` reserves, commits, retires and reclaims node
  slots without reusing a slot before its completion token;
- `sparse_voxel_tree64_publish.wgsl` validates build results and publishes a
  directory descriptor on the GPU;
- Tree64 trace shaders derive the physical node base from `nodeSlot` and keep
  child indices local to that slot;
- `SparseVoxelTree64DynamicSettings::modelNodeSlotCapacity` gives terrain
  models fixed physical node ranges;
- the terrain upload pass can write changed Tree64 ranges in the same final
  transaction as raster page state.

Terrain now keeps stable logical model/instance slots and fixed reference
slices for the generated-world directory. Internal LOD replacement leaves the
directory bytes untouched, while toroidal movement rewrites only entering
physical slots and affected stable instances. Retirement now follows completed
render-graph timeline values, and rejected transactions atomically roll back
their unpublished pages while retaining the previous raster/Tree64 owner. The
generated-terrain runtime now exposes the page-streaming path exclusively;
legacy whole-arena upload entry points and fallback callers have been removed.

## 4. Required invariants

The refactor must preserve these invariants at every frame boundary.

### 4.1 Immutable reachable pages

A node page referenced by a committed descriptor is read-only. Workers and
upload passes write only reserved pages that no committed descriptor can reach.

### 4.2 Raster and Tree64 generation agreement

For generated terrain, the following identity is indivisible:

```text
(chunk address, logical slot, residency generation, internal LOD,
 geometry revision, Tree64 page generation)
```

The raster page-state table and Tree64 descriptor must either both describe
this identity or both retain the previous identity. A mismatch fails closed
and must never trace a page belonging to a previous toroidal owner.

### 4.3 Descriptor publication is the only visibility transition

Uploading nodes does not make a page visible. A page becomes visible only when
the final publication pass rewrites its logical chunk descriptor.

### 4.4 Deferred reuse

The old page cannot return to the free list until the GPU completion token is
newer than every frame that could have read the old descriptor.

### 4.5 Fixed ownership during internal LOD changes

Refinement and coarsening do not change chunk world bounds, instance identity,
or directory membership. They replace only the Tree64 page descriptor and the
matching raster payload.

### 4.6 Retain-complete failure policy

Allocation failure, build overflow, cancellation, stale generation, upload
failure, or publication rejection retains the previous complete raster and
Tree64 pages. An already resident chunk must not be published as unknown merely
because its requested refinement failed.

## 5. Target data model

### 5.1 Logical descriptor

Add a terrain-specific descriptor table indexed by stable terrain slot. It may
reuse or evolve the existing dynamic-model record, but it must expose this
logical contract to CPU and GPU code:

```cpp
struct SparseVoxelTerrainTree64DescriptorGPU
{
  uint32_t terrainSlot;
  uint32_t terrainGeneration;
  uint32_t internalLOD;
  uint32_t flags;

  uint32_t nodePageSlot;
  uint32_t nodePageGeneration;
  uint32_t rootNodeOffset;
  uint32_t nodeCount;

  uint32_t materialPageSlot;
  uint32_t materialPageGeneration;
  uint32_t materialCellOffset;
  uint32_t materialCellCount;

  // Existing local bounds/cell size or a stable index to them.
};
```

The exact packing can change during Phase 1, but all publication-critical
fields must fit in one small descriptor rewrite. The trace validates both the
terrain generation and physical page generation before reading nodes.

### 5.2 Physical Tree64 page

Each physical page contains:

- immutable `SparseVoxelTree64NodeGPU` nodes;
- the padded-node representation where required by a specialized trace path;
- root offset and used node count;
- page generation and allocation state;
- payload revision and internal LOD for diagnostics;
- optional per-page material-cell data or a stable material-page reference.

Children remain page-local. `childBaseAndFlags >> 1` is an offset within the
descriptor's page, as it is today. Whole-chunk replacement therefore requires
no child-pointer ABI change.

### 5.3 Allocator

Introduce `SparseVoxelTree64PageAllocator` or generalize
`SparseVoxelTree64Directory` so terrain can manage:

- free pages;
- pages reserved by CPU jobs;
- pages ready for upload;
- pages uploaded but not published;
- committed pages;
- pages retired behind completion tokens.

The first implementation should use one fixed node capacity per page, matching
the current terrain model-slot capacity. This bounds addressing and preserves
the existing trace calculation. Size classes or multi-page BLAS storage are a
later optimization, not a prerequisite.

Capacity must cover:

```text
resident pages
+ maximum concurrently generated/uploading pages
+ pages retired for maxFramesInFlight
+ a bounded emergency replacement reserve
```

### 5.4 Page identity

A physical page handle is `(pageSlot, pageGeneration)`. Incrementing the page
generation on reuse prevents a delayed worker result or stale descriptor from
referencing a new occupant of the same physical slot.

## 6. Asynchronous lifecycle

Each chunk replacement moves through explicit states:

```text
Requested
  -> Generating
  -> Generated
  -> UploadQueued
  -> Uploading
  -> UploadedUnpublished
  -> PublishQueued
  -> Committed
  -> OldPageRetiring
  -> OldPageFree
```

Failure from any state before `Committed` releases or retires only the pending
page and restores the committed chunk unchanged.

### 6.1 Request admission

The existing projected-error feedback remains the source of refinement and
coarsening requests. Admission must:

- coalesce by logical terrain slot and expected terrain generation;
- keep only the newest requested LOD for an owner;
- prioritize camera-near/high-error chunks;
- reject requests for a reused toroidal slot;
- avoid launching a second replacement while a newer one is already pending;
- obey CPU generation, pending-page and GPU upload budgets.

### 6.2 Worker generation

Refactor `buildTerrainTree64Payload` into a retained full-build wrapper plus a
new per-chunk function such as:

```cpp
bool buildTerrainTree64Page(
    const PlanetVoxelChunkRecord &record,
    const SparseVoxelBrickHierarchy &bricks,
    uint32_t terrainGeneration,
    uint32_t internalLOD,
    SparseVoxelTree64PagePayload &page,
    ...);
```

The worker consumes the same complete chunk payload that will be rasterized.
It must not read mutable renderer-owned arrays. Inputs are immutable snapshots
or owned job data, and output contains no GPU pointers.

Workers run through the existing terrain asynchronous job infrastructure at
first. Once multiple concurrent jobs are needed, replace the single future with
a bounded worker queue and completion queue. Cancellation is cooperative;
completed stale jobs are still rejected by generation checks.

### 6.3 Upload

The render thread drains generated pages into bounded staging packets. Node and
material payloads may upload over multiple frames because their physical page
is unreachable.

Each upload records:

- terrain slot and expected terrain generation;
- physical page slot and page generation;
- uploaded byte range;
- total byte count;
- transaction ID;
- completion token.

A page reaches `UploadedUnpublished` only after every byte range is submitted
and the upload completion is visible to the publication pass. Transfer/write
to shader/read ordering must be explicit in the render graph.

### 6.4 Publication

The final publication record contains the new raster page state and Tree64
descriptor. The GPU pass validates:

- logical terrain slot is in range;
- current terrain generation equals the expected old generation;
- replacement terrain generation is the staged raster generation;
- node page slot and page generation match the uploaded page metadata;
- root offset and node count fit the physical page;
- internal LOD matches the raster page;
- material page, if used, is complete.

Only after validation does it replace the logical descriptor. The CPU commits
the corresponding cache transaction after GPU completion, then retires the old
node and material pages with that completion token.

### 6.5 Camera movement

When the camera moves by one chunk:

- retained logical owners keep their raster and Tree64 descriptors byte-identical;
- leaving owners remain valid until their replacement transaction publishes;
- entering owners build coarse raster and Tree64 pages asynchronously;
- only affected old/new membership ranges and instance records are rewritten;
- toroidal slot reuse increments both terrain and Tree64 generations;
- stale entering/refinement jobs are discarded without touching the new owner.

Fast movement or teleport may invalidate many pending jobs. Cancellation should
stop unnecessary CPU work, but correctness depends on generation rejection,
not on cancellation timing.

## 7. Child-pointer policy

The initial implementation rewrites the descriptor's root page pointer, not
children inside a live tree. This is the safe and sufficient operation for a
whole-chunk LOD replacement.

If later profiling justifies partial subtree paging, add it as a separate
extension. Cross-page children cannot use the current local
`childBaseAndFlags` contract without one of these ABIs:

- a tagged child base containing page ID and local offset; or
- an immutable link-table entry that contains page slot, page generation and
  local child base.

In either design, build child pages while unreachable and publish one versioned
link record. Do not patch a reachable parent node's masks and child pointer in
separate writes.

## 8. Material sidecar

The current terrain Tree64 material-cell hash is assembled as a global table.
That can remain a qualification fallback, but it would keep whole-table work in
an otherwise per-page transaction.

Migrate material data to one of these forms:

1. fixed material-cell range per terrain slot; or
2. independently allocated immutable material page referenced by the Tree64
   descriptor.

The second form matches node-page lifecycle and is preferred. Occupancy-only
shadow traces may ignore the material page, while material-aware traces validate
its generation before lookup. Node and material descriptors publish together.

## 9. Implementation phases

### Phase 0 — measurement and invariants

- Record current terrain Tree64 CPU build time, snapshot assembly time,
  changed-range diff time, upload bytes and publication latency.
- Add counters for models/nodes/memberships/references touched per transaction.
- Add a debug capture of `(terrain slot, terrain generation, LOD, node slot,
  node generation)` for raster and Tree64.
- Add a test that repeatedly traces the old committed tree while a replacement
  payload is generated but unpublished.

**Gate:** existing behavior is measurable and every wall/shadow artifact can be
associated with a terrain and Tree64 generation.

### Phase 1 — page ABI and CPU allocator

- Define the terrain Tree64 descriptor and physical page handle.
- Implement page states, generation increments, reservation, abort, commit,
  retirement and completion-based reclamation.
- Derive startup capacity from resident chunks, worker/upload concurrency and
  frames in flight.
- Keep current trace buffers and dynamic snapshot publication unchanged.

**Gate:** randomized allocator tests prove no committed/retired page is reused,
and stale handles never validate after slot reuse.

### Phase 2 — per-chunk Tree64 payloads

- Extract per-chunk Tree64 generation from `buildTerrainTree64Payload`.
- Generate nodes and material records only for changed chunks.
- Preserve byte-identical nodes for the same chunk identity and generator
  revision.
- Add optional bounded CPU caching by chunk address, LOD and payload revision.
- Continue assembling the old complete snapshot from the per-chunk outputs.

**Gate:** assembled snapshots and traces match the current implementation, and
an internal LOD rewrite regenerates exactly one chunk Tree64 payload.

### Phase 3 — unreachable page upload

**Implemented:** generated page payloads now reserve immutable node/material
page slots and upload through bounded batches. Allocator transitions are tied
to completed GPU frames; completed pages remain `Uploaded` and unreachable
because the compatibility trace has no binding or descriptor path to the new
pools. Partial and complete upload isolation is covered by a dedicated GPU
test.

- Add node-page and material-page destinations to the terrain upload system.
- Upload generated pages into reserved physical slots over bounded frames.
- Track byte ranges and GPU completion without changing any live descriptor.
- Add explicit upload-to-trace barriers.

**Gate:** GPU tests show a fully or partially uploaded unpublished page cannot
affect a trace or directory lookup.

### Phase 4 — descriptor-indirected terrain trace

**Implemented:** terrain instances now resolve a stable logical terrain
descriptor to immutable compact/padded node-page pools. Trace rejects stale
terrain ownership, stale node/material page generations, LOD mismatches and
out-of-page root/count ranges before reading payload data. Ordinary dynamic
instances still use the compatibility model/node arena. Startup and completed
terrain rewrites mirror committed payloads into both representations pending
the atomic publication work in Phase 5.

- Make terrain Tree64 traversal resolve the logical terrain descriptor before
  reading its node page.
- Validate terrain slot generation, node page generation and bounds.
- Keep ordinary dynamic instances on the existing dynamic-model path.
- Initially mirror current committed data into both old and new descriptor
  representations for trace parity tests.

**Gate:** exact CPU/GPU trace parity passes for empty, occupied, refined,
coarsened, stale-generation and overflow pages.

### Phase 5 — atomic raster/Tree64 publication

**Implemented:** the final terrain upload now carries sorted conditional
publication records containing both the expected and replacement raster page
state and Tree64 descriptor. A first GPU dispatch validates the complete batch,
including logical ownership, LOD, node/material page generations and page
bounds. After a compute barrier, a second dispatch either rewrites every paired
state/descriptor plus its generic visibility data or writes none of them.
Physical payload and generation uploads remain unreachable in earlier bounded
batches. Acceptance status is frame-local and read back before CPU allocator
ownership advances or old pages retire. GPU coverage includes accepted,
batch-wide stale, frozen, overflow, recycled-material-page and forced-split
transactions. Idle and payload-only frames leave the last final-publication
result untouched, so a later frame cannot erase the acceptance status before
the render thread consumes it.

- Extend the final terrain publication record with the Tree64 descriptor.
- Validate the complete old/new identity on the GPU.
- Publish raster page state, Tree64 descriptor and material descriptor in the
  same render-graph transaction.
- Retain the old descriptors when any component is incomplete or stale.

**Gate:** no test frame can observe new raster with old Tree64 or old raster
with new Tree64, including publication freeze and forced upload splitting.

### Phase 6 — asynchronous scheduler

**Implemented:** Tree64 page generation now runs through a bounded worker
scheduler instead of a shared atomic work index. Requests are prioritized and
coalesced by stable terrain slot/identity; superseded queued work is removed
and active work is cancelled cooperatively. Generated payload, publication,
CPU-byte, upload-byte, active-job, physical-page, and retirement-headroom
limits all participate in admission/backpressure. The render-facing generated
and publication drains are non-blocking, and stale uploaded pages are returned
for abort rather than publication. Terrain's compatibility snapshot wrapper
uses the same page scheduler while the existing bounded GPU batches continue
to keep replacement pages unreachable until the Phase 5 atomic publication.
Dedicated CPU coverage holds generation artificially, verifies that polling
does not stall, checks priority and bounded high-watermarks, fills byte/page
budgets, and supersedes an uploaded unpublished page.

- Introduce bounded generation, upload and publication queues.
- Allow multiple independent chunk Tree64 jobs in flight.
- Coalesce superseded LOD requests and cancel obsolete work opportunistically.
- Apply CPU, pending-page, upload-byte and retirement-headroom backpressure.
- Keep rendering and tracing the previous committed pages while queues drain.

**Gate:** artificial slow generation/upload does not stall frames, expose
partial pages, grow queues without bound, or violate request priority.

### Phase 7 — incremental directory membership

**Status:** complete. Terrain streaming now uses stable logical model/instance
slots and fixed per-physical-directory reference slices. Internal page LOD
replacements retain the dynamic generation and leave model, instance,
membership and reference bytes untouched; toroidal movement rebuilds only the
entering physical membership slots. Ordinary dynamic scenes retain compact
packing and generation-wide transactional updates.

- Stop rebuilding terrain model/instance/membership/reference snapshots for an
  internal LOD change.
- Keep memberships unchanged when chunk bounds and stable slot do not change.
- On camera movement, rewrite only entering/leaving membership ranges and the
  affected stable instance records.
- Preserve ordinary dynamic-scene membership behavior.

**Gate:** retained terrain membership/reference bytes remain identical across
LOD rewrites and one-cell camera moves touch only entering/leaving ranges.

### Phase 8 — retirement, rollback and recovery

**Status:** complete. Page replacement batches validate every old and new owner
before mutating allocator state, and old pages retire against the actual
completed render-graph frame that performed descriptor publication. A rejected
GPU compare aborts reserved, uploading and uploaded pages as one rollback,
discards the pending dynamic transaction, and leaves the last committed raster
and Tree64 cut active. Generation-tagged scheduler jobs already reject stale or
cancelled work under bounded allocation/upload queues; staging failures unwind
page reservations. The page builder conservatively retries node overflow on
successively coarser aligned lattices, while irreducible node and material
capacity failures reject the transaction before publication.

- Connect page retirement to real render-graph/RHI completion tokens.
- Cover stale workers, cancelled jobs, allocation exhaustion, node overflow,
  material overflow, upload failure and publication rejection.
- Retry node overflow at the next coarser admissible LOD when appropriate.
- Retain the committed raster/Tree64 pair on every failure.

**Gate:** stress tests with at least `maxFramesInFlight + 2` rapid rewrites never
reuse a readable page or lose the last complete terrain owner.

### Phase 9 — qualification and old-path removal

**Status:** complete. Generated terrain is staged exclusively through bounded
page-streaming batches. Capacity mismatches and rejected publications retain
the previous complete raster/Tree64 cut instead of draining work and replacing
the whole arena. The ImGui LOD/terrain panel reports allocator page states,
pending bytes and batches, queue latency, stale/cancelled work and cumulative
descriptor rewrites. A source-level cutover contract prevents the legacy
whole-arena upload entry points and compatibility state from returning, while
the focused CPU/GPU suite qualifies allocation, scheduling, upload visibility,
atomic publication and rollback. The existing T9 cutover and benchmark tests
retain the original wall/shadow regression and packed-DDA comparison gates.

The generated-terrain dynamic layer now emits explicit changed ranges instead
of cloning a complete committed snapshot: an internal LOD replacement emits no
directory bytes, while a toroidal owner change patches only the affected stable
instance and membership/reference slices. Node and material payloads live only
in immutable physical pages selected by the published descriptor; generated
terrain no longer retains or uploads compatibility node arenas or a global
material-cell table.

Post-cutover runtime qualification also closed four integration gaps exposed by
the generated-world sample: the immutable child-index arena reserves refinement
headroom even when the initial coarse cut contains no child links; raster page
ownership is persistent across frames in flight; terrain rays no longer apply
the coarse receiver-cell skip intended for diagnostic dynamic instances. The
generated-world renderer publishes only voxel chunks; it creates no planet cap
or mesh transition.

- Add UI diagnostics for free/reserved/uploading/committed/retired pages,
  pending bytes, queue latency, stale jobs and descriptor rewrites.
- Run startup, stationary refinement, movement, return travel, teleport,
  publication freeze, memory pressure and the original wall/shadow capture.
- Compare CPU time, upload bytes, frame-time spikes and Tree64 trace results.
- Make page streaming the generated-terrain default.
- Remove generated-terrain complete dynamic-snapshot assembly, whole-buffer
  diffs and their compatibility controls after qualification.

**Gate:** the old generated-terrain Tree64 publication path has no callers, all
correctness gates pass, memory stays bounded, and movement/refinement no longer
causes whole-snapshot work.

The automated Phase 9 gate is the `svraster.tree64.streaming.phase9` CTest
label. The wider qualification also runs the focused `Voxel`/`Tree64` tests,
including T9 cutover and benchmark coverage, with explicit per-test timeouts.
Sustained rewrite coverage is separately selectable with
`svraster.tree64.streaming.stability`; direct installed Tree64 versus raster
geometry checks use `svraster.tree64.raster-parity`. The stability gate cycles
allocator retirement 512 times, GPU atomic publication 128 times, real
brick-derived physical pages 128 times, both GPU traversal implementations 64
times, and bounded generated-world leaf-cut movement across nine camera cuts.
Each installed-page sequence checks the last owner before publication, the new
owner immediately afterward, and isolation from a poisoned retired slot.

## 10. Tests

### CPU tests

- page allocation, reservation, abort, commit, retirement and reuse;
- stale `(slot, generation)` rejection after physical-page reuse;
- deterministic per-chunk page generation at every admitted LOD;
- same LOD rewrite produces byte-identical nodes and material cells;
- retained chunks preserve descriptor and page identity;
- internal LOD rewrite leaves memberships and references unchanged;
- camera movement changes only entering/leaving membership ranges;
- cancellation and failure leave the committed descriptor unchanged;
- queue coalescing and priority are deterministic;
- memory and pending-work budgets remain bounded.

### GPU tests

- unpublished page is unreachable;
- partial page upload is unreachable;
- valid final descriptor swap changes the traced page;
- stale terrain or page generation fails closed;
- invalid root/count/capacity fails closed;
- raster and Tree64 descriptor publication is atomic;
- old descriptor remains valid when publication validation fails;
- upload/write visibility precedes descriptor publication;
- compact and padded Tree64 traces resolve the same page;
- material-aware lookup rejects a mismatched material-page generation.

### Integration tests

- stationary coarse-to-fine convergence;
- fine-to-coarse memory-pressure rewrite;
- one-cell camera move and return;
- continuous camera walk longer than the resident diameter;
- rapid reversal while generation and uploads are pending;
- teleport with stale worker completions;
- freeze requests and freeze publication independently;
- forced one-section/low-byte upload budgets;
- forced node and material allocation exhaustion;
- original wall/shadow camera path with raster/Tree64 ownership diagnostics;
- Tree64 versus packed-DDA visibility comparison on the same committed cut.

## 11. Diagnostics and UI

Add a **Tree64 page streaming** subsection reporting:

- physical page capacity and page size;
- free, reserved, generating, uploading, unpublished, committed and retired
  page counts;
- generation queue depth and oldest-job age;
- upload queue bytes and current per-frame budget;
- descriptor rewrites this frame;
- stale/cancelled/rejected jobs;
- page allocation and build overflows;
- committed terrain/Tree64 generation mismatch count;
- per-transaction generated, uploaded and published node counts.

Debug views should color terrain by:

- Tree64 physical page slot;
- Tree64 page generation;
- internal LOD;
- raster/Tree64 generation match;
- pending versus committed ownership.

## 12. Performance and memory gates

The migration is successful only if:

- an internal LOD rewrite builds and uploads one chunk Tree64 page;
- unchanged node pages upload zero bytes;
- unchanged memberships/references upload zero bytes for an internal LOD rewrite;
- CPU work is proportional to changed chunks, not resident chunks;
- upload bytes are bounded by the configured per-frame budget;
- no render-thread wait is introduced for CPU generation or page upload;
- node/material page memory and retired-page headroom stay within explicit
  budgets during continuous movement;
- the final descriptor publication remains a small bounded transaction;
- Tree64 traces match the currently committed raster generation;
- the original phantom-wall/shadow path remains clean before, during and after
  asynchronous updates.

## 13. Recommended implementation order

Implement immutable whole-chunk pages and root-descriptor swaps first. This
uses the existing Tree64 node encoding and obtains the principal scalability
benefit: asynchronous generation and upload with no whole-snapshot rebuild.

Only consider cross-page child links after measuring page-capacity waste or
per-chunk rebuild cost. They are not required for progressive terrain LOD and
would enlarge the trace ABI, validation surface and failure modes.
