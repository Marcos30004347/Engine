# BLAS completion at page installation

**Latest integration update:** [GPU cuts, RT depth pages, screen-first handoff and VSM light BVH](hybrid-shadow-gpu-integration-status.md). This update supersedes the older open-gate entries below for those components.

September 9, 2026. Encoded file/runtime pages and GPU-generated terrain pages
now have an opt-in BLAS completion gate. This is an integration milestone, not the finished hybrid
shadow renderer. Native Vulkan acceleration structures remain deferred.

## Publication invariant

For a scene constructed with `Settings::pageBlasGate`, every cluster in a page
must have a **completed, successful** BLAS before that page publishes its page
entry, residency, decode request or hierarchy install rewrites. Submission alone,
partial admission and partial completion are insufficient. A pending replacement
keeps the old page allocation and hierarchy unchanged. Direct/generated installs
also check readiness, so they cannot bypass the gate.

File roots queue during registration and publish after completion, even before
camera demand exists. File streaming does not interpret a pending BLAS as page-pool
pressure and evict unrelated pages. Runtime page and whole-object installs return
false with a pending diagnostic; callers retry the same immutable input after
`prepareFrame`. Whole-object preparation precedes registration, making that retry
possible without a half-registered object.

The native installer validates encoded cluster descriptors and byte indices, then
expands only positions/indices into temporary GPU triangle inputs. It uses the
same quantization layout as raster decoding, including cross-word and zero-bit
components. Scene raster buffers are allocated and decoded after BLAS completion.
This avoids publishing a provisional page-table entry and works with compressed
raster geometry. The additional expansion is deliberate; it is measured separately
in the [install benchmark](benchmarks/page-install-blas-m4-2026-09-09/README.md).

## Cache and scheduling

`MetalPageBlasGate` owns versioned page manifests. Keys include an independent
object lifetime, local page, 64-bit generation and geometry revision; recycled
page-table/object slots cannot alias the cache. Handles are exposed only when all
clusters complete. Reinstalling an immutable cached page reuses its BLASes.
Replacement commits retire older versions; object destruction/unregistration
retires its manifests. Submitted TLASes retain their existing AS references.

`prepareFrame` pumps an asynchronous native BuildOnly submission in a reserved
backend slot. It never waits on the CPU or reads back geometry/counts. Existing
build-count and AS-output-byte quotas admit work in queue order. BuildOnly does
not copy or retain the complete scene resource-ID table. A failed page allocation
rolls back all newly cached clusters; AS memory pressure returns pending and keeps
the existing cut. The installer does not automatically evict installed pages to
make room in the AS budget. A page larger than the available cache budget remains
pending until capacity becomes available; a cluster above the per-build byte
quota cannot be admitted.

Use `Frame::Work::TlasAndTrace` / graph `Inputs::buildPendingGeometry = false`
with the installer, so the trace pass does not consume a second build quota or
wait for unrelated installation work.

## Opt-in wiring

```cpp
using namespace virtualgeometry::raytracing;
using virtualgeometry::VirtualGeometryScene;
VirtualGeometryMetalRayTraceBackend::Configuration config;
config.metallibPath = VG_RT_METALLIB_PATH;
config.frameSlots = renderFrameSlots + 1;
config.maxBlasBuildsPerFrame = 64; // Tune using target-device measurements.
config.maxBlasBuildBytesPerFrame = 8 * 1024 * 1024;
auto backend = std::make_shared<VirtualGeometryMetalRayTraceBackend>(
    config, graph.getRHI()->exportMetalDevice());
// Check capabilities before enabling the hardware path.
VirtualGeometryScene::Settings settings;
settings.metalExportableBuffers = true;
settings.pageBlasGate = std::make_shared<MetalPageBlasGate>(backend, renderFrameSlots);
VirtualGeometryScene scene(&graph, hierarchyBytes, pageBytes, settings);
// Trace uses slots [0, renderFrameSlots), and buildPendingGeometry=false.
```

Include `MetalPageBlasGate.hpp` in Apple/native-RT code. Keep the reserved slot
exclusive and use this backend for tracing so handles name the same AS cache.
Set the gate at scene construction; switching it on for an already resident scene
requires a separate migration. `runtimePageBlasHandles` and the file streaming
manager's `pageBlasHandles` expose installed manifests for the next scene-cut step.

## Generated geometry and remaining integration

The native gate also accepts a decoded cluster manifest and a shared producer
event. It waits for that event before building and retains build resources through
completion. Counts and ranges come from the install manifest; arbitrary GPU-authored
BLAS counts still are not supported by this descriptor path. Private GPU geometry
and delayed producer signaling are covered by a real Metal intersection test.

Generated terrain is now wired through `VirtualGeometryGeneratedPayloadManager`.
The completed emit result's 64 tile face counters describe the exact cluster
ranges: at most 48 faces per cluster, four vertices and six indices per face.
The bridge derives a compact manifest from those counters and checks its totals
against emitted counts. It adds no vertex/index readback and no extra count pass.
The existing completed-result readback establishes producer completion; the
native gate also retains event-ordered input support for other producers.

`prepareCompleted` prepares a page without publishing it. The sample preflights
every lane in a completed terrain batch before committing any page or parent
flag rewrite. A pending batch retains its completed metadata and retries on later
frames while the emitter stays idle. It does not emit again into buffers being
read by BLAS construction. Pending builds are distinct from publish failures.

Hardware generated reservations have scene-owned lifetime tokens. Installs use
the authenticated allocation and reject stale object owners, overlapping ranges,
manifest changes, missing completion and mismatched emitted cluster counts. The
emitted count gates readiness; the full reservation capacity remains available
for correct retirement. Cancellation immediately drops the producer ticket, but
the scene pins the allocation until native input reads finish. Retirement and
submission checks share the backend lock. Queued builds can be cancelled without
spending future build budget. Object retirement waits to release the old owner
until its outstanding reservations drain, independently of recycled slots.
Only teardown waits for outstanding native reads; frame updates remain nonblocking.

As before, producers must finish GPU writes before reporting completion or
cancelling their reservation. A manager automatically retires completed pending
publications at destruction; unfinished producers still require explicit
completion/cancellation. Static generated and encoded pages are supported;
skinned geometry still needs a deforming-AS policy and is rejected by encoded
immutable builds. The sample supports the gate when supplied through scene
settings; it does not yet expose a complete hardware-shadow user option.

Also outstanding: GPU scene-cut conversion to these handles, camera-first and
shadow streaming-priority merging, replacement-group completeness across pages,
RT VSM depth-page production, area/sky receiver records and temporal convergence,
and full-renderer quality/performance acceptance. Existing raster mode remains the
default when no gate is supplied.

## Validation

- Native Metal tests: two-cluster partial admission, durable completion across
  frame-slot reuse, encoded GPU decode hit parity, malformed index rejection,
  immutable reuse, lifetime isolation, private generated inputs with delayed
  producer events, empty/install-managed trace, and transactional budget rollback.
- Runtime lifecycle tests: pending whole-object retry, unchanged old allocation
  and hierarchy under pending/partial replacement, direct-install bypass refusal,
  complete commit, owner retirement/recycled slots, and file roots whose pending
  AS work does not trigger page-pool eviction. Real compressor output also
  completes the native gate before runtime scene publication.
- Generated payload lifetime tests: pending preparation, compact count versus
  backing capacity, changed-manifest rejection, in-flight cancellation, owner
  retirement, stale-ticket rejection after slot reuse, and delayed reclamation.
- Real terrain GPU generation: emitted geometry enters native Metal BLAS builds
  with a one-cluster submission quota, then publishes its completed manifest.
- Eleven regression suites cover those paths plus graph handoff, scene culling,
  shadow cuts, tile lighting, shadow budgets/policies, soft layers and render graph.
  The release `SparseVoxelRendererSample` also builds with batch preflight/retry
  and passes its three-frame smoke test.
