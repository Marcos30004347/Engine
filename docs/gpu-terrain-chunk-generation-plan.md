# GPU terrain chunk generation implementation plan

**Status:** superseded by
[`gpu-terrain-generation-v2-plan.md`](gpu-terrain-generation-v2-plan.md),
which replaces whole-page jobs with bounded, vertically stackable 3D chunks
and parallel count/scan/emit/link passes.
**Date:** 2026-08-27

This plan moves generated-world terrain page construction from CPU-owned voxel
and hierarchy payloads to a budgeted GPU pipeline. It applies to planar terrain
backed by `MapExportBundle` and the progressive fixed-footprint terrain
streamer. The current CPU generator remains the reference and fallback until
the GPU path passes every correctness, streaming, and performance gate.

The goal is lower request-to-visible latency without turning background terrain
generation into a new frame-time spike. GPU generation is therefore resumable,
generation checked, and explicitly limited every frame.

---

## 1. Decision

Build raster bricks and Tree64 pages from one canonical terrain-column payload.

- The CPU remains the control plane for map file I/O, decompression, request
  prioritization, physical-page reservation, and deferred reclamation.
- The GPU samples resident map fields, evaluates the terrain height transform,
  extracts the exterior shell, and builds both acceleration structures.
- Raster output remains the existing explicit 8-cubed brick octree.
- Shadow output remains the existing explicit 4-cubed-branching Tree64.
- Do not introduce DAG node sharing. Streamed terrain pages are mutable as
  owners, spatially unique, and carry material and canonical-coordinate state;
  subtree deduplication would complicate publication and retirement for little
  expected memory reduction.
- Raster and Tree64 are derived independently from the same immutable column
  payload. Tree64 must not be reconstructed from a separately simplified
  heightfield or from a later raster LOD cut.
- A build may span many frames. No partial result becomes reachable until one
  final generation-checked publication commits raster and Tree64 together.

The first production milestone is a hybrid path: CPU map sampling produces the
canonical columns and the GPU builds both hierarchies. Full GPU map sampling is
a later phase because map residency and sampling parity are the largest pieces
of missing infrastructure.

---

## 2. Goals and non-goals

### Goals

- Reduce generated-page request-to-visible latency, especially during camera
  movement and refinement bursts.
- Remove the CPU `std::map`-based brick hierarchy build from the generated
  terrain hot path.
- Avoid materializing one CPU leaf record for every emitted shell voxel.
- Build raster and shadow occupancy from the same canonical source.
- Preserve cross-page and cross-LOD watertightness, conservative shadows, and
  monotonic raster LOD error.
- Bound GPU compute time, transfer bytes, pages started, pages published, and
  scratch memory per frame.
- Cancel stale work without waiting for it to finish or exposing its output.
- Preserve the last complete resident page on cancellation, overflow,
  allocation failure, shader failure, or publication rejection.
- Retain a deterministic CPU fallback for unsupported hardware, tests, and
  recovery.

### Non-goals

- Replacing raster traversal, Tree64 traversal, or their public node ABIs.
- Sharing terrain subtrees as a DAG.
- Moving world-map file I/O or decompression onto the GPU.
- Editing terrain or persisting player modifications in the first migration.
- Building arbitrary authored voxel models with the terrain-specific builder.
- Requiring CPU and GPU floating-point intermediates to be byte-identical.
  Their quantized terrain ownership, occupancy, materials, and errors must be
  semantically identical.
- Removing the existing CPU path before the full qualification phase.

### 2.1 Phase sequence at a glance

The critical path is deliberately split into a hybrid milestone and a full-GPU
milestone. A phase may merge only after its exit gate passes; a later phase may
prototype against mocks, but it must not become the production dependency of
an earlier phase.

| Phase | Depends on | Primary deliverable | Release checkpoint |
| --- | --- | --- | --- |
| 0. Reference freeze | current CPU path | semantic fixtures, stage timings, churn benchmark | comparisons identify the first differing cell or node |
| 1. Canonical columns | 0 | versioned CPU column ABI and column-fed CPU builders | existing CPU rendering is unchanged |
| 2. GPU job framework | 0 | resumable scheduler, fixed scratch, timestamps, cancellation | synthetic jobs obey every frame and lifetime budget |
| 3. GPU raster build | 1, 2 | canonical columns to existing brick ABI | GPU raster passes semantic and visual differential tests |
| 4. GPU Tree64 build | 1, 2 | the same columns to existing Tree64 ABI | exact/conservative occupancy and ray tests pass |
| 5. Streaming publication | 3, 4 | one generation-checked raster/Tree64 commit | **hybrid mode is shippable behind the feature gate** |
| 6. World-map atlas | 2 | typed, pinned, budgeted GPU map residency | GPU samples match the CPU map contract |
| 7. GPU columns and shell | 1, 5, 6 | atlas fields to canonical columns and sidecars | **full-GPU mode no longer uploads CPU columns** |
| 8. Qualification | 5, 7 | device policy, tuned budgets, automatic fallback | qualified devices may enable GPU generation by default |
| 9. Cleanup | 8 plus one soak period | retire only the obsolete generated-terrain hot path | CPU reference and fallback remain tested |

Phases 1 and 2 can be implemented concurrently because they meet at the
versioned column buffer. Phases 3 and 4 can also be developed independently
once that buffer and the job framework are stable. Phase 5 is the integration
barrier: it does not land as production behavior until both builders are ready.
Phase 6 may begin after Phase 2, but it stays out of the hybrid critical path.

The first useful vertical slice is therefore:

`CPU maps -> canonical columns -> budgeted GPU raster + Tree64 -> atomic publish`

Do not begin by porting procedural map sampling. That combines the hardest
semantic-parity work with the new hierarchy and scheduling work and leaves no
small end-to-end checkpoint.

---

## 3. Existing baseline

`buildWorldMapVoxelTerrain` currently performs five conceptually separate jobs:

1. sample a halo, canonical corners, boundary stitch samples, and map fields;
2. evaluate procedural elevation and infrastructure smoothing;
3. derive one exterior-shell run per horizontal column;
4. emit individual voxel leaves and build the generic sparse brick hierarchy;
5. hash and retain CPU payload arrays for caching and GPU upload.

Tree64 is then built separately by walking the resident finest brick nodes,
recovering vertical runs, and feeding those runs to the CPU Tree64 builder.

The 2026-08-25 Metal profile measured map sampling at roughly 55-175 ms per
observed worker batch and hierarchy construction at roughly 17-50 ms. Terrain
upload was not a leading GPU encoder and CPU work stayed off the render thread.
This project is therefore a time-to-detail and throughput optimization, not a
fix for a current render-thread stall. Every phase must prove that it does not
regress source-frame pacing.

The default generated page is favorable for GPU construction. A
128 x 128 x 128 voxel domain has only a 16 x 16 x 16 finest brick lattice, so a
dense temporary coordinate table contains at most 4096 brick entries. Dense
temporary indexing followed by sparse compaction is simpler than porting the
generic CPU associative-container builder.

---

## 4. Required invariants

These are release gates.

### 4.1 Canonical ownership

Surface ownership is quantized before shell extraction using one specified
integer or fixed-point representation. Adjacent pages and adjacent internal
LODs compute identical values for shared canonical corners and stitch samples.
Dispatch order, workgroup size, and frame slicing cannot change ownership.

### 4.2 One shell source

For every `(x, z)` column, the canonical payload describes the top voxel,
closure floor, material, and wall/context decisions. Raster bricks and Tree64
consume that payload. Neither path independently re-evaluates terrain height.

### 4.3 Conservative occupancy

Parent brick occupancy is the OR of its children. Tree64 occupancy contains
every shell cell that can cast or receive in the raster representation.
Coarsening may add conservative occupancy but may never remove a caster.

### 4.4 Monotonic raster error

Every parent `selfError` is at least the maximum error of its children.
`parentError` is populated from the actual parent. The two-test raster cut must
continue selecting exactly one node on every root-to-leaf path.

### 4.5 Immutable reachable pages

GPU builders write only reserved scratch ranges or inactive physical pages.
Committed raster roots and Tree64 descriptors are read-only. Uploading or
building a payload does not make it visible.

### 4.6 Atomic raster and Tree64 publication

A page becomes visible only after validation succeeds and one publication
transaction installs the matching raster page state and Tree64 descriptor.
Raster and Tree64 carry the same terrain slot, residency generation, internal
LOD, region mapping, and geometry revision.

### 4.7 Retain-complete failure

Any overflow, stale generation, cancellation, missing map tile, device loss,
allocator failure, or publication rejection leaves the previous complete page
visible. A new owner with no previous page remains absent rather than exposing
partial geometry.

### 4.8 Bounded work

Every compute stage has a bounded dispatch quantum. The scheduler never submits
an unbounded whole-page dispatch. GPU time, map transfer bytes, pages started,
pages published, and scratch memory have independent limits.

### 4.9 Deferred reuse

Scratch and inactive output allocations are reclaimed only after the last
submitted build using them has completed. Replaced committed pages are retired
behind the existing frame-completion token before reuse.

### 4.10 Stale work is harmless

Every job and every final validation record contains the expected terrain-slot
generation and build revision. A CPU queue entry is checked when selected, a
GPU stage is checked before writing, and publication checks once more. Stale
jobs may consume at most one already-submitted quantum and can never publish.

---

## 5. Target data model

Names are provisional; the contracts are not.

```cpp
struct TerrainGPUGenerationKey
{
  uint64_t bundleSignature;
  int32_t chunkX;
  int32_t chunkZ;
  uint32_t internalLOD;
  uint32_t generatorRevision;
  uint32_t terrainSlot;
  uint32_t expectedTerrainGeneration;
};

struct TerrainColumnGPU
{
  uint32_t topY;
  uint32_t firstSolidY;
  uint32_t packedMaterialContext;
  uint32_t packedFlags;
};

enum class TerrainGPUBuildStage : uint32_t
{
  WaitingForMaps,
  SampleColumns,
  FinalizeBoundaryStitches,
  CountFineBricks,
  CompactFineBricks,
  EmitFineBricks,
  ReduceBrickLevel,
  EmitBrickLevel,
  BuildTree64Level,
  FinalizeSidecars,
  Validate,
  ReadyToPublish,
  Published,
  Cancelled,
  Failed,
};

struct TerrainGPUBuildJob
{
  TerrainGPUGenerationKey key;
  TerrainGPUBuildStage stage;
  uint32_t stageCursor;
  uint32_t brickLevel;
  uint32_t tree64Level;
  uint32_t flags;
  // Reserved scratch and inactive output allocation handles.
};

struct TerrainGPUBuildResult
{
  uint32_t status;
  uint32_t overflowStage;
  uint32_t brickNodeCount;
  uint32_t tree64NodeCount;
  uint32_t occupiedVoxelCount;
  uint32_t terrainGeneration;
  uint64_t semanticRevision;
};
```

`TerrainColumnGPU` may gain a parallel corner-height or boundary record. Keep
the common record compact and put optional data in separate arrays so ordinary
interior columns do not pay for boundary-only information.

### 5.1 GPU map atlas

The full GPU path needs a typed atlas and directory for exported world fields.

- Continuous fields use the exact documented interpolation and gutter rules.
- Categorical IDs use nearest sampling and never pass through float filtering.
- Multi-component fields retain their declared component interpretation.
- Directory entries are keyed by field, map level, page X, and page Z.
- An atlas entry is immutable while referenced by a terrain job.
- Jobs pin all required atlas pages before entering `SampleColumns`.
- Atlas eviction is generation checked and deferred behind build completion.
- Missing optional fields use explicit shader defaults matching the CPU path.

The initial hybrid path does not need this atlas. It uploads completed canonical
column records from the CPU reference sampler.

### 5.2 Scratch layout

Use fixed-capacity scratch blocks per in-flight job initially:

- sampled center heights and materials including halo;
- canonical corner heights;
- boundary minima and maxima;
- column records;
- dense finest brick occupancy and coordinate-to-output index table;
- two ping-pong parent-level tables;
- Tree64 masks and level offsets;
- scan counters, validation status, and diagnostics.

Fixed blocks make cancellation and overflow simple. Size-classed or pooled
scratch can follow only after the measured concurrency warrants it.

---

## 6. GPU construction pipeline

### 6.1 Sample and quantize columns

The sampling stage evaluates the same fields and procedural signals as the CPU
generator. It writes continuous height only to temporary storage. Before any
occupancy decision, height is converted to the canonical fixed-point ownership
domain used for corner maxima, `ceil(height) - 1`, boundary minima, and stitch
closure.

The shader must not depend on fused-operation choices for voxel ownership.
Floating-point noise and transforms may differ slightly from CPU, but the
quantized result must remain within the same ownership bin. Where that cannot
be guaranteed, port that portion to integer/fixed-point arithmetic or define a
new generator revision and regenerate all affected pages consistently.

### 6.2 Extract the exterior shell

One thread owns one horizontal column and reads its four neighbours plus halo.
It computes:

- `topY` from the maximum canonical corner and boundary maximum;
- `firstSolidY` from neighbour exposure, minimum corner, closure depth, and
  boundary stitch floor;
- surface material;
- deterministic wall context for each non-top cell;
- surface height, envelope, and material sidecars.

The output is a compact vertical run, not a vector of voxel centers. The run is
the canonical input to both hierarchy builders.

### 6.3 Build finest raster bricks

Use the page's bounded dense brick lattice.

1. Each candidate 8-cubed brick computes its 512-bit occupancy from overlapping
   column runs.
2. It computes occupied bounds, coarse 4-cubed mask, occupied count, material
   count, and context/material selection without global atomics.
3. A prefix scan assigns compact node, occupancy, and material offsets to
   nonempty bricks.
4. A second pass writes the existing `SparseVoxelBrickNodeGPU`, occupancy,
   material-index, micro-context, and palette data.
5. A dense coordinate-to-compact-index table records every emitted brick.

Terrain uses a small known material domain. Start with a deterministic terrain
palette table rather than porting the generic weighted palette fitter. Any
material tie is resolved by a documented rank and stable numeric tie-break, not
by whichever GPU lane wins an atomic race.

### 6.4 Build raster parent levels

For each parent level:

1. OR-reduce the eight child brick regions into one candidate parent brick.
2. Measure vertical-structure deviation using the same semantic rule as the CPU
   reference.
3. Clamp parent error to the maximum child error.
4. Count nonempty parents and prefix-scan their output ranges.
5. Emit parents and child-index runs in canonical spatial order.
6. Fill child `parentIndex` and parent/child error fields.

One level is one resumable stage. No dispatch loops over an unknown number of
levels internally.

### 6.5 Build Tree64 directly

Do not walk the emitted brick tree to recover runs. Consume the canonical
column records directly.

- Form terminal 4-cubed occupancy masks from column runs.
- Compact nonempty terminal nodes in canonical order.
- Repeatedly form and compact upper 4-cubed levels until one root remains.
- Preserve early terminal nodes only where their conservative occupancy is
  semantically valid.
- If the exact tree exceeds its physical node-page capacity, report overflow.
  A later retry may request the existing power-of-four conservative reduction;
  it must never silently truncate nodes.

The existing GPU Tree64 builder supports sides only through 64. Generalize it
to a level-driven builder for arbitrary supported powers of four, or build
independent 64-cell subtrees and add compact upper levels. The level-driven
form is preferred because it matches the resumable budget model.

### 6.6 Validate and publish

The final validation pass checks:

- job and terrain-slot generations still match;
- every count fits its reserved physical range;
- root, child, occupancy, material, and Tree64 indices are in bounds;
- the raster hierarchy has at least one valid root;
- parent error is monotonic;
- Tree64 and raster report matching source occupancy statistics;
- all required map-atlas generations are still pinned;
- no stage reported overflow or invalid numeric input.

Successful validation writes a small ready record. The existing terrain upload
publication mechanism then installs the raster page state and Tree64 descriptor
together. Failed or stale records are never submitted for publication.

---

## 7. Per-frame build budget

GPU execution cannot be preempted in the middle of a dispatch, and timestamp
results arrive after the frame that produced them. A meaningful budget must
therefore combine small dispatch quanta with prediction. A single whole-page
dispatch followed by a timer check is not budgeted work.

### 7.1 Budget controls

Add settings equivalent to:

```cpp
struct TerrainGPUBuildBudget
{
  uint32_t targetGPUTimeMicroseconds = 750u;
  uint32_t admissionCeilingMicroseconds = 1250u;
  uint32_t maximumDispatchesPerFrame = 8u;
  uint32_t maximumPagesStartedPerFrame = 2u;
  uint32_t maximumPagesPublishedPerFrame = 4u;
  uint64_t maximumMapUploadBytesPerFrame = 4ull << 20u;
  uint64_t maximumScratchBytes = 64ull << 20u;
  uint32_t maximumInFlightJobs = 8u;
  uint32_t starvationAgeFrames = 120u;
};
```

These are conservative initial defaults for the current sample, not universal
hardware constants. Expose command-line and ImGui controls, persist them with
the terrain settings, and record them in benchmark JSON.

The target is the ordinary admission budget. The ceiling protects against
prediction noise; the scheduler must not intentionally admit work above it.
No unused budget accumulates into a later burst.

### 7.2 Work quanta

Every stage declares a countable work unit:

| Stage | Initial quantum |
| --- | ---: |
| CPU column upload | bounded byte range |
| map sampling | 16 rows of columns |
| boundary finalize | one page boundary set |
| fine brick count/emit | 256 candidate bricks |
| prefix scan | one bounded scan level |
| brick reduction | one hierarchy level or 256 parents |
| Tree64 masks/emit | one level or bounded node range |
| sidecars/hash | 16 rows or bounded byte range |
| validation | one page |
| publication | one page record |

During qualification, split any quantum whose p95 exceeds 250 microseconds on
the minimum supported GPU. This bounds the scheduler's unavoidable one-quantum
overshoot.

### 7.3 Predictive admission

Maintain an exponential moving average and a conservative high-water estimate
of GPU nanoseconds per work unit for each stage and hardware/backend key.

At frame update:

1. Start with `targetGPUTimeMicroseconds` tokens.
2. Subtract any prediction debt from an underestimated previous frame.
3. Select the highest-priority runnable job.
4. Estimate its next quantum using the larger of the EMA, recent p90 estimate,
   and cold-start table.
5. Admit it only if predicted accumulated time remains below the target and all
   count, byte, and memory budgets allow it.
6. Allow one smallest cold-start quantum when no estimate exists.
7. Stop at the target, dispatch-count limit, or admission ceiling.
8. Resolve timestamps asynchronously and update estimates when available.

If measured build time exceeds the target, carry only debt, not positive spare
time, into the next frame. Repeated ceiling violations reduce quantum size or
disable GPU generation and fall back to CPU generation.

### 7.4 Render-pressure governor

Background building yields to rendering.

- If recent source-frame GPU time exceeds 95% of the configured frame target,
  reduce the next build budget to zero.
- Between 85% and 95%, scale the target budget linearly down to one minimum
  quantum.
- Preserve upload/publication work needed to expose an already completed page,
  subject to its independent byte and publication-count budgets.
- A continuously visible request that is starved for
  `starvationAgeFrames` receives priority, not permission to violate the
  admission ceiling.
- Headless generation may opt into an unthrottled mode explicitly; interactive
  rendering never does so automatically.

### 7.5 Scheduling priority

Queue and in-flight selection use a recomputed score, not insertion order:

1. currently visible and missing coarse owner;
2. currently visible refinement by projected error;
3. near-frustum prefetch;
4. camera distance;
5. request age;
6. stable chunk address for deterministic ties.

Before selecting a queued job, recompute visibility, distance, desired LOD, and
slot generation. Remove stale requests. An in-flight job that is no longer
needed may be cancelled unless it is one bounded quantum from validation and
its output is still cacheable. Cancellation never blocks the frame waiting for
already submitted GPU work.

Prefer advancing a nearly complete visible page when that reduces
request-to-visible latency, but reserve at least one runnable quantum for a
missing coarse page before spending the frame on distant refinement.

### 7.6 Budget diagnostics

Expose and log:

- admitted and measured GPU build microseconds per frame;
- prediction error and accumulated debt;
- dispatches and work units by stage;
- map upload bytes and publication count;
- queued, runnable, in-flight, cancelled, failed, and completed jobs;
- request age and request-to-visible latency percentiles;
- scratch and inactive output bytes;
- stale work units executed after cancellation;
- overflow stage and CPU fallback count;
- render-pressure throttled frames.

GPU timeline names must include the common `TerrainGPUBuild` prefix and stage,
so a Metal or Vulkan capture can isolate total first-to-last build span as well
as individual pass time.

---

## 8. Cache and revision policy

The current CPU transient cache serializes exact hierarchy arrays. A GPU-only
payload should not be read back every time merely to preserve that cache; the
readback would recover much of the transfer cost the new design removes.

Use two cache layers during migration:

- The existing exact CPU payload cache remains for CPU-generated pages.
- GPU generation caches the canonical column payload or relies on resident map
  tiles and rebuilds the compact hierarchies when needed.

Define `semanticRevision` from the bundle signature, chunk address, LOD,
generator revision, quantization version, material-table revision, and relevant
settings. Do not use nondeterministic compact allocation order as content
identity.

A later optional GPU-payload disk cache may use an explicit canonical node
ordering and asynchronous readback outside interactive frame budgets. It is
not required to ship the first GPU path.

---

## 9. Implementation phases

Each phase lands behind `--gpu-terrain-build` or an equivalent disabled-by-
default feature gate until Phase 8. Every phase includes its tests and metrics;
testing is not deferred to final integration.

### 9.1 Expected code ownership

Keep terrain generation engine-owned. The renderer sample wires settings and
diagnostics but must not become the only home of scheduler or build logic.

| Area | Existing integration point | Planned responsibility |
| --- | --- | --- |
| CPU terrain reference | `PlanetVoxelChunk.hpp/.cpp` | column ABI, CPU producer, semantic decoder, fallback |
| Raster ABI/reference | `SparseVoxelBrick.hpp/.cpp` | existing node/occupancy/material contract and CPU differential oracle |
| Tree64 ABI/reference | `SparseVoxelTree64.hpp/.cpp`, `SparseVoxelTree64Reference.cpp` | model-side rules, decoded occupancy and ray oracle |
| GPU job control | new engine-owned `TerrainGPUBuildScheduler` and scratch allocator | queues, priorities, budgets, generations, cancellation, retirement |
| GPU pass chain | new headers under `src/voxel/svraster/rendering/` | fixed registered pass set, indirect work ranges, stage transitions |
| GPU kernels | new WGSL files under `assets/shaders/voxel/svraster/wgsl/` | columns, brick count/scan/emit/reduce, Tree64 levels, validation |
| Existing Tree64 GPU builder | `SparseVoxelTree64BatchBuildPass/Chain`, `sparse_voxel_tree64_batch_build.wgsl` | reuse tests and ABI; generalize only the level-building pieces that fit terrain |
| Atomic commit | `SparseVoxelTerrainUploadPass.hpp`, `sparse_voxel_terrain_upload.wgsl` | consume validated ready records through the existing publication CAS |
| Render-loop integration | `samples/sparseVoxelRenderer/SparseVoxelRendererSample.cpp` | feature mode, settings, admission call, diagnostics, fallback policy |
| Shader build | `src/runtime/CMakeLists.txt` | compile new WGSL targets into `SparseVoxelRasterShaders` |
| Tests | `tests/voxel/`, `tests/procedural/` | CPU units, GPU kernels, differential, streaming, failure and image gates |

The render graph owns a fixed-capacity pass chain and buffer set created at
initialization. Per-frame scheduling updates job buffers and indirect dispatch
counts; it does not register or destroy passes for individual terrain jobs.
Inactive stages dispatch zero work. This matches the current render-graph
lifetime model and makes resource retirement auditable.

### 9.2 Review-sized landing order

Within the phases, prefer these independently reviewable changes:

1. Baseline decoder, fixtures, and metrics with no generation behavior change.
2. Column structs, quantization helpers, and semantic revision.
3. CPU generator refactor to columns, still using the old hierarchy builder.
4. Column-fed CPU raster and Tree64 reference adapters.
5. Scheduler/state-machine units with a fake GPU executor and timing provider.
6. Fixed GPU resources, scratch allocator, generation checks, and synthetic
   kernels.
7. Fine raster brick count/scan/emit.
8. Raster parent reduction, links, sidecars, error, and validation.
9. Level-driven Tree64 masks, compaction, and validation.
10. Existing publication-CAS integration and hybrid streaming stress.
11. Typed map atlas and CPU/GPU sampling differential.
12. GPU elevation, shell, sidecars, and full-mode integration.
13. Qualification policy, default enablement, and later cleanup.

Every landing before step 10 keeps the runtime feature disabled by default.
Steps 7 through 9 may render only in isolated GPU tests until the atomic commit
path is ready; no debug path may publish just one hierarchy in the live world.

### Phase 0 - freeze the reference and instrumentation

**Status:** complete on 2026-08-27. The reproducible baseline workflow is
documented in `docs/terrain-generation-phase0-baseline.md`; its full matrix
covers nine fixtures at LODs 0, 1, and 2, and its streaming sequence covers
bootstrap, walking, teleports, and return. `SparseVoxelTerrainPhase0CPU`
enforces byte determinism, semantic decoding and first-difference reporting,
cross-LOD envelope containment, and raster/Tree64 occupancy parity.
`SparseVoxelTerrainPhase0BenchmarkSmoke` validates the JSON schema in CTest.

**Work**

- Capture CPU output and timings for flat, mountain, cliff, canyon, river,
  road-smoothed, coastline, snow, and cross-LOD fixtures.
- Add semantic payload decoding utilities shared by tests.
- Record sampling, shell, brick hierarchy, Tree64, upload, and
  request-to-visible timings separately.
- Add counters for page voxel dimensions, emitted runs, brick levels/nodes,
  Tree64 levels/nodes, and payload bytes.
- Add a deterministic camera-move and teleport benchmark with streaming churn.

**Tests**

- Existing generated-terrain determinism and image contracts remain green.
- Rebuilding each fixture at every admitted LOD produces byte-identical CPU
  payloads.
- Raster and Tree64 decoded finest occupancy agree for every fixture.

**Done when**

- Baseline artifacts and benchmark JSON are checked in or reproducibly
  generated.
- Every later GPU comparison can report the first differing canonical cell,
  material, context, node error, or page identity.

### Phase 1 - canonical column contract

**Status:** complete on 2026-08-27. `WorldMapTerrainColumnGPU` is the versioned
16-byte common record; wall thresholds, canonical signed fixed-point corners,
and boundary extrema are parallel sidecars. The production CPU generator now
constructs this payload before leaf emission. Both raster leaves and an exact
Tree64 reference can be built from the payload without sampling maps. The
semantic revision is included in terrain content identity. Contract details
and validation commands are recorded in
`docs/terrain-generation-phase1-columns.md`.

**Work**

- Introduce the terrain-column representation and fixed-point ownership rules.
- Refactor the CPU generator to produce columns before leaf emission.
- Add CPU reference builders that can consume columns without resampling maps.
- Define deterministic material/context tie-breaking.
- Version the column ABI and semantic revision.

**Tests**

- Old CPU path versus column-fed CPU path: exact decoded occupancy, material,
  context, sidecars, and LOD error.
- Adjacent pages: exact shared corner, top, closure, and stitch values.
- Adjacent LODs: coarse occupancy conservatively contains the fine shell.
- Randomized column properties: `firstSolidY <= topY`, bounded closure,
  representable vertical ranges, and deterministic wall ownership.

**Done when**

- The existing renderer can consume column-fed CPU output with no image or
  streaming behavior change.

### Phase 2 - budgeted GPU job framework

**Status:** complete on 2026-08-27. The engine-owned scheduler, fixed scratch
allocator, predictive stage timing, cancellation/generation rules, fixed
synthetic graph chain, shader, and deterministic lifecycle tests are described
in `docs/terrain-generation-phase2-gpu-jobs.md`. The chain is not connected to
the live terrain page directory and publication remains explicitly gated.

**Work**

- Add job, result, scratch allocator, timestamps, scheduler, and state machine.
- Register one fixed-capacity render-graph pass chain at initialization; update
  only job data and indirect dispatch ranges while the application runs.
- Implement generation checks, cancellation, failure injection, and deferred
  scratch reclamation.
- Implement predictive time admission, transfer-byte limits, render-pressure
  scaling, priority recomputation, and diagnostics.
- Start with synthetic kernels and column upload only.

**Tests**

- Deterministic scheduler tests using a fake timing provider.
- Target, ceiling, dispatch-count, page-start, publication, transfer, scratch,
  and in-flight limits tested independently and in combination.
- Priority tests for visible/missing, refinement, near-prefetch, distance, age,
  and deterministic tie-breaking.
- Stale queued requests are removed; stale in-flight jobs cannot validate.
- Cancellation at every state releases each allocation exactly once.
- Delayed timer results, underestimated quanta, zero budget, and permanent
  render pressure cannot corrupt state or create unbounded bursts.

**Done when**

- A synthetic multi-frame job advances predictably under the default budget,
  never publishes early, and survives randomized cancellation/overflow runs.

### Phase 3 - GPU raster brick hierarchy from CPU columns

**Status:** complete on 2026-08-27 for the inactive raster-page builder. The
fixed render-graph chain consumes Phase 1 columns, emits the existing brick
node/occupancy/material/context/link/root ABI in canonical order, supports
level-by-level frame slicing, and marks output ready only after GPU validation
and semantic hashing. Exact CPU/GPU payload differential and every
one-element output overflow are covered by
`SparseVoxelTerrainPhase3RasterGPU`; all Phase 0 fixtures and LODs pass the
planning/material/CPU-oracle matrix. The chain intentionally has no live
publication binding. Tree64 construction remains Phase 4 and atomic exposure
of both hierarchies remains Phase 5. Details and validation commands are in
`docs/terrain-generation-phase3-raster.md`.

**Work**

- Upload canonical columns under the transfer budget.
- Implement fine brick count, scan, emission, material/context streams, parent
  reduction, child links, roots, and monotonic error.
- Write directly to inactive physical raster-page ranges.
- Add a GPU semantic hash and validation counters.

**Tests**

- CPU/GPU differential tests for every Phase 0 fixture and every admitted LOD.
- Random property maps covering empty candidate bricks, full vertical runs,
  one-cell plains, tall cliffs, page-edge cliffs, and maximum vertical extent.
- Repeated builds with shuffled job ordering and different frame slicing
  produce the same canonical node order and semantic output.
- Capacity at exact fit succeeds; every one-element overflow fails closed.
- Parent OR containment, child links, root reachability, occupancy prefix words,
  material offsets, and monotonic errors are independently validated.

**Done when**

- Raster can render GPU-built pages through the existing shaders with no deep
  holes, black seams, shaking, ownership mismatch, or LOD-cut regression.

### Phase 4 - GPU Tree64 from the same columns

**Status:** implemented on 2026-08-27 for the inactive Tree64-page builder;
runtime GPU qualification is pending a clean device run. The
level-driven chain supports every power-of-four side whose dense mask pyramid
fits the fixed-capacity contract, builds exact masks directly from the Phase 1
column ABI, compacts them into the existing packed 12-byte node ABI, and
validates and hashes the result before returning a ready descriptor. Exact
cell occupancy, reference-ray parity, deterministic frame slicing, sides 64
and 256 on GPU, the side 4/16/64/256/1024 CPU planner matrix, and one-node
overflow are covered by the Phase 4 suites. No live directory or publication
binding exists; joint raster/Tree64 exposure remains Phase 5.
Details and validation commands are in
`docs/terrain-generation-phase4-tree64.md`.

**Work**

- Generalize Tree64 GPU construction to arbitrary supported power-of-four
  sides.
- Build terminal and upper masks directly from column runs.
- Write immutable inactive Tree64 node pages and result descriptors.
- Remove the GPU path's brick-to-run-to-Tree64 conversion.

**Tests**

- CPU/GPU Tree64 decoded occupancy equivalence at exact cell scale.
- Raster/Tree64 source occupancy equivalence before any conservative Tree64
  reduction.
- Reference ray tests from all principal directions, page edges, below cliffs,
  grazing slopes, and inside receiver cells.
- Random rays compare hit/miss and nearest occupied cell against the reference
  dense grid.
- Node capacity overflow, side 4/16/64/256 and larger required terrain sides,
  early terminal nodes, and conservative reduced pages.

**Done when**

- GPU-built Tree64 closes the existing shadow correctness suite with no new
  light leaks, black checkerboards, or one-frame shadow disagreement.

### Phase 5 - atomic streaming publication

**Status:** implemented on 2026-08-27 behind the CPU-generation fallback;
runtime GPU qualification is pending a clean device run. The three-stage GPU
bridge validates matching Phase 3/4 results, relocates renderer-native payloads
from explicitly suballocated source arenas, and emits the existing joint
raster/Tree64 compare-and-swap record. The upload pass now accepts that record
buffer without CPU readback. Scheduler publication is single-submit,
stale/cancel safe, and fence-retained; physical ownership retires the previous
lease on acceptance and the replacement lease on rejection. Shader compilation,
SPIR-V validation, CPU ownership/scheduler tests, and existing upload-consumer
compilation pass. Live GPU execution remains gated because the current device
also hangs on unchanged baseline Tree64 GPU tests. Details are in
`docs/terrain-generation-phase5-publication.md`.

**Work**

- Connect GPU build-ready records to `SparseVoxelTerrainUploadPass` and its
  existing `SparseVoxelTerrainPublicationRecordGPU` compare-and-swap contract.
- Publish matching raster and Tree64 descriptors in one transaction.
- Retain old pages through completion fences and reclaim failed inactive pages.
- Integrate current visible/distance/error request priority and stale-job
  rejection.
- Keep CPU generation selectable per job as fallback.

**Tests**

- Publication is attempted only after both payloads validate.
- Force failure before and after every stage; the previous complete page remains
  visible and shadow-active.
- Recycle a terrain slot while old work is in flight; all old results are
  rejected.
- Camera walk, rapid rotation, teleport, move/return, and refine/coarsen churn.
- Freeze publication while builds finish, then release it under normal budgets.
- Frames-in-flight retirement proves no page is reused early.

**Done when**

- Multi-frame builds and stale cancellations are visually invisible except for
  the expected persistence of the previous coarse page.

### Phase 6 - GPU world-map atlas

**Status:** implemented on 2026-08-27 behind the GPU-generation feature gate;
runtime GPU qualification is pending a clean device run. The existing typed
streamed-page layout and gutter-aware WGSL samplers are now connected to
terrain jobs through `TerrainGPUWorldMapAtlas`: worker I/O/decompression and
map upload budgets remain independent from build dispatch budgets, active jobs
pin their complete page/group dependencies, oversized targets fail closed, and
the scheduler leaves `WaitingForMaps` only after the GPU page-table commit.
Required terrain field formats are validated while missing optional fields are
accepted. CPU atlas, streaming, eviction, starvation, field-contract, and
scheduler-gate tests pass; the GPU sampler builds and passes SPIR-V validation.
Details are in `docs/terrain-generation-phase6-world-map-atlas.md`.

**Work**

- Add typed world-field atlas storage, field/page directory, gutters, pinning,
  upload budgeting, and eviction.
- Batch CPU I/O and decompression independently from GPU build scheduling.
- Implement continuous and categorical samplers matching `MapExportBundle`.
- Make map readiness a nonblocking build-job dependency.

**Tests**

- CPU/GPU field samples at centers, corners, gutters, map edges, page edges,
  every mip, and out-of-range clamps.
- Exact categorical IDs and bounded continuous-field error.
- Random atlas eviction while jobs are queued and in flight.
- Missing optional fields, corrupt directory entries, missing required fields,
  and upload-budget starvation fail safely.
- Shared page boundaries return identical quantized values regardless of which
  atlas page supplies the sample.

**Done when**

- The atlas can feed canonical CPU-equivalent columns without reading map data
  from CPU memory during the build.

### Phase 7 - GPU elevation and shell generation

**Implementation status (2026-08-27): complete behind the Phase 8 feature
gate.** Generator revision 14 uses one world vertical datum across pages and
LODs. The atlas kernel emits canonical columns, walls, boundary sidecars, a
raster-ready interleaved view, validation status, semantic hash, and occupied
count. CPU qualification and SPIR-V validation pass; clean-device GPU runtime
qualification remains part of Phase 8.

**Work**

- Port procedural elevation, uplift/erosion modulation, cliffs, canyons,
  terraces, rivers, roads, settlement smoothing, water, biome, and material
  decisions.
- Generate canonical columns, boundary records, and sidecars entirely on GPU.
- Remove CPU column uploads for atlas-resident jobs.
- Assign a new generator revision if fixed-point parity requires a deliberate
  output change.

**Tests**

- CPU/GPU quantized-column differential tests over real exported worlds and
  adversarial synthetic maps.
- Maximum 4500 m mountains and the configured vertical-capacity boundary.
- Flat asset-placement regions remain within their slope/elevation contracts.
- Rivers and roads remain smooth and connected across pages and LODs.
- Cliffs and canyons remain closed, with correct wall context and no shadow
  leaks.
- Repeated builds across workgroup sizes, dispatch order, and frame slicing are
  semantically deterministic.

**Done when**

- Full GPU pages match the approved generator revision and no longer require a
  CPU-generated column payload.

### Phase 8 - qualification and default enablement

**Implementation status (2026-08-27): rollout machinery implemented; device
certification pending.** Backend/tier budgets, exact device-profile suite
gates, `auto`/`cpu`/`gpu-hybrid`/`gpu-full` selection, performance gate
evaluation, session failure policy, scheduler p95 telemetry, and exact-key CPU
retry are implemented. The policy is fail-closed: no device becomes GPU-default
until its exact backend/device/driver profile passes the complete runtime and
image suite. CPU Phase 0-8 qualification and shader validation pass. This
machine's currently wedged Vulkan/Metal device is intentionally not certified,
so clean-device GPU execution and captured performance results remain required
before default enablement here. Details are in
`docs/terrain-generation-phase8-qualification.md`.

**Work**

- Run the complete terrain, raster, Tree64, light-field, streaming, and image
  suites with CPU and GPU builders.
- Profile static, walking, fast travel, and teleport workloads on the minimum
  supported GPU and at least one discrete GPU/backend when available.
- Tune cold-start estimates and quantum sizes per backend.
- Add automatic per-session fallback after repeated build failures or budget
  violations.
- Enable GPU building by default only on qualified devices. Keep a CLI switch
  for the CPU reference path.

**Performance gates**

- Default interactive target: 0.75 ms terrain-build GPU time per frame.
- The scheduler never intentionally admits more than the 1.25 ms ceiling.
- At least 99% of qualified interactive frames remain at or below the ceiling;
  any overshoot is at most one bounded quantum and is recorded.
- No terrain build quantum exceeds 0.25 ms p95 on the minimum supported GPU.
- Source-frame p95 does not regress by more than 0.3 ms in a settled scene with
  no terrain requests.
- Camera-walk source-frame p95 does not regress versus the CPU builder while
  request-to-visible p50 and p95 improve materially. Set the final required
  percentage from Phase 0 variance; target at least a 30% p50 reduction.
- Map transfer, publication, scratch, and physical-page budgets remain bounded
  during teleport stress.

**Correctness gates**

- Zero raster/Tree64 terrain-generation mismatches.
- Zero deep raster coverage holes, isolated hard-black terrain pixels,
  cross-page cracks, or stale-page publications.
- No page commit causes one-frame light blinking or invalidates unrelated light
  history.
- CPU fallback can replace a failed GPU job without changing ownership or
  requiring a renderer restart.

**Done when**

- GPU generation is the default on qualified hardware and the CPU path remains
  a tested fallback rather than a second independently evolving generator.

### Phase 9 - cleanup and optional follow-up

**Status (2026-08-27): measured cleanup implemented; optional work deferred.**

Generated terrain's default CPU fallback now builds hierarchy bricks directly
from the canonical column page. It no longer creates the three generic
per-voxel leaf arrays or enters the generic `std::map` hierarchy builder. The
old column-to-leaf adapter and generic hierarchy remain an independent test
oracle and continue serving unrelated scene/import paths. Differential Phase 9
coverage requires byte-identical output across every Phase 0 fixture at LOD
0-2, deterministic capacity behavior, and aggregate temporary-storage
contraction.

Phase 8 did not certify the local GPU device, so `auto` still selects the CPU
fallback and the reference path has not been deleted. Pooled scratch, GPU
payload disk caching, direct page-size classes, and DAG sharing are deferred
until captures on responsive qualified hardware demonstrate pressure or node
duplication sufficient to justify their lifetime, invalidation, and update
costs. See `terrain-generation-phase9-cleanup.md`.

After at least one qualification period:

- Remove generated-terrain use of generic CPU leaf vectors and `std::map`
  hierarchy construction from the default path.
- Keep canonical CPU column and hierarchy references in tests.
- Consider pooled scratch, GPU payload disk caching, or direct-to-page size
  classes only from measured pressure.
- Revisit DAG sharing only if captured node duplication shows a compelling
  memory win after accounting for metadata, reference counts, and update cost.

---

## 10. Test matrix

The minimum continuous test matrix is:

| Layer | Coverage |
| --- | --- |
| CPU unit | column quantization, shell closure, tie-breaking, revisions |
| Scheduler unit | priorities, budgets, prediction debt, starvation, cancellation |
| GPU kernel | count/scan/emit/reduce/validate with exact small fixtures |
| Differential | CPU versus GPU columns, bricks, Tree64, sidecars, errors |
| Property | randomized heights/materials, seams, containment, determinism |
| Streaming | movement, teleport, LOD churn, slot reuse, frames in flight |
| Failure | every overflow stage, missing maps, allocation and publication rejection |
| Rendering | close slope, cliff, canyon, horizon mountain, water/road boundaries |
| Lighting | grazing sun, receiver-cell skip, page commit, temporal stability |
| Performance | static, walk, fast travel, teleport, cold and warm atlas/cache |

GPU tests should decode semantic output into simple canonical grids where
possible. Byte-for-byte buffer comparison is required only for ABI fields whose
ordering is intentionally canonical. This avoids blessing accidental allocator
ordering while still catching every visible or traversable difference.

Every fixed regression seed must report the first differing world coordinate
and owning page. Random property tests must print and retain the seed on
failure.

---

## 11. Rollout and rollback

Use three runtime modes throughout migration:

- `cpu`: current reference generation;
- `gpu-hybrid`: CPU columns, GPU bricks and Tree64;
- `gpu-full`: GPU map sampling, columns, bricks and Tree64.

An unsupported feature, missing atlas capability, repeated shader overflow,
device loss, or sustained budget violation falls back per job or per session to
`cpu`. Fallback advances the terrain generation normally and uses the same
publication transaction; it must not clear resident pages first.

Do not maintain separate terrain semantics in the CPU and GPU implementations.
The canonical column contract, quantization version, material ranking, shell
rules, and semantic revision are shared specifications. The CPU implementation
is the executable reference for those rules.

---

## 12. Final conclusion

The implementation should proceed as two independently shippable milestones:

1. **Hybrid GPU construction:** CPU map sampling produces canonical columns;
   budgeted GPU work builds raster bricks and Tree64 and publishes them
   atomically.
2. **Full GPU construction:** a typed world-map atlas lets the GPU produce the
   same columns before building both hierarchies.

The hybrid milestone validates the risky hierarchy, scheduling, and publication
work without simultaneously changing map sampling. The full milestone then
targets the measured dominant cost. At every point, small resumable dispatches,
predictive per-frame admission, generation checks, and retain-complete failure
semantics keep terrain generation subordinate to rendering rather than turning
streaming throughput into frame-time instability.
