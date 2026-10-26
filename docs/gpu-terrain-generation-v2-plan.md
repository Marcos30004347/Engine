# GPU terrain generation v2: bounded 3D chunk pipeline

**Status:** implementation plan; replaces the runtime architecture proposed by
`gpu-terrain-chunk-generation-plan.md`

**Date:** 2026-08-27

## 1. Outcome

Replace the current whole-page GPU builder with a fully GPU-owned, bounded,
parallel terrain pipeline. A requested terrain region is decomposed into a
horizontal column tile and a stack of independent 3D voxel chunks. Each 8 x 8
x 8 raster brick is classified and emitted independently from canonical terrain
columns. Optional raster hierarchy levels and the Tree64 hierarchy are produced
by parallel count, scan, emit, and link dispatches.

The CPU is the control plane only. It may resolve request priority, calculate
dense level dimensions, reserve physical ranges, submit jobs, and retire fenced
allocations. It does not read generated columns, occupancy, materials, raster
nodes, or Tree64 nodes back from the GPU. If an output page has been evicted,
the page is rebuilt from maps.

The production path must satisfy all of the following:

- generation is incremental and cannot stall rendering;
- GPU work is budgeted in measured microseconds and bounded work units;
- 2D world maps use the existing virtual page table and physical atlas;
- chunks form a 3D grid and may be stacked vertically;
- finest raster output is solid 8-cubed voxel occupancy with no microgeometry;
- no two voxels or bricks overlap in canonical space;
- raster and Tree64 outputs come from the same quantized columns;
- no generated geometry or volume payload is copied GPU to CPU;
- publication is generation checked and atomic at an ownership boundary; and
- eviction is fence safe and can always fall back to regeneration.

## 2. Why the current builder is not a usable base

The present runtime has several architectural costs that optimization of the
map shader alone cannot repair:

1. A logical terrain page becomes one oversized build job, even when its
   vertical range is thousands of voxels.
2. Runtime submission drains all in-flight frames after a GPU page build.
3. Raster construction scans every dense candidate and its 512 cells from one
   shader invocation, constructs redundant coarse raster geometry, validates
   it serially, and hashes the full payload serially.
4. Tree64 masks are parallel, but compaction, linking, validation, and hashing
   contain page-sized serial work.
5. Scratch is sized as `nodeCapacity * 512` words, even though a leaf brick can
   be emitted without retaining 512 words per node.
6. Scheduling is expressed primarily as pages per update. One page can differ
   from another by orders of magnitude, so this is not a work or time budget.
7. One persistent build lane owns mutable configuration and scratch, which
   encourages synchronous reuse instead of multiple independently fenced jobs.

V2 is a new build graph with explicit work queues and fixed-size quanta. The
old builder remains only as a differential oracle and temporary fallback.

```text
virtual map atlas
       |
       v
parallel X/Z column tile -------------------------------+
       |                                                |
       +--> vertical chunk Y0 --> classify -> scan -----+--> emit leaf bricks
       +--> vertical chunk Y1 --> classify -> scan -----+--> emit leaf bricks
       +--> vertical chunk Yn --> classify -> scan -----+--> emit leaf bricks
                                                        |
       +------------------------------------------------+--> Tree64 masks
                                                               |
                                                  level reductions + scans
                                                               |
                                                               v
                                                     validate hidden output
                                                               |
                                                atomic ownership publication
```

Column tiles and atlas pages may be shared by a vertical stack. Raster and
Tree64 branches synchronize only through immutable columns and final
publication; neither reconstructs or reads back the other's generated payload.

## 3. Architectural decisions

### 3.1 Separate 2D column tiles from 3D voxel chunks

World maps describe surfaces in X/Z. Sample them once into a canonical column
tile, including the required halo and boundary-stitch values. A column tile is
then consumed by one or more vertically stacked 3D chunk jobs.

Provisional default dimensions are expressed in bricks, not metres:

```text
raster brick:            8 x 8 x 8 voxels
3D build chunk:          8 x 4 x 8 bricks
3D build chunk:         64 x 32 x 64 voxels
horizontal column tile: 64 x 64 columns plus halo
```

These defaults are starting points, not hard-coded assumptions. Qualification
must benchmark at least 4x4x4, 8x4x8, and 8x8x8 brick chunks. A device profile
selects the largest shape whose individual dispatch stages remain below their
latency cap. Chunk X, Y, and Z dimensions are part of the job description.

The vertical chunk coordinate is signed relative to the page elevation datum.
A column run intersects a vertical chunk if and only if its integer interval
overlaps that chunk's half-open Y interval. This makes every vertical slab
independent and removes the need for a page-height scratch allocation.

### 3.2 Finest raster bricks are the production raster representation

Spatial clipmap pages already provide terrain LOD. The default runtime raster
payload therefore contains only exact finest bricks for that page's clipmap
LOD. Every emitted node has:

- one canonical 8-cubed grid origin and positive scale;
- exactly sixteen occupancy words with correct prefixes;
- one material byte and one terrain-context record per occupied cell;
- no children and no parent;
- no micro-triangles, subvoxel faces, displacement geometry, or ray offset;
- bounds derived from occupied integer cells; and
- a generation, terrain slot, region mapping, and clipmap LOD installed during
  publication.

The DDA hit point is derived from the integer cell boundary. Shading may use a
normal-oriented epsilon only for a subsequent visibility ray if required by a
specific algorithm; the primary hit, depth, face ID, albedo lookup, and G-buffer
position must not be moved. Terrain light rays have no terrain-generation
offset.

An optional in-page raster hierarchy is a later device/profile choice, not a
prerequisite for the default pipeline. It may be enabled only when measured
raster savings exceed its generation, memory, and transition cost.

### 3.3 GPU hierarchy construction is the default

The CPU may calculate level extents and fixed buffer offsets because those are
pure functions of chunk dimensions. It must not read occupancy counts merely
to construct hierarchy metadata. Such a readback would serialize generation,
duplicate ownership, and prevent asynchronous jobs.

If optional raster parents are enabled, construct every level with this chain:

1. one invocation per dense parent candidate reads up to eight child records;
2. count nonempty parents, children, and material cells;
3. exclusive scan produces deterministic node/material/child offsets;
4. one invocation per retained parent emits its node and reduced occupancy;
5. one invocation per retained parent links its contiguous child range; and
6. indirect arguments for the next level are written on GPU.

Levels execute in sequence because level N consumes level N-1, but every
candidate within a level executes in parallel. Stop when the root level is
complete or compaction no longer reduces the level. No stage loops over a
whole level from one invocation.

There are two different orders, and the implementation must not confuse them:

- **Hierarchy construction is fine-to-coarse.** LOD/level 0 is the finest
  occupancy. Parent occupancy, material reduction, bounds, and error depend on
  children, so payload reduction proceeds from level 0 upward to the root.
- **Streaming visibility is coarse-to-fine.** A coarse independently generated
  chunk may publish first to provide coverage. Finer chunks build hidden and
  atomically replace the coarse owner's region only when the complete finer
  ownership group is ready.

Topology alone may be enumerated from the root downward because dense level
dimensions are known. Tree64 child links are also installed top-down after
scans assign compact indices. Neither changes the required bottom-up order for
occupancy and error values.

Each clipmap LOD can be generated directly and independently from the canonical
map/column contract at that LOD's integer grid. Initial coarse coverage therefore
does not wait for LOD0. Within one requested chunk, however, an optional linked
raster hierarchy still reduces its exact leaves bottom-up. The rewrite strategy
is consequently preserved:

```text
generate/publish coarse owner
        -> request finer ownership group
        -> build all finer chunks hidden
        -> validate shared boundaries
        -> atomically publish finer group and retire coarse owner
```

CPU hierarchy construction is permitted only for:

- the reference/fallback path;
- tiny test fixtures;
- static precomputation stored on disk; or
- an explicitly measured platform where transferring a small topology record
  is faster without transferring occupancy, materials, or geometry.

It is not the default runtime design.

### 3.4 Tree64 uses the same count/scan/link pattern

Tree64 remains the shadow and scene-ray acceleration structure. Its build is:

1. terminal masks: one workgroup per 4 x 4 x 4 terminal block;
2. upper masks: one invocation per dense parent, level by level;
3. live-node flags: one invocation per dense mask;
4. per-level exclusive scan for compact node indices;
5. top-down linking: one invocation per live parent reserves or writes its
   contiguous child block using scanned offsets;
6. terminal emission: one invocation per live terminal; and
7. parallel validation reductions plus one constant-time descriptor finalize.

No Tree64 pass may clear, compact, traverse, validate, or hash an entire page
from a single invocation. Full semantic hashes are test/debug work and are not
computed by the shipping stream builder.

### 3.5 Deterministic scans, not allocation atomics, define output layout

Atomics are acceptable for queue counters, status flags, telemetry, and an
optional unordered fast prototype. Production output uses count plus exclusive
scan because it provides:

- deterministic node and material ordering;
- exact allocation sizes;
- coalesced output writes;
- packed material bytes without cross-workgroup read/modify/write races;
- reproducible hashes in qualification tests; and
- straightforward overflow checks before payload emission.

Use the engine's reusable GPU scan primitive. If one does not exist with
segmented and indirect-dispatch support, Phase 2 creates it before terrain
payload work begins.

## 4. Data contracts

### 4.1 Request and identity

```cpp
struct TerrainGPUChunkKey {
  uint64_t worldBundleRevision;
  int32_t chunkX;
  int32_t chunkY;
  int32_t chunkZ;
  uint16_t clipmapLOD;
  uint16_t generatorRevision;
  uint32_t terrainSlot;
  uint32_t expectedGeneration;
};

struct TerrainGPUChunkShape {
  uint16_t bricksX;
  uint16_t bricksY;
  uint16_t bricksZ;
  uint16_t brickSide; // 8 for raster
};
```

The key identifies logical content. Physical node, material, Tree64, scratch,
and atlas ranges are leases stored separately. A rebuild can use different
physical ranges without changing logical identity.

### 4.2 Canonical columns

Columns use integer voxel coordinates after one quantization step:

```cpp
struct TerrainColumnGPU {
  int32_t firstSolidY;
  int32_t topY;
  uint32_t packedMaterialAndContext;
  uint32_t packedBoundaryFlags;
};
```

The column tile also carries a resolved integer elevation datum, canonical X/Z
origin, column side, halo, clipmap LOD, generator revision, and expected atlas
revision. Raster and Tree64 never re-sample height independently.

### 4.3 Per-brick intermediate

Classification writes one compact record per dense brick candidate:

```cpp
struct TerrainBrickCountGPU {
  uint32_t occupiedCount;
  uint32_t nonempty;
  uint32_t materialCount;
  uint32_t reserved;
};
```

Scanned offsets are stored in a second array. There is no `512 words * nodes`
cell scratch. Emission re-evaluates the inexpensive interval/material predicate
for its cells, trading a few cached column reads for hundreds of megabytes of
scratch and bandwidth.

### 4.4 Job state

```cpp
enum class TerrainGPUJobStage : uint8_t {
  WaitingForAtlas,
  SampleColumns,
  ClassifyFineBricks,
  ScanFineBricks,
  EmitFineBricks,
  BuildRasterParents,
  BuildTree64Masks,
  ScanTree64,
  LinkTree64,
  Validate,
  ReadyToPublish,
  SubmittedForPublication,
  Complete,
  Cancelled,
  Failed,
};
```

Every stage records its bounded item count, estimated cost, measured GPU time,
input/output leases, completion fence, and generation. State transitions are
monotonic. A stale generation may finish already submitted work but cannot
advance to publication.

## 5. Dispatch design

### 5.1 Column sampling

- Dispatch a 2D grid; never one workgroup for a full tile.
- Start with 8 x 8 or 16 x 8 threads per workgroup and profile texture locality.
- Each invocation owns one output column and samples the atlas through the
  existing virtual page table.
- Halo columns are explicit work items, not a serial post-pass.
- Use canonical integer/fixed-point map coordinates for shared page edges.
- Missing atlas pages set a compact fault bit/list and leave the job waiting;
  they never generate a zero-height replacement.
- Column results remain in GPU storage and may be shared by all vertical chunks
  with the same X/Z footprint and generation.

### 5.2 Fine brick classification

- Dispatch one workgroup per 8-cubed brick.
- Use 64 threads initially; each lane evaluates eight cells in a stable order.
- Cell occupancy is an interval test against the cached column record.
- Workgroup reductions produce occupied count, occupied bounds, coarse 4-cubed
  mask, and material count.
- Empty bricks write `nonempty = 0` and allocate nothing.
- The dispatch is three-dimensional or a flattened equivalent with exact
  integer decode. X/Y/Z chunk origin is included explicitly.

### 5.3 Scan and allocation

- Scan brick `nonempty` values for node indices.
- Scan `materialCount` for material bases.
- Compare totals with the output lease before emission.
- If the current lease is too small, request a larger size class or split the
  chunk; do not partially emit and do not grow an arena synchronously.
- Scan buffers are pooled by size class and fence retired.

### 5.4 Fine brick emission

- Dispatch one workgroup per nonempty brick using a compacted work list.
- Re-evaluate 512 inexpensive cell predicates in parallel.
- Use workgroup prefix sums to write sixteen occupancy words and prefixes.
- Write material/context records directly to the scanned non-overlapping range.
- Initialize every public node field explicitly; zero unused capacity only when
  required by an ABI consumer, and do so with a parallel clear pass.
- Do not emit microgeometry or use subvoxel bounds.

### 5.5 Optional raster parent levels

- Disabled by default for generated terrain.
- When enabled, execute count/scan/emit/link per level.
- Parent occupancy is a conservative reduction of children.
- Parent material selection uses a specified deterministic rule.
- Parent error is a parallel reduction and is monotonic by construction.
- Publication cannot mix leaf-only and hierarchical chunks under the same
  descriptor flag.

### 5.6 Tree64

- Generate terminal masks directly from the same columns and vertical chunk
  interval, not by reading raster geometry.
- Build upper masks bottom-up in parallel.
- Compact with scans, then link top-down in parallel.
- Compute occupied count with a reduction during terminal classification.
- Final descriptor creation is O(1).

### 5.7 Validation

Shipping validation is structural and parallel:

- generation/revision agreement;
- all scanned totals within leases;
- occupancy prefix and material range checks per raster brick;
- child ranges within compact Tree64 node count;
- raster/Tree64 occupied-count agreement; and
- root/descriptor presence.

Full payload hashing, exhaustive parent-child occupancy comparison, and ray
oracle comparison are debug/test modes only. Their passes are also parallel and
must obey a separate debug budget.

## 6. Scheduler and budget

### 6.1 Priority

The request queue orders work by projected error in pixels, then visibility,
distance, age, and dependency readiness. A practical score is:

```text
priority = max(projectedErrorPixels - targetPixels, 0)
         * visibilityWeight
         * ageBoost
         / max(estimatedGPUTimeMicroseconds, minimumCost)
```

Near, visible, high-error chunks therefore complete before far chunks. One
large region request is decomposed into independently prioritized 3D chunks;
it never enters the queue as one indivisible page.

### 6.2 Per-frame limits

Maintain independent limits for:

- terrain generation GPU microseconds;
- number of dispatch workgroups;
- atlas upload bytes;
- new output reservation bytes;
- scratch bytes in flight;
- jobs started;
- jobs published; and
- total resident terrain bytes.

The primary compute limit is time, measured with GPU timestamps. A feedback
controller uses an exponential moving average per `(stage, shape, device)` to
predict the next quantum. The scheduler submits work only while predicted cost
fits the remaining budget. Workgroup count is a hard safety backstop.

Suggested initial interactive budgets:

```text
target terrain generation:       1.0 ms/frame
hard terrain generation ceiling: 2.0 ms/frame
maximum single dispatch:         0.50 ms on the qualification device
atlas uploads:                   8 MiB/frame
scratch pool:                    min(128 MiB, 5% of resident GPU budget)
terrain high-water mark:         75% of its assigned resident budget
terrain emergency mark:          90% of its assigned resident budget
```

Values are settings and device-profile outputs, not universal constants.
Timestamp unavailability selects conservative calibrated workgroup caps.

### 6.3 Chunk-generator budget and variable N

The chunk generator has its own sub-budget inside the total terrain budget.
`maximumChunksPerFrame` is configurable and initially defaults to 128. It is a
hard upper bound on the number of distinct chunks allowed to advance during a
frame, not a promise to complete 128 chunks and not permission to exceed the
time or memory budgets.

The scheduler selects chunks in priority order and admits the next stage only
when every constraint fits:

```text
Nframe = min(
    configured maximum chunks per frame,
    chunks fitting remaining predicted GPU microseconds,
    chunks fitting remaining workgroups,
    chunks fitting remaining scratch bytes,
    chunks fitting remaining output-reservation bytes,
    chunks whose atlas dependencies are resident)
```

Because stages and chunk shapes have different costs, this is implemented as
an incremental admission loop rather than division by one global average:

```cpp
for (job : priorityQueue) {
  StageCost cost = predictor(job.stage, job.shape, device);
  if (advancedChunkCount == maximumChunksPerFrame) break;
  if (!frameBudget.fits(cost)) continue;
  submitBoundedStageQuantum(job);
  frameBudget.consume(cost);
  ++advancedChunkCount;
}
```

One chunk may advance by one bounded stage quantum per scheduler round. A stage
whose predicted cost exceeds the maximum-dispatch limit is divided by a brick,
mask, or scan-block cursor and resumes in a later quantum. A second round may
use otherwise stranded budget after every eligible high-priority chunk had one
opportunity.

Suggested initial chunk-generator controls are:

```text
chunk generation target:              0.75 ms/frame
chunk generation hard ceiling:        1.50 ms/frame
maximum chunks advanced:              128/frame
maximum chunks newly admitted:         32/frame
maximum chunk publications:           128/frame
maximum generation workgroups:       8192/frame
maximum one stage quantum:             0.50 ms
```

The remaining terrain budget covers atlas work and publication. Unused atlas,
generation, or publication time may be borrowed within the total hard ceiling,
but borrowed capacity is not carried into the next frame. Under light work and
small chunks the scheduler may genuinely finish 128 chunks. With 64 x 32 x 64
voxel chunks, generation will normally be limited by measured GPU time or
workgroups first, so actual N will be lower.

The cost predictor stores an exponential moving average for each
`(stage, chunkShape, deviceProfile)`. It predicts both a fixed dispatch cost and
a per-workgroup cost. Timestamp results arrive asynchronously and adjust future
frames; a frame never waits for them. New shapes start with a conservative
qualification value. Prediction error tightens or relaxes the soft budget, but
the hard workgroup, memory, and resident limits always remain enforced.

### 6.4 Submission and synchronization

- Never call device/queue wait-idle for terrain generation.
- Never drain the renderer's pending frame deque after a build.
- Use at least `framesInFlight + 1` immutable job-parameter/indirect lanes or a
  ring allocator whose ranges are fence protected.
- A lane is not rewritten until its completion fence passes.
- Jobs can have multiple stages in flight when dependencies permit.
- Prefer async compute when it is genuinely concurrent on the backend; retain
  the same budget on a unified graphics/compute queue.
- Read back only tiny asynchronous status/timestamp/fault records. Rendering
  never waits for those readbacks.

### 6.5 Fairness and cancellation

- Reserve most of the budget for visible high-error work and a small fraction
  for aged/background requests to prevent starvation.
- Re-score queued jobs each frame from the current camera.
- Cancel jobs that leave the required clipmap ownership set before allocating
  output, and mark submitted jobs stale after allocation.
- Stale jobs release leases behind their fences and never publish.

## 7. Atlas residency

Use `MapExportPageCache`/the existing GPU atlas as the only map source.

- The CPU performs file I/O and decompression into atlas upload pages.
- A GPU page table maps logical map field/page/level to physical atlas storage.
- Job preparation gathers the page footprint plus halo for every required map
  field and pins those pages until column sampling completes.
- Atlas faults are deduplicated and prioritized using the highest priority of
  their waiting terrain jobs.
- Atlas uploads consume a separate transfer budget.
- Physical map pages use LRU/clock eviction among unpinned entries.
- Column sampling validates the atlas revision captured in the job.
- A changed or missing revision makes the job stale; it does not sample a
  recycled physical page.

Map fields should be packed by compatible format and access pattern. Do not
force unrelated fields into one large texture if it increases bandwidth. The
qualification phase records texture transactions and cache hit rates for each
packing candidate.

## 8. Output ownership, publication, and eviction

### 8.1 Physical allocation

Use size-classed arenas for raster nodes, occupancy, materials, roots, Tree64
nodes, and build scratch. A job initially reserves an upper-bound size class or
performs count/scan first and then asynchronously requests an exact class.

Resident accounting includes committed pages, hidden replacements, retired
pages awaiting fences, atlas pages, and scratch. The scheduler must reserve
headroom for one replacement ownership group; reaching the high-water mark
triggers eviction before starting more builds.

### 8.2 Publication boundary

A single 3D chunk may publish independently only if its renderer ownership is
disjoint and its boundaries are defined by the same canonical integer grid.
LOD replacement uses an ownership group:

- build all required child chunks hidden;
- validate every member and its shared boundary signature;
- install page/chunk descriptors in one generation-checked metadata dispatch;
- retire the previous coarse owner behind the publication fence.

This prevents parent and child coverage overlap, holes, and flickering seams.

### 8.3 Eviction

Eviction score considers visibility, projected error, distance, age, clipmap
coverage responsibility, replacement availability, and rebuild cost.

- Never evict the only owner required to cover a visible region unless a
  coarser resident owner covers it.
- Descriptor removal is atomic and generation checked.
- Physical ranges return to their pools only after the last reader fence.
- No CPU page cache is created from evicted GPU output.
- A later request schedules regeneration from atlas maps.

## 9. Instrumentation

The renderer overlay and logs expose:

- GPU terrain mode and fallback reason;
- queued/running/waiting/stale jobs by stage;
- selected chunk shape and vertical stack count;
- predicted and measured microseconds by stage;
- submitted workgroups and budget remaining;
- atlas hits, faults, pinned pages, uploads, and evictions;
- committed, hidden, retired, scratch, and total resident MiB;
- output counts and compaction ratios;
- publication acceptance/rejection reason;
- nearest visible request's error in pixels and queue rank; and
- whether any terrain operation forced a CPU/GPU synchronization event.

Add debug views for chunk ownership, canonical brick coordinates, LOD owner,
occupancy, face ID, albedo, raw G-buffer depth, light visibility, and atlas page
identity. These views must be usable with temporal upscaling and lighting
disabled to isolate raster defects.

## 10. Implementation phases and exit tests

Each phase lands behind `terrain-gpu-v2`. The old full-GPU builder is not the
fallback for V2; the known-correct CPU column builder is.

### Phase 0 - Freeze the failure and performance baseline

**Status:** in progress. The automated ground-level GPU trace, per-pass timing,
drain/readback/memory counters, and first baseline are implemented. See
`terrain-gpu-v2-phase0-baseline.md`. CPU-builder, rendering-only, and full
16 km captures remain.

Deliverables:

- captures for current GPU generation, CPU generation, and rendering-only;
- GPU timestamp scopes around every current generation pass;
- explicit counters for queue drains, wait-idle calls, readback bytes, scratch,
  hidden output, and resident output;
- a ground-level scripted camera whose Y is resolved from the terrain column;
- a 16 km churn trace and a close-range seam/flyover trace; and
- a written list of current output invariants and known failures.

Tests/gates:

- reproduce the low-FPS build without relying on manual camera movement;
- the ground camera starts 1-2 metres above actual LOD0 terrain, never in sky
  or on a coarse placeholder;
- record median/p95/p99 frame time and stage time;
- detect any forced pending-frame drain as a test failure; and
- record peak resident and scratch MiB against the 2048 MiB configuration.

### Phase 1 - Define 3D jobs and canonical coordinate ownership

**Status:** complete. The signed 3D logical key, 128-byte GPU job ABI,
integer coordinate conversions, half-open ownership, cross-LOD boundary
signatures, vertical stack enumeration, dense-brick CPU oracle, and property
tests are implemented. See `terrain-gpu-v2-phase1-contract.md`.

Deliverables:

- `TerrainGPUChunkKey`, shape, bounds, generation, and stage ABI;
- conversion between world, clipmap, column-tile, chunk, brick, and voxel
  coordinates using integers after quantization;
- vertical chunk-stack enumeration from column min/max bounds;
- shared-edge ownership and boundary signatures; and
- CPU reference enumeration for every dense brick candidate.

Tests/gates:

- property tests for negative coordinates and every chunk face;
- adjacent chunks never overlap and union to the requested half-open volume;
- identical canonical shared-edge samples across adjacent pages and LODs;
- vertical chunks correctly intersect surface/foundation runs; and
- randomized shapes stay within 32-bit shader indexing limits.

### Phase 2 - Reusable GPU scan, work queues, and fenced arenas

**Status:** complete. The reusable three-level GPU exclusive scan and stable
compaction chain, indirect argument emission, immutable fenced parameter ring,
buddy scratch/output arenas, adaptive stage cost model, and multi-budget fair
scheduler are implemented. See `terrain-gpu-v2-phase2-infrastructure.md`.

Deliverables:

- exclusive scan and compact-work-list primitives;
- indirect dispatch generation;
- immutable frame/job parameter ring;
- size-classed scratch/output allocators with deferred reclamation;
- GPU timestamp feedback and stage cost model; and
- priority scheduler with cancellation and fairness.

Tests/gates:

- scan differential tests at 0, 1, workgroup boundaries, and maximum size;
- randomized compaction produces deterministic stable order;
- overflow is reported before output writes;
- no lane is reused before its fence;
- stale jobs cannot publish;
- priority tests prove larger on-screen error runs first;
- configured `maximumChunksPerFrame` is never exceeded;
- a 128-chunk-ready queue advances only the subset fitting measured time,
  workgroup, scratch, output, and atlas budgets;
- unused budget is recovered by a second fair scheduler round;
- delayed or deliberately wrong timestamp estimates converge without a frame
  exceeding the hard non-time limits;
- synthetic load stays within time, workgroup, scratch, and transfer budgets;
- no terrain submission drains renderer frames or waits for GPU idle.

### Phase 3 - Virtual atlas integration and parallel columns

**Status:** complete. Terrain jobs now derive a pinned virtual-atlas
footprint, share one canonical X/Z tile across vertical chunks, sample height
and material fields with bounded 2D GPU dispatches, and fail into compact
page-fault retry state without writing columns. See
`terrain-gpu-v2-phase3-columns.md`.

Deliverables:

- atlas footprint/pinning API for a terrain job;
- 2D parallel column sampling with halo;
- missing-page fault list and retry state;
- canonical fixed-point/integer quantization; and
- reusable column-tile cache shared by vertical chunks.

Tests/gates:

- GPU columns match CPU columns over all terrain-map fixtures;
- exact shared-edge equality across pages/LODs;
- atlas eviction cannot change a pinned job's samples;
- missing pages wait rather than produce geometry;
- upload bytes obey the transfer budget;
- no generated columns are read back in runtime mode; and
- performance sweeps select the workgroup shape per qualified device.

### Phase 4 - Occupancy-only fine raster bricks

**Status:** complete. Phase 3 columns now feed a compact leaf-only raster
chain that classifies 32 cells per invocation, performs paired node/material
scans, and emits renderer-native 8x8x8 occupancy and byte material streams.
Generated nodes select the occupancy-only renderer contract and allocate no
microgeometry. See `terrain-gpu-v2-phase4-fine-raster.md`.

Deliverables:

- per-brick classify/count dispatch;
- node/material scans and compact brick work list;
- parallel occupancy/material emission;
- leaf-only generated terrain descriptor mode; and
- removal of generated-terrain microgeometry consumption.

Tests/gates:

- byte/semantic differential against CPU finest bricks;
- occupancy prefixes and material addresses for every possible occupancy word;
- empty, full, one-cell, shell, cliff, and boundary-stitch bricks;
- deterministic output across dispatch order and workgroup variants;
- adjacent chunk AABBs meet exactly and never overlap;
- raw albedo shows axis-aligned square voxel faces;
- DDA hit/depth/face ID is stable at 1 cm and during subpixel camera motion;
- per-node 512-cell global scratch is zero; and
- every dispatch stays under its calibrated latency cap.

### Phase 5 - Parallel Tree64 build

**Status:** complete. Phase 4 occupancy now feeds a Morton-ordered Tree64 chain
with parallel terminal/upper masks, one global live-node scan, top-down bounded
linking, per-candidate structural validation, and O(1) descriptor finalization.
There is no runtime payload hash or GPU-to-CPU generated-data transfer. See
`terrain-gpu-v2-phase5-tree64.md`.

Deliverables:

- terminal and upper mask dispatches;
- live-node scan/compaction;
- top-down parallel linking;
- parallel structural validation; and
- O(1) descriptor finalization.

Tests/gates:

- exact Tree64 differential on small fixtures;
- conservative occupancy on randomized large fixtures;
- CPU ray oracle versus GPU Tree64 rays, including chunk boundaries;
- raster and Tree64 occupied counts agree;
- no single-invocation page loop in shader static analysis;
- no runtime full-payload hash; and
- compaction/link time scales with live work rather than maximum page height.

### Phase 6 - Optional raster hierarchy experiment

**Status:** complete as a qualified experiment; leaf-only remains the default.
The renderer-native parent ABI, CPU oracle, level-by-level GPU chain, exact
budgeting, capture comparison, and device p99 gate are implemented. See
`terrain-gpu-v2-phase6-raster-hierarchy.md`.

Deliverables:

- level-by-level count/scan/emit/link implementation;
- conservative parent materials and monotonic error;
- device setting selecting leaf-only or hierarchy; and
- capture comparison of render savings versus build/memory cost.

Tests/gates:

- one selected node per hierarchy path;
- no parent/child simultaneous coverage;
- parent occupancy/error is built LOD0-to-root while publication fixtures prove
  coarse-to-fine ownership replacement;
- independently generated coarse coverage does not require fine payload
  generation or readback;
- monotonic error and deterministic links;
- LOD transitions remain crack free at ground level;
- hierarchy mode is enabled only if end-to-end p99 improves on its device tier.

If the last gate fails, keep leaf-only raster and remove the experiment from
the production shader set.

### Phase 7 - Atomic publication and 3D eviction

Deliverables:

- hidden output leases per 3D chunk;
- ownership-group publication for LOD replacement;
- generation/revision compare-and-swap;
- resident high/emergency watermarks; and
- coverage-safe, fence-retired eviction.

Tests/gates:

- partial jobs are never visible;
- rejected/stale publications retain the prior complete owner;
- interrupted multi-chunk LOD replacement shows neither holes nor overlap;
- churn reclaims every hidden/retired allocation after fences;
- peak accounting includes all resident categories;
- the 2048 MiB test never exceeds its hard budget; and
- evicted content is regenerated without a CPU geometry cache.

### Phase 8 - Runtime integration and adaptive budgeting

**Status:** complete. The renderer-independent runtime admission layer, live
screen-error reprioritization, multi-job frame schedules, timestamp feedback,
conservative fallback profile, qualification matrix, and compact CPU fallback
requests are implemented. See `terrain-gpu-v2-phase8-runtime.md`.

Deliverables:

- V2 scheduler wired to screen-space-error requests;
- asynchronous multi-job submission;
- GPU timestamp controller and conservative no-timestamp profile;
- overlay/telemetry; and
- device-loss and qualification fallback to CPU columns/build.

Tests/gates:

- near visible chunks outrank far/off-screen chunks under churn;
- one large requested region is visibly decomposed into individual 3D chunks;
- setting the chunk cap to 1, 8, and 128 produces the same final terrain while
  changing only throughput and latency;
- actual chunks advanced may be below 128 whenever the measured GPU-time or
  workgroup budget is exhausted;
- p99 generation compute remains within the configured hard ceiling;
- generation does not introduce a queue-idle or renderer frame drain;
- interactive frame rate degradation is below 10% at the default 1 ms budget;
- nearest required LOD0 begins refining within the latency target;
- camera motion, temporal upscaling, and lighting on/off matrices are captured;
- raw raster stability passes before lighting stability is evaluated.

### Phase 9 - Visual stability qualification

**Status:** complete. The fine-surface ground camera path, five-layer capture
contract, raw-before-lighting stability analyzer, temporal isolation checks,
device qualification table, and runtime qualification gate are implemented.
The existing GPU fine-raster and Tree64 differential binaries are also run as
Phase 9 gates. See `terrain-gpu-v2-phase9-stability.md`.

Deliverables:

- automated close-ground camera paths;
- raw G-buffer, unlit albedo, direct-light-only, full-lighting, and upscaler
  captures;
- frame-to-frame image and geometry-ID analysis; and
- backend/device qualification table.

Tests/gates:

- no black cracks, overlaps, T-junction ownership gaps, or shifting voxel faces;
- stable primitive/brick/cell/face identity under stationary-camera jitter;
- terrain shadows do not move when geometry and light are stationary;
- disabling temporal upscaling does not hide a raster defect;
- no terrain ray-origin offset affects the primary surface or direct shadow;
- 1 cm camera distance remains finite and stable; and
- CPU and GPU paths agree on canonical occupancy at every reported visual
  discontinuity.

### Phase 10 - Default rollout and cleanup

Implementation status (2026-08-27): the qualified `auto` selector, persistent
session circuit breaker, strict budget/shape controls, compact CPU fallback,
and Phase 10 stress/soak suite are implemented. The sample quarantines the old
monolithic builder behind its deprecated explicit flag and otherwise fails
closed to CPU until its V2 RenderGraph consumer records created pipelines and a
passing Phase 9 device profile. Per the final gate below, deleting the legacy
source/readback code happens after that consumer completes one release/soak
interval; it is not an automatic V2 fallback. See
`terrain-gpu-v2-phase10-rollout.md`.

Deliverables:

- qualified-device V2 default;
- persistent fallback reason and one-session circuit breaker;
- removal of the current monolithic full-page runtime chain;
- removal of CPU generated-payload cache/readback paths; and
- retained CPU semantic oracle and compact fallback.

Tests/gates:

- cold start, 16 km streaming, teleport, rapid LOD oscillation, memory pressure,
  atlas thrash, and device reset soak tests;
- no performance regression against the CPU fallback on unqualified hardware;
- no orphan buffers or allocations after repeated enable/disable cycles;
- V2 can be disabled without changing public terrain data formats; and
- at least one release/soak interval passes before deleting fallback code.

## 11. Test suite layout

Add focused targets rather than one long integration binary:

```text
TerrainGPUV2CoordinateTests             CPU/property
TerrainGPUV2ScanGPUTests                GPU primitive
TerrainGPUV2SchedulerTests              CPU/synthetic timestamps
TerrainGPUV2AtlasGPUTests               GPU atlas and columns
TerrainGPUV2RasterGPUTests              GPU brick differential
TerrainGPUV2Tree64GPUTests              GPU hierarchy/ray differential
TerrainGPUV2PublicationGPUTests         GPU CAS and retirement
TerrainGPUV2BudgetIntegration           runtime/headless
TerrainGPUV2GroundStability             runtime/capture
TerrainGPUV2MemoryPressure              runtime/allocator
TerrainGPUV2Soak                         long-running qualification
```

GPU tests skip with an explicit reason only when the backend cannot run; a
timeout, device loss, validation error, or budget violation is a failure.
Performance baselines are stored per qualified device class and compared using
robust percentiles, not one elapsed-time assertion.

Shader static checks reject:

- `workgroup_size(1)` entry points that contain data-dependent page loops;
- loops bounded by page node/material/mask capacity in runtime finalize passes;
- runtime semantic hashing over full payloads;
- generated payload copy-to-readback bindings;
- per-node 512-cell scratch allocations; and
- synchronous queue/frame draining in terrain submission code.

## 12. Rollout controls

Generated-world terrain now defaults to the strict GPU-v2 path. Use explicit
modes during migration:

```text
--terrain-generation=cpu
--terrain-generation=gpu-v2
--terrain-generation=auto
--terrain-gpu-budget-ms=<value>
--terrain-gpu-chunk-budget-ms=<value>
--terrain-gpu-max-chunks-per-frame=<N>
--terrain-gpu-max-new-chunks-per-frame=<N>
--terrain-gpu-max-workgroups-per-frame=<N>
--terrain-gpu-chunk-bricks=<x,y,z>
--terrain-gpu-raster-hierarchy=off|on|auto
```

`auto` selects V2 only after device qualification and shader/pipeline creation.
Unlike the default `gpu-v2` request, `auto` may fall back to compact CPU
requests for compatibility. The CPU fallback must remain metadata-only for the
shipping path: no height sampling, voxel shell generation, Tree64 build, or
triangle cluster emission is allowed on the CPU unless `--terrain-generation=cpu`
or the deprecated CPU builder override is explicitly requested.
A session falls back before publication after repeated allocation, validation,
or device errors. Performance pressure lowers the generation budget or chunk
shape; it does not silently mix CPU and GPU ownership of one logical page.

## 13. Recommended implementation order

The fastest safe vertical slice is:

```text
3D job/coordinates
  -> GPU scan and fenced arenas
  -> existing atlas + parallel columns
  -> occupancy-only 8^3 raster bricks
  -> parallel Tree64
  -> hidden publish/evict
  -> adaptive screen-error scheduler
```

Do not begin with optional raster parent levels. Do not optimize procedural map
math before timestamp captures show it is dominant. Do not make V2 default
until the ground-stability, no-stall, memory, and p99 budget gates pass together.

The central performance rule is simple: a dispatch may process many independent
items in parallel, but no invocation may own a terrain page. The unit of
scheduling is a bounded 3D chunk stage; the unit of raster construction is one
8-cubed brick; and the unit of hierarchy reduction is one candidate node.
