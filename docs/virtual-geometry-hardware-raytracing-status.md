# Hardware ray tracing implementation status

**ReSTIR update:** [Hardware visibility, blue-noise proposals and native MetalFX denoising](restir-hardware-metalfx-status.md) are integrated into the resident sparse-voxel sample. This does not change the remaining terrain/VG or DDGI gates below.

**Latest integration update:** [GPU cuts, RT depth pages, screen-first handoff and VSM light BVH](hybrid-shadow-gpu-integration-status.md). This update supersedes the older open-gate entries below for those components.

September 9, 2026. This is a working Metal implementation and renderer integration
primitive. The automatic renderer shadow option is not complete; the remaining
work below is required before exposing it as a replacement for existing shadows.

## Implemented

- [BLAS completion at page installation](page-install-blas-status.md): opt-in file
  and runtime encoded-page publication gate, GPU encoded geometry expansion,
  versioned manifests, transactional budget rollback, retained parent fallback,
  nonblocking install-only scheduling and private generated-input producer events.
  Generated terrain now derives manifests from completed tile counts, retains
  reservations through native input reads, and preflights sample batches before
  publishing residency/parent rewrites. The hardware-shadow option remains open.

- `VirtualGeometryMetalRayTraceBackend`: immutable object/revision/cluster/format
  keys, deduplicated object-space triangle BLAS, explicit cache eviction and byte
  budget, retained in-flight resources, cached resource-ID snapshots, separate
  scratch ranges for batched builds, and three frame slots by default. Warm
  frames perform zero BLAS builds. CPU cache/submission access is serialized.
- Separate closest-hit queries return distance, instance, primitive and explicit
  miss/invalid status. Any-hit retains its compact visibility output. Graph inputs
  select the query; closest-hit output requires 16 bytes per ray.
- Optional per-submission BLAS build-count and AS-output-byte quotas. Pending work
  is retained in stable installation order, and unbuilt handles map to invalid
  scene results. Oversized jobs remain pending; raise the byte quota to admit them.
  These quotas do not by themselves select a complete parent/child scene cut.
- GPU instance conversion, GPU count and dispatch preparation, native Metal
  indirect TLAS builds, and indirectly dispatched opaque any-hit shadow queries.
  Empty scenes use a masked degenerate sentinel. Invalid handles, singular
  transforms and capacity overflow produce `ShadowResult::InvalidScene`; a caller
  must use its existing shadow fallback for that result.
- Optional GPU conservative caster filtering against receiver-to-light volume
  AABBs. The input must already be a complete, AS-ready, camera-error cut. Supplied
  volumes must enclose **every** relevant receiver-to-emitter segment, including
  offscreen blockers; this filter does not generate those volumes or traverse VG.
- `Settings::sceneCut` on `VirtualGeometryCullingMultipleDispatchesPass` disables
  camera frustum, HZB, cone, footprint, software routing and two-phase visibility
  rejection, retaining distance/error and residency tests. It writes its own HW
  cluster list. Streaming writes require a separate priority buffer, including
  when settings change after construction, to protect camera demand.
- `VirtualGeometryHardwareRayTracePass` declares buffer dependencies to the graph
  and records one shared-event native segment: pending BLAS builds, instance
  conversion/caster filtering, TLAS, trace. It accepts prepared instance/ray/count
  buffers and optional bounds/volume buffers. Build inputs must also be declared
  to order geometry upload/decode before the native segment.
- Offline MSL compilation, native tests, CPU cut-policy tests, a GPU culler
  regression and release JSON microbenchmarks with raw samples and provenance.

Metal BLAS builds execute on the GPU with CPU-encoded descriptors for cache
misses. Metal's triangle descriptor takes a scalar CPU triangle count; the
implementation does not claim support for arbitrary GPU-authored BLAS commands.
TLAS instance counts and trace dispatch sizes are GPU-driven with no synchronous
count readback. Only diagnostic tests/benchmarks call blocking completion APIs.

## Current integration contract

1. Construct the backend with the RHI's exported Metal device and an offline
   metallib path. Check `capabilities().available`; `appleFamily9OrNewer` is reported
   separately because API availability alone does not establish hardware RT units.
2. Call `cacheGeometry` once per immutable cluster version with valid native
   float3 vertex/index ranges. Cache hits reuse the handle. All referenced index
   values must be valid; byte-range validation cannot inspect GPU index contents.
3. Produce a complete cut using cached handles and packed column-major transforms.
   Keep any parent fallback until its full replacement group is ready. Build input
   buffers must remain unmodified until the submitted GPU work completes.
4. Allocate exported graph buffers covering the configured capacities, at offset
   zero, and register `VirtualGeometryHardwareRayTracePass` with their views.
   Graph frame indices must fit the backend's frame-slot count. Supply pending
   geometry buffers in `geometryBuildInputs`; the graph cannot infer native cache
   inputs from a BLAS handle.
5. Consume 0/1/2 visibility results (or `ClosestHitResult` for closest queries) after the native graph dependency. The pass
   does not reconstruct receivers, generate lights/rays, or compute RGB lighting.
   Retain the backend for the graph lifetime and retire graph frames before reuse.

## Remaining implementation gates

The next integration design is specified in the
[hybrid virtual shadow cache plan](hybrid-virtual-shadow-cache-plan.md). It connects
the Metal gates below to raster/RT page production, receiver accumulation, tile
light lists and coarse-first budgets. Native Vulkan AS is explicitly deferred
from that work to a later session.

| Gate | Work still required |
| --- | --- |
| VG installation adapter | Derive cluster build manifests from page installation, publish versioned GPU handle/readiness tables, schedule builds within a per-frame work budget and integrate decoded allocation retirement. |
| AS-aware cut publication | Keep complete replacement groups/parents across missing BLASes, streaming changes, queue overflow and build failure. The existing raster residency cut alone is insufficient. |
| Light-aware hierarchy requests | Generate conservative receiver/light volumes from all camera raster routes, traverse relevant offscreen roots, and emit bounded shadow page/BLAS demand. Current native filtering reduces TLAS instances after cut selection. |
| Demand scheduling | Merge independent camera/shadow requests with camera-first classes, saturation, budgets and starvation control; current code prevents camera buffer clobbering but does not implement the scheduler. |
| Renderer option | Generate bounded biased receiver/light rays, resolve visibility into the material lighting path, expose capability-gated selection, and preserve the existing shadow backend on invalid scenes. |
| Vulkan | Native Vulkan AS discovery/resources/builds/ray queries and indirect-build feature fallback. MoltenVK currently supplies raster and interoperability only. |
| Optimization | AS compaction/heap suballocation, build budgets, stable-TLAS reuse/refit, threadgroup/policy sweeps, grouping experiments and complete frame/quality benchmarks. |

No claim is made that these open gates are implemented by the standalone backend
or that its microbenchmark time is the complete renderer's shadow cost.

The [M4 baseline](benchmarks/vg-hardware-raytrace-m4-2026-09-09/README.md) reports
0.909 ms median for the full warm native segment with 1k instances/1M rays and
1.987 ms with 10k instances/1M rays. Corresponding median run p95 values are
4.568 and 5.690 ms. Cold builds of 1k BLASes cost 12.424 ms median. These figures
support cache reuse and build budgeting; they do not establish a renderer budget.

## Build and validation

Final regression run on Apple M4: **5/5 passed**, including the actual GPU tests
(no skips): `RenderGraphTests`, `VirtualGeometryCullingMultipleDispatchesPassTests`,
`VirtualGeometryShadowCutReferenceTests`, `VirtualGeometryHardwareRayTraceGraphTests`
and `VirtualGeometryHardwareRayTraceTests`. The five-repeat release benchmark
matrix completed with all visibility checks passing and zero warm BLAS builds.

```sh
cmake -S . -B build
cmake --build build --target VirtualGeometryHardwareRayTraceTests VirtualGeometryHardwareRayTraceGraphTests VirtualGeometryCullingMultipleDispatchesPassTests RenderGraphTests VirtualGeometryShadowCutReferenceTests -j4
ctest --test-dir build -R '^(VirtualGeometryHardwareRayTraceTests|VirtualGeometryHardwareRayTraceGraphTests|VirtualGeometryCullingMultipleDispatchesPassTests|RenderGraphTests|VirtualGeometryShadowCutReferenceTests)$' --output-on-failure
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target VirtualGeometryHardwareRayTraceBenchmark -j4
python3 scripts/benchmark-vg-hardware-raytrace.py --executable build-release/tests/VirtualGeometryHardwareRayTraceBenchmark --output /tmp/vg-rt-benchmarks
```

Native tests require a Metal GPU accessible to the process. Code 77 is an explicit
skip, not a passing GPU test. The native suite covers counts generated by GPU
compute into private buffers, actual intersections, ray bounds and masks,
transforms, empty scenes, overflow, stale handles, in-flight eviction, cache reuse,
and offscreen-blocker parity before/after conservative light filtering.

`-DENGINE_VG_HARDWARE_RAYTRACE=OFF` excludes the native library and GPU targets;
the default is enabled on Apple builds. The backend needs a Metal 3.1-capable
shader toolchain and macOS 14 or newer at runtime. This build option does not
automatically switch the renderer's shadow backend.

`VirtualGeometryHardwareRayTraceGraphTests` executes four real graph frames with
MoltenVK-exported buffers and shared events, native BLAS/TLAS/trace, and a graph
copy into readback. It verifies hit/miss output and exactly one BLAS build.

The culling regression now prepares deferred page decoding and frame overrides,
uploads matrices in column-major order, and validates current padded index/span
formats and the single-draw fallback. The Suzanne fixture previously exercised
zero visible clusters; it now validates eight decoded clusters and an identical
eight-cluster scene cut in opposite camera orientations without overwriting
camera page priorities.

Benchmark output distinguishes BLAS-only, TLAS preparation/build, trace with a
prebuilt TLAS, and the full native segment. Warm suites use 120 warmup frames and
600 measured frames, repeated five times in interleaved order. Cold BLAS suites
use 20 measured frames per run. Runs intentionally serialize diagnostic waits;
timestamps measure native GPU command-buffer elapsed time, including scheduling
gaps. They exclude receiver generation, VG traversal, streaming, the MoltenVK
handoff, and lighting resolve. They are not full-frame performance numbers.

The [new any-hit/closest-hit comparison](benchmarks/hybrid-shadow-foundation-m4-2026-09-09/README.md)
adds ten interleaved release runs. See the [VSM status](hybrid-virtual-shadow-cache-status.md)
for the integrated raster scheduling, tile lights and distance work. RT depth page
production and temporal area/sky pages remain separate implementation gates.
