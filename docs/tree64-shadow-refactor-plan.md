# Chunked Tree64 shadow refactor plan

Status: active — T0-T8 complete; T9 renderer cutover implemented, with release-hardware gates and the stabilization/removal interval still pending
Date: 2026-07-24
Scope: `src/voxel/svraster`, `assets/shaders/voxel/svraster`, the sparse-voxel sample, focused voxel tests, and shadow-acceleration benchmark tooling

## Executive decision

Replace the camera-centered packed occupancy texture and hierarchical DDA shadow consumer with a camera-centered, world-aligned directory of immutable Tree64 chunks. Do not put a general instance TLAS on the primary software shadow-ray path.

The initial production path is:

```text
exact camera shadow AABB
    -> toroidal world-chunk directory
    -> regular chunk DDA
    -> one immutable Tree64 per occupied chunk
    -> any-hit shadow result
```

The default chunk side is 64 coarse cells. A sliding 512-cubed exact shadow AABB requires a 9 x 9 x 9 resident directory: eight chunks cover each aligned axis and one extra chunk provides the alignment halo when the exact AABB is not chunk-aligned. The 729-entry directory is directly indexed, remains small, and scrolls toroidally. Camera motion inside the current 64-cell chunk does no structural work. Crossing a chunk boundary replaces only the entering chunk slab and retires the leaving slab.

Static and streamed geometry is baked into world-aligned Tree64 chunks. A transformed instance initially invalidates only the chunks intersecting the union of its old and new world bounds; those chunks are cleared and rebuilt from all current contributors. This keeps shadow traversal free of a global TLAS and preserves the current conservative world-grid occupancy semantics.

If transform benchmarks show that rebaking frequently moving objects is too expensive, add a second dynamic layer:

```text
chunk descriptor
    -> static world Tree64 root
    -> short dynamic-instance list
         -> shared model-local Tree64 roots
```

Linear dynamic lists are used while they remain short. A per-chunk micro-BVH is allowed only after measured list lengths and trace cost justify it. A global software TLAS is a diagnostic comparison backend, not the default design.

Keep the current packed occupancy volume and DDA available as the correctness and performance baseline until every cutover gate in this plan passes. Do not delete it early.

## Goals

1. Make update work proportional to newly entering, newly resident, removed, or explicitly invalidated chunks rather than the complete camera-centered volume.
2. Make stationary and within-chunk camera motion perform zero Tree64 build, allocation, upload, or publication work.
3. Keep the common software shadow-ray path regular: chunk DDA, one directory lookup per crossed chunk, contiguous Tree64 node reads, and any-hit early termination.
4. Preserve conservative shadow safety. A published clear region must contain no represented source occupancy. Incomplete or failed updates must not create false-empty space or light leaks.
5. Support added, removed, transformed, and residency-changed instances without rebuilding unrelated chunks.
6. Bound node memory, build scratch, dynamic references, update queues, and retired allocations before scheduling work.
7. Publish complete generations only. Readers must never observe a new directory entry that points to an incomplete or already recycled tree.
8. Remove all device-idle waits from normal update, publication, and reclamation paths.
9. Retain the existing half-resolution surface-to-light ray generation, result/debug ABI, and renderer isolation while the new backend is validated.
10. Produce repeatable CPU, GPU, memory, traversal, and update benchmarks with machine-readable output.

## Non-goals

- Do not change the primary sparse-voxel rasterizer, VSVO file format, authored material data, G-buffer, depth, LOD selection, or streaming-page contract.
- Do not serialize the derived world Tree64 chunks into VSVO in the first implementation.
- Do not make primary rendering correctness depend on shadow acceleration.
- Do not claim exact authored-surface shadows. The first Tree64 backend stores the same conservative, material-free coarse occupancy meaning as the current packed volume.
- Do not add a global software TLAS to the default shadow path merely to simplify transformed instances.
- Do not mutate compact child arrays in place while a frame can read them.
- Do not free or overwrite a node slot based only on CPU frame number; reclamation must be tied to completed GPU work.
- Do not accept a faster result that introduces a false-empty cell, stale transformed instance, stale removed instance, or generation tear.

## Current baseline

The existing path already provides useful seams:

- `SparseVoxelOccupancyVolume` computes camera-cell, exact camera-relative position, pending origin, toroidal wrap, scene revision, and explicit invalidation metadata.
- `SparseVoxelOccupancyMipChain` consumes the resident instance, BLAS-root, hierarchy-node, and center/length buffers; traverses current resident scene data on the GPU; splats conservative leaf AABBs; and publishes a base occupancy texture plus packed hierarchy.
- `SparseVoxelOccupancyShadowRayPass` emits the desired half-resolution surface-to-light rays.
- `SparseVoxelOccupancyDDAPass` consumes those rays and writes a result record already used by debug presentation and readback.
- The sample has opt-in RenderGraph GPU timestamps, a CPU phase timeline, bounded CLI runs, frame signatures, moving-camera fixtures, transformed-instance invalidation, and streaming-page integration.

At the default 512 x 512 x 512 resolution:

```text
base packed bits          = 16 MiB
atomic mutation mirror    = 16 MiB
base + mirror             = 32 MiB
```

That excludes the packed occupancy hierarchy and the dense dirty-mask/index scratch allocations. Record the actual compiled RenderGraph allocation total before implementing Tree64; estimates are not an acceptable baseline.

The existing implementation status documents two update blockers for genuinely dirty frames: instance-array scanning before BLAS traversal and complete hierarchy rebuilding after any non-empty base update. Tree64 should be evaluated against the measured current implementation, not against an assumed full-volume rebuild.

[VoxelRT](https://github.com/dubiousconst282/VoxelRT), inspected at revision [`e605812840a9f6abbadf84a436f8602e2972ee43`](https://github.com/dubiousconst282/VoxelRT/tree/e605812840a9f6abbadf84a436f8602e2972ee43), is the algorithm and implementation reference. Its Tree64 node is 12 bytes with a packed child pointer and 64-bit population mask, and its renderer rebuilds complete 64-cubed chunks on change. Its allocator path at that revision waits for device idle before recycling old ranges. Reuse the node/traversal ideas under the MIT notice, but do not copy the idle-wait reclamation behavior.

## Target architecture

### Coordinate systems

Keep these coordinates distinct:

- `globalCell`: signed world-aligned coarse occupancy cell.
- `worldChunk`: mathematical floor division of `globalCell` by `chunkSide`.
- `chunkCell`: positive local coordinate in `[0, chunkSide)`.
- `logicalChunk`: directory coordinate relative to the current camera-centered chunk range.
- `physicalSlot`: toroidal directory slot.
- `nodeSlot`: fixed-capacity Tree64 storage slot owned by one published chunk generation.

All CPU and WGSL helpers must use mathematical floor division for negative coordinates. Directory identity is the full signed `worldChunk`, not only the physical toroidal slot. Every physical directory entry stores its expected world coordinate so stale wrapped entries are rejected.

The exact represented shadow AABB remains centered on the camera's containing coarse cell. For each axis, allocate `ceil(logicalCellResolution / chunkSide) + 1` resident chunks. The extra chunk is an alignment halo that lets the resident directory remain unchanged while the exact AABB moves within its current world chunk. Rays clip against the exact shadow AABB before chunk traversal, so a retained or halo chunk cannot cast from outside the configured range.

### Chunk size

Support power-of-four chunk sides, initially 16 and 64, so every Tree64 level has a natural 4 x 4 x 4 branching factor.

Use 64 as the initial production candidate:

- a sliding 512-cubed region has 729 resident directory entries, including the one-chunk alignment halo on each axis;
- a ray crosses at most a bounded small number of chunk planes through the region;
- a full 64-cubed binary chunk contains at most 4,161 nodes:

```text
1 root at scale 64
64 internal nodes at scale 16
4,096 terminal-mask nodes at scale 4
```

At 12 bytes per node the worst packed payload is 49,932 bytes; at the 16-byte experimental stride it is 66,576 bytes. Represent a full-capacity slot as 4,161 nodes rather than assuming a 64 KiB byte size. Fixed-capacity slots eliminate variable-range fragmentation and avoid a GPU-to-CPU node-count synchronization before allocation.

Keep 16-cubed chunks as a benchmark variant:

- at most 65 nodes per chunk;
- 65 nodes maximum, or 780 bytes at 12-byte stride and 1,040 bytes at 16-byte stride;
- more directory entries and more chunk-boundary traversal;
- finer invalidation and lower rebuild amplification.

Do not promote either size solely from synthetic results. Sponza, sparse fixtures, camera motion, and transformed-instance sequences decide the default.

### GPU records

Introduce reflected CPU/WGSL records equivalent to:

```cpp
struct SparseVoxelTree64NodeGPU
{
  uint32_t childBaseAndFlags;
  uint32_t populationMaskLo;
  uint32_t populationMaskHi;
};

struct SparseVoxelTree64ChunkGPU
{
  int32_t worldChunk[3];
  uint32_t nodeSlot;
  uint32_t rootNodeOffset;
  uint32_t nodeCount;
  uint32_t contentGeneration;
  uint32_t flags;
};

struct SparseVoxelTree64StateGPU
{
  uint32_t logicalChunkResolution[3];
  uint32_t chunkSide;
  int32_t originWorldChunk[3];
  uint32_t valid;
  uint32_t wrapOffset[3];
  uint32_t contentGeneration;
  int32_t originCell[3];
  uint32_t reserved0;
  uint32_t logicalCellResolution[3];
  uint32_t reserved1;
  float cellSize[3];
  uint32_t reserved2;
  float worldExtent[3];
  uint32_t overflowStage;
};
```

The exact field order may change after reflection tests, but the meaning may not.

Node rules:

- bit zero of `childBaseAndFlags` identifies a terminal-mask node;
- the remaining bits address a child block relative to the owning chunk slot;
- terminal-mask nodes use only the 64-bit population mask; there is no material payload;
- children selected by a population mask are contiguous and addressed by prefix popcount;
- mask operations use two `u32` words in portable WGSL;
- node indices are local to a chunk slot so a chunk blob can be copied, retired, and reused without patching unrelated chunks.

Benchmark both 12-byte and 16-byte node strides. Twelve bytes minimizes payload; sixteen bytes may improve aligned/vectorized loads on some GPUs. The default is selected by measured trace bandwidth and total memory, not assumption.

### Directory and node-slot pool

The directory is a persistent buffer with one descriptor per physical toroidal chunk slot. Each descriptor is independently publishable after its replacement tree is complete.

Start with a node pool of fixed-capacity chunk slots:

- 4,161 nodes per 64-cubed chunk candidate;
- 65 nodes per 16-cubed chunk candidate;
- byte capacity derived from the selected 12 or 16-byte node stride;
- explicit configurable memory ceiling;
- free-slot stack or bitmap;
- high-watermark and failed-allocation counters;
- no individual node frees;
- old slots enter a retirement queue after publication;
- a retired slot returns to the free pool only after the last GPU submission that could reference its directory generation has completed.

During development, CPU slot ownership may be authoritative if it does not require waiting for the GPU. Production build dispatches receive already reserved destination slots. GPU-side allocation is optional and must not be added unless CPU reservation becomes a measured bottleneck.

Fixed-capacity slots are the correctness-first allocator, not an exemption from the memory gate. If internal waste or the in-flight retirement reserve prevents cutover, add a bounded size-class arena after mask reduction computes the exact node count. Allocation and compaction remain GPU-resident or use delayed batched metadata; they must not introduce a per-update GPU-to-CPU synchronization. Preserve a contiguous node range per chunk and the same generation-safe retirement contract.

### Build scratch

Build one binary occupancy tree per dirty chunk from fixed-layout scratch:

- two `atomic<u32>` words per terminal 4-cubed block, representing one 64-bit mask;
- parent occupancy/count scratch for each higher Tree64 level;
- dirty-chunk coordinate and destination-slot arrays;
- overflow state and indirect dispatch arguments.

For a 64-cubed chunk, terminal masks require 4,096 x 8 bytes = 32 KiB per concurrently built chunk. Bound the number of chunks built in one batch. Reuse scratch across batches rather than allocating for every active chunk.

### Conservative source generation

Reuse the current resident scene inputs and conservative leaf-AABB rule:

1. Clear every dirty chunk's build scratch.
2. Traverse current resident instance/BLAS hierarchy once against the dirty chunk set.
3. Reject nodes that overlap no dirty chunk.
4. For each reached resident leaf, compute its conservative world AABB.
5. Intersect it with every overlapped dirty chunk.
6. Atomically OR covered cells into that chunk's terminal 4-cubed masks.
7. Reduce masks bottom-up and compact the complete chunk into its reserved node slot.

Use the existing scene TLAS only in this update/build pass to find contributors. It is not traversed per shadow ray, so update-time divergence is amortized over all shadow rays.

Removal and movement clear and reconstruct affected chunks from all current contributors. Never attempt to clear a world occupancy bit merely because one instance left it; another instance may still contribute to the same bit.

### Shadow traversal

Keep ray generation and result consumption unchanged during the migration.

The Tree64 trace pass:

1. Reject disabled, invalid, or negative-distance rays.
2. Intersect the ray with the exact shadow AABB.
3. Initialize a signed world-chunk DDA at the clipped entry point.
4. For each crossed chunk:
   - map `worldChunk` to the toroidal directory slot;
   - verify the descriptor's stored world coordinate and generation;
   - skip empty chunks directly to the next chunk plane;
   - normalize position into the owning Tree64 chunk;
   - run mirrored Tree64 any-hit traversal;
   - return on the first occupied terminal cell.
5. Return miss at the exact shadow AABB exit.

Preserve robust boundary ownership from the current occupancy DDA:

- exact-boundary selection happens in cell/chunk coordinate space;
- zero direction components use infinite exit distance;
- tied edge/corner crossings advance all tied axes;
- progress uses the next representable distance after an exit plane;
- negative world coordinates and large camera-relative positions use checked signed coordinates rather than subtracting large absolute floats in the shader.

Start with a private fixed traversal stack. Benchmark a workgroup-shared per-lane stack only after correctness; VoxelRT's shared-stack win is not assumed to transfer to this WGSL compiler or target GPU.

### Unknown and failure policy

A missing entering tree, pool exhaustion, build overflow, stale descriptor, or incomplete generation must not publish clear space.

During dual-backend migration, fall back to the current packed volume for the affected frame or chunk.

After cutover, use one of these explicit conservative policies:

- retain the previous complete directory generation if it still covers the exact requested region; or
- publish the affected chunk as `unknown/occluding`, causing an approximate shadow hit until its tree is available.

Never silently publish an empty root for failed work.

## Incremental update behavior

### Stationary and within-chunk camera motion

- Update exact camera metadata used for ray clipping.
- Do not dirty chunks.
- Do not allocate slots.
- Do not dispatch source traversal or Tree64 builds.
- Do not increment chunk content generations.

### Camera crossing one chunk boundary

- Compute entering and leaving world-chunk slabs.
- Reserve slots only for entering chunks that have possible contributors.
- Build entering chunks.
- Publish new descriptors only after their trees complete.
- Update the toroidal origin/wrap under one committed state generation.
- Retire leaving and replaced slots after publication.
- Retained chunk descriptors and node slots remain byte-identical.

### Diagonal camera movement

Construct the nonduplicated union of entering X/Y/Z chunk slabs. A chunk is built at most once per update. A chunk leaving on more than one axis is retired once.

### Teleport or settings change

If the new active chunk range has no useful overlap with the old range, create a fresh pending directory generation. Reuse complete matching world chunks only when identity, cell size, chunk side, source scene revision, and conservative occupancy semantics all match.

Until the new generation is complete, retain the old complete backend or use conservative unknown chunks. Do not destructively clear the committed directory first.

### Added or newly resident scene section

- Convert its world bounds to intersected active world chunks.
- Mark only those chunks dirty.
- Rebuild them from all current resident contributors.
- Do not touch unrelated chunks.
- If the added section is outside the active camera range, record the scene/residency revision but build nothing.

### Removed or evicted scene section

- Use its prior published world bounds to mark intersected active chunks dirty.
- Rebuild those chunks without the removed contributor.
- Publish replacements before retiring old chunk slots.
- If complete prior bounds are unavailable, conservatively rebuild every active chunk rather than leaving stale occupancy.

### Transformed instance: rebake policy

- Compute conservative old and new world AABBs.
- Dirty the union of active chunks intersecting either AABB.
- Rebuild only that chunk union from all current contributors.
- Translation, rotation, reflection, non-uniform scale, and a transform crossing the exact volume boundary use the same path.
- A transform revision with missing old bounds forces a full active-range rebuild.

This is the required first production transform policy because it keeps ray traversal simple and preserves current conservative occupancy semantics.

### Frequently transformed instances: optional dynamic policy

Implement only after the rebake benchmarks are captured.

An instance may migrate to a dynamic layer when a documented policy is met, for example:

- it changes transform in consecutive frames;
- its rebake chunk count exceeds a threshold;
- its measured rebake time exceeds the configured update budget.

The dynamic layer requires:

- one shared model-local binary Tree64 per resident model cut;
- per-active-chunk compact dynamic instance ranges;
- instance transform and inverse transform records;
- old/new chunk-membership updates;
- no duplicate instance reference within one chunk;
- a linear list for short ranges;
- an optional per-chunk micro-BVH only when benchmarked list lengths justify it.

The ray traverses the static world chunk and the dynamic references associated with that same chunk, then returns the closest/first valid blocker. Do not renormalize the world-to-model transformed direction; preserving `localOrigin + t * localDirection` preserves the world ray parameter.

Migration between baked and dynamic representations must be transactional:

1. build/publish the destination representation;
2. switch one instance policy generation;
3. invalidate/rebuild old baked chunks or remove old dynamic references;
4. retire old resources after GPU completion.

At no point may the instance disappear from both representations or exist in both in a way that violates diagnostics.

## RenderGraph pass layout

The production pass chain should converge on:

```text
CPU PrepareTree64Update
    -> pending exact camera/chunk state
    -> entering/leaving/invalidated world chunks
    -> reserved destination node slots

GPU ClassifyDirtyTree64Chunks
    -> stable unique dirty chunk list
    -> build batches and indirect arguments

GPU ClearTree64BuildMasks[batch]

GPU SeedTree64SceneTraversal
GPU TraverseTree64DirtyTLAS[level]
GPU TraverseTree64DirtyBLAS[level]
    -> conservative reached-leaf splat into per-chunk terminal masks

GPU BuildTree64TerminalNodes[batch]
GPU BuildTree64Parents[level][batch]
GPU FinalizeTree64Chunks[batch]
    -> node count, root offset, checksum/debug metadata

GPU PublishTree64Directory
GPU CommitTree64State

GPU GenerateOccupancyShadowRays
GPU TraceTree64Shadows
GPU PresentShadowDebug
```

Publication passes depend on every build pass that writes their destination slots. Trace depends on committed directory/state publication. No pass may overwrite a slot referenced by the committed directory.

The current packed-volume build and DDA remain schedulable under an explicit backend selector during migration:

```text
--shadow-backend packed-dda
--shadow-backend tree64
--shadow-backend dual-validate
```

`dual-validate` runs both consumers on the same ray buffer, compares results, and presents the selected backend. It is for bounded tests and profiling, not normal interactive rendering.

## Proposed source layout

Names may be adjusted to repository conventions, but keep ownership separated:

```text
src/voxel/svraster/
  SparseVoxelTree64.hpp/.cpp
  SparseVoxelTree64Builder.hpp/.cpp
  SparseVoxelTree64Directory.hpp/.cpp
  SparseVoxelTree64Reference.hpp/.cpp

src/voxel/svraster/rendering/
  SparseVoxelTree64DirtyPass.hpp
  SparseVoxelTree64ScenePass.hpp
  SparseVoxelTree64BuildPass.hpp
  SparseVoxelTree64PublishPass.hpp
  SparseVoxelTree64TracePass.hpp
  SparseVoxelTree64ComparePass.hpp

assets/shaders/voxel/svraster/wgsl/
  sparse_voxel_tree64_common.wgsl
  sparse_voxel_tree64_dirty.wgsl
  sparse_voxel_tree64_scene.wgsl
  sparse_voxel_tree64_build.wgsl
  sparse_voxel_tree64_trace.wgsl
  sparse_voxel_tree64_compare.wgsl

tests/voxel/
  SparseVoxelTree64BuilderTests.cpp/.cmake
  SparseVoxelTree64TraversalTests.cpp/.cmake
  SparseVoxelTree64GPUTests.cpp/.cmake
  SparseVoxelTree64StreamingTests.cpp/.cmake
  SparseVoxelTree64Benchmark.cmake

scripts/
  benchmark_sparse_voxel_tree64.py
```

Avoid adding a dependency from the new voxel renderer to `runtime/softwarebvh`. Any optional micro-BVH should be a narrow material-free structure owned by this subsystem unless profiling proves that a format-neutral shared primitive is appropriate.

## Implementation stages and promotion gates

### T0: Freeze the baseline

Add no Tree64 code yet.

Implementation status: the packed-DDA JSON capture path, warm-up exclusion,
raw CPU/GPU samples, aggregate statistics, environment/memory metadata, final
ray/result/output hashes, deterministic static/camera-yaw/transform sequences,
and GPU smoke test are implemented. Full release captures across the promotion
matrix remain pending.

- Add stable backend-independent ray/result capture for current occupancy shadows.
- Add bounded benchmark mode that avoids per-frame visibility/page readbacks except at final aggregation.
- Record current GPU pass times, CPU update/upload time, memory allocation totals, query counts, and result hashes.
- Capture the reference device, driver, build type, resolution, scene hash, occupancy settings, and git commit.

Promotion gate:

- repeated runs produce the same result hashes and bounded metric variance;
- the baseline report includes static, camera-motion, transform, streaming-add, removal, and teleport sequences;
- the benchmark output is machine-readable.

### T1: CPU Tree64 format and independent reference

- Implement checked node/directory/state layouts.
- Implement signed global-cell/chunk mapping and toroidal directory mapping.
- Implement a deterministic CPU chunk builder from scalar occupancy cells/AABBs.
- Implement an independent scalar Tree64 traversal.
- Add validation for child ranges, mask/popcount consistency, root reachability, node uniqueness, and slot bounds.

Implementation status: complete for 4, 16, and 64-cell reference chunks.
The checked 12-byte node and 32-byte directory ABI, signed/toroidal coordinate
helpers, deterministic scalar-cell/AABB builder, structural validator,
hierarchical traversal, and independent cell-DDA oracle are covered by analytic,
maximum-density, corruption, negative-coordinate, exact-boundary, and
fixed-seed randomized tests.

Promotion gate:

- hand-authored empty, single-cell, full terminal node, sparse multilevel, full chunk, negative-coordinate, and boundary fixtures pass;
- randomized CPU Tree64 traversal matches scalar cell DDA first-hit results;
- builder output is deterministic byte-for-byte.

### T2: Static GPU traversal

- Upload CPU-built chunk trees.
- Add chunk DDA and WGSL Tree64 traversal.
- Preserve the existing shadow-ray and result ABI.
- Add query/node/chunk-step diagnostics.

Implementation status: complete and accepted for the static reference path.
CPU-built chunks are uploaded into fixed-capacity slots, addressed through a
generation-checked toroidal directory, and traced by a chunk DDA plus
hierarchical Tree64 region marcher. The pass reuses the packed-DDA ray/result
ABI and reports node visits, hierarchy level, chunk steps, and query count.
Analytic boundary rays and 512 fixed-seed random rays across negative chunk
coordinates and non-zero wrap offsets match both an independent scalar-cell
oracle and a direct packed-DDA GPU execution of the identical occupancy
fixture.

The original T2 benchmark matrix covered packed 12-byte and padded 16-byte
nodes with a private candidate stack, plus packed nodes with a 32-KiB
workgroup-shared candidate stack. Three submit/wait-inclusive local runs for
16,736 rays measured approximately
44.9-51.4 ms for packed/private, 42.2-44.2 ms for padded/private, and
119.3-119.9 ms for packed/shared. The small synchronized batch cannot
distinguish the two private node strides reliably, while the shared candidate
is roughly 2.5x slower and restricts the workgroup to 16 lanes. Packed/private
was selected for the initial reference because it preserved correctness, used
25% less node memory than padded nodes, and avoided the measured shared-stack
regression. The later fast region marcher removes this candidate stack
entirely; see the T8 traversal specialization below.

Promotion gate:

- analytic and fixed-seed GPU rays match the independent scalar oracle;
- no query-limit, stack-overflow, or non-progress result occurs;
- packed-DDA and Tree64 agree on hit/miss and first conservative cell for identical scalar occupancy;
- 12-byte/16-byte and private/shared-stack variants are benchmarked before choosing defaults.

### T3: GPU chunk build

- Add terminal-mask splat, bottom-up parent generation, compaction, and fixed-slot writes.
- Initially build one chunk per dispatch batch.
- Expand to bounded multi-chunk batches after single-chunk parity.

Implementation status: complete and accepted for the bounded GPU chunk-build
reference path. The original transactional single-chunk scalar kernel remains
as a narrow publication/overflow oracle. The production-oriented reference is
a reusable six-stage RenderGraph chain: clear chunk-local cell scratch, splat
chunk-local conservative AABB sources, generate terminal masks, reduce middle
and root masks, and compact canonical nodes into caller-reserved fixed slots.
Command dispatches are recorded at immutable chain capacities while uniforms
select the active job/source prefixes, so later streaming updates can reuse the
compiled chain without re-recording commands.

The batched path supports 4-, 16-, and 64-cell chunks and multiple independent
jobs per submission. Empty, full-density, overlapping, adaptive, transformed,
and fixed-seed randomized source fixtures match CPU node bytes exactly. A
same-graph build-to-trace test validates the generated node pool through the T2
GPU traversal. Canary nodes prove that successful builds do not write beyond
their slots. Forced preflight overflow reports the required node count and
leaves the prior sentinel slot byte-for-byte unchanged. Scene-buffer traversal,
dirty-chunk selection, publication, and retirement intentionally remain T4-T6
work.

Promotion gate:

- GPU-built node bytes and traversal results match the CPU builder or satisfy a documented canonical-equivalence rule;
- empty, full, randomized, overlapping-source, adaptive-leaf, and transformed-source chunks pass;
- build overflow leaves the prior committed chunk readable;
- no destination write exceeds its reserved slot.

### T4: Toroidal directory and camera streaming

- Add pending/committed chunk states.
- Add entering/leaving slab planning.
- Add exact AABB clipping and directory-coordinate validation.
- Add deferred slot retirement tied to completed GPU submissions.

Implementation status: complete and accepted for the reference runtime path.
The CPU directory manager derives the exact camera-centered cell AABB and its
one-chunk alignment halo, classifies deterministic retained/entering/leaving
sets, preserves retained physical toroidal slots, and emits T3 build jobs plus
GPU publication records only for entering chunks. Motion within the current
world chunk updates exact clipping metadata without allocating slots or
scheduling builds. Positive, negative, diagonal, wrapped, and teleport plans
are covered.

Node slots move through explicit free, reserved, committed, and retired states.
Old and failed-build destination slots are reclaimed only after the caller
reports completion of a monotonically ordered RenderGraph/RHI submission
token; the manager performs no device-idle wait. A forced three-submission
overlap exhausts the spare pool, proves that none of the retired slots are
reused early, and then reuses only the first completed retirement set.

The GPU publication pass updates only entering physical directory entries and
the target state. Successful builds publish their exact node bounds and
occupancy flag. Invalid or overflowed builds publish a valid
`unknown/occluding` descriptor, preventing false-empty space. The strict CPU
policy can instead abort and retain the prior complete generation. Chunk build
generations are per descriptor; the state generation identifies the directory
publication, allowing retained descriptors and node payloads to remain
byte-identical across scrolls while full signed world-chunk identity rejects
stale wrapped entries.

Promotion gate:

- stationary and within-chunk motion report zero build work;
- positive, negative, diagonal, wrapping, and teleport paths match fresh complete builds after every published generation;
- retained chunks keep identical descriptors and node bytes;
- no normal path calls `waitIdle`;
- forced three-frame overlap proves retired slots are not reused early.

### T5: Scene/residency incremental rebuild

- Connect current resident instance/BLAS buffers directly.
- Add dirty-chunk scene traversal and conservative leaf splat.
- Handle add, remove, selective page install, page eviction, and scene replacement.

Implemented reference:

- `SparseVoxelTree64SceneBuildChain` consumes the renderer's resident instance,
  flattened hierarchy-node, center/length, and BLAS-root buffers directly.
- The GPU scans only the active dirty-job prefix, traverses intersecting BLAS
  nodes with bounded indirect queues, and splats reached leaves directly into
  T3 cell scratch. No CPU leaf list or per-shadow-ray TLAS is introduced.
- Queue overflow fails closed by filling the affected build cells before
  compaction. GPU counters expose instances tested, nodes tested, leaves
  reached, chunk splats, cell writes, and overflow.
- Complete old/new invalidation bounds classify a stable unique active chunk
  set. Changes outside the active directory schedule zero builds. Missing
  prior bounds select the complete active range.
- T4 now reserves replacement slots for dirty retained chunks, publishes only
  the entering/rebuilt union, keeps unrelated retained descriptors byte
  identical, and retires replaced slots only after the supplied completion
  token.
- Scene-bound publication carries an exact scene revision. A completion for a
  different revision is rejected while the committed directory remains
  unchanged.
- CPU and Vulkan tests cover selective install spanning chunks, eviction from
  the current resident hierarchy, overlapping retained contributors,
  out-of-range changes, safe complete fallback, stale completion rejection,
  exact GPU/CPU Tree64 parity, and machine-readable classification/traversal
  counters.

Promotion gate:

- every incremental result matches a fresh rebuild of the same resident scene cut;
- changes outside the active region schedule zero chunk builds;
- one newly resident page rebuilds only intersected active chunks;
- missing invalidation bounds force a safe complete active-range rebuild;
- streaming completion from a stale scene generation cannot publish.

### T6: Transformed-instance rebake

- Connect old/new instance bounds.
- Dirty only their active chunk union.
- Rebuild from all current contributors.

Implemented reference path:

- `SparseVoxelTree64TransformTracker` captures stable instance IDs, transforms,
  BLAS identities, and conservative world bounds relative to the last
  successfully published Tree64 generation, rather than the previous CPU
  frame.
- Prepare/commit/abort transactions preserve the published snapshot while a
  build is in flight. Scene, topology, or transform changes reject stale
  completion and allow replanning from the last published state.
- Matrix changes mark an instance dirty even when a symmetric root AABB is
  unchanged. Added, removed, translated, rotated, reflected, non-uniformly
  scaled, and topology-replaced instances all feed the same old/new active
  chunk-union classifier.
- The directory bridge carries the exact target scene revision. Missing prior
  bounds fail closed to a complete active-range rebuild.
- The resident-scene GPU build chain accepts active instance, hierarchy, and
  BLAS counts within fixed buffer capacities, so removal and scene replacement
  do not require RenderGraph reconstruction.
- CPU tests cover chunk and active-volume boundary crossings, no-op and
  out-of-range transforms, overlap retention, removal after movement, topology
  replacement, and stale in-flight publication. The Vulkan test rebuilds each
  dirty chunk from every current contributor and compares its node bytes with a
  fresh CPU Tree64.
- Machine-readable `TREE64_T6_BENCH` output captures planning plus complete
  reference rebake work for small, medium, large, and overlapping cases.
  `TREE64_T6_GPU` output captures synchronized GPU rebuild latency and traversal
  counters for transformed correctness variants. These are focused smoke
  measurements; the optimized renderer-level p50/p95/p99 campaign remains part
  of the cutover benchmark work.

Promotion gate:

- translation, rotation, reflection, non-uniform scale, movement across chunk/volume boundaries, overlap with another instance, and removal after movement match a fresh rebuild;
- clearing one contributor never clears occupancy retained by another;
- transform-only frames do not touch unrelated chunks;
- rebake performance is captured for small, medium, large, and overlapping instances.

### T7: Dynamic per-chunk instance layer

This stage is conditional on T6 benchmark results but is part of the full target if rebake cannot meet the transform budget.

- Build shared model-local Tree64 roots.
- Add dynamic instance membership and per-chunk ranges.
- Add linear dynamic traversal.
- Add policy migration and conservative failure handling.
- Benchmark but do not automatically add micro-BVH construction.

Implemented reference path:

- Model-local Tree64 payloads are immutable and indexed by a compact model
  descriptor. Multiple instances reference one model node range; transform-only
  updates contain no model descriptors or node bytes.
- Dynamic instances carry `worldToModel`, conservative world bounds, stable
  instance identity, model index, and publication generation. Rays are
  transformed without direction normalization, preserving the world-space ray
  parameter under rotation, reflection, and non-uniform scale.
- Every physical chunk-directory slot has a generation-stamped dynamic
  membership descriptor and a compact range of instance indices. Membership
  construction is deterministic and rejects duplicate instance references.
- The packed/private-stack dynamic trace variant tests the static world Tree64
  and the chunk's linear dynamic list, returning the nearest blocker. Invalid
  membership identity, generation, bounds, model range, reference, or capacity
  fails closed as a conservative hit.
- Static-only frames continue to select the unchanged static trace pass. The
  dynamic shader and its additional bindings are used only while dynamic
  instances or a conservative dynamic fallback exist.
- Transactional instance/membership publication rejects stale scene revisions
  and retains the prior complete snapshot until commit. Model, instance,
  reference, or per-chunk-list overflow creates valid conservative-unknown
  membership descriptors rather than stale or false-empty references.
- Destination-first policy migration is enforced for both baked-to-dynamic and
  dynamic-to-baked transitions. Source removal cannot begin until the
  destination is published; rollback is permitted only before publication.
- CPU tests cover model sharing, transform-only payloads, old/new membership
  chunks, removal, stale completion, capacity fallback, and migration order.
  Vulkan tests compare combined static/dynamic traversal with an independent
  transformed-source oracle and inject stale membership generations and
  references.
- Machine-readable `TREE64_T7_BENCH` output compares transform/membership
  publication against the T6 reference rebake for 1, 16, and 64 instances.
  The focused local measurements favor dynamic publication for all three
  tested moving workloads. `TREE64_T7_GPU` captures combined traversal smoke
  timing and sharing/reference counts. Optimized renderer p50/p95/p99 results
  remain required before choosing production migration thresholds.
- No micro-BVH was added. The first dynamic path intentionally remains a
  bounded linear list until representative list-length and ray-cost data
  justify another acceleration structure.

Promotion gate:

- dynamic traversal matches an independent transformed-source oracle;
- model sharing does not duplicate Tree64 payload;
- moving an instance updates only transform/membership buffers;
- no stale reference survives old/new membership publication;
- dynamic lists outperform rebake for the workload that triggered this stage without regressing static-only trace performance.

### T8: Performance specialization

Only optimize measured bottlenecks.

Candidate experiments:

- 16 versus 64 chunk side;
- 12 versus 16-byte nodes;
- private versus workgroup-shared stacks;
- depth-first versus breadth/level-local chunk node layout;
- directional child ordering;
- subgroup active-lane early-out diagnostics;
- multiple dirty chunks per source traversal;
- fixed-capacity slots versus a bounded size-class node arena;
- static/dynamic traversal order based on near chunk interval;
- linear dynamic-list threshold and optional per-chunk micro-BVH;
- compact versus fixed-stride directory records;
- async compute build where RenderGraph dependencies and queue ownership make it safe.

Implementation status: complete and accepted for the reference configuration.

The measured CPU bottleneck was dynamic membership construction. The T7
implementation tested every active chunk for every instance. The runtime now
derives a conservatively expanded chunk-coordinate range from each world AABB,
clips it to the active directory, and applies the original exact closed-AABB
predicate only to those candidates. Reference order is unchanged. A retained
brute-force classifier is exposed only for parity tests and before/after
measurements. The optimized and exhaustive snapshots are byte-identical for
16/64-cell chunks, empty through 257-instance randomized fixtures, non-unit
cell sizes, negative coordinates, and exact boundary touches. On this debug
build's 729-chunk fixture, 20 complete snapshot builds measured:

| Instances | References | Range-specialized | Exhaustive | Speedup |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 6 | 1,712 us | 1,932 us | 1.13x |
| 16 | 43 | 2,153 us | 5,061 us | 2.35x |
| 64 | 200 | 3,425 us | 14,961 us | 4.37x |
| 256 | 791 | 8,358 us | 55,200 us | 6.60x |

The configuration decisions are:

- Retain 64-cell chunks as the default. A same-occupancy 64-cubed GPU fixture
  made 16-cell trees 7-19% faster to trace across four local runs, but the
  production 512-cubed window would grow from 729 to 35,937 descriptors and a
  one-axis scroll from 81 to 1,089 entering chunks. Twenty CPU planner runs
  measured 1,202 us for side 64 and 51,780 us for side 16. Side 16 remains an
  explicit diagnostic/workload specialization, not the camera-streaming
  default.
- Retain packed 12-byte nodes. The T2 synchronized microbenchmark ranges for
  packed/private and padded/private overlap; padded was faster in some local
  runs but costs 33% more node bandwidth/capacity. Promote padded nodes only
  if representative release captures establish a stable frame-time win that
  pays for that memory.
- Reject the original workgroup-shared candidate stack. It was about 2.5x
  slower and reduced the workgroup to 16 lanes in the T2 runs. The final fast
  traversal removes the candidate stack entirely instead of moving it between
  memory classes.
- Retain the T5 single source traversal that splats into all matching dirty
  chunk jobs. Repeating hierarchy traversal once per dirty chunk is rejected.
- Keep fixed slots, static-first dynamic traversal, linear dynamic lists,
  compact references, and the existing node order for now. No production
  high-watermark, long-list, lane-divergence, or queue-overlap measurement
  identifies them as the next bottleneck, so a size-class arena, micro-BVH,
  layout reorder, subgroup specialization, and async compute are deferred
  rather than added speculatively.
- Keep the global TLAS off the default path. T8 found no evidence that its
  extra indirection and divergent memory access would outperform chunk DDA
  plus short local dynamic lists for this workload.

#### Fast Tree64 region traversal

The initial GPU walker intersected every occupied child of each visited node,
pushed full ray/AABB tasks into a 64-entry per-invocation array, and repeatedly
scanned that array for the nearest task. That caused high private-memory
pressure, up to 64 child-box tests per node, and divergent nearest-task scans.

The accepted walker follows the control-flow shape of VoxelRT's fast Tree64
kernel: inspect only the child containing the current ray position, use the
node's 64-bit occupancy mask to classify it, and advance directly to the exit
plane of the largest known-empty child region. It re-descends the shallow
64-cell tree for each skipped region, preserving this renderer's half-open
boundary convention and validation behavior. This removes the 64-entry
candidate array, all-child AABB enumeration, and nearest-task selection without
changing the packed 12-byte node or ray-result ABIs.

On the same 2,052-ray, 32-frame synchronized specialization test:

| Chunk side | Original candidate walk | Region marcher | Reduction | Speedup |
| ---: | ---: | ---: | ---: | ---: |
| 16 | 93,530.9 us | 48,438.5 us | 48.2% | 1.93x |
| 64 | 105,569 us | 45,729 us | 56.7% | 2.31x |

In the 320-by-180 occupancy-cell debug sequence, `Tree64.Trace` GPU time changed
from 0.141 ms median / 3.109 ms p95 to 0.003 ms median / 0.071 ms p95. Total GPU
frame median changed from 11.154 ms to 10.041 ms because unrelated passes
dominate this small synthetic capture. Hit/miss/cell/distance parity remains
covered by the independent CPU oracle and all 17 focused CPU/GPU tests.

A 3D texture is not selected for compact nodes. Node indices are allocation
order, not spatial XYZ coordinates, so texture addressing does not provide a
natural locality mapping. One `rgba32uint` texel would inflate each 12-byte node
to 16 bytes; three `r32uint` fetches retain 12 bytes but add fetches and address
work. A dense spatial texture would also discard the sparse-tree memory
advantage. The packed storage buffer remains the default; the region marcher
reduces bandwidth at the source by fetching only nodes on the current lookup
path.

#### VoxelRT traversal follow-up

The remaining active optimizations from VoxelRT's pinned Tree64 kernel were
implemented as a separate `mirrored-ancestor` specialization:

- mirror positive ray axes so traversal marches in one negative octant;
- retain one ancestor node index per hierarchy level instead of re-descending;
- store the three-index-per-lane ancestor stack in 768 bytes of workgroup
  memory;
- ascend to the common containing ancestor after crossing a region boundary;
- test the `0x00330033` mask pattern and skip an empty 2-by-2-by-2 child group
  in one step.

The full specialization is correct for the static randomized oracle and the
dynamic transformed-instance oracle, but is not the default. This renderer's
trees have at most three node levels, while the measured camera-primary rays
usually terminate after one region query. On the 1280-by-720 occupancy-cell
capture, the full specialization increased `Tree64.Trace` from 0.062 ms to
0.088 ms median and from 0.181 ms to 0.220 ms mean. Five synchronized T2 runs
had a 28.333 ms median for the region marcher and 34.485 ms for the
mirrored-ancestor variant. The paired dynamic fixture was also approximately
3% slower. The extra coordinate transforms and workgroup-stack traffic do not
amortize on these shallow, short traces.

The 2-by-2-by-2 mask skip does amortize and is promoted independently into the
default region marcher. In the same 1280-by-720 capture it reduced
`Tree64.Trace` from 0.062 ms to 0.053 ms median (13.8%) and from 0.181 ms to
0.132 ms mean (27.0%), with identical status counts and hit-cell validation.
The full specialization remains selectable with
`--tree64-traversal mirrored-ancestor` for future deeper-tree or long-ray
workloads; `region` is the measured default.

`TREE64_T8_CPU` and `TREE64_T8_GPU` lines are machine-readable artifacts. The
GPU chunk-size test traces 2,052 identical rays, checks first-hit parity between
both layouts, and reports the median of three 32-frame synchronized runs.
These local debug/synchronized results choose the reference defaults; the
representative release-hardware campaign remains a T9 cutover gate.
The final focused T0-T8 regression ran 17/17 CPU/GPU tests successfully,
including overflow, stale-generation, publication, reclamation, transformed
instance, and independent-oracle coverage.

Promotion gate:

- every accepted optimization has before/after benchmark artifacts;
- correctness and failure tests remain green;
- rejected experiments are documented to prevent accidental reintroduction.

### T9: Cutover and removal

- Make Tree64 the default only after the acceptance criteria below pass.
- Retain `packed-dda` for one stabilization period behind a diagnostic build option.
- Remove the packed occupancy texture, atomic mirror, mip-chain build passes, DDA bindings, obsolete UI, and obsolete tests only after the stabilization period.
- Keep backend-independent ray generation, result ABI, correctness fixtures, and benchmark sequences.
- Update the sparse-voxel migration document to replace the old milestone-4/5 implementation description with the accepted Tree64 architecture and measured results.

Implementation status (2026-07-24):

- `tree64` is now the sample default and `packed-dda` is an explicit diagnostic
  selection.
- The renderer owns a 9-by-9-by-9 toroidal directory of 64-cell chunks and
  submits at most 81 GPU scene-build/publication jobs per frame.
- Packed DDA remains live during the stabilization interval and covers initial
  warm-up or a multi-frame Tree64 invalidation. Tree64 takes over only after the
  current bounded publication queue drains; the transition is logged and
  covered by a bounded GPU test.
- Once Tree64 is complete, packed-DDA maintenance is paused instead of scrolling
  its 512-cubed camera-centered volume on every base-cell crossing. A scene edit
  observed while paused marks the diagnostic fallback for one conservative full
  rebuild if it is selected again.
- Entering Tree64 chunks are conservatively tested through the existing CPU
  scene TLAS, with exact instance-bound tests only in overlapping leaves, before
  GPU job submission. This TLAS is control-plane classification and is not added
  to the software ray path. Non-overlapping chunks still receive valid empty
  descriptors, but skip scene traversal, cell emission, and Tree64 construction.
  A one-axis camera crossing therefore publishes at most 81 descriptors and
  builds only the overlapping subset.
- Camera/dirtiness classification, generation ownership, bounded job packing,
  and small buffer writes remain on CPU. Scene traversal, cell emission, mask
  reduction, node compaction, descriptor publication, and ray traversal execute
  on GPU. Benchmark schema 2 reports the Tree64 CPU planning/upload slices and
  directory, reserved node-pool, and build-scratch bytes.
- Removal of packed resources is intentionally deferred until the stabilization
  period and the designated release-hardware performance/memory campaign pass.
  This is a remaining release gate, not silently treated as complete.
- The design reference and MIT notice are retained in
  `docs/voxelrt-attribution.md`.

Promotion gate:

- repository audit finds no live packed occupancy build path outside an explicitly retained reference module;
- default sample, selective streaming, transformed instances, bounded GPU tests, and benchmark smoke tests pass;
- final documentation includes the selected configuration and benchmark report.

## Correctness test plan

### CPU unit tests

- checked layout and memory arithmetic;
- signed floor division and positive modulo;
- world cell to world chunk/chunk cell mapping;
- logical-to-physical directory bijection;
- stale toroidal slot rejection;
- node mask bit tests and prefix popcount for all 64 child indices;
- empty, single-child, low/high-mask, full-mask, and maximum-node chunks;
- deterministic builder byte output;
- old/new camera chunk slab union;
- old/new transformed AABB chunk union;
- overlapping contributor removal;
- slot allocation, retirement, delayed reuse, and pool exhaustion;
- generation wrap/overflow rejection;
- scalar Tree64 versus scalar DDA randomized equivalence.

### GPU pass tests

- reflected CPU/WGSL ABI;
- manually uploaded tree traversal;
- GPU terminal-mask splat versus scalar occupancy;
- GPU bottom-up build versus CPU validation;
- negative origins and exact boundaries;
- zero-axis and tied edge/corner rays;
- rays beginning inside occupied cells;
- maximum distance and exact volume clipping;
- empty/full/random chunk patterns;
- chunk DDA across empty/populated/empty sequences;
- directory wrap and stale-coordinate poison;
- forced private-stack capacity failure;
- build scratch and node-slot overflow;
- pending/committed generation mismatch;
- retained old generation after injected failure;
- three or more frames in flight with delayed slot reuse.

### Integration tests

- generated exterior shell;
- Suzanne, Stanford bunny, and Sponza OBJ-derived scenes;
- selective VSVO root-only residency followed by refinement;
- residency install and eviction inside/outside the active range;
- stationary camera;
- sub-cell motion;
- motion inside one chunk;
- positive/negative one-chunk crossing;
- diagonal crossing;
- teleport;
- resize and occupancy setting change;
- transformed instance old/new invalidation;
- overlapping transformed instances;
- instance addition and removal;
- scene replacement with stale async completion;
- empty scene;
- unknown/occluding conservative fallback;
- dual-backend result comparison.

Correctness comparisons must distinguish:

- exact parity cases, where both backends represent the same scalar occupancy;
- conservative source cases, where Tree64 must not miss a source-overlapped cell even if the current volume over-shadows differently;
- dynamic model-local cases, which compare against transformed source geometry rather than requiring parity with a world-AABB approximation.

No golden image may be updated merely because the acceleration structure changed. Explain every changed shadow result from the representation contract.

## Benchmark framework

### Harness

Extend `SparseVoxelRendererSample` or add a focused executable with:

```text
--shadow-backend packed-dda|tree64|dual-validate|tree64-global-tlas
--tree64-chunk-side 16|64
--tree64-node-stride 12|16
--tree64-stack private|workgroup
--tree64-dynamic-policy rebake|chunk-list|micro-bvh
--benchmark-shadow <output.json>
--benchmark-sequence <name>
--benchmark-warmup <frames>
--benchmark-frames <frames>
--benchmark-repetitions <count>
--benchmark-resolution <width>x<height>
```

The optional `tree64-global-tlas` backend exists only to quantify the user's memory-divergence concern against the selected chunk directory. It is not required for cutover.

Default benchmark method:

- release/optimized build with assertions appropriate for benchmark configuration;
- Vulkan validation disabled for performance runs and enabled in separate correctness runs;
- 120 warm-up frames;
- at least 600 measured frames for static sequences;
- enough repetitions to report between-run variance;
- no per-frame CPU readback or logging;
- timestamp resolve and metric readback batched after measured work;
- fixed camera/instance paths driven by frame index, not wall-clock delta;
- no interactive UI work;
- record swapchain/present separately from offscreen shadow work;
- record device name, vendor/device IDs, driver, subgroup size, timestamp period, CPU, OS, build type, compiler, shader hash, scene hash, settings, and git commit.

Run an instrumented pass-timestamp profile and an uninstrumented end-to-end profile. Timestamp instrumentation overhead must be measured rather than assumed zero.

Write raw per-frame values and aggregate:

- minimum;
- median/p50;
- p90;
- p95;
- p99;
- maximum;
- arithmetic mean;
- standard deviation;
- median absolute deviation.

Performance CI on shared runners reports data but does not gate on timing. Correctness and zero-work counters do gate. Relative performance thresholds run on designated reference hardware or a controlled nightly machine.

### Backend comparisons

Every representative workload compares:

1. current packed hierarchy plus DDA;
2. current mip-0 fallback, where useful to separate hierarchy benefit;
3. chunk directory plus Tree64;
4. Tree64 with transformed-instance rebake;
5. Tree64 with per-chunk dynamic lists if T7 is implemented;
6. optional global TLAS plus model-local Tree64 diagnostic;
7. 16 versus 64 chunk size;
8. 12 versus 16-byte node stride;
9. private versus workgroup stack after T2.

All backends consume the same ray buffer and configured shadow distance. Dual runs validate results but are excluded from standalone performance comparisons.

### Workload matrix

#### Static traversal

- empty active region;
- one occupied cell;
- generated 128-cubed exterior shell;
- randomized occupancy at 0.1%, 1%, 10%, 50%, and 100%;
- Suzanne;
- Stanford bunny;
- Sponza fully resident;
- Sponza selective/coarse resident cut;
- rays with 0%, 25%, 50%, 75%, and 100% blocker rate;
- directional coherent rays;
- camera-primary rays as an incoherent diagnostic;
- randomized rays for worst-case divergence diagnostics.

#### Camera updates

- stationary;
- sub-cell motion;
- continuous movement inside one chunk;
- one-axis crossing every N frames;
- negative-axis crossing;
- diagonal X/Y/Z crossing;
- back-and-forth boundary thrash;
- teleport with no retained chunks;
- teleport with partial world-chunk overlap;
- camera path through empty space;
- camera path through dense Sponza regions.

#### Scene and streaming updates

- add one empty section;
- add one sparse occupied chunk;
- add one dense occupied chunk;
- add a 1, 8, 32, and 64-chunk batch;
- selective page install outside the active range;
- selective page install inside one chunk;
- page install spanning several chunks;
- page eviction with overlapping retained contributors;
- repeated install/evict churn;
- complete scene replacement.

#### Transforms

- one small translated instance;
- one small rotating instance;
- one non-uniformly scaled instance;
- one large instance spanning many chunks;
- 1, 4, 16, 64, and 256 moving instances;
- many instances sharing one model Tree64;
- overlapping moving instances;
- instances crossing chunk boundaries;
- instances entering/leaving the exact camera range;
- transform reversal and boundary thrash;
- rebake versus dynamic-list crossover.

#### Capacity and failure

- node pool at 50%, 90%, 100%, and one allocation beyond capacity;
- retirement backlog under maximum frames in flight;
- maximum dirty chunks per batch;
- scratch batch subdivision;
- dynamic-list threshold and overflow;
- deliberately stale generation;
- injected failed build before publication.

### Required metrics

#### GPU time

- total offscreen shadow subsystem;
- ray generation;
- dirty-chunk classification;
- scene/TLAS source traversal for builds;
- terminal-mask splat;
- parent build/compaction;
- directory publication;
- Tree64 shadow traversal;
- comparison/debug/readback overhead separately;
- total renderer frame;
- present separately.

#### CPU time

- update planning;
- world-bounds and dirty-chunk derivation;
- slot reservation/retirement;
- buffer upload;
- RenderGraph submission;
- GPU wait/synchronization;
- metric readback outside the measured interval;
- total frame.

#### Traversal

- rays launched;
- hits and misses;
- rays per second and nanoseconds per ray;
- chunk steps per ray;
- directory loads per ray;
- Tree64 node visits per ray;
- terminal-mask tests per ray;
- stack pushes/pops and spills;
- maximum traversal iterations;
- skipped world distance;
- subgroup active-lane histogram or sampled active-lane counts where supported;
- dynamic instance candidates per traversed chunk;
- dynamic model-tree traversals per ray;
- early-out rate.

#### Update/build

- dirty chunk count;
- entering, leaving, transformed, residency, and full-rebuild reasons;
- source TLAS/BLAS nodes tested;
- reached leaves;
- terminal masks touched;
- nodes emitted per chunk;
- bytes written;
- build batches;
- slots reserved, published, retired, and reclaimed;
- frames from request to publication;
- zero-work update count;
- fallback/unknown chunk count;
- overflow/failure stage.

#### Memory

- directory bytes;
- node-pool reserved bytes;
- live node-slot bytes;
- live node payload bytes;
- slot internal waste;
- build scratch;
- dynamic reference ranges;
- model-local dynamic Tree64s;
- retirement backlog;
- high-watermark;
- total RenderGraph persistent and scratch allocation;
- current packed-volume baseline allocation under identical settings.

#### Correctness

- hit/miss mismatch count;
- first-cell mismatch count for parity fixtures;
- false-empty count;
- conservative extra-hit count;
- stale generation/descriptor count;
- output hash;
- query/stack/capacity failure count.

### Acceptance thresholds

Correctness and safety are hard gates:

- zero false-empty results against the accepted scalar/source oracle;
- zero stale removed/transformed instance occupancy after completed publication;
- zero generation tears;
- zero out-of-bounds writes under validation;
- zero device-idle waits in normal operation;
- zero early node-slot reuse;
- deterministic result hashes for deterministic sequences;
- every overflow retains a complete prior result or publishes conservative unknown occupancy.

Primary performance gates are relative to the current packed-DDA baseline on the same machine:

- stationary/within-chunk maintenance dispatches perform zero Tree64 build work;
- static Tree64 trace p50 and p95 must not regress by more than 5% on any promoted representative scene;
- geometric-mean static shadow trace time across exterior shell, bunny, and Sponza improves by at least 20%;
- total shadow-subsystem p95 across the representative static set improves by at least 15%;
- a one-axis chunk crossing touches only the entering slab and costs at most 25% of the measured current complete active-volume rebuild;
- adding one sparse chunk or transforming one small instance touches no unrelated chunk and improves update p95 by at least 4x over a measured full active-range rebuild;
- at the accepted 512-cubed settings, total reserved Tree64 directory, node-pool, and persistent build memory is no greater than the current packed-volume persistent plus hierarchy allocation; if fixed-capacity slots miss this gate, size-class allocation is required before cutover;
- reference-hardware node-pool high-watermark stays below 80% for the accepted Sponza path;
- retirement backlog remains bounded by configured frames in flight plus build latency;
- global-TLAS diagnostic is not selected unless it beats the chunk directory on both p95 trace time and total memory for the transformed-instance workload without regressing static Sponza.

Absolute frame budgets should be added only after T0 records the designated reference hardware. Record both target and hard ceiling for:

- static shadow traversal;
- one chunk-slab update;
- one small transformed-instance update;
- 64 moving instances;
- teleport/full active-range build.

Do not hide an update spike behind an average. Camera and transform sequences gate on p95/p99 and maximum publication latency as well as median.

## Benchmark artifacts

Each benchmark run writes JSON with:

```text
schema version
git commit and dirty-worktree flag
device/driver/CPU/OS/compiler/shader identifiers
scene and settings hashes
backend configuration
warm-up/measurement/repetition counts
raw per-frame CPU/GPU/metric samples
aggregate statistics
correctness counters and hashes
memory totals and high-watermarks
```

Add a comparison script that:

- validates compatible configurations;
- computes absolute and relative changes;
- reports confidence/variance indicators;
- generates Markdown and CSV summaries;
- highlights correctness failures before performance;
- refuses to compare different scene/settings hashes unless explicitly overridden.

Check in the benchmark schema and representative summarized reports, not large raw captures from every local experiment. Keep raw artifacts in CI/performance storage.

## Cutover checklist

- [ ] T0 baseline captured on designated reference hardware.
- [x] CPU builder and scalar traversal accepted.
- [x] Static GPU traversal accepted.
- [x] GPU chunk builder accepted.
- [x] Toroidal camera update accepted.
- [x] Streaming add/remove/evict accepted.
- [x] Transformed-instance rebake accepted.
- [x] Dynamic layer implemented or explicitly rejected by benchmark evidence.
- [x] 16 versus 64 chunk decision recorded.
- [x] 12 versus 16-byte node decision recorded.
- [x] Stack placement decision recorded.
- [x] No global TLAS on the default path unless its benchmark gate passes.
- [x] Correctness, validation, failure, and multi-frame reclamation tests green.
- [ ] Relative performance gates green.
- [ ] Memory ceiling and high-watermark gates green.
- [x] Default backend changed to Tree64.
- [ ] Stabilization period completed with packed-DDA diagnostic fallback.
- [ ] Obsolete packed occupancy resources and passes removed.
- [x] Documentation, UI diagnostics, CLI help, and benchmark report updated.
- [x] VoxelRT MIT attribution and notice retained for copied or substantially adapted code.

## Principal risks and mitigations

### Chunk rebuild amplification

A large transformed or adaptive leaf may touch many chunks. Measure dirty chunk count and reached-leaf fanout. Use 16-cubed chunks or the dynamic layer only when the measured crossover warrants it.

### Directory traversal overhead

Small chunks reduce update work but increase chunk steps and directory loads. Benchmark 16 and 64 on the same rays. Do not infer the winner from node counts alone.

### Tree node bandwidth and divergence

Tree64 is shallower than an octree but still pointer-driven. Keep each chunk contiguous, preserve directional child ordering, measure node visits and active lanes, and compare 12/16-byte stride and stack placement.

### Dynamic-list explosion

Large or overlapping dynamic instances may create long per-chunk lists. Expose list histograms and candidate counts. Add a micro-BVH only above a measured threshold; retain a bounded conservative overflow policy.

### Build scratch pressure

Terminal masks are fixed but multi-chunk batches can be large. Bound concurrent builds, use indirect batches, and report deferred chunks rather than exceeding scratch.

### False-empty publication

Generation mismatch, failed allocation, or partial build can leak light. Separate pending and committed state, publish descriptor last, retain old slots until GPU completion, and use packed-DDA or unknown/occluding fallback.

### Allocator fragmentation or stalls

Use fixed-size chunk slots and whole-slot retirement. Avoid general variable-node allocation and avoid readback before allocation. Tie reuse to GPU completion.

### Update-time scene traversal

Rebuilding several chunks could repeatedly traverse the same resident hierarchy. Traverse the dirty chunk union once and route reached leaves to chunk scratch. Add a dirty-chunk hierarchy only if source traversal becomes a measured bottleneck.

### Representation drift

Static world chunks preserve the current conservative AABB occupancy meaning. A future model-local dynamic tree has different, usually tighter semantics. Mark that transition explicitly and compare it with a transformed source oracle rather than silently changing expected hashes.

### Benchmark distortion

Per-frame logging, readback, validation, timer resolve, DVFS, present blocking, and asynchronous streaming can dominate small timings. Separate instrumented/uninstrumented runs, batch readback, record environment, warm up, retain raw samples, and report tails.

## Final recommendation

Proceed in three decision points:

1. Prove that regular chunk DDA plus static Tree64 traversal beats the current packed hierarchy on the actual shadow rays.
2. Prove that 64-cubed incremental chunk builds meet camera and occasional-transform budgets without a trace-time TLAS.
3. Add per-chunk dynamic instance lists only if transformed-instance rebake misses its measured budget.

This sequencing preserves the simplest and most coherent software trace path while making every added layer earn its complexity through benchmarks.
