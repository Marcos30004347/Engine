# Virtual geometry hardware ray tracing implementation plan

Status: partially implemented, September 9, 2026. The Metal backend, native GPU
tests, benchmark runner, all-direction WGSL culling mode, and render-graph native
buffer dependency bridge are implemented. Renderer-wide streaming/readiness
publication, automatic shadow-lighting selection, and Vulkan hardware RT remain
open. See the [implementation status](virtual-geometry-hardware-raytracing-status.md)
for executable tests, measured scope, and remaining acceptance gates.

This plan implements the [shadow-cut design](virtual-geometry-hardware-raytracing-plan.md).
The initial product is optional, direct-light shadows over static opaque VG triangles.
The objective is minimum total shadow cost at a fixed image-quality setting, including
culling, streaming preparation, acceleration structures, synchronization and tracing.

## Decisions and invariants

1. Cache object-space cluster BLAS. Build once per immutable geometry version while
   its cache entry survives; camera motion, light motion, cut changes, and rigid
   instances do not rebuild it. Deformation/content changes create a new version.
   Actual BLAS-cache eviction requires rebuilding on a later miss. Compaction is a
   copy operation and must not trigger another geometry build.
2. GPU compute selects the shadow cut, writes native TLAS instance descriptors and
   the instance count, and supplies indirect dispatch arguments for ray work.
   No synchronous GPU count readback on the frame path.
3. Use native Metal primitive AS builds and indirect instance AS builds on Apple.
   Use Vulkan BLAS/TLAS and ray queries on supporting Vulkan devices. MoltenVK is
   the raster bridge on Apple, not the hardware RT implementation.
4. Keep a complete fallback until the whole replacement group is AS-ready. Never
   publish a partial cut on queue overflow, allocation failure or delayed builds.
5. Start with one BLAS per cluster for exact selection. Benchmark its metadata,
   build and TLAS overhead early; measure grouping only where a group can be
   selected as a complete compatible unit. Do not put competing LODs into a page
   BLAS and treat that as equivalent geometry.
6. Warm, unchanged resident geometry must produce **zero BLAS builds**. Persistent
   allocations, descriptor tables and scratch pools must not grow after warmup.

## Metal: what “indirect” means here

The implementation must distinguish GPU execution, GPU-written inputs, and
GPU-generated commands. They are different capabilities.

| Work | Metal implementation | CPU involvement |
| --- | --- | --- |
| Geometry preparation | Indirect compute dispatches decode/pack new cluster vertices and indices. | Allocate/cache fixed-capacity storage and publish install metadata. |
| Cluster BLAS build | `MTLPrimitiveAccelerationStructureDescriptor` with triangle descriptors; `MTLAccelerationStructureCommandEncoder` executes native GPU builds. | Encode builds for new cache entries with known ranges/counts; allocate AS objects and scratch. |
| Frame instance selection | Compute writes `MTLIndirectAccelerationStructureInstanceDescriptor` records, including BLAS resource IDs, transforms, masks and user IDs. | Bind persistent buffers and capacity, not a read-back selected list. |
| Frame TLAS build | `MTLIndirectInstanceAccelerationStructureDescriptor`, using `instanceDescriptorBuffer`, `instanceCountBuffer`, and `maxInstanceCount`. | Encode a build using the persistent descriptor and destination. GPU determines actual instances/count. |
| Shadow trace | MSL triangle intersector in an indirectly dispatched compute kernel. | Encode fixed bindings/dispatch command; GPU determines ray work. |

Apple's [indirect instance descriptor](https://developer.apple.com/documentation/metal/mtlindirectinstanceaccelerationstructuredescriptor)
and [indirect instance record](https://developer.apple.com/documentation/metal/mtlindirectaccelerationstructureinstancedescriptor)
provide the TLAS path. The installed SDK exposes these starting at macOS 14/iOS 17;
check OS availability and device support during initialization, then run a small
capability smoke test. `supportsRaytracing` alone is not evidence of dedicated
hardware ray-tracing units or every newer AS feature.

**BLAS API boundary:** the checked SDK exposes `triangleCount` on CPU geometry
descriptors, not a GPU triangle-count buffer or a GPU-authored arbitrary AS-build
command list. Metal 4's address-based triangle descriptors still expose a scalar
triangle count. Its compute encoder supports AS builds, but that does not make
those build commands writable by a compute shader. Do not design an unsupported
`dispatchIndirect`-style variable BLAS build API. See Apple's
[triangle descriptor](https://developer.apple.com/documentation/metal/mtlaccelerationstructuretrianglegeometrydescriptor)
and [Metal 4 triangle descriptor](https://developer.apple.com/documentation/metal/mtl4accelerationstructuretrianglegeometrydescriptor).

For encoded assets, read cluster ranges/counts from asset/install metadata and
encode the one-time builds before GPU decode completes; GPU dependencies order
the build after decode. If metadata is not currently available before decoding,
extend the install manifest rather than reading geometry back.

For GPU-generated geometry with unknown counts, use a small **asynchronous** build
manifest readback during installation and defer publication until builds complete.
The camera frame continues on its valid parent cut. This is a documented latency
tradeoff, not a fully GPU-authored BLAS command stream. A second experimental option
is known-capacity cluster slots with GPU-filled valid degenerate padding and fixed
build counts; benchmark wasted storage/build work before adopting it. Never build
all cached BLAS every frame to simulate an indirect build queue.

A strict requirement for arbitrary GPU-authored BLAS commands with no installation
metadata handoff remains a capability gate; it is not supplied by the APIs above.
Metal 4 is an optional measured follow-up, not a prerequisite for indirect TLAS.
Its address-driven path also requires explicit resource residency; see
[Metal 4 indirect AS](https://developer.apple.com/documentation/metal/mtl4indirectinstanceaccelerationstructuredescriptor).

## Components and ownership

All names below are proposed new files/types unless explicitly called existing.

| Location | Responsibility |
| --- | --- |
| `src/runtime/virtualgeometry/raytracing/VirtualGeometryRayTraceTypes.hpp` | Backend-neutral cut records, geometry keys, readiness, limits, diagnostics. |
| `.../VirtualGeometryRayTraceScene.{hpp,cpp}` | BLAS cache index, install requests, retained RT fallback cut, per-frame references and retirement. |
| `.../VirtualGeometryRayTraceBackend.hpp` | Capability query, immutable build batches, frame-AS allocation, encode operations and timestamps. |
| `.../metal/VirtualGeometryMetalRayTraceBackend.{hpp,mm}` | Native Metal AS, heap/scratch management, pipeline creation, interop and residency. |
| `.../vulkan/VirtualGeometryVulkanRayTraceBackend.{hpp,cpp}` | Vulkan AS resources/builds/query bindings; feature-gated indirect counts. |
| `src/runtime/virtualgeometry/rendering/VirtualGeometryRayTraceCullPass.hpp` | Shared cut policy, receiver/light volumes and demand outputs. |
| `.../VirtualGeometryTopAccelerationStructureBuildPass.hpp` | Native instance conversion and TLAS build; dependencies on BLAS readiness. |
| `.../VirtualGeometryRayTraceShadowPass.hpp` | Ray generation, optional compaction and opaque visibility output. |
| `assets/shaders/virtualgeometry/metal/` | MSL shadow traversal, instance preparation and trace kernels. |
| `tests/virtualgeometry/` | CPU contracts, Metal GPU smoke/parity tests and benchmark executable. |

Modify existing `VirtualGeometryScene` install/retirement hooks, renderer pass
scheduling, streaming priority selection and CMake shader compilation. Keep Metal
objects private to Objective-C++; add backend-neutral graph resource ownership and
dependencies rather than representing AS as an ordinary buffer with no AS hazards.

The existing `RHI::cmdMetalCompute`, Metal device/buffer/texture exports and
`TerrainMetalComputeRasterPass::encodeVisibility` already demonstrate GPU shared-event
handoff. Reuse that route for bring-up; do not require the full planned Metal RHI.
Record the native shadow chain as one submission segment where possible, with one
ready/done handoff. Measure semaphore/submission overhead and only split BLAS
maintenance onto another queue when measurements show a benefit.

## Persistent BLAS cache and publication

Separate immutable geometry identity from transient storage identity:

```text
GeometryKey = asset/runtime-object identity + content revision + local cluster
              + decoded geometry/build format + geometry-affecting options
SlotToken   = physical slot + installation generation
CacheEntry  = GeometryKey, native BLAS, resource ID, state, bytes, build policy,
              last use, in-flight references, completion serial
```

Maintain generation checks in the GPU lookup table to prevent slot-reuse errors.
A reload of unchanged geometry can find a still-cached BLAS even if decoded page
offsets move. A different content version must never reuse it. Rigid instances
share the same object-space BLAS and use different TLAS records.

State progression: `Absent -> Allocated -> Building -> Ready -> Retiring`.
Optional compaction publishes a replacement resource ID after the copy is ordered
complete, and retains the old AS until old TLAS users retire. Publish readiness only
when the consuming GPU timeline is guaranteed to observe the finished AS, not just
when the CPU has encoded its build. A resource ID alone does not establish readiness.

On install, schedule missing immutable versions within a byte/build budget. Build
coarse fallback representations first, then camera-needed and shadow-needed children.
Shared descendants must deduplicate builds. Give parallel builds nonoverlapping
scratch; reuse scratch only after a proven dependency. Query native AS/scratch and
heap size/alignment; cache size results for matching descriptor classes where valid.

Start static BLAS with the default quality policy and no refit capability. Evaluate
`PreferFastIntersection` when available (macOS 26 SDK/API) against default and compacted
variants; avoid `PreferFastBuild` as an unmeasured static-BLAS default. Compaction
should run outside the critical frame path and only when memory savings justify it.

Track decoded bytes, BLAS bytes, TLAS bytes, scratch and retired bytes separately.
Retain build-input buffers until builds finish; retain any externally referenced
shader attributes for their consumers. Tie BLAS lifetime to all TLAS references.
For initial integration pin referenced VG allocations conservatively, then relax
build-input retention only after backend-specific lifetime validation.

## Frame algorithm

```text
camera cull/raster/depth + current lights + decoded page availability
    -> GPU receiver/light volumes
    -> RayTraceCull over full scene roots
         -> required page demand and missing-BLAS demand
         -> complete AS-ready shadow cut
    -> GPU instance preparation + instance count
    -> Metal indirect TLAS build
    -> GPU ray generation/optional compaction + indirect dispatch arguments
    -> opaque first-accepted-hit trace -> shadow visibility -> lighting

camera demand + shadow demand -> merge -> future streaming/build work
```

Native Metal shadow culling keeps the native frame chain together. Extract common
selection contracts from the existing WGSL implementation, implement MSL/Vulkan
variants, and test parity on the same hierarchy/cut fixtures. Do not assume existing
WGSL culling can directly write a correctly laid-out Metal AS record. Keep cut
records backend-neutral and convert in a cheap compute kernel; fuse conversion
with final emission only after validating ABI and measuring the cost.

Camera frustum, HZB, cone, projected-footprint and two-phase raster visibility tests
must be disabled for the shadow traversal. Keep valid topology/residency/LOD tests.
Use all camera raster phases/routes for receivers. Merge demand after both traversals;
do not run the existing priority-clearing initialization a second time.

Use a per-frame ring for instance buffers/counts, TLAS destinations, scratch and
diagnostics. Instance transforms use the native packed-matrix ABI, with tests for
translation, rotation, nonuniform/negative scale and instance IDs. Reference cached
BLAS through resource IDs and declare the indirectly accessed resources resident
using the supported encoder/heap mechanisms; Metal 4 uses residency sets.

Size TLAS resources for capacity, but build from the actual GPU count. Verify the
zero-count case. If unsupported, use a validated masked sentinel path. If the cut
exceeds capacity, fall back to a complete coarser cut or report the frame invalid
and use the existing shadow backend; clamping the count alone would lose shadows.

Rebuild TLAS for cut membership changes. Refitting is an opt-in experiment for
stable topology/compatible descriptors, with traversal-quality tracking. An exactly
unchanged AS can be reused when membership, resource versions, transforms, masks
and options all match. Light motion only forces rebuild when it changes the cut.
Do not add a synchronous readback just to detect an unchanged cut. Initially rebuild;
implement reuse only with a proven nonblocking invalidation/selection mechanism.

Declare compute-write -> AS-build-read, BLAS-build-write -> TLAS-build-read, and
TLAS-build-write -> trace-read dependencies. Across the MoltenVK/Metal boundary,
shared-event signaling must cover the entire native segment and frame retirement.
No `waitUntilCompleted`, `blockUntil`, or mapped count reads in the steady-state
path. Diagnostic readback waits are permitted only after measured work completes.

## Fast shadow kernel and optimization order

Begin with one opaque shadow sample for each valid receiver/light pair. Reconstruct
the surface, bias along a tested geometric normal, bound the ray toward its light,
and terminate on the first accepted blocker. Use triangle-specialized MSL code with
minimal result state; skip material interpolation, custom intersections and alpha
sampling in this path. Keep alpha/deformation in separately reported future variants.

Measure these choices in order, at equal rays/geometry/quality:

1. Cached BLAS, stable buffers, no frame readbacks and one native submission segment.
2. Tighter receiver/light bounds to reduce cut size and streaming demand.
3. Tile/light-coherent work ordering; skip sky, nonreceivers and inactive lights.
4. Direct tiled trace versus compacted ray worklist. Compaction adds scans, memory
   traffic and dispatches; use it only when sparse work repays that cost.
5. Threadgroup sizes and register pressure; measure 64/128/256-thread variants.
6. TLAS default/fast-build/refit policies using **build plus trace** cost. A cheaper
   build with slower traversal can lose overall.
7. Cluster BLAS size/grouping, AS heap allocation and compaction. Static build cost
   amortizes; cache memory and TLAS fan-out remain per-frame concerns.
8. Only then introduce shadow-specific LOD, fewer samples, temporal reuse or denoising.
   These change quality and must be plotted against error, not called equal-work wins.

Apple discusses ray specialization, AS build/refit tradeoffs and profiling in
[Maximize your Metal ray tracing performance](https://developer.apple.com/videos/play/wwdc2022/10105/).
Treat those techniques as experiments; the fastest combination must be measured on
the target GPU and this engine's clustered scenes.

## Milestones and acceptance tests

| Phase | Deliverable | Exit condition |
| --- | --- | --- |
| RT0: capability and measurement | Native Metal test target, offline metallib, GPU timing, JSON report, device/OS/API classification. | Static triangle BLAS/TLAS hit/miss and empty scene pass; unsupported devices report skip, not fabricated timings. |
| RT1: cached BLAS | Cluster range adapter, immutable keys, deduplicated build batches, heap/scratch pools, readiness and retirement. | Repeated frames/instances build each version once; generation reuse and delayed completion tests pass; build microbenchmarks available. |
| RT2: indirect TLAS | GPU instance records/count, frame resource ring and indirect native build. | GPU-varying counts without CPU reads, transforms/masks/IDs, overflow fallback, zero count and in-flight slot reuse pass; TLAS benchmarks available. |
| RT3: opaque trace | Native MSL ray generation/query/output and indirect work dispatch. | Compare fixed rays to CPU brute-force triangles; render a visible receiver with an offscreen blocker; trace-only benchmark available. |
| RT4: scene cut and streaming | Independent GPU traversal, light volumes, separate demand merge, AS-aware replacement retention. | Unculled/cull parity at the same cut; missing sibling/BLAS preserves fallback; both raster phases/routes and moving lights tested. |
| RT5: renderer integration | Optional backend selection, native shared-event segment and lighting input; Vulkan equivalent following Metal contracts. | Whole-frame correctness and timing, no host waits, no warm BLAS rebuilds, stable resource usage over a long replay. |
| RT6: optimize and lock budgets | A/B sweeps, chosen policies, baseline reports and regression runner. | Publish quality/cost results and device-specific budgets; retain only improvements outside measurement noise. |

Vulkan work adds AS feature discovery/resources/descriptors/build stages and a
verified ray-query shader route. GPU count builds require the separate
`accelerationStructureIndirectBuild` feature; capability fallback must be identified
in reports. CPU count-readback prototypes are not accepted as the production path.

## Benchmark plan

Add a standalone `VirtualGeometryHardwareRayTraceBenchmark`, with no display/vsync
dependency, plus a renderer replay mode for complete integration costs. Reuse the
JSON/hash conventions of `SparseVoxelShadowBenchmarkSmoke.cmake` and the existing
terrain benchmark, but provide RT-specific fields. Add smoke/schema tests to CTest;
keep performance thresholds on controlled GPU machines, outside generic CI.

### Workloads

| Suite | Controlled inputs | What it answers |
| --- | --- | --- |
| BLAS cold build | 1K/10K/100K unique clusters, legal 32/64/128-triangle buckets; default/fast-intersection, compacted/uncompacted. | Build throughput, CPU encoding cost, bytes per cluster/triangle, scratch and peak memory. |
| Cache warm/reload | Same scene for 1,000 frames; repeated instances; unchanged reload; content change; AS-cache eviction. | Zero warm rebuilds, deduplication, correct cache invalidation and recovery cost. |
| TLAS only | 1K/10K/100K instances; 1M only if supported and within memory budget; GPU-generated changing counts. | Instance preparation and build scaling, fixed-capacity versus actual-count cost. |
| Trace only | Fixed prebuilt TLAS, 1M/2M/8M rays; coherent/incoherent, near blocker, long miss, mixed hits. | Query throughput isolated from construction and ray generation. |
| Renderer steady | 720p/1080p/1440p/4K, 1/4/16 lights, fixed deterministic sampling. | Actual rays and total shadow/frame cost; light counts do not imply tracing every light per pixel. |
| Offscreen shadows | Blocker outside view, near-light magnification, directional long shadow, area emitter. | Conservative culling and quality at matched LOD/rays. |
| Motion and churn | Fixed camera/light paths, rigid motion, 0/1/5/20% page turnover per frame and budget exhaustion. | p95/p99 spikes, fallback quality, AS work/backlog and recovery latency. |
| Representation A/B | Per-cluster versus compatible grouped BLAS on exactly the same active triangles. | Whether many small AS objects cost more than grouping saves; no overlapping-LOD shortcuts. |

Cap each case against reported memory limits and record explicit skips. Keep small
correctness scenes deterministic and generated in the test; large imported scenes
need pinned assets, transforms and content hashes. Include both repeated assets
and unique terrain to avoid overstating instancing/cache benefits.

### Measurement protocol

- Release build, offline shaders, no validation/capture for timing runs; run a
  separate validation configuration first. Record GPU name/family, hardware-RT
  classification and evidence, OS, SDK, commit, backend, power mode and thermal state.
- Warm steady workloads for at least 120 frames, measure at least 600 frames, repeat
  five times with interleaved A/B order. Keep cold-cache runs separate and reset
  only benchmark-owned cache state. Extend runs when timings/thermal state drift.
- Use GPU timestamps/counter samples around receiver preparation, culling, demand
  merge, geometry preparation, BLAS builds, compaction, instance preparation, TLAS,
  ray generation/compaction and trace. Where a device cannot timestamp a boundary,
  use an isolated command-buffer timing variant and label the limitation.
- Read timing/results from completed older frame slots. Time CPU encode/submit and
  wall-clock frames separately. Record bridge waits and native submission count.
  Report measured total critical-path GPU/frame time; sums of overlapping stages
  are work totals, not necessarily elapsed latency.
- Report p50/p95/p99, per-run variation, actual traced rays, hits/misses, and Mray/s
  for trace-only work. `Mray/s = tracedRayCount / (traceMilliseconds * 1000)`.
  Also report rays/s for the complete pipeline with its denominator identified.
- Report resident/selected/unique clusters, triangle count, instance count,
  BLAS cache hit/miss/build counts, build queue backlog, pages requested/installed,
  AS-not-ready fallbacks, overflow counters and every relevant memory category.
- Keep ray, geometry/cut and result hashes. Compare visibility with brute force
  for small fixtures and the unculled same-cut hardware baseline for larger scenes;
  record false-lit/false-shadowed counts with a documented boundary tolerance.
  Different LODs use a separate image-error report; hashes alone are insufficient.

### Baselines and budgets

Always report four relevant baselines: raster with shadows disabled; existing
software shadow backend; hardware tracing with the same unculled valid cut; hardware
tracing with light-aware culling. For a fair hardware/software trace comparison,
use identical triangles/rays where available. Voxel/SDF versus triangle rendering
is a representation/quality comparison and must be labeled separately.

Use **2 ms p95 incremental shadow GPU cost at 1080p, one opaque sample per valid
receiver, on the chosen hardware-RT reference device** as an initial engineering
target, not a measured capability or universal promise. RT0 chooses and records the
actual reference GPU; RT3 establishes whether the target is realistic. Include
cull/instance/TLAS/trace and synchronization cost. Report streaming BLAS maintenance
inside stressed whole-frame runs, even when it runs asynchronously.

Structural acceptance gates are unconditional: zero unchanged warm BLAS builds,
zero synchronous frame-path readbacks, no unreported cut loss, no invalid AS use,
and bounded cache/scratch/retirement memory. Allocate the timing budget among stages
after the first measurements, then optimize the dominant measured stage.

Store per-device JSON/CSV and plots of total milliseconds against instance count,
ray count, memory and image error. A provisional performance-regression flag is
more than 5% **and** 0.1 ms total p95 regression, confirmed across repeated runs
outside the measured noise band. Never accept a speedup caused by missing blockers,
skipped receivers, uncounted streaming work, or silently reduced quality.

### Proposed commands (not implemented yet)

```sh
VirtualGeometryHardwareRayTraceBenchmark --backend metal --suite blas --output blas.json
VirtualGeometryHardwareRayTraceBenchmark --backend metal --suite tlas --instances 100000 --output tlas.json
VirtualGeometryHardwareRayTraceBenchmark --backend metal --suite trace --rays 2000000 --output trace.json
VirtualGeometryHardwareRayTraceBenchmark --backend metal --suite replay --width 1920 --height 1080 --warmup 120 --frames 600 --output replay.json
```

The first implementation should deliver RT0–RT3 and their benchmark reports before
adding streaming complexity. That makes native GPU speed and per-cluster AS overhead
measurable early enough to change the design if necessary.
