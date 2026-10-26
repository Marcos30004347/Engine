# Terrain voxel triangle cluster raster plan

This plan replaces the current fragment-DDA brick raster path with triangle
clusters generated from the same installed voxel pages. The destination is not a
separate mesh system: raster clusters and the existing streamed trace payloads
share the same page identity, dependency graph, cut authority, and fallback
rules.

Virtual geometry remains the reference model for page cuts, cluster grouping,
and pixel-projected error. The concrete runtime integration should use the
terrain/trace streamed structure we already have.

## Feasibility read

The approach is very plausible for raster. The current brick renderer pays for
large proxy boxes and then performs voxel DDA in the fragment shader. Even after
tight occupied bounds, the 1220x720 world-map diagnostic still produced roughly
94.6M brick fragments. A triangle path would draw only the exposed surface faces,
then let hardware depth, early-Z, HZB, and cluster culling work on normal
triangles.

The first CPU fixture experiment (`SparseVoxelTerrainTriangleMeshingTests`) uses
the CPU terrain build path, decodes real emitted voxel cells, and compares:

- naive exposed cube faces;
- greedy meshing inside each 8x8x8 brick, which matches the natural streaming
  unit;
- page-wide greedy meshing, which is an optimistic lower bound for cross-brick
  merging.

Measured on the terrain fixture set at LOD 0:

| case | exposed faces | brick-local greedy quads | page-wide greedy quads |
| --- | ---: | ---: | ---: |
| total fixtures | 58,800 | 7,821 | 6,440 |

That is an 86.7% quad reduction for the realistic brick-local version and an
89.0% reduction for the page-wide ceiling. The gap between brick-local and
page-wide is small enough that a brick/page cluster path should be worth testing
before chasing cross-page meshing.

The current prototype uses simple unordered-map CPU code and still emits all
fixtures in about tens of milliseconds total for the meshing stage. A production
CPU path should use fixed dense masks per brick/page and should be much faster.
A GPU path can reuse the same contract once the CPU oracle is stable.

## Shape of the first renderer path

Use exact cubical surface meshing first, not marching cubes.

Marching cubes or dual contouring can come later for smoother silhouettes, but
they change the surface. That means they need a stronger error metric,
crack/seam certification, material interpolation rules, and trace/raster parity
tests. Exact exposed-face meshing is boring in the best possible way: it should
match the installed voxel occupancy exactly and is therefore easy to compare
against Tree64 tracing.

Each generated raster page gets a triangle sidecar:

```text
TerrainPage
  voxel payloads          existing raster + Tree64/trace data
  triangle cluster table  new raster sidecar
  vertex stream           quantized local positions, packed normals, uv, material
  index stream            u16 indices per cluster
  dependency metadata     same page/parent/fallback identity as voxel payloads
```

Recommended first cluster format:

- one cluster per brick, split every 64 quads / 128 triangles;
- 16-bit local position in page or brick space;
- 3-bit face normal or octa-normal if we later add smoothing;
- simple generated UV from dominant face axes;
- packed terrain material/context copied from the exposed voxel face;
- per-cluster AABB, normal cone, page id, LOD/group id, and triangle range.

This format maps cleanly to normal indexed triangle draws today and to the same
page/cut machinery that already drives streamed trace payloads. The important
part is the contract: the cluster is a page-resident raster representation of
the same voxel cut, not a new independent LOD system.

## Unified streamed geometry contract

The raster cut and trace cut must share identity.

Raster may use triangle clusters for the G-buffer, but lighting and shadows keep
using the installed voxel/Tree64/virtual trace pages. This avoids the common
trap where the screen shows one geometry representation while shadows query
another.

Runtime rule:

```text
If triangle sidecar is resident for the selected voxel page/group:
    raster triangle clusters
else:
    raster current voxel brick path as fallback

Lighting/shadows:
    trace the existing streamed trace payload for the same selected page
```

A page can become visible only when both required contracts are satisfied:

- the trace payload is available, or a parent/fallback trace page is protected;
- the raster payload is available, or the old voxel brick raster fallback is
  selected for that page.

The page state should evolve toward:

```text
TerrainGeometryPage
  identity
    logical page id
    generation/revision
    parent/fallback page id
    cut/group id

  trace payload
    Tree64 / virtual trace nodes
    trace materials
    trace page dependencies

  raster payload
    triangle cluster descriptors
    vertices
    indices
    raster materials

  shared metadata
    bounds
    geometric error in metres
    projected error policy
    seam/group flags
    residency state
```

That gives us one terrain geometry page with two views: a trace view and a
raster view. The CPU/GPU cut never chooses a raster page without choosing the
matching trace page or a protected parent fallback.

## Pass topology

Generation, streaming, raster, and tracing should be separate passes with an
explicit page-state handoff between them. Avoid a monolithic "terrain build"
kernel that quietly mutates everything at once; it is too hard to test and too
easy to publish half a page.

Recommended shared streaming/build/raster pass families:

```text
Terrain.PageRequest
  consumes camera/light/trace feedback
  writes desired page ids, desired LOD/error, and priority

Terrain.PagePlan
  resolves parent/fallback dependencies
  reserves hidden page slots and payload ranges

Terrain.SourceGenerate
  generates/samples source occupancy, materials, height/error metadata

Terrain.TriangleClassify
  classifies exposed faces from occupancy
  writes per-brick/per-page face counts

Terrain.TrianglePrefix
  allocates cluster/vertex/index ranges

Terrain.TriangleEmit
  greedy-meshes faces into triangle clusters
  writes raster cluster descriptors, vertices, and indices

Terrain.DependencyBuild
  writes parent/fallback/child dependency edges and group/cut identity

Terrain.PageValidate
  checks raster ranges, trace ranges, dependency ranges, byte limits, and
  generation identity

Terrain.PagePublish
  atomically commits page state or leaves old/protected fallback pages active

Terrain.RasterCull
  frustum/HZB/error culls raster clusters for the selected cut

Terrain.RasterDraw
  draws triangle clusters into the existing G-buffer
  falls back per page to brick raster while sidecars are missing
```

Recommended native compute-raster backend pass shape. This is an optional
backend sibling of hardware triangle raster, not a separate terrain
representation. It consumes the same committed page ids, meshlet/cluster data,
cut/group identity, and material contract as `Terrain.RasterDraw`.

```text
Terrain.ComputeRasterCapabilities
  probes backend support:
    Metal compute raster through native Metal interop
    Vulkan compute raster through portable SPIR-V compute
    DirectX compute raster through HLSL compute

Terrain.ComputeRasterPlan
  reads renderer settings and backend capabilities
  disables the path before graph creation when unsupported
  gates all backend-only buffer/texture allocations

Terrain.ComputeRasterBin
  bins visible meshlet/cluster triangles into screen tiles
  writes compact per-tile primitive ranges and overflow counters

Terrain.ComputeRasterHZB
  consumes the previous/current HZB and rejects hidden clusters/tiles

Terrain.ComputeRasterFine
  software-rasterizes small or high-overdraw clusters into the G-buffer
  uses the same depth, coverage, material, motion, and identity outputs as the
  hardware VG raster path

Terrain.ComputeRasterMerge
  merges compute-raster output with hardware raster output when the route is
  hybrid
```

The route must be selected by capability plus a uniform/settings bit:

```text
if !backendSupportsComputeRaster or !settings.enableTerrainComputeRaster:
    do not allocate compute-raster-only buffers
    do not register compute-raster passes
else:
    allocate backend buffers
    register compute raster passes
    publish counters for benchmark/parity tests
```

First implementation target:

- Metal-only compute raster, because Apple/MoltenVK is the current platform
  where the brick draw overdraw is most visible;
- keep it behind an explicit renderer setting and/or CLI flag;
- never make it the default until it beats hardware VG raster and passes
  G-buffer/trace parity;
- use tile binning and fixed-size tile queues inspired by Nanite-style software
  raster, but keep terrain page identity and dependency rules unchanged.

Recommended compute/software trace pass families:

```text
Terrain.TraceSchedule
  schedules screen, shadow, and off-screen rays against selected trace pages

Terrain.ComputeTraceBuild
  builds Tree64 / virtual trace payloads used by compute traversal when the
  source page is produced on GPU or CPU

Terrain.ComputeTraceCompact
  compacts/sorts ray work, trace requests, and expansion queues by error,
  distance, and ray class

Terrain.ComputeTrace
  traces existing streamed Tree64 / virtual trace payloads
  emits page requests for unresolved or too-coarse trace nodes
```

Recommended native ray tracing backend pass shape. These passes are backend
siblings of the compute trace path; they should share page identity, dependency
records, cut decisions, fallback rules, and request queues. MetalRT is the first
research backend because our current renderer is MoltenVK-backed on Apple, but
the same contract should be usable later for VulkanRT and DirectXRT.

```text
Terrain.RTBackendCapabilities
  probes backend support:
    MetalRT through native Metal interop
    VulkanRT through VK_KHR_acceleration_structure/ray_tracing_pipeline
    DirectXRT through DXR

Terrain.RTASPlan
  consumes committed page ids, cut/group identity, dependency state, and dirty
  page revisions
  schedules backend-specific AS build/refit/retire work

Terrain.RTBLASBuild
  builds/refits backend acceleration structures for committed triangle cluster
  pages or virtual node/group bounding boxes

Terrain.RTTLASPublish
  publishes top-level/instance acceleration structures referencing only
  committed page generations admitted by the shared cut authority

Terrain.RTTrace
  runs backend ray tracing / intersection shaders
  writes lighting/shadow hits and page requests for unresolved finer nodes

Terrain.RTRetire
  releases old AS objects only after raster, compute trace, native RT, and
  backend command-buffer references have drained
```

Milestone-2 MetalRT concrete pass names may wrap the generic RT backend passes
where platform-specific resource ownership matters:

```text
Terrain.MetalRTCapabilities
  probes Apple/Metal RT support and MoltenVK Metal-object interop support

Terrain.MetalRTASPlan
  consumes committed page ids, cut/group identity, dependency state, and dirty
  page revisions
  schedules AS build/refit/retire work

Terrain.MetalRTBLASBuild
  builds/refits Metal primitive acceleration structures for committed triangle
  cluster pages or virtual node/group bounding boxes

Terrain.MetalRTTLASPublish
  publishes a TLAS/instance AS that references only committed page generations
  admitted by the shared cut authority

Terrain.MetalRTTrace
  runs MSL ray tracing/intersection shaders
  writes lighting/shadow hits and page requests for unresolved finer nodes

Terrain.MetalRTRetire
  releases old AS objects only after Vulkan, software trace, raster, and Metal
  command-buffer references have drained
```

Milestone 2 research revised three details of that MetalRT topology.
`Terrain.MetalRTBLASBuild` builds one BLAS **per group**, not per cluster and
not per page, and it builds triangle clusters *and* node bounding boxes as two
coexisting views, because the hardware triangle intersector measured 5.3x faster
than a custom-AABB shader loop. There is no refit path: every page publication
pushes different clusters, so every upload is a full build.
`Terrain.MetalRTTLASPublish` is a full per-frame indirect rebuild rather than
incremental surgery, because a 32,768-instance TLAS rebuilds in 0.82 ms.
See "Milestone 2" below for the measurements behind both.

Each pass should have small counters and invariants. The page publisher is the
only pass allowed to move hidden ranges into the committed page table.

## Phases

### Phase 0: CPU oracle and feasibility benchmark

Started with `SparseVoxelTerrainTriangleMeshingTests`, and promoted into the
runtime-side CPU builder in `SparseVoxelTerrainTriangleMesh`.

Goals:

- build terrain via the CPU path;
- extract occupied cells and materials from the existing semantic decoder;
- count naive exposed faces;
- greedy mesh per brick and per page;
- report quads, estimated cluster count, estimated upload bytes, and CPU time.

Regression tests:

- greedy quads are never greater than exposed faces;
- page-wide greedy is never greater than brick-local greedy;
- all fixtures emit non-empty surfaces;
- material-aware merging never merges unlike material keys;
- production sidecar output matches the independent CPU oracle for occupied
  cells, exposed faces, and brick-local greedy quads;
- rebuilding the same page produces the same sidecar hash;
- every emitted cluster stays within the 64-quad / 128-triangle meshlet budget;
- the sidecar can be mirrored into a one-page virtual-geometry build preview.

Runtime-side contract now present:

```text
SparseVoxelTerrainTrianglePageSidecar
  vertices
  indices
  clusters
  page identity: chunk index, region mapping, brick revision, Tree64 revision
  counters: occupied cells, exposed faces, greedy quads

SparseVoxelTerrainTriangleVirtualGeometryPreview
  page count
  hierarchy node count
  meshlet count
  group count
  byte estimates
  cluster-geometry compatibility flag
```

The current virtual-geometry preview is a compatibility shim, not the desired
runtime endpoint. It proves the emitted terrain clusters fit the same cluster
size and hierarchy vocabulary, but the next runtime phase should upload them
through the terrain streamed page system beside the trace payloads.

Phase 0 validation snapshot:

```text
Command:
  cmake --build build-release --target \
    SparseVoxelTerrainPhase0Tests \
    SparseVoxelTerrainConvergencePhase0Tests \
    SparseVoxelTerrainPhase0Benchmark \
    SparseVoxelTerrainTriangleMeshingTests -j 8

  ctest --test-dir build-release -R \
    'SparseVoxelTerrain(Phase0CPU|ConvergencePhase0|Phase0BenchmarkSmoke|TriangleMeshingCPU)' \
    --output-on-failure

Result:
  4/4 tests passed
  total phase0 test time: 1.24s

Triangle oracle totals, LOD 0:
  occupied cells: fixture-dependent
  naive exposed quads: 58,800
  brick-local greedy quads: 7,821
  page-wide greedy quads: 6,440
  brick-local reduction: 86.7%
  page-wide reduction: 89.0%
  64-quad brick clusters: 123
  brick-local estimated upload: 588.152 KiB
  page-wide estimated upload: 484.281 KiB
  brick-local oracle time: 41.737 ms
  page-wide oracle time: 47.847 ms
```

### Phase 1: Production CPU sidecar builder

Move the prototype into `src/voxel/svraster` as a reusable builder.

Implementation:

- consume `SparseVoxelBrickHierarchy` plus page records;
- use dense per-slice masks over each brick-local meshing bound;
- cull faces against neighbour cells across brick boundaries within the same
  page;
- emit stable deterministic cluster records;
- keep one sidecar per terrain page generation.
- expose a runtime validation hook so streaming/upload code can reject malformed
  sidecars before page publication.

Tests:

- exact face parity against a CPU occupancy oracle;
- deterministic output hash across repeated builds;
- material/context parity for top, side, and bottom faces;
- no internal faces between adjacent occupied cells;
- no missing exterior faces;
- validator accepts every generated fixture sidecar;
- validator rejects out-of-range indices, over-budget clusters, and invalid
  normals.

Phase 1 landed:

```text
Runtime API:
  buildSparseVoxelTerrainTrianglePageSidecar
  validateSparseVoxelTerrainTrianglePageSidecar
  sparseVoxelTerrainTrianglePageSidecarHash
  summarizeSparseVoxelTerrainTriangleVirtualGeometryPreview
  buildSparseVoxelTerrainTriangleVirtualGeometryPreview

Validation:
  ctest --test-dir build-release -R \
    'SparseVoxelTerrain(Phase0CPU|ConvergencePhase0|Phase0BenchmarkSmoke|TriangleMeshingCPU)' \
    --output-on-failure

Result:
  4/4 tests passed
  total test time: 0.59s
```

### Phase 2: Streamed terrain page integration

Add triangle sidecar storage to the terrain streamed page model, but keep the
current brick raster as the fallback/default until signatures are stable.

Implementation:

- extend page residency records with optional raster-triangle ranges;
- upload sidecar buffers through the same page residency update as trace
  payloads;
- preserve parent/fallback trace payloads while child raster sidecars are
  missing;
- expose residency counters for pages with trace-only, raster-only, and
  trace+raster payloads;
- do not introduce a second independent raster page cache.

Tests:

- installing a page installs matching trace/raster identities;
- missing triangle sidecar falls back to brick raster without holes;
- stale sidecar generation/revision is rejected;
- parent fallback remains protected while a child raster sidecar is unavailable.

Phase 2 landed:

```text
Shared page table additions:
  SparseVoxelPhysicalPageAllocation::triangleClusters
  SparseVoxelPhysicalPageAllocation::triangleVertices
  SparseVoxelPhysicalPageAllocation::triangleIndices

Allocator additions:
  SparseVoxelPhysicalBufferKind::TriangleClusters
  SparseVoxelPhysicalBufferKind::TriangleVertices
  SparseVoxelPhysicalBufferKind::TriangleIndices

Runtime API:
  sparseVoxelProducedPagePayloadFromTerrainTriangleSidecar

Residency behavior:
  triangle sidecars allocate through the shared page table
  triangle ranges count toward packed page-slot byte budgets
  triangle-only raster sidecars may install without scene-tree rewrites
  malformed/stale sidecars fail before range reservation
  missing child sidecars keep selecting the resident root/fallback page
  retirement fences and reclaims triangle ranges through the same allocators
```

Phase 2 validation snapshot:

```text
Command:
  cmake -S . -B build-release
  cmake --build build-release --target \
    SparseVoxelTerrainTrianglePhase2Tests \
    SparseVoxelStreamingTests -j 8

  ctest --test-dir build-release -R \
    'SparseVoxelStreaming(G16Tests|GPUContractG16Tests)|SparseVoxelTerrainTrianglePhase2Streaming' \
    --output-on-failure

  ctest --test-dir build-release -R \
    'SparseVoxelTerrain(Phase0CPU|ConvergencePhase0|Phase0BenchmarkSmoke|TriangleMeshingCPU)|SparseVoxelTerrainTrianglePhase2Streaming' \
    --output-on-failure

Result:
  streaming regression: 3/3 tests passed
  terrain/triangle focused regression: 5/5 tests passed
```

This phase defines the page contract used by the GPU publisher later. A page is
not considered fully promoted until both payload views are known:

```text
TerrainGeometryPageState
  source/identity
  trace range: nodes/materials/accel metadata
  raster range: clusters/vertices/indices
  dependency range: parents/fallbacks/children
  publication state: hidden, validating, committed, retiring
```

CPU pages may fill this contract immediately. GPU pages must write it through a
hidden publication lane and commit it atomically.

### Phase 3: Renderer fallback draw path

Add triangle cluster buffers and draw pass.

Implementation:

- add cluster frustum culling and indirect draw args;
- draw triangle clusters into the existing G-buffer format;
- fallback per page to brick raster when the sidecar is missing.

Tests:

- raster signature parity against brick raster on fixed CPU fixtures;
- depth parity within one voxel face epsilon;
- material id parity at representative sample pixels;
- fallback test where half the pages have triangle sidecars and half use brick
  raster.

Phase 3 landed the renderer-facing draw-selection contract:

```text
Runtime API:
  buildSparseVoxelTerrainTriangleRasterDrawList

Renderer contract:
  select the desired streamed page through the shared page table
  choose triangle clusters when the selected page has matching resident sidecar
    ranges and matching geometry revision
  emit one VkDrawIndexedIndirect-compatible command per resident triangle
    cluster
  use page-local sidecar indices with the resident vertex-range base as
    vertexOffset
  fall back to brick raster for the selected page only when it has brick ranges
  fall back to the protected root brick page when a desired triangle sidecar is
    missing, stale, or not drawable
```

Phase 3 test coverage:

- missing child sidecar emits no triangle draws and keeps the root brick
  fallback selected;
- resident child sidecar emits indexed cluster draw commands with correct
  firstIndex, vertexOffset, firstInstance, page id, cluster id, and LOD group;
- stale sidecar metadata is counted and falls back to the protected root brick
  page instead of drawing mismatched triangles.

Phase 3 validation snapshot:

```text
Command:
  cmake -S . -B build-release
  cmake --build build-release --target \
    SparseVoxelTerrainTrianglePhase3Tests -j 8

  ctest --test-dir build-release -R \
    'SparseVoxelTerrain(Phase0CPU|ConvergencePhase0|Phase0BenchmarkSmoke|TriangleMeshingCPU)|SparseVoxelTerrainTrianglePhase(2Streaming|3Renderer)|SparseVoxelStreaming(G16Tests|GPUContractG16Tests)' \
    --output-on-failure

Result:
  phase3 renderer contract: 1/1 tests passed
  phase0-3 + streaming focused regression: 8/8 tests passed
```

Remaining Phase 3 GPU work:

- bind the triangle cluster/vertex/index buffers in a graphics pass;
- run the emitted indexed indirect commands into the existing G-buffer;
- add image/depth/material parity tests against brick raster fixtures. The CPU
  draw-selection tests are the safety rail for this pass, not a replacement for
  the final GPU parity tests.

### Phase 4: HZB and Niagara-style cluster culling

Apply the same early/late idea used by Niagara, now on much smaller triangle
clusters instead of fat voxel boxes:

```text
early pass:
    draw clusters visible last frame / selected occluder clusters
build HZB
late pass:
    frustum + HZB test all remaining clusters
```

Cluster bounds should be tight AABBs from emitted quads, with optional normal
cones for backface/cone rejection on terrain slopes and cliffs.

Tests:

- culling equivalence with occlusion disabled;
- disocclusion test with previous-HZB rejection disabled on camera cut;
- no missing clusters after camera teleport;
- overdraw counter drops relative to brick raster.

Phase 4 landed the CPU culling oracle and renderer contract:

```text
Runtime API:
  buildSparseVoxelTerrainTriangleCullPlan

Culling contract:
  consume the Phase 3 triangle draw list
  reject invalid/empty draw commands before submission
  apply current-frame frustum culling to every triangle cluster
  place previously-visible in-frustum clusters in the early pass
  optionally seed deterministic occluder clusters into the early pass
  reject remaining late-pass clusters with HZB visibility flags
  disable previous-visibility reuse and HZB rejection on camera cuts
  emit compact early/late VkDrawIndexedIndirect-compatible command streams
```

The camera-cut rule is intentionally conservative: after a teleport or hard cut,
the culler keeps only current frustum rejection active and redraws all
in-frustum clusters. That is the guard rail against exactly the “missing chunk
after disocclusion” failure mode that made the brick path scary.

Counters now exposed by the contract:

```text
inputDrawCount
frustumRejectedCount
hzbRejectedCount
previousVisibleEarlyCount
occluderEarlyCount
submittedIndexCount
rejectedIndexCount
```

Phase 4 test coverage:

- HZB-disabled culling keeps parity with plain frustum + draw submission;
- previous-visible clusters go through the early pass even if the previous HZB
  says occluded;
- late non-camera-cut clusters can be rejected by HZB;
- camera cuts disable previous-frame early reuse and HZB rejection while keeping
  current frustum rejection;
- deterministic occluder seeding cannot override frustum rejection.

Phase 4 validation snapshot:

```text
Command:
  cmake -S . -B build-release
  cmake --build build-release --target \
    SparseVoxelTerrainTrianglePhase4Tests -j 8

  ctest --test-dir build-release -R \
    'SparseVoxelTerrain(Phase0CPU|ConvergencePhase0|Phase0BenchmarkSmoke|TriangleMeshingCPU)|SparseVoxelTerrainTrianglePhase(2Streaming|3Renderer|4Culling)|SparseVoxelStreaming(G16Tests|GPUContractG16Tests)' \
    --output-on-failure

Result:
  phase4 culling contract: 1/1 tests passed
  phase0-4 + streaming focused regression: 9/9 tests passed
```

Remaining Phase 4 GPU work:

- port this CPU oracle into a compute cull pass that consumes GPU cluster bounds;
- sample the real HZB texture instead of the CPU test visibility flags;
- write early/late indirect command buffers on GPU;
- add renderer overdraw counters comparing triangle clusters against brick
  raster on the same camera path.

### Phase 5: Cut, streaming, and dependency integration

Triangle sidecars must follow the page dependency model, not become a separate
cache with separate truth.

Implementation:

- page slot owns voxel source metadata + trace payload + optional triangle
  raster sidecar;
- parent/fallback page stays resident while child triangle sidecar is missing;
- eviction only retires a page after no raster or trace consumer references it;
- expose counters: triangle pages resident, sidecar bytes, fallback brick pages,
  missing sidecars, protected parent pages;
- reuse the existing trace streamed structure's dependency edges for page
  install, parent protection, and retirement;
- make the cut selector request raster sidecars for the same pages/groups it
  requests for tracing.

Tests:

- async streaming with sidecar lag never produces holes;
- parent fallback remains valid during child sidecar generation;
- page eviction does not remove trace data used by visible triangle pages;
- screen cut and trace cut keep matching page identities;
- off-screen lighting/shadow requests can request matching raster/trace page
  identities without depending on screen visibility.

Phase 5 landed the unified cut/dependency contract:

```text
Runtime API:
  selectSparseVoxelUnifiedGeometryLOD
  traverseSparseVoxelUnifiedGeometryLOD
  summarizeSparseVoxelPageConsumerReferences
  canEvictSparseVoxelPageWithConsumers

Unified cut rule:
  a desired page may win the cut only when its required views are resident:
    raster payload, by default
    direct trace payload, by default
    material trace payload, optional

  if the desired page is resident but incomplete:
    keep the protected root/fallback page selected
    keep requesting the desired page identity for completion

  if the desired page is missing:
    request the full dependency closure plus the desired page
    keep the protected root/fallback page selected
```

This is the important “no raster-only terrain” seatbelt. Phase 2 still permits a
triangle-only sidecar to be installed for compatibility and staged upload, but
the Phase 5 selector will not let that page become the unified renderer cut
while its trace view is absent. The renderer can therefore avoid showing
geometry that lighting/shadow rays cannot query under the same page identity.

Phase 5 also added transient consumer protection for retirement:

```text
Consumer references:
  page id
  page generation
  consumer kind: raster or trace

Retirement rule:
  base page-table eviction must allow retirement
  AND no live raster consumer references the same page generation
  AND no live trace consumer references the same page generation

Stale generation references:
  diagnosed, but do not block retirement
```

Counters exposed by unified traversal:

```text
inputCandidateCount
selectedChildCount
selectedFallbackCount
requestedPageCount
missingRasterPageCount
missingTracePageCount
missingMaterialPageCount
brickFallbackPageCount
triangleRasterPageCount
tracePageCount
protectedParentPageCount
offscreenRequestCount
triangleSidecarBytes
```

Phase 5 test coverage:

- a triangle-only child sidecar is counted as raster-resident but cannot win the
  unified cut until its trace payload arrives;
- resident-but-incomplete pages are still requested so streaming can complete
  them instead of going silent;
- a child page with matching triangle + trace payloads wins the screen cut and
  matches the trace receiver generation;
- off-screen exact shadow/lighting feedback can request the same missing page
  identity without depending on screen visibility;
- live raster and trace consumers block retirement of the same page generation;
- stale consumer generations are diagnosed separately and do not block
  retirement;
- parent/fallback pages stay protected while child sidecar uploads are still
  in-flight.

Phase 5 validation snapshot:

```text
Command:
  cmake -S . -B build-release
  cmake --build build-release --target \
    SparseVoxelTerrainTrianglePhase5Tests -j 8

  ctest --test-dir build-release -R \
    'SparseVoxelTerrain(Phase0CPU|ConvergencePhase0|Phase0BenchmarkSmoke|TriangleMeshingCPU)|SparseVoxelTerrainTrianglePhase(2Streaming|3Renderer|4Culling|5StreamingDependencies)|SparseVoxelStreaming(G16Tests|GPUContractG16Tests)' \
    --output-on-failure

Result:
  phase5 streaming/dependency contract: 1/1 tests passed
  phase0-5 + streaming focused regression: 10/10 tests passed
```

Remaining Phase 5 renderer/scheduler wiring:

- switch the production triangle-cluster renderer from the legacy
  raster-only selector to `selectSparseVoxelUnifiedGeometryLOD`;
- feed the unified traversal page requests into the async streaming scheduler;
- plumb real per-frame raster/trace consumer references from command-buffer
  lifetime fences into eviction decisions;
- expose the new counters in renderer diagnostics beside the existing terrain
  residency counters.

### Phase 6: GPU page build, acceleration build, and publication

Once the CPU builder is trusted, implement the production GPU path for full-GPU
terrain. This phase must build the raster triangle payload and the trace
acceleration payload together, then publish the page as one transaction. A
triangle-only page is not a complete page.

Recommended GPU page-build approach:

1. run `Terrain.SourceGenerate` to produce occupancy/material/error metadata;
2. run `Terrain.TriangleClassify` to classify exposed faces from occupancy
   words;
3. run `Terrain.TrianglePrefix` to prefix-sum face counts per brick/page;
4. run `Terrain.TriangleEmit` to greedily merge faces within each brick
   face/slice using shared-memory masks and emit quads/vertices/indices into
   page-local append ranges;
5. build cluster AABBs and indirect draw records;
6. run `Terrain.ComputeTraceBuild` to build/update the streamed compute-trace
   acceleration payload for the same page:
   Tree64 / virtual trace nodes, material cells, page descriptors, and any
   compact child/bounds records;
7. run `Terrain.DependencyBuild` to emit dependency records: parent page,
   fallback page, children, and group/cut identity;
8. run `Terrain.PageValidate` to validate raster and trace payload counts
   against reserved page-slot capacity;
9. run `Terrain.PagePublish` to publish the hidden page atomically by updating
   page state, raster ranges, trace ranges, dependency edges, and
   generation/revision together.

This is likely faster than CPU for runtime-generated pages, especially because
the data already lives on GPU during full-GPU terrain generation. More
importantly, it prevents the renderer from moving the bottleneck to CPU
meshing/upload.

Required GPU buffers:

- source occupancy/material words;
- triangle cluster descriptors;
- triangle vertices and indices;
- trace acceleration nodes/materials/page descriptors;
- page dependency records;
- hidden publication state;
- committed page table;
- overflow/error counters.

Publication rule:

```text
if raster payload valid AND trace payload valid AND dependencies valid:
    commit page generation
else:
    keep previous page or protected parent fallback active
```

This mirrors the existing streamed trace safety model: no partial page can
become visible, no sidecar can outrun its trace payload, and no acceleration
structure can be retired while raster still references the page identity.

Tests:

- GPU output count/hash matches CPU oracle on synthetic pages;
- GPU triangle payload matches CPU sidecar face/quad/material counts;
- GPU trace payload matches the existing CPU/GPU Tree64 oracle for the same
  source occupancy;
- GPU page publication writes trace and raster payload state atomically or
  leaves the old page/fallback active;
- page generation respects raster, trace, and dependency byte limits;
- overflow falls back to the previous page or parent brick/trace fallback
  instead of publishing partial mesh data;
- renderer never observes a committed page with only raster or only trace
  payload ranges;
- page retirement waits for both raster and trace references to drain.

Phase 6 landed the GPU-build publication contract:

```text
Runtime API:
  validateSparseVoxelTerrainTriangleGPUPageBuild
  sparseVoxelProducedPagePayloadFromTerrainTriangleGPUBuild
  prepareSparseVoxelTerrainTriangleGPUPageBuildPublication
  publishSparseVoxelTerrainTriangleGPUPageBuild

Required GPU stage flags:
  SourceGenerate
  TriangleClassify
  TrianglePrefix
  TriangleEmit
  ComputeTraceBuild
  DependencyBuild
  PageValidate

Publication contract:
  validate GPU result readback / hidden metadata
  reject if status is not Ready or validationErrors != 0
  reject if any required stage flag is missing
  reject triangle-only pages when trace payload is required
  reject trace-only pages when triangle payload is required
  reject child pages without parent/fallback dependency metadata
  reject payloads that exceed the packed page-slot byte budget
  convert only complete results into SparseVoxelProducedPagePayload
  reserve/copy hidden page ranges before the final install rewrite
  final commit still goes through the shared page table generation and
    dependency checks
```

The important behavior is now explicit: a full-GPU terrain page can be staged in
hidden storage, but it cannot become renderer-visible unless the triangle
raster payload, direct trace payload, material payload when requested, and
dependency metadata all describe the same page generation/revision.

Phase 6 API note:

```text
prepareSparseVoxelTerrainTriangleGPUPageBuildPublication(...)
  validates the GPU build packet
  converts it to a produced page payload
  reserves shared physical ranges
  marks the hidden copy complete

caller then synthesizes install rewrites from publication.reservation
and commits through installCopiedPagePayload(...)
```

That two-step shape is intentional because install rewrites usually need the
reserved physical hierarchy-node offsets. A convenience one-shot publish helper
also exists for callers that already have the correct rewrite records.

Phase 6 test coverage:

- incomplete GPU build with triangle raster but missing trace nodes is rejected
  before it becomes a produced payload;
- complete GPU build validates raster, trace, material, dependency and byte
  budget metadata;
- produced payload carries triangle ranges, trace ranges, material ranges,
  geometry revision, and rewrite counts;
- invalid stage flags reject before any triangle storage is reserved;
- valid prepared publication reserves/copies hidden ranges, then commits
  atomically through the shared page table;
- committed GPU page becomes `unifiedGeometryVisible()` and wins the Phase 5
  unified cut;
- packed page-slot overflow rejects before hidden range reservation and keeps
  root fallback selected;
- copied child page cannot commit while its parent dependency is not resident;
- failed dependency commit leaves the child upload-queued and non-visible, while
  traversal requests the missing parent and incomplete child closure.

Phase 6 validation snapshot:

```text
Command:
  cmake -S . -B build-release
  cmake --build build-release --target \
    SparseVoxelTerrainTrianglePhase6Tests -j 8

  ctest --test-dir build-release -R \
    'SparseVoxelTerrain(Phase0CPU|ConvergencePhase0|Phase0BenchmarkSmoke|TriangleMeshingCPU)|SparseVoxelTerrainTrianglePhase(2Streaming|3Renderer|4Culling|5StreamingDependencies|6GPUBuildPublication)|SparseVoxelStreaming(G16Tests|GPUContractG16Tests)' \
    --output-on-failure

Result:
  phase6 GPU-build publication contract: 1/1 tests passed
  phase0-6 + streaming focused regression: 11/11 tests passed
```

Remaining Phase 6 GPU work:

- implement the actual WGSL/Metal compute passes for `TriangleClassify`,
  `TrianglePrefix`, and `TriangleEmit`;
- build/populate the direct trace payload from the same GPU source occupancy;
- write the GPU-side dependency records into the hidden publication packet;
- add GPU readback/hash parity against the CPU triangle sidecar oracle;
- feed prepared publications from the GPU scheduler into the shared page table
  with real render-thread copy/commit fences.

### Phase 7: plug terrain triangles into virtual geometry raster

Terrain triangle clusters should not grow a separate runtime CPU culler. The
correct raster path is:

```text
terrain voxel page
  -> exposed-face / greedy triangle sidecar
  -> virtual-geometry clusters
  -> VirtualGeometryBuilder page/group/rewrite finalization
  -> VirtualGeometryCompressor encoded pages
  -> VirtualGeometryScene streaming + GPU page decode
  -> VirtualGeometryRendererPass prepass cull, HZB, final cull, hardware draw
  -> G-buffer
```

That means GPU overdraw and culling optimization happens in the existing
virtual-geometry culling/render passes. CPU culling remains an oracle only for
parity tests; it must not become the hot renderer path.

Phase 7 landed the first editor/build-side bridge:

```text
Runtime/editor API:
  buildSparseVoxelTerrainTriangleVirtualGeometryAsset(...)

Bridge contract:
  validate the terrain triangle sidecar
  reject clusters that exceed virtual-geometry meshlet limits
  convert terrain triangle clusters to local virtual-geometry clusters
  preserve terrain cluster bounds and self error for VG LOD bounds
  let VirtualGeometryBuilder create real hierarchy nodes, groups, pages, and
    install/uninstall rewrite records
  compress the finalized result into VirtualGeometryEncodedData
```

Phase 7 test coverage:

- production cliff terrain fixture builds a triangle sidecar;
- the sidecar is promoted into a real encoded virtual-geometry asset;
- encoded pages, meshlets, group spans, shape, and hierarchy are present;
- encoded meshlets respect the `ClusterSize` triangle budget;
- decoded meshlet positions are finite;
- bridge stats expose source clusters/triangles, encoded pages, encoded
  meshlets, groups, hierarchy nodes, and build time.

Phase 7 local measurement:

```text
Fixture:
  terrain_phase0::FixtureKind::Cliff

Result:
  source_clusters=94
  source_triangles=364
  encoded_pages=1
  encoded_meshlets=94
  encoded_groups=12
  encoded_hierarchy_nodes=14
  sidecar_to_virtual_geometry_encode=1.868 ms
```

Remaining Phase 7 runtime work:

- register terrain-produced `VirtualGeometryEncodedData`/file-backed pages into
  `VirtualGeometryScene`;
- map terrain material payloads into the VG material/G-buffer contract;
- make terrain page residency/fallback protect the matching trace payload while
  the VG raster page streams;
- remove the terrain-specific triangle draw-list/cull path from runtime use and
  keep it as oracle-only test code;
- run the sample with terrain VG raster enabled and compare:
  - VG culling pass timings;
  - meshlet draw count;
  - HZB rejection count;
  - submitted triangle count;
  - G-buffer image/depth/material parity against the voxel path.

### Phase 8: GPU-owned terrain VG page build contract

Phase 8 starts the runtime-facing GPU path for terrain raster geometry. The
goal is no longer "triangle sidecar plus custom terrain draw list"; it is a
GPU-produced terrain page that emits both views required by the shared page
identity:

```text
GPU terrain source page
  -> Virtual Geometry raster payload
       meshlets, hierarchy nodes, groups, encoded vertex/index streams,
       install/uninstall rewrite metadata
  -> compute trace payload
       Tree64 / virtual trace nodes, error sidecars, cull-error sidecars
  -> dependency payload
       parent/fallback/root identity and page generation/revision
  -> hidden page validation
  -> later shared page-table publish
```

CPU culling remains an oracle only. Runtime G-buffer raster must go through
`VirtualGeometryScene`/`VirtualGeometryRendererPass` GPU culling and hardware
draw, so terrain gets the same HZB, prepass/final-pass cut, and page install
rules as ordinary virtual geometry.

Implementation:

- add a runtime validation packet for GPU-generated terrain VG pages;
- require all generation/build/publication stage bits before a page can publish;
- require non-empty Virtual Geometry raster payload metadata;
- require non-empty compute trace payload metadata using the existing streamed
  trace formats;
- require parent/fallback dependency metadata for non-root pages;
- enforce the packed page-slot byte budget before a hidden page can commit;
- validate meshlet triangle/vertex budgets at the terrain/VG boundary;
- keep material payload optional for this phase while still checking revision
  consistency when material data is present.

Phase 8 landed:

```text
Runtime API:
  sparseVoxelTerrainVGPUPageRasterBytes
  sparseVoxelTerrainVGPUPageTraceBytes
  validateSparseVoxelTerrainVGPUPageBuild

Required GPU stage flags:
  SourceGenerated
  SurfaceClassified
  MeshletPrefixed
  MeshletEmitted
  VGPageEncoded
  TraceClassified
  TraceBuilt
  DependenciesBuilt
  PageValidated
```

Tests:

- complete GPU packet validates as a joint VG-raster + compute-trace page;
- missing VG raster payload is rejected;
- missing trace payload is rejected;
- non-root pages without dependency metadata are rejected;
- root pages can bootstrap without parent/fallback dependencies;
- oversized payloads reject before packed page-slot publication;
- meshlet vertex/triangle overflows reject at the terrain/VG boundary;
- missing required stage bits reject the page.

Phase 8 validation snapshot:

```text
Command:
  cmake -S . -B build-release
  cmake --build build-release --target \
    SparseVoxelTerrainVirtualGeometryPhase8Tests \
    VirtualGeometryCullingMultipleDispatchesPassTests -j 8

  ctest --test-dir build-release -R \
    'SparseVoxelTerrainTriangle|SparseVoxelTerrainVirtualGeometryPhase8GPUBuildContract|SparseVoxelTerrainPhase0CPU|VirtualGeometryCullingMultipleDispatchesPassTests' \
    --output-on-failure

Result:
  phase8 GPU VG build contract: 1/1 tests passed
  triangle/VG focused regression: 10/10 tests passed
  VirtualGeometryCullingMultipleDispatchesPassTests no longer segfaults
```

Remaining Phase 8 GPU work:

- write the actual WGSL/Metal build passes for classify/prefix/emit/encode;
- encode meshlets/hierarchy/groups into the same buffer layout used by
  `VirtualGeometryRendererPass`;
- build the trace nodes and error sidecars from the same generated occupancy;
- read back or validate counters against CPU oracle fixtures;
- expose per-pass counters: source voxels, exposed faces, greedy quads,
  meshlets, VG groups, hierarchy nodes, trace nodes, dependency records,
  rejected pages, and packed-slot utilization.

### Phase 9: shared residency, packed slots, and page publication

Phase 9 connects Phase 8 packets to the real shared page table. The hidden page
allocator should own both the VG raster view and the compute trace view, with
the same parent/fallback protection used by virtual geometry page streaming.

Implementation:

- reserve one packed page slot, or multiple dependent pagelets when a single
  slot is too small;
- track pagelet dependencies and parent/fallback protection before commit;
- publish VG raster ranges, trace ranges, dependency edges, and generation
  identity atomically;
- prevent a page from becoming renderer-visible if either view is stale,
  incomplete, or over budget;
- retire old page generations only after VG raster, compute trace, optional
  native RT, and command-buffer references drain;
- add streaming counters for slot waste, split pages, dependency misses,
  fallback selections, and protected parent pages.

Tests:

- complete Phase 8 packet reserves hidden VG+trace ranges and commits
  atomically;
- packed-slot overflow either splits into valid dependent pagelets or rejects
  without modifying committed state;
- missing parent/fallback pages keep the child hidden and request the closure;
- stale generation/revision packets are rejected;
- retirement waits for raster and trace references from the same generation;
- failed publish keeps the previous/fallback page selected, with no terrain
  holes.

Phase 9 landed:

```text
Shared page table additions:
  SparseVoxelPhysicalPageAllocation::virtualGeometryMeshlets
  SparseVoxelPhysicalPageAllocation::virtualGeometryHierarchyNodes
  SparseVoxelPhysicalPageAllocation::virtualGeometryGroups
  SparseVoxelPhysicalPageAllocation::virtualGeometryPositionWords
  SparseVoxelPhysicalPageAllocation::virtualGeometryNormalWords
  SparseVoxelPhysicalPageAllocation::virtualGeometryUVWords
  SparseVoxelPhysicalPageAllocation::virtualGeometryIndexBytes
  SparseVoxelPhysicalPageAllocation::virtualGeometryBoneWeightWords

Allocator additions:
  SparseVoxelPhysicalBufferKind::VirtualGeometryMeshlets
  SparseVoxelPhysicalBufferKind::VirtualGeometryHierarchyNodes
  SparseVoxelPhysicalBufferKind::VirtualGeometryGroups
  SparseVoxelPhysicalBufferKind::VirtualGeometryPositionWords
  SparseVoxelPhysicalBufferKind::VirtualGeometryNormalWords
  SparseVoxelPhysicalBufferKind::VirtualGeometryUVWords
  SparseVoxelPhysicalBufferKind::VirtualGeometryIndexBytes
  SparseVoxelPhysicalBufferKind::VirtualGeometryBoneWeightWords

Runtime API:
  sparseVoxelProducedPagePayloadFromTerrainVGPUPageBuild
  prepareSparseVoxelTerrainVGPUPageBuildPublication
  publishSparseVoxelTerrainVGPUPageBuild
```

Phase 9 residency behavior:

- a terrain VG GPU packet converts to one shared produced page payload;
- VG raster ranges and compute trace ranges reserve together;
- hidden copied pages stay `UploadQueued` and are not raster-visible or
  traceable before commit;
- non-root pages cannot commit while parent/fallback dependencies are missing;
- packed slot/pagelet accounting includes VG meshlets, hierarchy, groups,
  encoded streams, trace nodes, error sidecars, cull sidecars, and materials;
- committed VG pages satisfy `unifiedGeometryVisible()` and can win the unified
  cut only when the trace view is present too.

Phase 9 validation snapshot:

```text
Command:
  cmake -S . -B build-release
  cmake --build build-release --target \
    SparseVoxelTerrainVirtualGeometryPhase9Tests \
    SparseVoxelTerrainVirtualGeometryPhase8Tests \
    SparseVoxelTerrainTrianglePhase6Tests -j 8

  ctest --test-dir build-release -R \
    'SparseVoxelTerrainTriangle|SparseVoxelTerrainVirtualGeometryPhase(8GPUBuildContract|9ResidencyPublication)|SparseVoxelTerrainPhase0CPU|SparseVoxelStreaming(G16Tests|GPUContractG16Tests)|VirtualGeometryCullingMultipleDispatchesPassTests' \
    --output-on-failure

Result:
  phase9 VG residency/publication contract: 1/1 tests passed
  focused terrain/VG/streaming regression: 13/13 tests passed
```

### Phase 10: VirtualGeometryScene runtime install

Phase 10 makes terrain pages first-class virtual-geometry scene content. The
terrain page publisher should install/rewrite the same scene-visible hierarchy
that ordinary virtual geometry uses, instead of maintaining a terrain-specific
draw list.

Implementation:

- register terrain VG pages in `VirtualGeometryScene`;
- map terrain page ids/generations to virtual-geometry page ids and rewrite
  records;
- bind terrain material payloads into the existing VG material/G-buffer ABI;
- route terrain page installs/uninstalls through VG page install/rewrite
  commands;
- preserve matching compute trace payloads and fallback trace parents while VG
  raster pages stream;
- expose diagnostics that show terrain VG pages, meshlets, groups, submitted
  draws, HZB rejects, and fallback bricks.

Tests:

- terrain page install produces visible VG scene roots/groups;
- uninstall/reinstall with a newer generation updates scene rewrites without
  stale pointers;
- material ids in the G-buffer match the voxel/trace material oracle;
- missing VG pages fall back to protected terrain parent pages;
- screen cut and trace cut report the same page/generation identity.

Phase 10 landed:

```text
Runtime API:
  buildSparseVoxelTerrainVGScenePageInstall

Bridge output:
  SparseVoxelTerrainVGScenePageInstall
    shared terrain page id/generation/revision
    trace format and trace/material availability
    packed slot/pagelet identity
    virtualgeometry::PageAllocation
    virtualgeometry::PageTableEntry
```

Phase 10 scene-bridge behavior:

- consumes a committed shared terrain VG page and the GPU build metadata that
  produced it;
- rejects non-resident pages before they can enter `VirtualGeometryScene`;
- rejects stale build metadata whose page generation or geometry revision no
  longer matches the committed page;
- rejects pages without VG raster ranges;
- rejects pages without trace payload when the scene install requires trace
  parity;
- rejects pages without material payload when exact material tracing is
  required;
- maps shared VG meshlet, hierarchy, group, position, normal, uv, index, and
  bone-weight ranges into a `virtualgeometry::PageAllocation`;
- maps packed slot/pagelet placement into the VG page-table byte range;
- preserves priority slot, page-table offset, unit-scale bits, decoded vertex
  count, decoded index count, and installed page-table state.

Phase 10 validation snapshot:

```text
Command:
  cmake -S . -B build-release
  cmake --build build-release --target \
    SparseVoxelTerrainVirtualGeometryPhase10Tests \
    SparseVoxelTerrainVirtualGeometryPhase9Tests \
    SparseVoxelTerrainVirtualGeometryPhase8Tests -j 8

  ctest --test-dir build-release -R \
    'SparseVoxelTerrainTriangle|SparseVoxelTerrainVirtualGeometryPhase(8GPUBuildContract|9ResidencyPublication|10SceneBridge)|SparseVoxelTerrainPhase0CPU|SparseVoxelStreaming(G16Tests|GPUContractG16Tests)|VirtualGeometryCullingMultipleDispatchesPassTests' \
    --output-on-failure

Result:
  phase10 VG scene bridge contract: 1/1 tests passed
  focused terrain/VG/streaming regression: 14/14 tests passed
```

### Phase 11: default G-buffer raster path through Virtual Geometry

Phase 11 flips the renderer path: terrain triangle raster becomes a normal
virtual-geometry raster source. The old brick DDA path remains as fallback and
diagnostic baseline, not the default hot path.

Phase 11 landed the renderer-facing route policy:

- added `SparseVoxelTerrainVirtualGeometryRenderRoute`;
- added `selectSparseVoxelTerrainGBufferRoute(...)`;
- default/auto routing selects Virtual Geometry when a Phase 10 scene install is
  ready;
- forced Virtual Geometry fails closed when the page install is missing, stale,
  or missing required trace/material parity;
- brick raster remains available only as fallback/diagnostic route;
- route decisions explicitly require the VG renderer and GPU VG culling for the
  default hot path;
- route decisions never enable CPU terrain culling.

Important boundary: Phase 11 establishes the renderer contract and default route
selection policy. The live sample render-graph handoff and 30-second renderer
benchmark remain Phase 12 work.

Tests:

- default route selects VG when the Phase 10 scene install is ready;
- default route falls back to bricks only when VG is unavailable/incomplete and
  fallback is allowed;
- forced VG rejects missing trace parity;
- optional material parity keeps default routing on fallback until material data
  is ready;
- explicit brick route remains a diagnostic override;
- all route outcomes assert that CPU terrain culling is not used.

Phase 11 validation snapshot:

```text
Command:
  cmake --build build-release --target \
    SparseVoxelTerrainVirtualGeometryPhase11Tests \
    SparseVoxelTerrainVirtualGeometryPhase10Tests \
    SparseVoxelTerrainVirtualGeometryPhase9Tests \
    SparseVoxelTerrainVirtualGeometryPhase8Tests -j 8

  ./build-release/tests/SparseVoxelTerrainVirtualGeometryPhase11Tests

  ctest --test-dir build-release -R \
    'SparseVoxelTerrainTriangle|SparseVoxelTerrainVirtualGeometryPhase(8GPUBuildContract|9ResidencyPublication|10SceneBridge|11RenderRoute)|SparseVoxelTerrainPhase0CPU|SparseVoxelStreaming(G16Tests|GPUContractG16Tests)|VirtualGeometryCullingMultipleDispatchesPassTests' \
    --output-on-failure

Result:
  phase11 terrain VG render-route contract: passed
  focused terrain/VG/streaming regression: 15/15 tests passed
```

### Phase 12: full renderer benchmark and regression gate

Phase 12 is the benchmark/quality gate before removing the old path from normal
use. It must run the renderer, not only CPU contracts.

Phase 12 landed the benchmark gate contract:

- added `SparseVoxelTerrainVirtualGeometryBenchmark`;
- added a two-path benchmark plan:
  - `brick-baseline`, explicitly pinned to the old brick fallback route;
  - `terrain-vg-default`, selected only by the Phase 11 default/auto route;
- requires 1220x720, 5 km terrain render distance, streaming enabled, GPU
  timings, and a 30 second capture window;
- refuses to call a run "terrain VG" unless the Phase 10 scene install is ready
  and the Phase 11 route resolves to Virtual Geometry;
- metric validation rejects:
  - wrong G-buffer route;
  - CPU terrain culling;
  - missing timing/FPS data;
  - missing terrain pixels;
  - visible holes or cracks;
  - VG runs without VG renderer/GPU culling/meshlet/triangle/trace counters;
  - brick runs without visible brick work.

Important boundary: the live `SparseVoxelRendererSample` still hard-selects the
brick hierarchy renderer. Phase 12 now catches that as an invalid terrain-VG
benchmark instead of silently blessing it. The remaining renderer handoff is to
register committed terrain VG page installs into the live VG cull/prepass/draw
graph.

Benchmark command:

```text
cmake --build build-release --target SparseVoxelRendererSample
./build-release/tests/SparseVoxelRendererSample \
  --world-map world-data \
  --terrain-render-distance-km 5 \
  --resolution 1220x720
```

Measured incumbent brick/default path on this machine:

```text
Command:
  ./build-release/tests/SparseVoxelRendererSample \
    --world-map world-data \
    --terrain-render-distance-km 5 \
    --resolution 1220x720 \
    --gbuffer-resolver bricks \
    --benchmark-lighting /tmp/svr-phase12-brick-30s.json \
    --benchmark-sequence static \
    --benchmark-warmup 120 \
    --benchmark-frames 1800 \
    --benchmark-resolution 1220x720 \
    --disable-vsync \
    --gpu-timings \
    --terrain-pages-per-update 64 \
    --terrain-upload-budget-mb 256

Result:
  measured frames: 1800
  terrain streaming: progressive-chunks
  CPU frame p50: 28.608 ms
  CPU frame p95: 30.854 ms
  CPU frame mean: 27.366 ms
  FPS from CPU p50: 34.96
  FPS from CPU mean: 36.54
  GPU frame p50: 25.526 ms
  GPU frame p95: 27.046 ms
  GPU frame mean: 24.608 ms
  brick phase-0 p50: 12.743 ms
  brick phase-0 p95: 13.962 ms
  brick draw p50: 14.437 ms
  brick cull p50: 0.789 ms
  brick HZB p50: 0.852 ms
  scene ray schedule p50: 6.334 ms
  terrain GPU-v2 build/publish p50: 0.932 ms
```

Observed live-renderer caveat:

```text
Default sample run still registers:
  svr.sample.Final.BrickPhase0.*
  svr.sample.Final.BrickPhase1.*
  svr.sample.Final.BrickResolve

It does not yet register terrain VG cull/prepass/final draw passes for the
world-map terrain pages.
```

Live wiring audit after the Phase 12 benchmark gate:

```text
SparseVoxelRendererSample state:
  --gbuffer-resolver parser accepts only legacy aliases
  selectedGBufferResolver is forced to BrickHierarchy
  registerRasterPhase() immediately calls registerBrickResolver() and returns

VirtualGeometryScene state:
  public API supports file-backed objects through registerObjectForStreaming()
  scene-owned streaming manager owns page allocations, page table writes,
  hierarchy uploads, decoded buffers, and page decode requests
  no public runtime API exists yet for external terrain pages that are already
  resident in SparseVoxelSharedPageTable

Terrain VG state:
  CPU sidecar -> encoded VG asset path is proven
  GPU page build/publication metadata path is proven
  scene-install descriptor maps shared terrain ranges to PageAllocation and
  PageTableEntry
  live render graph still lacks the handoff that writes those descriptors into
  VirtualGeometryScene and registers the VG renderer as the terrain G-buffer
  publisher
```

So Phase 12 did not finish the live renderer flip. It correctly prevents a
brick fallback run from being reported as a terrain-VG result.

Required captures:

- 30 seconds on old voxel brick raster path with streaming enabled;
- 30 seconds on new terrain VG raster path with streaming enabled;
- median/p95 frame time and FPS;
- VG culling/prepass/final pass timings;
- submitted meshlets/triangles and rejected meshlets/triangles;
- overdraw estimate or depth complexity proxy;
- compute trace ray count, trace time, unresolved page requests, and page
  install latency;
- visible holes/crack screenshot regression near chunk boundaries and horizon
  silhouettes;
- lighting/shadow parity on terrain/mountain occluders.

Pass/fail gate:

```text
New terrain VG raster can become the default only if:
  no holes/cracks on the fixed world-map captures
  screen cut and trace cut use matching page/generation identity
  lighting/shadow ray hits match installed raster geometry within tolerance
  full-renderer FPS is at least baseline-neutral after streaming settles
  HZB/culling counters show lower overdraw than the brick DDA path
```

Phase 12 validation snapshot:

```text
Command:
  cmake --build build-release --target \
    SparseVoxelTerrainVirtualGeometryPhase12Tests \
    SparseVoxelTerrainVirtualGeometryPhase11Tests \
    SparseVoxelTerrainConvergencePhase12Tests -j 8

  ctest --test-dir build-release -R \
    'SparseVoxelTerrainVirtualGeometryPhase(8GPUBuildContract|9ResidencyPublication|10SceneBridge|11RenderRoute|12BenchmarkGate)|SparseVoxelTerrainConvergencePhase12|SparseVoxelVirtualTraceBenchmarkSmoke' \
    --output-on-failure

Result:
  phase12 terrain VG benchmark gate: passed
  focused phase8-12/convergence regression: 7/7 tests passed
  SparseVoxelRendererSample build: passed
  incumbent full-renderer brick benchmark: passed, 1800 measured frames
```

Latest renderer audit capture:

```text
Command:
  ./build-release/tests/SparseVoxelRendererSample \
    --world-map world-data \
    --terrain-render-distance-km 5 \
    --resolution 1220x720 \
    --gbuffer-resolver bricks \
    --benchmark-lighting /tmp/svr-gbuffer-full.XXXXXX.json \
    --benchmark-sequence static \
    --benchmark-warmup 120 \
    --benchmark-frames 1800 \
    --benchmark-resolution 1220x720 \
    --disable-vsync \
    --gpu-timings \
    --terrain-pages-per-update 64 \
    --terrain-upload-budget-mb 256 \
    --skip-output-signature

Full lighting, internal 610x360 because TSR/render-scale is active:
  CPU frame p50: 28.261 ms
  CPU frame p95: 97.216 ms
  CPU frame mean: 35.197 ms
  GPU frame p50: 25.396 ms
  GPU frame p95: 55.161 ms
  GPU frame mean: 28.339 ms
  brick phase-0 p50: 12.491 ms
  brick phase-0 draw p50: 10.687 ms
  brick phase-1 draw p50: 3.581 ms
  brick HZB p50: 0.821 ms
  brick resolve p50: 0.298 ms
  SceneRaySchedule p50: 6.332 ms
  resident terrain pages at end: 138 / 512
  maximum pending refinement requests: 22

G-buffer only, native 1220x720 because lighting/TSR is disabled:
  CPU frame p50: 58.717 ms
  CPU frame p95: 62.316 ms
  CPU frame mean: 56.100 ms
  GPU frame p50: 57.519 ms
  GPU frame p95: 60.532 ms
  GPU frame mean: 55.483 ms
  brick phase-0 p50: 44.128 ms
  brick phase-0 draw p50: 42.340 ms
  brick phase-1 draw p50: 7.743 ms
  brick HZB p50: 0.906 ms
  brick resolve p50: 1.350 ms
  material G-buffer p50: 0.357 ms
  resident terrain pages at end: 138 / 512
  maximum pending refinement requests: 22

Observation:
  raw full-resolution G-buffer is draw/overdraw dominated. Cull and HZB are not
  the bottleneck in the current brick fallback path.
```

### Phase 14: live VirtualGeometryScene runtime-install API

Phase 14 adds the missing scene seam. Terrain pages must be able to enter the
same `VirtualGeometryScene` buffers that file-backed VG assets use, without
round-tripping through a temporary asset file and without CPU culling.

Implementation:

- add a narrow terrain/runtime object registration API to `VirtualGeometryScene`;
- reserve page-table and priority slots for a terrain runtime object;
- accept committed `SparseVoxelTerrainVGScenePageInstall` descriptors;
- write `PageTableEntry` records into the scene page table;
- upload or alias VG hierarchy/page payload ranges according to the descriptor;
- register hierarchy root slices and instance records exactly like file-backed
  objects do;
- enqueue page decode requests for installed pages when decoded buffers need
  population;
- reject stale page generations and missing trace/material parity;
- expose counters for installed terrain VG pages, decoded pages, roots, meshlets,
  groups, and pending decode requests.

Tests:

- a committed terrain page installs into `VirtualGeometryScene` and creates a
  visible runtime object root;
- reinstalling a newer generation rewrites the page table and hierarchy without
  stale pointers;
- uninstall protects parent/fallback pages before removing child pages;
- page decode requests are emitted only when runtime installs provide or point
  at scene-owned encoded page data;
- file-backed VG objects and terrain runtime objects can coexist in one scene;
- CPU terrain culling remains unused.

Phase 14 status:

- implemented a generic `VirtualGeometryScene` runtime object/page install API
  so terrain does not force terrain headers into `virtualgeometry`;
- added the terrain bridge adapter from committed
  `SparseVoxelTerrainVGScenePageInstall` descriptors to live VG scene page-table
  installs;
- preserved the split between shared terrain storage and encoded VG page data:
  shared/pre-decoded terrain descriptors update residency/page-table state,
  while generic encoded runtime installs enqueue decode requests only when page
  words are provided or pre-written into the scene page buffer;
- added `SparseVoxelTerrainVirtualGeometryPhase14SceneRuntimeInstall` covering
  runtime object registration, page-table writes, reinstall generation guards,
  parent/fallback uninstall protection, trace/material parity diagnostics,
  decode-request emission for encoded runtime pages, and CPU-culling absence.

### Phase 15: terrain VG G-buffer publisher in SparseVoxelRendererSample

Phase 15 flips the live sample renderer. The terrain VG object should publish
the same G-buffer contract that the brick resolver currently publishes:
`resolvedSurfaces`, `coverage`, `linearDepth`, material sample addresses, and
the voxel G-buffer textures consumed by lighting.

Status:

- added the `TerrainVirtualGeometry` resolver enum and `auto|terrain-vg|bricks`
  CLI route vocabulary;
- default `auto` now goes through the terrain VG route helper instead of being
  hard-forced to bricks;
- route selection now distinguishes Phase 14 descriptor/install readiness from
  scene-visible raster payload readiness, so metadata-only terrain pages cannot
  be accidentally drawn through an empty VG hardware path;
- `--gbuffer-resolver bricks` remains the explicit rollback, and legacy
  `svraster`/`tree64` aliases still map to bricks for existing tests;
- removed the unconditional brick early-return in `registerRasterPhase()`.

Current blocker:

- GPU V2 terrain pages still publish descriptors/sidecars into terrain-owned
  shared storage. The current `VirtualGeometryRendererPass` consumes decoded
  `VirtualGeometryScene` draw buffers. Until a bridge pass either aliases or
  copies the terrain page payload into that scene-visible draw ABI, forced
  `--gbuffer-resolver terrain-vg` fails closed and `auto` safely falls back to
  bricks.

Implementation:

- add `GBufferResolver::TerrainVirtualGeometry`;
- make `auto` select TerrainVirtualGeometry only when the Phase 14 scene object
  is ready and trace parity is satisfied;
- keep `--gbuffer-resolver bricks` as the explicit baseline/rollback path;
- remove the unconditional brick early-return in `registerRasterPhase()`;
- register VG culling/depth/material draw passes for terrain pages after the
  scene-visible payload bridge exists;
- bridge VG material output into the sparse-voxel G-buffer ABI;
- keep GPU page feedback/request buffers active for off-screen trace and shadow
  rays;
- ensure screen cut and trace cut use the same page/generation identity;
- leave all CPU culling disabled.

Tests:

- CLI/default route test proves the sample no longer forces BrickHierarchy;
- render graph inspection finds terrain VG cull/prepass/draw passes in the
  default world-map path;
- brick resolver pass names are absent from the default path and present only
  when `--gbuffer-resolver bricks` is requested;
- G-buffer coverage/hash tests pass for the terrain VG path;
- raster hit ids match trace hit ids for fully installed geometry;
- off-screen shadow/lighting rays can request missing terrain pages;
- no visible holes/cracks on chunk boundaries and horizon silhouettes.

Added Phase 15 regression:

```bash
cmake --build build-release --target SparseVoxelTerrainVirtualGeometryPhase15Tests -j 8
./build-release/tests/SparseVoxelTerrainVirtualGeometryPhase15Tests
ctest --test-dir build-release -R 'SparseVoxelTerrainVirtualGeometryPhase(11RenderRoute|12BenchmarkGate|14SceneRuntimeInstall|15GBufferPublisherGate)' --output-on-failure
```

### Phase 16: proper terrain VG benchmark gate

Phase 16 runs the benchmark that Phase 12 intentionally refused to fake.

Status:

- expanded the benchmark plan from two arms to the required three-arm capture:
  brick baseline, Terrain VG full renderer, and Terrain VG G-buffer-only;
- added an optional settled-residency Terrain VG rerun arm;
- added explicit metric fields for mean CPU/GPU frame time, VG material pass,
  trace timing, page install latency, page publication rejects, overdraw proxy,
  live render-graph route identity, screen/trace page identity parity, and
  G-buffer-only/lighting state;
- added a benchmark gate helper that refuses Terrain VG default promotion unless:
  - the live render graph uses VG passes and not BrickPhase passes;
  - terrain pixels are non-zero;
  - hole/crack counters are zero;
  - screen and trace page identities match;
  - full-frame FPS is baseline-neutral within the declared tolerance;
  - raw Terrain VG G-buffer time is materially lower than the brick baseline;
  - CPU terrain culling is not present in any capture;
- kept the Phase 15 caveat active: without the scene-visible payload bridge,
  the benchmark plan still refuses to create fake VG captures.

Required captures:

- 30 seconds, brick baseline, streaming enabled, 1220x720;
- 30 seconds, terrain VG default, streaming enabled, 1220x720;
- 30 seconds, terrain VG G-buffer-only, lighting disabled, native 1220x720;
- optional settled-residency rerun after page publication stops changing.

Metrics:

- CPU/GPU p50/p95/mean frame time;
- VG cull/prepass/material/final draw timings;
- submitted/rejected meshlets and triangles;
- terrain pixels, visible holes, crack pixels;
- trace ray count, trace time, unresolved page requests;
- page install latency and publication rejection count;
- overdraw/depth-complexity proxy.

Pass/fail gate:

```text
Terrain VG can become the default only if:
  live render graph uses VG passes, not BrickPhase passes
  terrain pixels are non-zero
  hole/crack counters are zero
  screen and trace page identities match
  full-frame FPS is baseline-neutral or better after streaming settles
  raw G-buffer time is materially lower than the brick draw dominated baseline
```

Phase 16 validation:

```bash
cmake --build build-release --target SparseVoxelTerrainVirtualGeometryPhase16Tests -j 8
./build-release/tests/SparseVoxelTerrainVirtualGeometryPhase16Tests
ctest --test-dir build-release -R 'SparseVoxelTerrainVirtualGeometryPhase(12BenchmarkGate|15GBufferPublisherGate|16BenchmarkGate)' --output-on-failure
```

### Phase 17: experimental native compute raster backend

Phase 17 adds a compute-raster route contract that can be plugged into the
terrain VG G-buffer publisher after Phase 15. The first native backend is
Metal-only on Apple platforms, with a portable Vulkan/MoltenVK 32-bit tiled
fallback kept as a correctness/perf comparison. The goal is to test whether
Nanite-style tile binning/software raster beats hardware triangle raster for
small terrain clusters or overdraw-heavy views.

The important platform rule is:

```text
MoltenVK's missing Vulkan shaderBufferInt64Atomics bit does not disable the
native Metal route.

If the process can acquire a compatible native Metal device/queue/resource
interop path for the same MoltenVK-backed renderer, and that native Metal
adapter supports the required 64-bit atomic min/max operation, the Metal compute
raster may bypass the Vulkan/MoltenVK atomic64 feature check entirely.

The Vulkan/MoltenVK route still must not request atomic64 on platforms that do
not expose it; it uses the bounded 32-bit tile-slot path instead.
```

This is explicitly optional and gated:

- `enableTerrainComputeRaster` defaults off;
- backend capability probe must pass before graph creation:
  - native Metal adapter/device available;
  - native Metal command queue can synchronize with the MoltenVK-backed frame;
  - Metal buffers/textures used by the G-buffer publisher can be shared,
    imported, or copied through an explicit handoff without hidden lifetime
    hazards;
  - native Metal exposes the needed 64-bit atomic min/max operation for the
    visibility/depth-payload buffer, or the route falls back to the 32-bit
    tiled path;
- unsupported platforms register no passes and allocate no compute-raster-only
  buffers;
- all output must use the same terrain G-buffer ABI as the VG hardware raster
  path;
- CPU terrain culling remains unused.

Implementation:

- add a backend-neutral `TerrainComputeRasterCapabilities` result;
- add a Metal implementation first:
  - shader source:
    `assets/shaders/voxel/svraster/metal/sparse_voxel_terrain_compute_raster_atomic64.metal`;
  - C++ pass-plan/registration contract:
    `src/voxel/svraster/rendering/TerrainMetalComputeRasterPass.hpp`;
  - native Metal device/queue/interoperability probe;
  - explicit `bypassesMoltenVKAtomic64Feature` diagnostic when Metal atomic64 is
    used while Vulkan reports `shaderBufferInt64Atomics = false`;
  - Metal buffer/texture allocation only when enabled and supported;
  - tile primitive-count, prefix, scatter/bin, fine-raster, and merge kernels;
  - packed 64-bit visibility key for native Metal:

    ```text
    key64 = packed_depth | packed_payload_id
    atomic_min(key64)
    ```

    where payload identity resolves the winning terrain VG page/cluster/triangle
    and depth is recomputed/validated during merge;
  - per-tile overflow fallback to hardware VG raster;
- keep the Vulkan/MoltenVK fallback on the existing bounded 32-bit tile-slot
  design:

  ```text
  key32 = packed_depth20 | tile_local_slot12
  atomic_min(key32)
  ```

  The fallback is valid only when the candidate census proves every tile fits
  in the declared slot capacity; overflow routes to hardware VG raster.
- add renderer settings/CLI:
  - default off;
  - force-on fails closed if unsupported;
  - auto may choose it only after benchmark gates pass;
- require the Terrain Virtual Geometry G-buffer route for production compute
  raster:
  - brick fallback and legacy sparse-voxel resolvers keep their own raster
    paths;
  - force-on compute raster fails closed outside the VG G-buffer route;
  - old brick tile experiments may opt out only as explicit diagnostics;
- route only small/high-overdraw clusters through compute raster at first:
  - default candidate threshold:

    ```text
    average_projected_triangle_pixels < 12.0
    ```

  - this is a sweepable benchmark knob, not a baked-in fact; captures must test
    neighbouring thresholds such as 4, 8, 12, 16 and 24 pixels and keep the
    smallest route that wins p50/p95 without quality regressions;
  - clusters with `average_projected_triangle_pixels >= threshold` remain on
    hardware unless the measured overdraw/depth-complexity proxy is high enough
    to justify compute;
- keep large low-overdraw clusters on hardware raster;
- emit diagnostics:
  - binned clusters/triangles;
  - rejected clusters/tiles;
  - compute-raster pixels;
  - overflow/fallback count;
  - G-buffer mismatch count versus hardware VG raster.

Future backend expansion:

- Vulkan compute raster uses the same pass contract with SPIR-V kernels;
- DirectX compute raster uses the same pass contract with HLSL kernels;
- backend-specific code owns only resource binding/synchronization details, not
  page identity, cut policy, or G-buffer semantics.

Tests:

- capability-disabled path allocates no compute-raster-only buffers and
  registers no compute-raster passes;
- force-on unsupported path fails with a clear error;
- native Metal on a MoltenVK-backed Apple renderer may select the 64-bit atomic
  path even when Vulkan reports no `shaderBufferInt64Atomics`, but only if the
  native Metal adapter/interoperability probe passes;
- Vulkan/MoltenVK fallback uses bounded 32-bit tile-slot atomics and refuses
  overflow;
- Metal-supported path registers the expected pass sequence;
- triangle routing sends clusters below the default 32 pixel average projected
  triangle edge length to compute and keeps threshold-or-larger clusters on
  hardware unless high overdraw is measured;
- compute-raster output matches hardware VG G-buffer for a fully resident
  fixture;
- hybrid route preserves nearest-depth ownership when hardware and compute
  outputs overlap;
- tile overflow falls back to hardware VG raster without holes;
- no CPU culling is used;
- benchmark captures compare:
  - hardware VG only;
  - Metal compute raster only where supported;
  - hybrid hardware + compute raster.

Pass/fail gate:

```text
Keep/expand compute raster only if:
  G-buffer parity with hardware VG raster is exact or within declared tolerance
  no holes/cracks appear under tile overflow
  raw G-buffer p50/p95 improves on overdraw-heavy captures
  full-frame performance does not regress after streaming settles
else:
  leave the backend experimental/off and continue optimizing VG hardware raster.
```

### Phase 18: native Metal buffer interop and Terrain VG compute-raster pass

Phase 18 removes the native buffer-interop caveat from Phase 17 and adds the
runtime pass wrapper. The native Metal compute-raster pass must consume the same
virtual-geometry terrain buffers that the G-buffer route already owns, instead
of asking the CPU to copy or rebuild terrain payloads.

Status:

- landed: explicit exportable-buffer intent, RHI Metal-buffer export hooks,
  Vulkan/MoltenVK `MTLBuffer` export, native prepared-tile Metal pass wrapper,
  metallib build target, and focused regression tests;
- remaining: final scene-visible Terrain VG G-buffer bridge wiring in the sample
  render path, including exportable allocation of the actual VG raster buffers
  only when the native compute-raster route is selected.

Plan steps:

1. Add explicit RHI resource-export intent.
   - `BufferInfo::metalExportable` requests an exportable `MTLBuffer` on
     MoltenVK/Apple.
   - The flag defaults off so ordinary buffers keep their normal allocation and
     lifetime.
   - Texture export remains opt-in through the existing texture flag.
2. Add backend-neutral export hooks on the RHI.
   - `exportMetalDevice()`;
   - `exportMetalCommandQueue(queue)`;
   - `exportMetalBuffer(buffer)`;
   - `exportMetalTexture(texture)`.
   Non-Apple or unsupported backends return `nullptr`.
3. Implement Vulkan/MoltenVK buffer export.
   - Chain `VkExportMetalObjectCreateInfoEXT` into exportable buffer memory
     allocation.
   - Export `VkDeviceMemory` through `VkExportMetalBufferInfoEXT`.
   - Refuse export for buffers not created with `metalExportable = true`.
4. Keep ownership GPU/RHI-local.
   - The native Metal pass validates exported `MTLBuffer`s but does not map or
     mutate them from CPU.
   - Uniforms, tile ranges, cluster triangles, visibility keys and resolved
     payloads are uploaded or produced through normal render-graph/RHI passes
     before the external Metal encoder runs.
5. Add the native Metal prepared-tile pass.
   - Load the Phase 17 metallib.
   - Create clear, prepared-tile raster, and merge compute pipelines.
   - Consume VG-prepared tile ranges/prefix data and terrain cluster triangle
     buffers.
   - Emit the same resolved payload/visibility key ABI as the hardware Terrain
     VG G-buffer path.
6. Wire pass selection behind the existing compute-raster route.
   - Production route still requires the Terrain Virtual Geometry G-buffer
     resolver.
   - Unsupported backends allocate no compute-raster buffers and register no
     native pass.
   - Force-on fails closed if device, queue, buffer interop, or Metal atomic64
     support is missing.
7. Finish full G-buffer bridge wiring.
   - Allocate Terrain VG scene-visible raster buffers with
     `metalExportable = true` only when the native Metal route is selected.
   - Mark `sceneVisibleRasterPayloadReady = true` only after the terrain page
     publisher has installed cluster/tile/material/trace payloads into the
     `VirtualGeometryScene` ABI.
   - Replace the sample's temporary Terrain VG resolver failure with the
     hardware/VG-or-native-Metal compute raster dispatch path.
8. Add synchronization and diagnostics.
   - Name the native Metal command buffer and encoder for Instruments.
   - Ensure Metal work is submitted on the MoltenVK-compatible command queue.
   - Track unsupported export count, unexported-buffer rejects, tile overflow,
     compute-raster pixels, G-buffer mismatch count, and hardware fallback
     count.

Phase 18 validation:

```bash
cmake --build build-release --target SparseVoxelTerrainVirtualGeometryPhase17Tests -j 8
./build-release/tests/SparseVoxelTerrainVirtualGeometryPhase17Tests
cmake --build build-release --target SparseVoxelRendererSample -j 8
ctest --test-dir build-release -R 'SparseVoxelTerrainVirtualGeometryPhase(15|16|17)' --output-on-failure
```

Pass/fail gate:

```text
Native Metal compute raster may enter renderer benchmarks only if:
  every consumed terrain/VG buffer was explicitly exportable
  the pass consumes VG-owned cluster/tile buffers without CPU payload copies
  unsupported interop fails closed before graph allocation
  Terrain VG G-buffer output matches the hardware VG path on a resident fixture
  trace/page identity still matches the raster cut
  no holes/cracks appear under tile overflow or hardware fallback
```

### Milestone 2: native ray tracing backend

This is a second milestone, not a dependency for the triangle-cluster raster
path. The default trace backend should remain the existing streamed Tree64 /
virtual compute trace path until a native RT backend proves faster and can be
integrated safely with the active graphics backend.

MetalRT is the first concrete target because the current Apple renderer is
MoltenVK-backed. VulkanRT and DirectXRT should use the same higher-level pass
contract later, instead of becoming separate terrain systems.


#### Verdict: yes, with one qualification about "runtime error metrics"

**Can Metal RT trace dynamically streamed geometry driven by the DAG's error
metrics? Yes. The architecture question is settled; the performance question is
not.**

The adopted design is three levels, mirroring the hierarchy that
`VirtualGeometryHierarchyBuilder` already produces:

```text
scene TLAS                 one instance per resident page
  -> cut AS                one intermediate instance AS per LOD-level root,
                           containing exactly the groups that level's cut selected
      -> group BLAS        one per leaf hierarchy node (a group, <= 8 clusters),
                           built once when its page installs, shared by every cut
                           that contains it, never rebuilt while resident
```

What is measured and settled:

| question | answer | evidence |
| --- | --- | --- |
| Is Metal RT reachable from this renderer? | Yes, as a native sidecar | MoltenVK exposes no Vulkan RT; `VK_EXT_metal_objects` rev 2 present; the interop pattern already ships in `TerrainMetalComputeRasterPass` |
| Does the DAG structure map onto Metal? | One-to-one | level root -> intermediate AS, group leaf -> BLAS, cluster -> triangles |
| Does a ray see only the selected LOD? | Yes, exactly one | 1.00 candidates/hit, 1.000 distinct LODs/hit, **0** of 921,600 rays saw two levels |
| Does it survive partial residency? | Yes, with sibling-complete descent | byte-identical coverage at 100 / 75 / 50 / **25%** BLAS residency |
| Can the cut be GPU-driven? | Fully | instance descriptors are GPU-writable and `MTLAccelerationStructureID` is a plain 64-bit handle; no CPU readback |
| Is it fast? | 1,575 Mray/s at 1080p | opaque triangles, no intersection function on the ray path |

Steady-state per-frame cost, from the measurements in this section:

```text
cut AS rebuild        0.2 - 0.33 ms per LOD level whose membership changed
scene TLAS rebuild    0.27 ms (256 page instances) - 0.39 ms (1,024)
group BLAS build      ~1.9 ms per full page, ONLY at stream-in, never on
                      camera motion
trace                 1.3 - 1.6 ms at 1080p on the measured scenes
```

##### The qualification

"Runtime error metrics" holds, but not in the sense of evaluating error *per
ray*. In the adopted design the two-test cut runs **once per frame in the
culling pass that already computes it**, and its result is baked into which
groups each cut AS contains. The trace kernel reads no error value, binds no
camera matrix, and runs no intersection function.

That is a feature rather than a limitation, because with camera-driven error the
per-ray test computes the same answer the cut already did - verified, and it is
why the exact view runs opaque at full hardware speed instead of paying 2.0x for
a per-candidate test.

Per-ray error evaluation remains available where it genuinely differs from the
camera cut:

```text
8 instance mask bands     free, hardware, culls before descending into a subtree
                          measured: masked-out levels contribute ZERO candidates,
                          and one band traces 2.3-2.9x faster than all four
                          each band must be a COMPLETE cut at a different quality,
                          never a single LOD level
bounding-box refinement    per-candidate two-test cut with commit-based subtree
view                       pruning (585 primitives -> 1 candidate); ~2x traversal,
                           for coarse/offscreen geometry and unresident regions
```

##### What is not settled

None of these blocks starting; all of them gate shipping.

- **The interop has never been exercised for ray tracing.** Every probe in this
  section ran against a standalone `MTLDevice`. Whether a MoltenVK-exported
  `MTLBuffer` is legal as acceleration-structure geometry input, and what an AS
  build costs when serialised behind the frame's Vulkan submission on the shared
  queue, are both unmeasured. This is the single biggest risk and it is cheap to
  retire.
- **Incoherent rays.** Every throughput number is a coherent primary fan. The
  engine's real load is ReSTIR, probe and shadow rays. Until those are measured,
  no claim that MetalRT beats the existing compute trace is supportable.
- **Memory.** Every resident DAG node needs a BLAS at ~109 bytes per triangle
  plus per-BLAS overhead, and residency - not the cut - sets the total. The
  working assumption is that this fits within the envelope the Tree64 trace
  payloads already occupy, since the RT structures would replace rather than
  accompany them; that substitution has not been costed, and the two are not the
  same shape - Tree64 stores occupancy bits, a BLAS stores triangles.
- **Scale extrapolation.** Several probes shared one BLAS across many instances.
  The structural results hold (hit counts matched exactly), but build time and
  memory for thousands of *distinct* group BLASes are extrapolated.
- **Cut-change rate** under real camera motion, which sizes the per-frame
  rebuild budget.
- **Crack-freeness** across LOD boundaries is a property of the simplifier, not
  of the acceleration structures. Overlaps are harmless; gaps read as light
  leaks, and the residency work above showed the failure mode is holes rather
  than overlap.

##### Recommended next step

M2.0 through M2.2 retire the two biggest unknowns cheaply and in order: land the
probe harness as a test target, add the capability probe, then do nothing more
ambitious than building one acceleration structure from a MoltenVK-exported
buffer and tracing it on the shared queue while the render graph is running.
If that works, the rest of the design is measurement rather than discovery. If
it does not, the milestone stops there and the streamed Tree64 / virtual compute
trace path continues, which is exactly what the decision gate is for.

#### The design in one page

Everything below elaborates this. The rejected alternatives and the reasoning
that eliminated them are in the appendix at the end of the milestone.

```text
STRUCTURE                             mirrors VirtualGeometryHierarchyBuilder

scene TLAS                            one instance per resident page
  -> cut AS                           one intermediate instance AS per LOD-level
                                      root, holding exactly the groups that
                                      level's two-test cut selected
      -> group BLAS                   one per leaf hierarchy node (a group,
                                      <= MaxBVHChildren clusters, ~896 tri)
          -> triangles                opaque; no intersection function is linked
```

```text
WHAT IS BUILT WHEN

group BLAS      built once when its page installs; shared by every cut that
                contains it; never rebuilt while the node stays resident;
                freed when the page uninstalls
                ~1.9 ms GPU per full page at group granularity

cut AS          rebuilt when that LOD level's membership changes
                0.2 - 0.33 ms; a pointer list, touches no geometry

scene TLAS      rebuilt per frame over resident pages
                0.27 ms at 256 instances, 0.39 ms at 1,024

handle table    16-byte write per node, published only from the BLAS build's
                completion handler
```

```text
WHERE THE ERROR TEST RUNS

once per frame, in the culling pass that already computes it:
    projSelfError / projParentError with camera-projected error
    -> which groups each cut AS contains

per ray:
    nothing. Triangles are opaque, no camera matrix is bound, no error value
    is read. The cut AS already holds only the selected groups.
```

```text
INVARIANTS THAT MUST HOLD

ancestor-closed      if a node's BLAS is installed, every ancestor's is too
sibling-complete     descend only when EVERY child's BLAS is installed;
                     otherwise the parent owns the whole group
single-generation    all per-level cut structures and the scene TLAS publish
                     from one cut evaluation
handle-after-build   a node's acceleration-structure handle is published only
                     from the build's completion handler
three-way retire     a BLAS is freed only after raster, trace and Metal
                     command-buffer references have all drained
```

```text
PER-RAY QUALITY (optional)

8 instance mask bands, culled in hardware before descending into a subtree.
Each band is a COMPLETE cut at a different error threshold - never a single
LOD level. A band costs a pointer list; group BLASes are shared.
    band 0  threshold T     primary, reflection
    band 1  threshold 2T    shadows
    band 2  threshold 4T    wide diffuse GI
```
#### Platform capability, verified

Everything in the "measured" tables in this milestone was produced by standalone Metal
probes run on this machine (Apple M4, macOS 26, Metal toolchain 32023). They
live in `ignore/metal-rt-research/` and are reproducible. Anything not measured
is called out explicitly as an open question.

Verified device capability, from a direct `MTLDevice` probe:

```text
device                       : Apple M4          (MTLGPUFamilyApple9, Metal3)
supportsRaytracing           : yes
supportsRaytracingFromRender : yes
supportsFunctionPointers     : yes
hasUnifiedMemory             : yes
maxBufferLength              : 9093 MB
MTLIndirectInstanceAccelerationStructureDescriptor : available
MTLAccelerationStructureBoundingBoxGeometryDescriptor : available
primitiveDataBuffer (per-primitive data)           : available
heapAccelerationStructureSizeAndAlignWithSize:     : available
```

Verified Vulkan capability, from `vulkaninfo` against the installed MoltenVK
(1.2.11, Vulkan 1.2.296, VulkanSDK 1.4.313.1):

```text
VK_EXT_metal_objects            : present, revision 2
VK_KHR_acceleration_structure   : ABSENT
VK_KHR_ray_query                : ABSENT
VK_KHR_ray_tracing_pipeline     : ABSENT
```

That settles the portability question for this repo: there is no
"turn on Vulkan RT through MoltenVK" path. Native Metal RT is the only way to
reach the M4's ray tracing hardware from this renderer, and it has to be a
sidecar backend exactly like `TerrainMetalComputeRasterPass` already is.

#### Adopted: three-level structure, one intermediate AS per DAG cut

The LOD-window design above answers "how do we avoid rebuilds" by carrying
several levels of geometry and filtering per candidate. There is a better answer,
and it removes the filtering entirely.

Metal instance acceleration structures can reference **other instance
acceleration structures** (multi-level instancing, traced with
`intersector<instancing, max_levels<N>>`). That allows:

```text
scene TLAS                one instance per resident page
  -> cut AS               one intermediate instance AS per DAG cut
                          its members ARE the clusters that cut selects
      -> cluster BLAS     one per DAG node, built once at stream-in,
                          SHARED by every cut that contains it
```

The property that makes this the right structure: **a cut is a list of pointers,
not a copy of geometry.** Adjacent cuts of a DAG overlap heavily, and a cluster
BLAS referenced by three cuts is stored once. A cut costs 72 bytes per selected
cluster - the instance descriptor - plus a small instance AS.

Measured against the LOD-window design on the same 3-level DAG page
(level *k* has 4^*k* clusters of 128 triangles), 1920x1080, 256 page instances:

| | structure | trace | throughput | cut change costs |
| --- | --- | ---: | ---: | --- |
| **A** LOD window | one page BLAS, all levels, non-opaque, per-candidate cut | 3.225 ms | 643 Mray/s | rebuild the page BLAS |
| **B** three-level | shared cluster BLASes + one cut AS, **opaque** | **1.316 ms** | **1,575 Mray/s** | rebuild a 1.8 KB pointer list, **0.233 ms** |

Per-cut intermediate AS sizes on that page: 0.5 KB for a 1-cluster cut, 1.2 KB
for 4 clusters, 3.7 KB for 16 clusters, rebuilding in 0.096 / 0.185 / 0.233 ms.

(Caveat on the trace column: hit counts differ between the two rows -
135,090 versus 78,282 - because design A can hit any of three levels while
design B contains only the selected cut, and the cluster placement differs
between them. The ~2.4x is consistent with the independent opaque-versus-
per-candidate measurement elsewhere in this section, but it is not a
controlled A/B.)

##### So how are hits filtered by error? They are not.

This is the point of the structure, and it is worth stating plainly because it
inverts the previous design. In the three-level structure there is **no
per-candidate error test at all**:

- the cut AS contains exactly the clusters the two-test cut selected, and
  nothing else;
- so every triangle a ray can reach is already at the correct LOD;
- triangles stay `opaque = YES`, no intersection function is linked, and no
  shader runs during traversal;
- `selfError` and `parentError` are evaluated **once per cut change**, on the
  CPU or in the existing culling compute pass - not per ray, not per candidate.

The error test moves out of the ray loop entirely. That is where the 643 ->
1,575 Mray/s comes from.

##### One correction to cluster granularity

Making the BLAS unit a single 128-triangle cluster is the one part to change.
Heap placement alignment is 16 KB, and a 128-triangle BLAS lands at roughly
14-18 KB, so each one rounds up to a 32 KB slot. Measured on the page above:
21 cluster BLASes consumed **672 KB** where the same geometry inside one BLAS
took **432 KB** - the overhead is almost entirely alignment.

Use a **group** (7 clusters, 896 triangles, ~95 KB AS) as the BLAS unit
instead. Alignment waste drops to about 1%, and a group is already the unit
that shares a projected-error decision in this codebase, so a cut naturally
selects groups rather than individual clusters.

##### Optional: per-ray LOD for free via the instance mask

If a ray class wants a different LOD than the camera-driven cut - a wide diffuse
bounce that can afford coarser geometry, say - publish several cuts into the
scene TLAS with `mask = 1 << band` and let each ray choose its band in
`intersect(r, accel, mask)`. That is a hardware cull evaluated during TLAS
traversal, costs no shader, and gives 8 bands. Measured effect of narrowing the
mask: 2.24 ms to 1.46 ms at 4 of 8 bands live, non-monotonic (fewer bands means
longer rays), so treat it as a useful option rather than an LOD mechanism in its
own right.

##### What each structure is rebuilt by, and when

```text
cluster/group BLAS   built once when its DAG node streams in
                     never rebuilt while the node stays resident
                     shared by every cut that contains it

cut AS               rebuilt when that page's cut changes: 0.1-0.25 ms
                     a pointer list; touches no geometry

scene TLAS           one instance per resident page, thousands not millions
                     0.46 ms at 1,024 instances
```

This is the adopted design. The per-group-BLAS alternative rebuilt geometry on
every cut change; the LOD-window alternative avoided that by duplicating
geometry and paying a 2x traversal penalty. The three-level
structure pays neither: geometry is built once and shared, cuts are cheap
pointer lists, and traversal runs at full opaque hardware speed.

#### The VG hierarchy already has this shape; mirror it

Before designing an acceleration-structure layout, it is worth reading what
`VirtualGeometryHierarchyBuilder` already builds, because the structure the
three-level Metal design wants is the structure that exists.

`buildHierarchy` (`VirtualGeometryHierarchyBuilder.cpp:701`) does this:

```text
for each LOD level:
    buildHierarchyLevel(groupsPerLevel[level])   -> returns a LEVEL ROOT
        one BVHNode per GROUP, children = that group's clusters, isLeaf = true
        then buildBVHIntermediateNodes over those group nodes
buildBVHIntermediateNodes(roots)                 -> top BVH over the level roots
```

So the hierarchy is:

```text
hierarchyRoot
  +- BVH over per-LOD-level roots
       +- LOD level 0 root  -- BVH over level-0 group leaves
       +- LOD level 1 root  -- BVH over level-1 group leaves
       +- ...
                              each group leaf: <= MaxBVHChildren (8) clusters,
                              GroupSize = 7, ClusterSize = 128 triangles
```

Two facts fall out, and both are load-bearing:

**A leaf hierarchy node is exactly one group.** `buildHierarchyLevel` pushes one
`BVHNode` per group with `children` = that group's cluster indices and
`isLeaf = true`. So the natural BLAS unit is a leaf hierarchy node, which is
896-1024 triangles. That is the same unit this section arrived at independently
from heap-alignment measurements, and the agreement is a good sign rather than a
coincidence: it is the unit that shares an error decision.

**The top of the hierarchy is already partitioned by LOD level.** Each LOD level
has its own root and its own subtree. That is precisely the boundary an
intermediate acceleration structure wants.

The mapping is therefore one-to-one:

```text
VG hierarchy                          Metal acceleration structures
-------------------------------       --------------------------------------
hierarchyRoot / instance BVH     ->   scene TLAS
per-LOD-level root               ->   one intermediate instance AS per level
group leaf node (<= 8 clusters)  ->   one group BLAS
cluster (128 triangles)          ->   geometry inside that BLAS
```

Node fields carry across without translation:

```text
min_lod_error         min self error over the group's clusters
max_parent_lod_error  max parent error       -> the two-test cut, unchanged
child_start/count     the group's clusters   -> the BLAS's triangles
pageIndex             residency              -> whether the BLAS exists yet
flags bit 0           isLeaf
flags [15:8]          streamingLeafsBitset   -> effectiveSelfError = 0,
                                                i.e. always admissible; this is
                                                the same guard as publishing
                                                parentError = +inf
flags [23:16]         enabledClustersBitset  -> which clusters the BLAS contains
```

##### Measured: splitting the cut per LOD level is faster, not just tidier

The cut is a *partition* - each group belongs to exactly one LOD level - so the
union of per-level intermediate structures is exactly the cut. That makes the
split free in correctness terms. The question is what it costs.

A realistic cut frontier: 256 groups of 896 triangles spread 16 / 64 / 128 / 48
across four LOD levels, 256 page instances, 1920x1080.

| | structure | scene TLAS | trace | throughput | hits |
| --- | --- | ---: | ---: | ---: | ---: |
| FLAT | one cut AS holding all 256 groups | 256 inst, 54.5 KB | 2.415 ms | 859 Mray/s | 285,742 |
| SPLIT | one cut AS per LOD level (4) | 1,024 inst, 216.8 KB | **1.646 ms** | **1,260 Mray/s** | 285,742 |

Hit counts are identical, so this is a controlled comparison. **Mirroring the
hierarchy's LOD partition traces 1.47x faster** for the same geometry and the
same cut. The reason is that each intermediate structure then holds groups of
similar size and density, which builds a better BVH, and the scene TLAS gets
1,024 finer-grained instances to cull against instead of 256 coarse ones.

Per-level intermediate AS sizes and rebuilds on that cut:

| level | groups | AS size | rebuild |
| ---: | ---: | ---: | ---: |
| 0 | 16 | 3.7 KB | 0.215 ms |
| 1 | 64 | 13.9 KB | 0.282 ms |
| 2 | 128 | 27.4 KB | 0.322 ms |
| 3 | 48 | 10.4 KB | 0.275 ms |
| FLAT, for comparison | 256 | 54.5 KB | 0.325 ms |

##### The rebuild saving does not materialise, and that is worth knowing

The expectation was that splitting per level would make a cut change cheap
because only one level rebuilds. It does not, at this scale: the worst single
level costs **0.322 ms** against **0.325 ms** for rebuilding the entire flat cut.

The reason is a large fixed cost per instance-AS build - 16 groups still take
0.215 ms - plus the fact that a frontier concentrates most of its groups in one
or two levels, so "the level that changed" is often the big one. Rebuilding
*all* levels costs 1.093 ms, which is worse than the flat 0.325 ms.

So the split should be adopted **for the 1.47x trace win**, not for rebuild
economics, and the rebuild policy should be "rebuild the levels that changed,
but if more than one changed, consider rebuilding flat". The scene TLAS also
grows 4x in instance count (0.268 -> 0.392 ms to build, 54.5 -> 216.8 KB), which
is affordable but not free.

##### What this removes from the ray path

Today `processHierarchyNodeElement` (`culling_multipass.wgsl:963`) is a
persistent-queue compute traversal: dequeue a node, test `max_parent_lod_error`,
frustum, projected footprint and occlusion, enqueue children, and at a leaf
enqueue its clusters. That is affordable once per frame for the screen cut. It
is not affordable per ray.

With the structure mirrored into acceleration structures, none of it runs on the
ray path:

```text
compute hierarchy traversal   once per frame, for the screen cut and to decide
                              which groups each cut AS contains
hardware TLAS descent         per ray, replaces the queue traversal entirely
error test                    not on the ray path at all - the cut AS already
                              contains only the selected groups
```

The remaining per-frame CPU/GPU work is writing instance descriptors for the
levels whose membership changed and rebuilding those structures.

#### Resident hierarchy, streamed BLASes, and the descent rule that keeps it whole

The hierarchy is cheap to keep whole - `VirtualGeometryHierarchy` is a flat
array of small nodes, and `VirtualGeometryScene` already installs it in full
while pages stream. Acceleration structures are the expensive part and should be
the streamed resource. So the model is:

```text
always resident   the hierarchy: bounds, min_lod_error, max_parent_lod_error,
                  child_start/count, pageIndex, flags
streamed          one BLAS per node, built when that node's page installs
updated in place  a per-node handle table entry, written on install/uninstall
```

##### Linking a built BLAS back into the hierarchy

The link is a parallel array indexed by hierarchy node index:

```cpp
struct VGNodeAccelerationSlot
{
  MTLAccelerationStructureID blas;   // 64-bit GPU handle, 0 when not installed
  uint32_t generation;
  uint32_t flags;
};
```

`MTLAccelerationStructureID` is exactly the type
`MTLIndirectAccelerationStructureInstanceDescriptor.accelerationStructureID`
expects, and indirect instance descriptors are GPU-writable. So the cut pass can
read the handle out of this table and write it straight into an instance
descriptor with no CPU involvement:

```text
cut pass (compute, GPU):
  for each hierarchy node:
     evaluate the two tests with camera-projected error
     clamp to the installed frontier (slot.blas != 0)
     if selected: append an instance descriptor with
                  accelerationStructureID = slot.blas
  write instanceCountBuffer
CPU: encode the per-level AS builds. Nothing is read back.
```

That is the whole of "link them to the hierarchy": a 16-byte table write when a
BLAS finishes building, and the entry is only published from the Metal command
buffer's completion handler so no cut can reference a structure that is still
being built.

##### Measured: the descent rule decides whether holes appear

The cut is evaluated over the full hierarchy but can only select nodes whose
BLAS exists. What happens at the frontier is not a detail - it is the difference
between a correct image and a hole.

A 4-level binary DAG over a receding ground plane (8 / 16 / 32 / 64 nodes, each
coarse node spanning exactly its two children's area, so selecting an ancestor
covers both), 120 node BLASes, 1280x720. Two descent rules compared at several
BLAS residency levels:

- **ANY-child**: descend into whichever children are installed.
- **SIBLING-COMPLETE**: descend only if *every* child is installed; otherwise the
  parent owns the whole group.

| residency | rule | BLASes | cut size | rays hitting geometry | cand/hit | multi-LOD rays |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 100% | - | 120 | 49 | 238,932 | 1.000 | 0 |
| 75% | ANY-child | 64 | 23 | **100,056** | 1.000 | 0 |
| 75% | SIBLING-COMPLETE | 64 | 19 | **238,932** | 1.000 | 0 |
| 50% | ANY-child | 27 | 12 | **96,892** | 1.000 | 0 |
| 50% | SIBLING-COMPLETE | 27 | 9 | **238,932** | 1.000 | 0 |
| 25% | ANY-child | 17 | 10 | **193,640** | 1.000 | 0 |
| 25% | SIBLING-COMPLETE | 17 | 10 | **238,932** | 1.000 | 0 |

**Sibling-complete descent gives byte-identical coverage to a fully installed
scene at every residency level tested, down to 25% of BLASes present.** The cut
shrinks gracefully - 49 instances to 9 - and the geometry gets coarser, but not
one ray is lost. The any-child rule loses up to 58% of the surface.

The reason is simple once seen: with a node whose left child is installed and
whose right child is not, selecting the installed child leaves the sibling's
area uncovered, and adding the parent as well would cover the installed child
twice. Neither is acceptable, so the parent must own the whole group. In this
codebase that means all `child_count` children (up to `MaxBVHChildren` = 8),
which is the same "ownership moves a whole group, never one child" rule the
terrain cut already had to learn.

Double coverage never appeared in any configuration: `cand/hit` stayed at 1.000
and no ray saw two LOD levels. The failure mode of a wrong descent rule is
holes, not overlap - which is worse, because a hole is a ray that escapes to the
sky and reads as a light leak rather than as a visible seam.

##### Residency must also be ancestor-closed

Sibling-completeness is necessary but not sufficient. Evicting interior nodes
while keeping their descendants breaks it, because there is then no ancestor to
fall back to:

| scenario | rule | BLASes | cut size | rays hitting geometry |
| --- | --- | ---: | ---: | ---: |
| interior nodes evicted | ANY-child | 94 | 13 | 126,350 |
| interior nodes evicted | SIBLING-COMPLETE | 94 | 4 | **189,982** |

Even with the right descent rule, coverage drops from 238,932 to 189,982. So the
BLAS residency set must satisfy both invariants:

```text
ancestor-closed     if a node's BLAS is installed, every ancestor's is too
sibling-complete    a group of siblings is installed or evicted together
```

Both are already implied by the existing page dependency graph, and both have
prior form in this repo: ancestor-closed residency was what finally removed the
terrain holes, and "ownership must move a whole child group" was what stopped a
staged child from evicting its parent. The RT path inherits the same rules
rather than needing new ones.

##### How this maps onto the flags that already exist

```text
streamingLeafsBitset (flags[15:8])
    clusters on the streaming boundary; the shader forces
    effectiveSelfError = 0 so they are always admissible
    -> this IS the installed-frontier clamp, already implemented

enabledClustersBitset (flags[23:16])
    clusters enabled in the installed DAG cut, updated on page
    install/uninstall via setNodeClusterMasks
    -> determines a group BLAS's CONTENT, so it changes only when
       a page installs, which is when the BLAS is built anyway

per-frame camera cut
    -> determines cut AS MEMBERSHIP, rebuilt when the cut moves
```

The two are distinct and change on different clocks. Conflating them would
rebuild geometry on camera motion, which is the mistake the earlier drafts of
this section made.

#### Verified: a ray hits exactly one LOD, and the trace needs no camera

The correctness question the three-level design rests on is whether a ray sees
only the geometry at the selected error threshold, or whether it sees every LOD
representation stacked at that position. It was measured, not assumed.

Setup: a ground plane receding from the camera, split into 64 spatial regions,
each carrying 4 LOD representations of the same patch (`selfError` 8/4/2/1,
`parentError` = the parent level's `selfError`) - 256 group BLASes, 6.8 MB. The
two-test cut is evaluated per region with camera-projected error, exactly as
`culling_multipass.wgsl` does, and picks L0 for 1 region, L1 for 11, L2 for 12
and L3 for 40 - coarse far, fine near. Then 1280x720 rays, counting for each ray
how many triangle candidates it was shown and from how many distinct levels.

| structure | hits | candidates/hit | distinct LODs/hit | rays seeing >1 LOD |
| --- | ---: | ---: | ---: | ---: |
| **CUT** - per-level cut AS, selected groups only | 238,932 | **1.00** | **1.000** | **0** |
| NO CUT - every level of every region present | 238,932 | 1.54 | 1.540 | **117,086** |

Hit counts are identical, so both structures cover the same surface. With the
cut applied, **every ray sees exactly one triangle at exactly one LOD**, and not
a single ray out of 921,600 encountered two levels. Without it, 49% of rays hit
stacked LOD representations of the same surface.

##### The camera is used to build the cut, not to trace it

This is the part worth stating explicitly, because it inverts the earlier
LOD-window design. In the three-level structure the trace kernel does **not**
need the camera matrices:

```text
once per frame, in the existing culling pass:
    projSelfError / projParentError from the camera
    -> which groups each per-level cut AS contains

per ray, at trace time:
    nothing. The cut AS already holds only the selected groups.
    Triangles stay opaque. No intersection function is linked.
    No error value is read. No camera matrix is bound.
```

Binding camera uniforms to the trace kernel is only required for the *other*
view - the bounding-box refinement structure, which carries several levels and
resolves them per candidate. The exact triangle view does not.

##### Why this is guaranteed rather than lucky

The two-test cut plus the monotonicity invariant is what makes it exact.
`SparseVoxelBrickNodeGPU` documents the guarantee: a parent's error is at least
the maximum of its children's. Given that, exactly one node per root-to-leaf
path satisfies `selfError < threshold && parentError >= threshold`. The cut is
therefore a **partition** of the surface, and the union of the per-level cut
structures covers every region exactly once - which is also why splitting the
cut per LOD level is free in correctness terms.

Three ways this can still break, none of them a property of the acceleration
structures:

- **A stale cut.** If a per-level cut AS was built for one camera and the scene
  TLAS or a sibling level for another, regions can be covered twice or not at
  all. Every per-level structure and the TLAS must be published from one cut
  evaluation, the way page publication is already transactional.
- **A non-resident desired level.** The cut must fall back to a resident
  ancestor, and must then *not* also include that ancestor's descendants. This
  is the same conservative-ancestor rule the raster cut already applies, and it
  is what `enabledClustersBitset` encodes: the installed DAG cut, distinct from
  the per-frame camera cut. That mask changes on page install/uninstall, so it
  determines BLAS *content*; the camera cut determines cut AS *membership*.
- **Cracks at LOD boundaries.** Where two adjacent regions sit at different
  levels, an overlap is harmless - it is bounded by the published error - but a
  gap lets rays through and reads as a light leak. Crack-freeness is a property
  of the simplifier's locked group boundaries, not of the acceleration
  structure, and it is the same property the raster path already depends on.

#### Measured: the instance mask culls during hierarchy traversal

There **is** a hardware mechanism that skips a subtree during traversal without
running any shader: the 8-bit instance mask. `intersect(r, accel, rayMask)`
skips an instance when `(instance.mask & rayMask) == 0`, and the test happens in
the traversal unit *before* descending into that instance.

Verified on a scene TLAS holding one instance per LOD level, each instance
pointing at that level's intermediate AS with `mask = 1 << level`, counting
triangle candidates per level at 1280x720:

| ray mask | L0 candidates | L1 | L2 | L3 | trace (opaque) |
| --- | ---: | ---: | ---: | ---: | ---: |
| 0xF (all) | 238,932 | 66,057 | 37,578 | 25,328 | 1.555 ms |
| 0x1 | 238,932 | **0** | **0** | **0** | 0.680 ms |
| 0x2 | **0** | 238,932 | **0** | **0** | 0.601 ms |
| 0x4 | **0** | **0** | 238,932 | **0** | 0.600 ms |
| 0x8 | **0** | **0** | **0** | 238,933 | 0.540 ms |
| 0x9 | 238,932 | **0** | **0** | 71,480 | 0.709 ms |

**Zero leakage.** A masked-out level contributes exactly zero candidates - the
hardware never enters its intermediate acceleration structure. Narrowing from
all four levels to one is **2.3-2.9x faster** (1.555 ms to 0.54-0.68 ms).

So the answer to "can traversal be culled on LOD, the way the culling pass
does" is yes, at the subtree level, in hardware, for free. With three
qualifications that shape how it must be used.

##### Qualification 1: eight bands, and the mask is fixed for the whole ray

The mask is 8 bits and is chosen when the ray is spawned. It cannot change as
the ray travels, so it cannot express "use coarser geometry once I am 500 m
along". A ray commits to its bands up front.

##### Qualification 2: a band must be a COMPLETE cut, not a single LOD level

This is the part that is easy to get wrong. In the table above each level's AS
held *all 64 regions* at that level, so each band was already a complete,
uniform-LOD cut - which is why masking to a single band still returned full
coverage (238,932 hits).

If instead each band held only the regions whose camera-driven cut chose that
level, then a single-band ray would see only part of the world and the rest
would be a hole. Membership already encodes the camera cut; the mask must not
try to encode it a second time.

The correct use is therefore:

```text
band 0   the cut at error threshold T        (sharp)
band 1   the cut at error threshold 2T       (one step coarser)
band 2   the cut at error threshold 4T       (coarser still)
...
each band is a COMPLETE cut covering the whole world, at a different quality.
Within one band, per-region LOD still varies correctly with camera distance.
```

A primary or reflection ray picks band 0. A wide diffuse GI bounce picks band 2
and traverses 2-3x less. Shadow rays pick whatever their receiver can justify.
That is a per-ray-class LOD dial, evaluated in hardware, at no shader cost.

The memory story is what makes this affordable: **an extra band is an extra
list of pointers, not an extra copy of geometry.** Group BLASes are shared
across every cut and every band; a band costs 72 bytes per selected group plus a
small instance AS. Publishing three quality bands roughly triples the cut
structures, which are kilobytes, and changes the BLAS pool not at all.

##### Qualification 3: this is not how streaming culling works

"Cull during traversal based on loading" happens at cut-build time, not at trace
time. A group whose BLAS is not installed is simply absent from the cut AS, and
the sibling-complete descent rule ensures its parent covers the gap. By the time
a ray runs, nothing unloaded is reachable, so there is nothing to cull. The mask
is for *quality* selection, not *residency*.

##### On "one BLAS per LOD level"

Worth separating two readings. One *intermediate acceleration structure* per LOD
level is the adopted design and measured 1.47x faster than a single flat cut
structure. One *BLAS* per LOD level - a single bottom-level structure holding
all of that level's triangles - would be worse: any group installing or
uninstalling anywhere in the level would force a rebuild of the whole thing,
which is exactly the geometry-rebuild-on-change cost this design exists to
avoid. Keep BLASes at group granularity and let the per-level structure be an
instance list.

#### Two AS views, and what each is for

```text
committed group (7 clusters, 896 triangles)
  -> exact view : triangle BLAS, hardware intersector, ~109 B/tri
                  built at upload, at the LOD the group is resident at
                  the cut is expressed by WHICH groups are instanced
                  used by primary, reflection and exact shadow rays

  -> refinement view : bounding-box BLAS over DAG nodes, ~85 B/box + metadata
                  carries {selfError, parentError, pageId, nodeIndex}
                  several LOD levels present as nested primitives
                  the intersection function evaluates the two-test cut with
                  camera-projected error and COMMITS on the accepted node,
                  which collapses that node's whole subtree
                  used for coarse/offscreen occluders, for regions whose
                  exact view is not resident, and to emit page requests
```

Finding: the hardware triangle intersector is **5.3x faster** than a custom
AABB shader loop (924 vs 175 Mray/s; 4,880 vs 1,267 any-hit). So the exact view
carries the primary load and the refinement view is deliberately the smaller,
coarser, cheaper structure - not a full re-emission of the DAG at leaf
resolution.

Both are instanced into one TLAS and selected per ray by instance mask, which
costs nothing.

#### Linking new pages into the hierarchy

"As we upload new pages we build new acceleration structures for the new
clusters and link them to the hierarchy" is the whole streaming contract, and it
decomposes cleanly because the BLAS unit and the hierarchy's leaf unit are the
same thing - a group.

```text
page install transaction
  1. GPU emits the page's cluster geometry            (Milestone 1 build)
  2. GPU emits group-local triangle/index slots at fixed capacity,
     stamping unused primitives degenerate so the CPU never needs the count
  3. CPU encodes one buildAccelerationStructure: per leaf hierarchy node in
     the page, each with its own scratch slice        ~1.9 ms GPU per full page
  4. the Metal command buffer's completion handler fires
  5. each node's MTLAccelerationStructureID is written into the per-node
     handle table, keyed by hierarchy node index      16 bytes per node
  6. the page's nodes become eligible for selection
```

Step 5 is the "link to the hierarchy" step and it is just a table write. A
node's handle is zero until its build has completed, and the cut pass treats a
zero handle as "not installed" - which, with sibling-complete descent, means the
parent owns that group.

Nothing in steps 1-6 happens per frame. Per frame, only this runs:

```text
per frame
  a. cut pass evaluates the two tests over the resident hierarchy with
     camera-projected error, clamped to the installed frontier, and writes
     instance descriptors per LOD level                GPU, no readback
  b. rebuild the per-level cut structures whose membership changed
                                                       0.2 - 0.33 ms each
  c. rebuild the scene TLAS over resident pages        0.27 ms at 256 instances
```

An LOD change is step b alone - a pointer list, no geometry. A page install is
steps 1-6. The two are on different clocks, and keeping them separate is the
whole reason this design avoids rebuilding geometry when the camera moves.

Page uninstall is the reverse: the node's handle table entry is zeroed first, so
no later cut can select it, and the BLAS is only freed once the three reference
classes have drained.

The streaming budget follows directly: at **~1.9 ms of GPU acceleration-structure
build per full page**, a 2 ms/frame build budget installs roughly one full page
per frame, and proportionally more for the partially-occupied pages real terrain
produces - the fixture measurements earlier in this plan show 7,821 brick-local
greedy quads across the whole fixture set, far under the 57,344 page capacity.

##### Measured: what we control about placement

Everything except the acceleration structure's internal layout. Probed
directly:

| what | control | rule |
| --- | --- | --- |
| vertex / index / bounding-box / primitive-data buffers | `buffer` + `offset` | arbitrary slices of one shared arena |
| instance descriptor buffer | `buffer` + `offset` | same |
| build scratch | `scratchBuffer` + `scratchBufferOffset` | any 4-byte-aligned offset |
| the acceleration structure itself | `MTLHeapTypePlacement` heap + chosen offset | must be `heapAccelerationStructureSizeAndAlign` aligned |
| the structure's internal bytes | **none** | opaque; `gpuResourceID` and `size` only |

Verified behaviours:

```text
geometry from a shared arena     four groups built from one vertex arena and one
                                 index arena at offsets 4096 / 36352 / 68608 /
                                 100864 - all built cleanly

scratch offsets                  0, 4, 16, 32, 64, 128, 256, 4096 all accepted.
                                 Per-build scratch slices are what turn a 20x
                                 serialisation into parallel builds, so this is
                                 the important one

AS placement                     MTLHeapTypePlacement:  offset 0 -> ok,
                                                        offset 98304 -> ok
                                 MTLHeapTypeAutomatic:  returns nil, always
                                 misaligned offset:     returns nil
                                 introspection works: heapOffset, allocatedSize,
                                 size, heap

alignment                        a 95,232-byte group AS reports align = 16,384
                                 -> 98,304-byte slot, 3% waste at group
                                 granularity, but 2x waste for a 14 KB
                                 cluster-sized AS. Reconfirms group granularity.

slot reuse                       release the structure, place a DIFFERENT-sized
                                 one at the same offset, rebuild: works.
                                 That is the page uninstall/install cycle.
```

Two consequences for the pool:

- **Placement heaps only.** `newAccelerationStructureWithSize:offset:` returns
  nil on an automatic heap, so the BLAS pool must be `MTLHeapTypePlacement` and
  the offset arithmetic is ours to get right.
- **Aliasing is ours to police.** A placement heap does not track lifetime. The
  reuse above worked because the probe waited for completion; in a pipelined
  renderer, placing a new structure over a slot the GPU may still be reading is
  a data race. This is the same three-way retirement rule stated above - raster
  references, trace references, and the Metal command buffer - and the placement
  heap is precisely why it cannot be skipped.

So the pool is three arenas, all under our control:

```text
BLAS pool        one MTLHeapTypePlacement heap, slots by group size class,
                 16 KB granularity, reused on uninstall after retirement
geometry arena   one buffer, per-group vertex/index slices
scratch arena    one buffer, per-in-flight-build slices from the existing
                 TerrainGPUV2FencedArena - never one shared offset
```

One caveat on the offset test: the probe checked that the *build succeeds*, not
that the resulting geometry is correct. Metal's documented rule ties
`vertexBufferOffset` to `vertexStride`, and a 4-byte offset against a 12-byte
stride was accepted here. Align vertex slices to the stride anyway; a build that
does not error is not proof of a correct structure.

#### Representation, mapping, and the streaming flow

This section is the concrete design: what data exists, how the identity spaces
map onto each other, and what happens on install, on uninstall, and per frame.
Every existing name below is real - `VirtualGeometryHierarchy`,
`PageTableEntry`, `pagePriorities`, `TerrainGPUV2Scheduler` - and only the
structures marked NEW have to be added.

##### Four identity spaces, and the maps between them

```text
hierarchy node index   stable for the lifetime of the asset; the whole
                       hierarchy array is resident, installed or not
        |
        |  node.pageIndex  ->  resolveGlobalPageIndex(raw, instance.pageTableOffset)
        v
global page index      the streaming unit; pageTable[] entry says whether the
                       geometry is resident
        |
        |  pageEntry.clusterOffset + (node.child_start + i)
        v
global cluster index   the geometry: DecodedClusterCullData +
                       DecodedClusterGeometryInfo
        |
        |  NEW: accelerationSlot[nodeIndex]
        v
acceleration slot      a placement-heap offset holding this node's group BLAS
```

The first three already exist and the culling shader already walks them. The
fourth is the only new one, and it is keyed by **hierarchy node index**, not by
page or cluster - because a leaf hierarchy node is exactly one group, which is
exactly one BLAS.

##### NEW: the per-node acceleration table

One entry per hierarchy node, sized with the hierarchy, resident whenever the
hierarchy is:

```cpp
struct VGNodeAccelerationSlot          // 16 bytes
{
  // MTLAccelerationStructureID. Zero means "no BLAS": either this node is not
  // a leaf, or its page is not installed, or its build has not completed.
  uint64_t blasHandle;
  // The page generation this BLAS was built from. A cut that sees a stale
  // generation treats the slot as empty rather than tracing old geometry.
  uint32_t generation;
  // bit 0  build in flight
  // bit 1  retirement pending
  // bits 8..15  heap size class, for slot reuse
  uint32_t flags;
};
```

`blasHandle` is the exact type `MTLIndirectAccelerationStructureInstanceDescriptor.accelerationStructureID`
expects, so a GPU cut pass copies it straight through with no translation and no
CPU readback.

##### NEW: per-level cut state

The cut is published per LOD level, mirroring the hierarchy's own per-level
roots. For each instance and each level:

```cpp
struct RTLevelCut                      // CPU-side bookkeeping
{
  uint32_t instanceIndex;
  uint32_t lodLevel;
  BufferId instanceDescriptors;        // GPU-written, maxInstanceCount capacity
  BufferId instanceCount;              // GPU-written, feeds instanceCountBuffer
  void    *cutAccelerationStructure;   // id<MTLAccelerationStructure>
  uint32_t membershipRevision;         // bumped by the cut pass when it changes
  uint32_t builtRevision;              // last revision actually rebuilt
};
```

`membershipRevision != builtRevision` is the only signal needed to decide which
levels to rebuild this frame. The cut pass computes membership and bumps the
revision; the CPU encodes rebuilds only for levels where the two disagree.

##### The BLAS pool

```text
one MTLHeapTypePlacement heap
    slots grouped into size classes by group triangle capacity
    each slot aligned to heapAccelerationStructureSizeAndAlign (16 KB)
    a free list per size class

one geometry arena buffer
    per-group vertex and index slices, addressed by offset
    written by the GPU during EmitRTPrimitives

one scratch arena
    per-in-flight-build slices from TerrainGPUV2FencedArena
    never a single shared offset - that serialises builds up to 20x
```

##### Streaming requests: what already exists, and what is added

The demand signal already works and does not need replacing.
`processHierarchyNodeElement` does:

```wgsl
atomicAdd(&pagePriorities[entry.prioritySlot],
          computePriority(node, instance, nodeModelMatrix, projParentError));
```

`prioritySlot` is assigned permanently at page registration and is valid whether
or not the page is installed, which is precisely what lets demand accumulate for
pages that do not exist yet. The CPU reads `pagePriorityBuffer` back, builds
install candidates (not installed, priority > 0, descending) and evict
candidates (installed, ascending), and hands them to the streaming manager.

The RT path adds one producer and changes nothing else:

```text
producer 1  raster cut          existing: pages the screen needs
producer 2  RT cut pass         NEW: pages the trace cut needs, including
                                offscreen occluders the screen never touches
producer 3  trace feedback      NEW: rays that hit a coarse fallback and want
                                finer geometry, appended to the same buffer
```

All three write the same `pagePriorities[prioritySlot]` accumulator, so the
existing readback, candidate sort and eviction policy are unchanged. Producer 2
matters because a screen-only demand signal makes shadows and GI
frustum-dependent - turning the camera would evict occluders that are still
casting.

##### Install: page -> BLAS -> link

This runs inside `TerrainGPUV2Scheduler` as three new `TerrainGPUJobStage`
values between `Validate` and `ReadyToPublish`.

```text
1  the existing page install completes: PageTableEntry gets bufferOffset,
   size, clusterOffset, clusterCount, isInstalled = 1, and
   setNodeClusterMasks writes streamingLeafsBitset / enabledClustersBitset
   into each leaf node's flags

2  EmitRTPrimitives  (GPU)
   for each leaf hierarchy node in the page:
       for each cluster i in [child_start, child_start + child_count):
           skip unless (enabledClustersBitset >> i) & 1
           read DecodedClusterGeometryInfo at clusterOffset + i
           write its triangles into the node's geometry-arena slice
       stamp the unused tail of the slice degenerate, so the CPU never needs
       to know the emitted count

3  BuildRTAcceleration  (CPU encode, GPU execute)
   allocate a pool slot in the node's size class
   encode buildAccelerationStructure: with
       vertexBuffer  = geometry arena,  vertexBufferOffset  = node slice
       indexBuffer   = geometry arena,  indexBufferOffset   = node slice
       scratchBuffer = scratch arena,   scratchBufferOffset = per-build slice
   ~1.9 ms GPU for a full page's worth of nodes

4  RTAccelerationFenced
   on the Metal command buffer's completion handler:
       accelerationSlot[nodeIndex].blasHandle  = [as gpuResourceID]
       accelerationSlot[nodeIndex].generation  = page generation
       clear the build-in-flight bit
   only now can any cut select this node
```

Step 4 is the link, and doing it from the completion handler rather than at
encode time is what makes it impossible for a cut to reference a structure that
is still being built.

##### Uninstall: unlink -> drain -> reuse

```text
1  zero accelerationSlot[nodeIndex].blasHandle FIRST
   no later cut pass can select the node from this point on

2  mark retirement pending; the slot is not returned to the free list yet

3  drain three reference classes, all of which must complete:
       raster references          existing Vulkan frame fence
       trace references           existing Vulkan frame fence
       Metal command buffer       completion handler of the last RT encoder
                                  that traced it

4  release the MTLAccelerationStructure and return the slot to the free list
```

The third class is the new one and it is on a different timeline from the other
two. A placement heap tracks no lifetime, so placing a new structure over a slot
the GPU may still be reading is a silent data race with no validation to catch
it. This is why the drain cannot be shortened.

##### Three fields the existing structs do not have

The design above needs three values that `VirtualGeometryHierarchy` and
`PageTableEntry` do not currently carry. All three fit in space that already
exists, so none of them grows a struct:

```text
node LOD level      VirtualGeometryHierarchy has no lodLevel; only the
                    build-time BVHNode does. The cut pass needs it to know
                    which per-level instance list to append to.
                    -> pack into flags bits [31:24], which are free
                       (bit 0 leaf, bit 1 legacy streaming, bit 2 force
                        traversal, [15:8] streaming mask, [23:16] enabled mask)
                    -> HIERARCHY_LOD_LEVEL_SHIFT = 24u

page generation     PageTableEntry has no generation, so a cut cannot tell a
                    reinstalled page from the one it was built against.
                    -> use one of the two existing _padding words
                    -> bumped on every install, compared against
                       VGNodeAccelerationSlot::generation

self bounding       the node carries a parent sphere (max_center_*, max_radius)
sphere              but only an AABB for itself. The existing
                    aabbBoundingSphere(nodeCullAABB(node, isSkinned)) helper
                    already derives it; no new field is needed, just the call.
```

The first two are the only real additions in this entire design, and both are
writes into padding that is already reserved.

##### The per-frame cut pass

One compute dispatch over the resident hierarchy. It is the only place the error
metric is evaluated, and it writes instance descriptors directly.

```text
for each hierarchy node, top down from the instance's root:

  1  residency
     inst = accelerationSlot[node].blasHandle != 0
         && accelerationSlot[node].generation == pageTable[globalPage].generation

  2  sibling-complete descent
     allChildrenInstalled = every child in [child_start, child_start+child_count)
                            has a non-zero, current slot
     if !allChildrenInstalled: this node owns the whole group, do not descend

  3  the two tests, camera-projected, exactly as culling_multipass.wgsl does
     streamingLeaf = inst && !allChildrenInstalled
     effectiveSelfError = streamingLeaf ? 0.0 : node.min_lod_error
     // the node carries a parent bounding SPHERE (max_center_*, max_radius)
     // but only an AABB for itself, so the self test uses the existing
     // aabbBoundingSphere(nodeCullAABB(node)) helper
     selfSphere = aabbBoundingSphere(nodeCullAABB(node, isSkinned))
     projSelf   = calculateProjectedError(selfSphere.center, selfSphere.radius,
                                          effectiveSelfError, model, viewPos)
     projParent = calculateProjectedError(parentCenter, node.max_radius,
                                          node.max_parent_lod_error, model, viewPos)
     selected   = inst && projSelf < threshold && projParent >= threshold

  4  emit
     if selected:
        level = (node.flags >> HIERARCHY_LOD_LEVEL_SHIFT) & 0xFFu
        slot  = atomicAdd(&instanceCount[level], 1)
        instanceDescriptors[level][slot] = {
            accelerationStructureID = accelerationSlot[node].blasHandle,
            transformationMatrix    = resolveMeshPartModelMatrix(instance,
                                                                 node.meshPartIndex),
            mask                    = qualityBandBits,
            userID                  = nodeIndex }
     else if !selected && allChildrenInstalled:
        descend into children
```

Then, on the CPU: for every level whose `membershipRevision` moved, encode a
rebuild of that level's cut AS (0.2-0.33 ms each); rebuild the scene TLAS over
resident pages (0.27 ms at 256 instances); publish all of them in one
transaction so no level can be a generation ahead of its siblings.

##### Failure and fallback policy

```text
node's page not installed              -> slot is zero -> parent owns the group
                                          (sibling-complete descent)
build still in flight                  -> slot is zero -> same
page generation moved under a cut      -> generation mismatch -> treat as zero,
                                          and the page re-requests
no level admissible on a path          -> the coarsest resident node publishes
                                          parentError = +inf, so the two tests
                                          always have an answer
ray wanted finer geometry than resident-> hit the coarse fallback, write demand
                                          into pagePriorities, and report the
                                          hit as coarse rather than as a miss
cut structures out of sync             -> reject the publish; never render a
                                          mixed-generation cut
```

The last two are the ones that keep the failure mode visible. A missing page
must degrade to coarser geometry plus a request, never to a miss - a ray that
hits nothing escapes to the sky and reads as a light leak, which is exactly the
failure the residency measurements showed is the real risk here.

##### Sizing, from the measurements

```text
per hierarchy node        16 bytes of acceleration table
per group BLAS            ~95 KB at 896 triangles, in a 98 KB placement slot
per cut AS                3.7 KB at 16 groups, 27 KB at 128 groups
per scene TLAS            54 KB at 256 page instances
geometry arena            per-group vertex+index slices, fixed group capacity
scratch arena             ~55 KB per concurrent build, from the fenced arena

per-frame cost            cut pass (compute) + 0.2-0.33 ms per changed level
                          + 0.27 ms scene TLAS
per-install cost          ~1.9 ms GPU per full page of BLAS builds
```

#### Streaming, workers, and retirement

There should be no new worker system. `TerrainGPUV2Scheduler`
(`TerrainGPUV2Scheduling.hpp`) already runs GPU terrain page generation as a
fenced, budgeted stage machine - `TerrainGPUJobStage` from `WaitingForAtlas`
through `Validate` to `Complete`, under `TerrainGPUV2Budget` microsecond
targets, `TerrainGPUV2PriorityClass` (`VisibleMissing`, `VisibleRefinement`,
`NearFrustumPrefetch`, `Background`), fenced scratch/output arenas, a parameter
ring, starvation ageing and a stage cost model.

The AS build inserts as new stages between `Validate` and `ReadyToPublish`:

```text
Validate
  -> EmitRTPrimitives      GPU writes group-local triangle/index slots and
                           per-primitive metadata, stamps unused slots degenerate
  -> BuildRTAcceleration   CPU encodes one build per leaf hierarchy node on the
                           exported MTLCommandQueue, each with its own scratch
                           slice
  -> RTAccelerationFenced  Metal command buffer completion handler; only then is
                           the node's handle written into the per-node table
ReadyToPublish
```

`TerrainGPUV2StageCostModel` then budgets AS builds with the same machinery as
every other stage, seeded from the measured 1.9 ms/page. Scratch comes from
`TerrainGPUV2FencedArena` as per-build slices - never one shared buffer, per the
20x trap above.

`SparseVoxelTree64PageScheduler` (`SparseVoxelTree64Streaming.hpp`) is the
second reusable shape, for CPU-side and non-V2 paths: bounded workers, a
`BuildFunction` with a cancellation token, coalescing by `logicalPageId`,
supersession by generation and geometry revision, pending-byte caps, and the
non-blocking handoff `request -> takeGenerated -> finishUpload ->
takePublication -> finishPublication` where nothing becomes visible until
`finishPublication`. Its `SparseVoxelTree64PageJobState` gains an
`AccelerationBuilding` state; nothing else about it changes.

Retirement needs new care, because a Metal acceleration structure has **three**
references on two different timelines:

```text
retire a group's BLAS only after all of:
  raster references drained        (existing Vulkan frame fence)
  compute-trace references drained (existing Vulkan frame fence)
  Metal command buffer completed   (MTLCommandBuffer completion handler on the
                                    last RT encoder that traced it)
```

The third is new. The existing `Tree64 pool retire never reclaimed` failure -
a slot never returned because one lane forgot to signal - is the exact hazard,
so the heap pool reclaim path needs the same "did anything actually come back"
telemetry that pool eventually grew.

#### Measured reference

##### Refit is not applicable to this workload

Refit updates the bounds of an existing acceleration structure while preserving
its topology and primitive count. That fits a skinned mesh or a moving rigid
body. It does not fit this system at all: every page publication pushes
**different clusters** - different triangle counts, different topology, often a
different LOD entirely. There is nothing for refit to preserve.

So the rule for the whole milestone is: **every page upload is a full
acceleration structure build.** Refit is measured below only to establish that
it is not the escape hatch, and it should not appear in any budget or schedule.
The cost model is rebuild cost, and the streaming budget has to absorb it
directly.

(An earlier draft of this section proposed refit as a cheap path for content
changes. That was wrong for this workload and the numbers it leaned on are
superseded by the corrected tables below.)

##### Measured: acceleration structure memory

Sizes from `accelerationStructureSizesWithDescriptor:`, default usage, no
per-primitive data:

| primitives | bbox AS | bbox build scratch | triangle AS | triangle build scratch |
| ---: | ---: | ---: | ---: | ---: |
| 256 | 22,208 | 10,816 | 27,520 | 15,424 |
| 1,024 | 87,104 | 42,432 | 108,736 | 60,864 |
| 4,096 | 347,008 | 177,408 | 433,216 | 250,816 |
| 16,384 | 1,386,304 | 708,736 | 1,730,752 | 1,002,112 |
| 65,536 | 5,912,384 | 2,884,224 | 7,177,856 | 4,057,792 |
| 262,144 | 23,507,008 | 11,434,560 | 28,554,880 | 16,128,640 |

Linear and predictable: **~85 bytes per bounding box, ~109 bytes per triangle**,
plus ~44 B/box and ~62 B/triangle of transient build scratch.

Per-primitive data is *copied into* the acceleration structure. A 4,096-box AS
grew from 347,008 to 412,544 bytes when a 16-byte `primitiveDataBuffer` element
was attached: exactly `4096 * 16`. So the `{selfError, parentError, pageId,
nodeIndex}` payload the cut test needs is an AS memory line item, not free
metadata.

Indirect instance (TLAS) memory: 221,952 bytes for 1,024 instances,
1,772,992 for 8,192, 14,536,896 for 65,536 - about 217 B/instance.
`sizeof(MTLIndirectAccelerationStructureInstanceDescriptor)` is 72 bytes;
`MTLAxisAlignedBoundingBox` is 24 bytes.

##### Measured: BLAS granularity, and the scratch-buffer trap

This is the measurement that decides the architecture, and it has a trap in it
that cost real time to find.

**Builds encoded into one command buffer against a single shared scratch buffer
serialise completely.** Metal inserts the dependency the aliasing implies. Give
each build its own scratch slice and they overlap:

| granularity | BLAS | shared scratch | per-BLAS scratch | speedup |
| --- | ---: | ---: | ---: | ---: |
| 128 tri | 448 | 91.80 ms | 4.52 ms | 20.3x |
| 896 tri | 64 | 12.98 ms | 1.68 ms | 7.8x |
| 3,584 tri | 16 | 7.29 ms | 1.77 ms | 4.1x |

Any AS build budget measured with a shared scratch buffer is wrong by up to 20x.
Scratch has to be a per-build arena slice - which `TerrainGPUV2FencedArena`
already is.

With that fixed, the granularity question against this repo's real page shape
(`ClusterSize = 128`, `GroupSize = 7`, `MAX_GROUPS_PER_PAGE = 64`, so
448 clusters and 57,344 triangles in a full VG page):

| granularity | tri/BLAS | BLAS/page | AS per page | scratch per page | encode | GPU per page |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| per cluster | 128 | 448 | 6.02 MB | 3.39 MB | 1.971 ms | **6.86 ms** |
| per group | 896 | 64 | 5.81 MB | 3.27 MB | 0.303 ms | **1.89 ms** |
| per 4 groups | 3,584 | 16 | 5.79 MB | 3.35 MB | 0.051 ms | **1.84 ms** |
| per page | 57,344 | 1 | 5.99 MB | 3.39 MB | 0.797 ms | **1.86 ms** |

Two things fall straight out:

- **Per-cluster BLASes are 3.7x more expensive and buy nothing.** There is a
  large fixed cost per build (~0.15 ms encode+dispatch overhead each) and 448
  of them per page swamps the actual triangle work. Cluster granularity is off
  the table.
- **From group granularity to whole-page granularity the cost is flat**, around
  1.85-1.9 ms per full page. Group granularity is therefore free relative to
  one monolithic page BLAS, and it is the only one of the two that can express
  a cut.

**Provisional decision at this point: one BLAS per group.** A group is already
the unit that shares a projected-error decision in this codebase -
`SparseVoxelBrickNodeGPU`'s comment is explicit that groups exist "so LOD
transitions can happen as stable patches inside a chunk rather than as unrelated
per-brick flips along chunk edges" - and it costs the same as the coarsest
option. 64 builds per page, ~91 KB of AS each.

**This was superseded.** Build cost is the wrong sole criterion: a coarse
BLAS forces a *rebuild* every time the cut moves inside it. With two-level
instancing, per-cluster BLASes plus per-group instance acceleration structures
beat this on both rebuild cost and trace time. See "One BLAS per cluster, and
two-level instancing".

Corrected bounding-box build cost with per-BLAS scratch: 256 BLASes of 4,096
boxes take 9.76 ms (was 75.23 ms shared), 1,024 take 38.91 ms - about
**0.038 ms per 4,096-box BLAS**.

##### Measured: TLAS rebuild is not free, and that changes the design

An earlier draft of this section reported indirect TLAS builds as
"essentially free - 0.822 ms for 32,768 instances". **That measurement was
wrong.** Its instance descriptor buffer was allocated `StorageModePrivate` and
never written, so the builder was handed 32,768 zeroed descriptors with null
acceleration structure ids and did almost no work.

With the descriptor buffer actually populated and instances spatially
separated:

| instances | TLAS build | TLAS bytes | descriptor bytes |
| ---: | ---: | ---: | ---: |
| 1,024 | 0.462 ms | 0.21 MB | 0.07 MB |
| 4,096 | 0.803 ms | 0.85 MB | 0.28 MB |
| 16,384 | 3.271 ms | 3.38 MB | 1.12 MB |
| 65,536 | 4.597 ms | 13.86 MB | 4.50 MB |
| 262,144 | 16.083 ms | 55.35 MB | 18.00 MB |

Piling every instance on the same spot instead of separating them changes this
by under 15%, so the degeneracy in the bad measurement was the empty
descriptors, not the layout.

This matters because "express the LOD cut as TLAS membership" is the whole
point of a fine BLAS granularity, and at cluster granularity a real terrain view
is tens of thousands of instances. A flat 16,384-instance rebuild at 3.27 ms per
frame is not a free cut; it is a fifth of a 16 ms budget.

##### MSL: use `intersection_query`, not intersection function tables

The refinement view needs no `MTLLinkedFunctions`, no visible function table, no
`MTLIntersectionFunctionTable`, and no pipeline built from
`newComputePipelineStateWithDescriptor:`.
`metal::raytracing::intersection_query` handles bounding-box candidates inline
in an ordinary compute kernel:

```metal
intersection_params ip;
ip.assume_geometry_type(geometry_type::bounding_box);
ip.force_opacity(forced_opacity::non_opaque);

intersection_query<instancing> q;
q.reset(r, accel, bandMask, ip);
while (q.next()) {
    if (q.get_candidate_intersection_type() == intersection_type::bounding_box) {
        const device VGNodeCut *n =
            (const device VGNodeCut *)q.get_candidate_primitive_data();
        const float3 o = q.get_candidate_ray_origin();
        const float3 d = q.get_candidate_ray_direction();
        // ray-cone threshold at this candidate, then the same two tests the
        // raster cut uses, which select exactly one node per root-to-leaf path
        // because the error metric is monotonic
        if (n->selfError < threshold && n->parentError >= threshold) {
            // slab test for t, then:
            q.commit_bounding_box_intersection(t);
        }
    }
}
```

This means the RT kernels compile with the same
`xcrun -sdk macosx metal -c` + `metallib` custom command already used for
`sparse_voxel_terrain_compute_raster_atomic64.metal` in
`src/runtime/CMakeLists.txt`. No new toolchain, no pipeline linking step.

Two API details worth recording, because both cost time to find:

- there is no `get_candidate_bounding_box_distance()`. For bounding-box
  candidates the shader computes `t` itself from `get_candidate_ray_origin()` /
  `get_candidate_ray_direction()` and passes it to
  `commit_bounding_box_intersection(float)`.
- `intersection_query` accepts only `instancing`, `triangle_data`,
  `curve_data`, `world_space_data`, `max_levels<>`. `primitive_motion` is an
  `intersector<>` tag and fails a static assert on `intersection_query`.

##### GPU-authored geometry without an indirect primitive count

Metal has an indirect *instance* acceleration structure descriptor but **no**
indirect primitive count for a BLAS: `boundingBoxCount` / `triangleCount` are
CPU-side properties. That looks like a blocker for GPU-generated pages.

It is not. A probe built a 64-box BLAS where only 4 boxes were live and the
other 60 were written `min > max` on every axis. Traversal never produced a
single candidate for a degenerate box - `candidatesVisited` was exactly the
number of live boxes along the ray, never 64 - and the live hits, their
primitive ids and their per-primitive metadata all came back correct.

So: allocate group slots at **fixed capacity** (896 triangles, the group's
maximum), let the GPU write however many primitives it actually produced and
stamp the remainder degenerate, and build at full capacity. The count never
crosses to the CPU. The *geometry* is fully GPU-authored; the *build* is
CPU-encoded and GPU-executed. There is no way to encode a build from inside a
shader on any backend.

The cost is wasted AS memory proportional to slot slack, which is why group
capacity should come from measured cluster-occupancy distributions rather than
the 7x128 worst case.

##### Metal 3 vs Metal 4, and interop

Metal 4 (`MTL4CommandQueue`, `MTL4CommandBuffer`, `MTL4ComputeCommandEncoder`,
which absorbs `MTLAccelerationStructureCommandEncoder`) is a **separate stack**
from `MTLCommandQueue`. `VK_EXT_metal_objects` exports a legacy
`MTLCommandQueue` from MoltenVK, and same-queue commit ordering is the entire
synchronization story this repo relies on today. **The MetalRT backend uses the
Metal 3 API surface**, like `TerrainMetalComputeRasterPass` and `MetalFXPass`
already do.

Those two files are the working precedent. Both pull the device and queue with
`rhi.exportMetalDevice()` / `rhi.exportMetalCommandQueue(Queue::Graphics)`
(`VulkanRHI.cpp:4598`, `:4615`), pull buffers with `rhi.exportMetalBuffer`
(requires `BufferInfo::metalExportable` at creation, `VulkanRHI.cpp:1638`), and
allocate their own `MTLCommandBuffer` on the exported queue with no explicit
fence. **Ordering comes from committing to the same `MTLCommandQueue` MoltenVK
submits on** - Metal command buffers on one queue execute in commit order. That
is why `MetalFXPass` runs from the sample's main loop after render-graph
execution rather than inside a graph pass, and the RT backend inherits the same
constraint.

What crosses the boundary as `metalExportable` Vulkan buffers:

```text
in : scene ray buffer                (SparseVoxelSceneRayGPU)
in : group triangle/index slots      (GPU-written, fixed capacity)
in : DAG node cut metadata           (selfError, parentError, pageId, nodeIndex)
in : TLAS instance descriptors + instance count (GPU-written by the cut pass)
out: scene ray hits                  (SparseVoxelSceneRayHitGPU)
out: trace page requests             (existing GPU feedback buffer)
```

What never crosses, because Vulkan never needs to see it:

```text
MTLHeap holding the BLAS pool
MTLAccelerationStructure objects (group BLASes, TLAS)
build scratch arenas
MTLComputePipelineState, MTLLibrary
```

Acceleration structures have no Vulkan representation here at all, which removes
the hardest part of the interop worry.

The output side keeps the rest of the engine backend-agnostic: the RT kernels
write the **existing** `SparseVoxelSceneRayHitGPU` ABI
(`SparseVoxelGPU.hpp:954`) so the merge, ReSTIR, probe and light-field passes
are unchanged. `SparseVoxelSceneRayBackend` gains `NativeRT = 3u` beside
`Screen` and `Tree64`. `SparseVoxelSceneRayStatus` already carries
`UnknownResidency` and `QueryLimit`, which are exactly the two non-answers the
fallback policy needs.

##### Compute fallback and parity

The compute fallback already exists and needs wiring, not writing.
`voxelTraceCubic64` / `voxelTraceBounds8` / `voxelTraceBounds16` in
`SparseVoxelVirtualTrace.hpp` are a complete CPU reference traversal with the
same error sidecar, coarse-fallback and generation-rejection policy, and
`sparse_voxel_virtual_trace_common.wgsl` is its GPU twin.
`VoxelTraceRuntimeBackend` already selects `Tree64` vs `DirectVirtual`; it gains
`NativeRT`.

That gives a three-way oracle per parity fixture - CPU reference, WGSL compute,
MetalRT - which is the only reasonable way to trust a hardware traversal whose
internals are opaque. `VoxelTraceHitCPU` already reports `nodeVisits`,
`childTests`, `groupTests` and `skippedChildGroups`, so divergence can be
localised rather than just detected.

One asymmetry must be designed for rather than tested away: the triangle view
and the voxel view are *different geometry*. A cluster is the exposed surface of
a voxel set, so a triangle hit and a Tree64 hit agree on the surface but not in
the interior. The parity contract is "same first surface within the group's
published geometric error", not "same t". Interior rays, rays starting inside a
voxel, and rays grazing a cluster seam are the cases to enumerate first.

#### Implementation phases

Ordered so the two largest risks - interop and incoherent-ray throughput - are
retired before any structural work is built on them.

```text
M2.0  Land the probe harness.
      ignore/metal-rt-research/ becomes a test target that skips cleanly
      off-Apple. It is the regression baseline for every number in this
      milestone, including the two that were measured wrong the first time
      (shared build scratch, unwritten instance descriptors).

M2.1  Interop smoke test. THE GATING STEP.
      Build one acceleration structure whose geometry buffer is a
      MoltenVK-exported MTLBuffer, and trace it on the exported queue while the
      render graph is submitting. Measure the build cost with the Vulkan queue
      busy, not idle. If this does not work, the milestone stops here.

M2.2  Terrain.RTBackendCapabilities + TerrainMetalRTCapabilities.
      Selects none / MetalRT / VulkanRT / DirectXRT and fails closed with a
      reason string, mirroring the existing SparseVoxelTerrainComputeRaster
      capabilities + route + buildPassPlan trio.

M2.3  Memory substitution budget.
      Arithmetic, not research: total group-BLAS bytes for a realistic resident
      set against what the Tree64 trace payloads occupy today. The RT structures
      must replace those payloads, not accompany them. Occupancy bits and
      triangles are not the same shape; this has to be summed before the pool is
      sized.

M2.4  Heap-backed BLAS pool at group granularity.
      MTLHeapTypePlacement (automatic heaps cannot place an acceleration
      structure), slots aligned to heapAccelerationStructureSizeAndAlign
      (16 KB), geometry from a shared arena by offset, per-build scratch slices
      from the fenced arena. Never one shared scratch offset. Slot reuse after
      uninstall is ours to police - a placement heap tracks no lifetime.

M2.5  Static single page, no streaming.
      Group BLASes from a published cluster payload, one cut AS per LOD level,
      one TLAS instance. Trace and compare against voxelTraceCubic64. First real
      parity number.

M2.6  Incoherent ray throughput. THE SECOND GATING STEP.
      Re-measure with the engine's own ReSTIR / probe / shadow ray
      distributions. Every throughput figure in this milestone is a coherent
      primary fan and is an upper bound. If MetalRT does not beat the streamed
      trace kernels here, the rest is not worth building.

M2.7  GPU-driven cut.
      Compute pass evaluates the two tests over the resident hierarchy, clamps
      to the installed frontier with sibling-complete descent, and writes
      instance descriptors plus instanceCountBuffer directly. No CPU readback.

M2.8  Streaming integration.
      EmitRTPrimitives / BuildRTAcceleration / RTAccelerationFenced stages in
      TerrainGPUV2Scheduler; the per-node handle table published from build
      completion; three-way retirement; ancestor-closed and sibling-complete
      residency enforced at publish time.

M2.9  Quality bands and the refinement view.
      Optional per-ray-class bands via the instance mask. Bounding-box
      refinement structure for coarse, offscreen and unresident geometry, with
      trace-driven page requests through the existing feedback buffer.

M2.10 Renderer integration behind the route decision, then benchmark against
      the streamed Tree64 / virtual compute trace path on the matched-pose
      harness. Only then judge.
```

Later, VulkanRT and DirectXRT implement the same `TerrainNativeRTBackend`
interface - same page ids, group ids, error metadata and dependency edges,
backend-specific allocation/build/retire, identical parity fixtures.

```text
TerrainNativeRTBackend
  capabilities
  import/export or native allocation hooks
  build a group acceleration structure when its page installs
  publish its handle into the per-node table on build completion
  rebuild the per-level cut structures whose membership changed
  publish the scene structure over resident pages
  trace screen, shadow, and off-screen rays
  emit page requests
  retire structures after all three reference classes drain
```
#### Milestone 2 tests

Capability and interop:

- the capability probe selects the expected backend or disables native RT
  cleanly, and a Vulkan-only renderer is unchanged when Metal RT is absent;
- an acceleration structure builds and traces correctly from a
  MoltenVK-exported buffer, and exported buffers preserve page ids and
  revisions across the synchronization boundary;
- acceleration-structure builds use per-build scratch slices; a shared scratch
  buffer is caught, because it is a silent up-to-20x serialisation.

Cut correctness - the invariants the design rests on:

- exactly one node per root-to-leaf path satisfies
  `selfError < t && parentError >= t`, over a range of thresholds, on a real
  hierarchy: the monotonicity guarantee the whole scheme depends on;
- every ray sees exactly one LOD level: candidates-per-hit is 1.0 and no ray
  encounters two levels, on a fixture whose cut spans several levels;
- the trace kernel and the raster cut, given the same camera and hierarchy,
  select the same groups;
- all per-level cut structures and the scene TLAS publish from a single cut
  evaluation; a mixed-generation publish is rejected, not rendered.

Streaming and residency:

- the cut descends only into nodes whose whole sibling group has an installed
  BLAS, and coverage is byte-identical to a fully installed scene at 25%
  residency; a regression shows up as lost rays, not as visible overlap;
- the BLAS residency set is ancestor-closed and sibling-complete, and a set
  violating either is rejected at publish time;
- a node's handle is published only from its build's completion handler, so no
  cut can reference a structure still being built;
- a group BLAS contains exactly the clusters `flags[23:16]` marks enabled, and
  a cluster in `flags[15:8]` (streaming leaf) is always admissible;
- a cut change rebuilds only the affected per-level pointer lists, never a BLAS;
- GPU page requests emitted by the RT path are consumed by the existing
  scheduler and publish in a later committed generation;
- retirement waits for raster, trace and Metal command-buffer references, and
  the heap pool reclaim counter returns to zero.

Parity and structure:

- Metal triangle hits match Tree64 hits for fully installed fixture pages,
  within the group's published geometric error - the contract is "same first
  surface within error", not "same t";
- one BLAS per leaf hierarchy node and one intermediate structure per LOD-level
  root: a mismatch against `VirtualGeometryHierarchyBuilder`'s own levels is a
  build-time assertion, not a rendering artifact;
- a masked-out quality band contributes zero candidates;
- each quality band is a complete cut: tracing any single band alone yields
  full coverage;
- shared parity fixtures run against compute trace now, and against MetalRT,
  VulkanRT and DirectXRT when those exist.

Refinement view (M2.9 only):

- the per-candidate two-test cut matches `voxelTraceCullExpansionDecisionQ8`
  on shared fixtures;
- an accepted node commits at its representative geometry's distance, never at
  its bounding-box entry;
- committing on an ancestor prunes its descendants: a candidate counter shows
  subtree collapse;
- the coarsest resident node of every subtree publishes `parentError = +inf`,
  so a ray whose desired level is missing still commits instead of traversing
  to tMax - the guard against the measured 3x no-admissible-level cliff.
#### Open questions, unmeasured

None of these blocks M2.0-M2.2. Each is tied to the phase that answers it.

- **Cost of committing at real proxy geometry rather than at box entry.** The
  175 Mray/s custom-AABB figure committed at a slab-test distance. A correct
  implementation intersects the accepted node's representative geometry first,
  and that cost is unmeasured. It is the single biggest unknown left in the
  refinement view (M2.9).
- **How deep the flat all-levels encoding should go.** Committing prunes
  everything below the accepted node, but rejections on the way down do not, so
  a chain of rejected ancestors is pure cost - measured at 13 candidates versus
  5 for a no-LOD leaf-only baseline when the cut reaches the bottom. There is an
  optimal depth beyond which residency should take over from per-candidate
  selection, and it has not been measured.
- **Does rebuilding a group instance AS force a scene TLAS rebuild?** The group
  AS keeps its `gpuResourceID` across an in-place rebuild, but the scene TLAS
  caches its bounds from build time. An LOD change inside a group barely moves
  its bounds, so the parent rebuild should be rare - but this is the difference
  between 0.230 ms and 0.553 ms per cut change and it is unverified.
- **Two-level instancing was measured with every group sharing one group AS and
  every cluster sharing one BLAS.** The structural results (multi-level works,
  partial rebuild is cheap, trace is faster than flat) hold, but build and
  memory figures for thousands of *distinct* cluster BLASes and group ASes are
  extrapolated, not measured.
- **How many pages change cut per frame, and at how many LOD levels each.** A
  per-level rebuild costs 0.2-0.33 ms and has a large fixed component, so
  rebuilding two or more levels is worse than rebuilding the cut flat. The
  policy needs the measured distribution from the dolly harness, not a guess.
- **Whether the 1.47x split win survives distinct geometry.** The flat-versus-
  split comparison shared one group BLAS across every instance. The hit counts
  matched exactly so the structural result is sound, but per-level BVH quality
  with genuinely different group geometry is extrapolated.
- **Total BLAS memory for every resident DAG node, not just the selected cut.**
  The three-level structure stores every node's geometry once and shares it
  across cuts, so residency - not the cut - sets memory. For a 4-ary DAG the
  levels above the leaf add about 33%. This has not been computed against the
  existing page pool budgets and it is the binding constraint.
- **Whether a cut-AS rebuild forces a scene TLAS rebuild.** The cut AS keeps its
  `gpuResourceID` across an in-place rebuild, but the scene TLAS caches its
  bounds. A cut change barely moves a page's bounds, so this should be rare -
  unverified, and it is the difference between 0.233 ms and ~0.7 ms per change.
- **How many LOD levels the near-field all-LOD encoding should span.** The
  traversal penalty is sublinear (2.0x for 5.25x triangles) but the AS memory
  penalty is linear at ~109 B/triangle. Memory bounds this, not speed.
- **Incoherent ray throughput.** Every trace number above is a coherent primary
  fan; the engine's real load is ReSTIR, probe and shadow rays. Re-measure with
  the engine's own ray distributions before trusting any speedup claim.
- **Concurrent AS build and Vulkan graph work on one queue.** All builds were
  measured on an idle queue. Serialising a 1.9 ms page build behind the frame's
  Vulkan submission may cost more than 1.9 ms of wall clock. The
  `Pass timeline mis-attribution` result is the warning here.
- **Whether an exported MoltenVK `MTLBuffer` is legal as AS geometry input.**
  It is a plain `MTLBuffer` so it should be, but storage mode and offset
  alignment are unverified against MoltenVK's allocations specifically.
- **Total AS memory at realistic residency.** ~5.9 MB per *full* page is a
  capacity figure; real terrain pages are far under capacity, but the resident
  page count times actual occupancy has not been computed against the existing
  page pool budgets.
- **`useHeap:` residency cost** at thousands of resident BLASes, and whether
  MoltenVK's own residency management interferes.

#### Decision gate

```text
Ship/continue a native RT backend only if:
  M2.1  interop works: an acceleration structure builds from a MoltenVK-exported
        buffer and traces on the shared queue while the graph is submitting,
        and the build cost with the queue busy is acceptable
  M2.3  the RT structures fit by REPLACING the Tree64 trace payloads, summed,
        not assumed
  M2.6  trace beats the streamed trace kernels measured with the engine's own
        incoherent ray distributions, not a coherent primary fan
  M2.8  streaming converges: no holes, no stale generations, and the residency
        set stays ancestor-closed and sibling-complete under camera motion
  the trace cut and the raster cut provably select the same geometry
  hit/material/error parity matches Tree64 on installed geometry
else:
  keep that native RT backend experimental and continue optimizing streamed
  Tree64 / virtual compute trace kernels.
```
#### Appendix: designs considered and rejected

These are kept because each carries measurements that eliminated it, and
because the reasoning is the argument for the adopted design. None of them
describes what should be built.

```text
per-group BLAS, cut = which BLAS is built
    rebuilds geometry on every cut change, 1.89 ms/page          REJECTED

per-cluster BLAS + two-level instancing, cut = instance membership
    instance count scales with selected clusters; TLAS rebuild
    3.27 ms at 16k instances, 16.08 ms at 262k                   REJECTED at scale
    still correct for a small exact cut (a hero asset)

per-page BLAS carrying a 2-3 level LOD window, cut evaluated per candidate
    no rebuilds, but non-opaque triangles and a 2.0x traversal
    penalty plus a 3x no-admissible-level cliff                  REJECTED
    the mechanism survives in the refinement view
```
##### One BLAS per cluster, and two-level instancing

*Rejected. Kept for the measurements that eliminated it.*

Fine BLAS granularity buys one specific thing: **the cut becomes membership
rather than geometry.** If every cluster (or every DAG node) owns its own BLAS,
built once when its page streams in, then moving the cut never rebuilds a BLAS -
it only changes which instances are published. That is exactly the property
NVIDIA's Mega Geometry gets from CLAS, and Metal has no CLAS, so emulating it
with real per-cluster BLASes is the natural substitute.

The cost of that, measured on identical world geometry (a 32x32x16 grid of
16,384 cells, 128 triangles each, 2.1M triangles total; only the *partition*
into BLASes changes, instance transforms place each block at its own origin so
the world-space triangle set is identical in every row):

| partition | instances | BLAS memory | TLAS build | trace nearest | any-hit |
| --- | ---: | ---: | ---: | ---: | ---: |
| per cluster (128 tri) | 16,384 | 256.0 MB | 3.006 ms | 1.798 ms | 1.767 ms |
| per 2x2x2 cells (1,024 tri) | 2,048 | 224.0 MB | 0.205 ms | 1.269 ms | 1.219 ms |
| per 4x4x4 cells (8,192 tri) | 256 | 212.0 MB | 0.107 ms | 1.060 ms | 1.014 ms |

Hit counts agree within 1.4% across all three, so this is a fair comparison.
Per-cluster granularity costs **1.70x trace, 21% memory, and a 28x more
expensive TLAS rebuild** than the coarsest partition. The memory overhead is
per-BLAS fixed cost plus the 16 KB heap placement alignment against a
14,080-byte cluster AS.

The trace and memory penalties are tolerable. The 3.0 ms flat TLAS rebuild is
not - and it is avoidable.

##### Two-level instancing removes the rebuild cost

Metal supports instance acceleration structures that reference **other instance
acceleration structures** (multi-level instancing, WWDC23), traced with
`intersector<instancing, max_levels<N>>`. A probe confirmed this works and
measured the structure that matters:

```text
scene TLAS  (256 group instances)
  -> group instance AS  (64 cluster BLASes each)
      -> cluster BLAS   (128 triangles)
= 16,384 clusters reachable
```

| | size | rebuild |
| --- | ---: | ---: |
| group instance AS (64 cluster BLASes) | 14 KB | **0.230 ms** |
| scene TLAS (256 group instances) | 54 KB | **0.323 ms** |
| flat 16,384-instance TLAS, for comparison | 3.38 MB | 3.271 ms |

Trace at 1920x1080: **1.538 ms, 1,348 Mray/s**, with `instance_count == 2`
confirming both levels were traversed. That is *faster* than the flat
16,384-instance TLAS over the same geometry (1.798 ms), because the scene-level
structure has 256 well-separated instances instead of 16,384.

So two-level instancing is better on both axes at once:

- **a cut change inside one group rebuilds 14 KB in 0.230 ms**, instead of
  3.38 MB in 3.271 ms - a 14x reduction, and it scales with what actually
  changed rather than with scene size;
- **tracing is 1.17x faster** than the flat equivalent.

This is the answer to the granularity tension. The earlier conclusion in this
plan - "one BLAS per group, because per-cluster builds cost 3.7x" - weighed
build cost alone and ignored that a coarse BLAS forces a *rebuild* every time
the cut moves inside it. With two-level instancing the recommendation becomes:

```text
cluster BLAS       one per DAG node, built once when its page streams in
                   never rebuilt while the node stays resident
                   7.17 ms per full page at stream-in, +21% memory

group instance AS  one per group, membership = the two-test cut's selection
                   rebuilt only when that group's cut changes: 0.230 ms

scene TLAS         one per frame over resident groups: 0.323 ms at 256 groups
```

**This is in turn rejected for streamed terrain**: instance membership scales with selected cluster count, and a terrain
trace cut including offscreen occluders is far larger than the ~16k instances
where the TLAS curve is still affordable. See "The LOD window".

The remaining question is whether a group instance AS rebuild also forces a
scene TLAS rebuild. Rebuilding a group AS in place preserves its
`gpuResourceID`, but the scene TLAS caches the group's bounds from build time,
so a group whose bounds change needs its parent rebuilt too. LOD changes inside
a group barely move its bounds, so the parent rebuild should be rare - but this
is unverified and it is the difference between 0.230 ms and 0.553 ms per cut
change.

##### So: can traversal be stopped on error and parent error?

Directly, no. Metal has no per-instance shader predicate - there is no
callback that runs when traversal reaches an instance and can reject it. The
mechanisms that exist are, in order of strength:

```text
1. membership   the node is not in the group instance AS at all
                the cut decides this, once per frame, in the existing
                culling pass - not per ray
2. commit       accepting a coarse node shortens the ray interval and the
                hardware discards everything behind it
                measured 585 primitives -> 1 candidate on nested volumes
3. instance     8 hardware-enforced mask bands, free per ray
   mask         measured 2.24 ms -> 1.46 ms at 4 of 8 bands, non-monotonic
4. reject       filters the answer, prunes nothing
```

But with per-cluster BLASes and two-level instancing, **the question mostly
dissolves.** The cut is expressed as membership in the group instance AS, and
membership is resolved once per frame by the culling pass that already computes
`selfError` and `parentError` - not per ray, not inside an intersection
function. Rays then trace opaque triangles at full hardware speed, with no
intersection shader at all.

That is strictly better than evaluating the cut per ray, which was measured at a
2.0x traversal penalty plus a 3x cliff when no level is admissible. The per-ray
cut keeps a narrower job: the bounding-box refinement view, for coarse and
offscreen geometry and for regions whose exact view is not resident.

Trace throughput, 1920x1080 = 2.07 Mrays, TLAS of 256 page instances,
~1M primitives, best of five:

| path | GPU | throughput |
| --- | ---: | ---: |
| hardware triangle intersector, nearest hit | 2.24 ms | 924 Mray/s |
| custom AABB `intersection_query`, nearest hit | 11.87 ms | 175 Mray/s |
| custom AABB + projected-error cull | 11.44 ms | 181 Mray/s |
| hardware triangle, `accept_any_intersection` | 0.425 ms | 4,880 Mray/s |
| custom AABB + error cull, `accept_any_intersection` | 1.636 ms | 1,267 Mray/s |

Caveat that must not be dropped when quoting these: a coherent primary ray fan
over a small fully-resident synthetic scene. Incoherent GI and reflection rays
over real streamed terrain will be materially slower. The *ratios* are the
durable result; the absolute rates are an upper bound.

Allocation: `newAccelerationStructureWithSize:` cost 2.59-6.97 ms for 1,024
objects; placement-allocating the same 1,024 from an `MTLHeap` took 0.85 ms,
and heap-backed structures build and trace correctly. Heap placement alignment
is 16 KB per AS - which is another argument against per-cluster BLASes, since a
128-triangle AS is 14,080 bytes and wastes the rest of its 16 KB slot.
`useHeap:` once also replaces `useResource:` per resident BLAS per frame.

##### The LOD window: keep the cut inside the BLAS, rebuild nothing

**Rejected for the exact triangle view.** The window is still the right model
for the bounding-box refinement view, where geometry is coarse and cheap.
For the exact triangle view, one intermediate acceleration structure per DAG cut
over shared cluster BLASes is faster (1,575 vs 643 Mray/s), avoids duplicating
geometry, and makes a cut change a 0.233 ms pointer-list rebuild.

Expressing the cut as instance membership does not scale to millions of
clusters. Even with two-level instancing the instance count tracks the *selected
cluster count*, and the measured TLAS curve is unforgiving above ~16k instances:
3.27 ms at 16,384, 4.60 ms at 65,536, 16.08 ms at 262,144. A terrain trace cut
that includes offscreen occluders for shadows and GI is far larger than a
screen's worth of clusters. Rebuilding that every frame is not viable.

The alternative is to put the LOD choice **inside a static BLAS** and never
rebuild it. Each page's BLAS carries a bounded *window* of LOD levels; the ray
picks the level with the two-test cut and commits, and the commit collapses the
rest of the window. The TLAS then holds one instance per resident **page**, not
per cluster - thousands, not millions - and costs 0.46 ms at 1,024 instances.

The question is what a window costs. Measured, 1920x1080, 512 instances, level 0
= 128 triangles and each additional level 4x the triangles at half the error,
with the two-test cut evaluated per candidate and camera-projected error:

| LOD levels in the BLAS | triangles | AS memory | trace | vs one level |
| ---: | ---: | ---: | ---: | ---: |
| 1 (pre-resolved, opaque) | 128 | 0.01 MB | 1.720 ms | 1.00x |
| 2 | 640 | 0.07 MB | 2.268 ms | **1.32x** |
| 3 | 2,688 | 0.28 MB | 3.044 ms | **1.77x** |
| 4 | 10,880 | 1.13 MB | 3.874 ms | 2.25x |

The cost grows far slower than the geometry: **85x the triangles for 2.25x the
trace time.** That is commit-pruning working - each ray only pays for the levels
it rejects on the way down, and the accepted level's commit discards everything
behind it.

A **2-level window at 1.32x** is close to free, and a 3-level window at 1.77x is
affordable. Each level covers a 2x camera-distance range, so:

```text
2-level window  ->  4x distance range covered with no rebuild, 1.32x trace
3-level window  ->  8x distance range covered with no rebuild, 1.77x trace
```

A page only needs a rebuild when it leaves its window entirely, which is a large
camera movement, not a per-frame cut adjustment. That is the property the design
needs: **the cut moves every frame, the acceleration structures do not.**

Memory is the real budget line, and it is the one that grows linearly: a
3-level window is 21x the triangles of a single level, at ~109 bytes of AS per
triangle. Window depth should be chosen from the AS memory budget, not from the
trace curve.

##### How this supersedes the instance-membership designs

Both earlier recommendations in this section are superseded for the terrain
case:

```text
per-group BLAS, cut = which BLAS is built
    rebuild on every cut change            1.89 ms/page      REJECTED

per-cluster BLAS + two-level instancing
    cut = group instance AS membership     0.230 ms/group
    instance count scales with clusters    3.3-16 ms TLAS    REJECTED at scale

per-page BLAS carrying a 2-3 level LOD window
    cut = commit inside the intersection function
    rebuild only when a page leaves its window
    TLAS = one instance per resident page  0.46 ms @ 1k      ADOPTED
```

Two-level instancing is not wasted - it remains the right tool when the cut is
genuinely small and exact (a hero asset, a reflection probe's local set), and
the measurement stands: a 14 KB group AS rebuilds in 0.230 ms and the two-level
trace is faster than the flat equivalent. But for streamed terrain with millions
of resident clusters, the LOD window inside a static BLAS is the design that
holds up.

##### How the LOD window actually works: no subtree is skipped

The natural reading of this design is "the ray walks the DAG and skips subtrees
whose error is too high". That is not what happens, and holding that model makes
the rest of the design look impossible. The correct model is simpler.

**The BLAS is not the DAG.** When several LOD levels of a surface are put into
one acceleration structure, Metal builds a single *spatial* BVH over all their
triangles. That BVH groups triangles by position. It has no knowledge of levels,
parents, children, or error, and it contains no subtree corresponding to a DAG
node. There is no hierarchy in it to skip.

**A ray only ever touches its own neighbourhood.** This is the part that makes
the whole thing work, and it is ordinary BVH behaviour rather than anything
LOD-specific. A ray is a thin line through the scene; the BVH culls everything
whose bounding box it misses, in log time. Adding more LOD levels adds triangles
*in the same place*, so the ray still only encounters the handful that lie on its
path.

Measured, 1920x1080, 512 instances, counting the triangle candidates the shader
is actually shown, averaged over every ray:

| levels in BLAS | triangles | candidates/ray | too coarse | too fine | accepted |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 128 | 0.45 | 0.29 | 0.00 | 0.16 |
| 2 | 640 | 0.39 | 0.15 | 0.02 | 0.22 |
| 3 | 2,688 | 0.53 | 0.16 | 0.15 | 0.22 |
| 4 | 10,880 | 0.65 | 0.16 | 0.26 | 0.22 |

**85x the triangles produces 1.44x the candidates.** Not 85x. The shader is
invoked well under once per ray on average, and the accepted count is flat at
0.22 from two levels on - the cut picks exactly one level, consistently, which
is the monotonicity invariant doing its job.

**So where does the 2.25x trace cost come from?** Not from the candidates -
there are almost none. It comes from BVH depth. 85x more triangles is about 6.4
more levels of tree to descend, on every ray, hit or miss. That is the entire
cost of a wider LOD window, and it is why the cost curve is logarithmic in
geometry rather than linear:

```text
triangles   1x  ->  85x
candidates  1x  -> 1.44x     the ray still touches only its own neighbourhood
trace time  1x  -> 2.25x     BVH is ~6 levels deeper
memory      1x  ->   85x     this is the one that grows linearly
```

**The error test is a filter, not a traversal control.** It runs on the ~0.65
candidates per ray that the BVH already narrowed down to, and picks which of
them is the right level:

```text
too coarse  selfError projects above threshold      -> skip, keep traversing
too fine    parentError already below threshold     -> skip, keep traversing
admissible  selfError < threshold <= parentError    -> commit
```

Nothing is pruned by this test, and nothing needs to be. The expensive work -
discarding the other 99.99% of the geometry - was already done by the BVH on
spatial grounds before the shader was ever called.

##### Where subtree pruning does happen, and why it is a different mechanism

Commit-based pruning is real, but it belongs to the *other* view and it is worth
keeping the two straight, because conflating them is what made this confusing.

```text
bounding-box refinement view    nested volumes: a child box is strictly inside
                                its parent, so t_child >= t_parent
                                committing an ancestor puts every descendant
                                beyond the committed distance and the hardware
                                discards the whole subtree
                                measured: 585 primitives -> 1 candidate

triangle LOD window             coincident surfaces: all levels sit at the same
                                depth, so committing one level does not
                                systematically cull the others
                                pruning here is spatial (the BVH) and by ray
                                interval, not hierarchical
```

Both are cheap, for different reasons. The bounding-box view is cheap because
accepting a coarse node collapses its subtree. The triangle window is cheap
because a ray only intersects a few triangles no matter how many are present.
Only the first is subtree pruning in any real sense.

##### What "avoid traversal on high-error nodes" resolves to

Metal has no per-instance or per-geometry shader predicate that can veto a
subtree, and none is needed. Inside the window, the mechanism is:

```text
candidate arrives (non-opaque triangle or bounding box)
  read {selfError, parentError} from per-primitive data
  project both from the camera - the same function the raster cut uses
  selfError too coarse   -> reject, traversal continues (cheap: coarse levels
                            hold geometrically few primitives)
  admissible             -> commit at the node's real geometry
                            the interval shortens and every finer level
                            behind it is discarded by the hardware
```

What makes it cheap is *not* that rejections are cheap or that the commit prunes
the expensive levels - see "How the LOD window actually works" above. It is that
a ray only ever intersects the few triangles on its own path, so 85x the
geometry yields 1.44x the candidates. The extra cost is BVH depth, which grows
logarithmically. The error test never prunes anything; it chooses among
candidates the BVH already narrowed down.

Two constraints carry over and must not be dropped:

- **the commit must be at real geometry**, never at a bounding-box entry, or
  every hit distance is wrong by up to a box diagonal;
- **the coarsest level in each window must always be admissible**
  (`parentError = +inf` on it), or a ray whose desired level is outside the
  window finds nothing, never commits, and traverses to `tMax` - the 3x cliff
  measured earlier.

##### The hierarchy is the cut, and the ray can evaluate it directly

*Applies to the bounding-box refinement view only. The exact triangle view
evaluates the cut once per frame, not per ray.*

The virtual geometry hierarchy already carries everything the RT backend needs,
in the right form. `SparseVoxelBrickNodeGPU` stores `selfError` and
`parentError` as measured world-space deviation with an explicitly documented
monotonicity guarantee - a parent's error is at least the maximum of its
children's - which is what admits exactly one cut per root-to-leaf path.
`culling_multipass.wgsl` evaluates the two independent tests:

```wgsl
// descend iff the coarser representation is not good enough
shouldVisitChildNodes  : projParentError >= errorThresh
// render iff this node's own geometry is good enough
isClusterCoarseEnough  : projSelfError   <  errorThresh

calculateProjectedError = errorLocal * worldScale / distance
                        * proj[1][1] * 0.5 * viewport.y
```

The RT backend must not reimplement this. It consumes it, and it can consume it
literally: **the trace kernel binds the same camera uniforms the raster cut
uses, so `calculateProjectedError` works verbatim inside an intersection
function.** LOD is then camera-driven for every ray class - primary, shadow,
GI, reflection - which makes "the trace cut and the raster cut select the same
geometry" true by construction rather than by a tuning exercise. There is no
ray-cone metric to invent and no second threshold to keep in sync.

The consequence to accept deliberately: a shadow ray travelling toward geometry
1 km from the camera gets that geometry's *camera* LOD, not a LOD derived from
the ray. For shadows and GI that is usually what you want - the occluder only
needs to be as detailed as the shading it affects. Where it is wrong is a
mirror reflection of something far behind the camera, which will be coarser
than the reflection deserves. That case is a known, bounded limitation, not a
correctness bug, and it can be addressed later by scaling the threshold per ray
class.

##### Rejection does not prune, but commitment does

This is the mechanism question, and it needed an experiment rather than an
argument. An earlier draft of this section claimed hardware traversal cannot be
pruned from an intersection function at all. That is half right and the wrong
half is the important one.

A probe built a fully nested hierarchy in one bounding-box BLAS and counted how
many candidates the shader was actually asked about:

```text
BLAS: 73 primitives, 1 root + 8 children + 64 grandchildren, spatially nested
one ray straight through the structure

                              L0    L1    L2   committed
reject every candidate         1     2     4      none
reject L0 and L1               1     1     1      L2
reject L0 only                 1     1     0      L1
accept the first candidate     1     0     0      L0
```

Read the first row and the last row together:

- **Rejecting a parent does not prune its children.** With every candidate
  rejected, the shader still saw both overlapping L1 boxes and all four
  overlapping L2 boxes. An intersection function cannot tell the traversal unit
  to skip a subtree.
- **Committing an intersection prunes the subtree completely.** Accepting the
  root collapsed 73 primitives to a single candidate.

The reason is geometric, not API trivia. `commit_bounding_box_intersection(t)`
shortens the ray's search interval to `t`, and the hardware then culls
everything farther away. A child box is spatially *inside* its parent, so the
ray always enters the child at `t_child >= t_parent`. Committing an ancestor
therefore places every descendant beyond the committed distance, and the
hardware discards the entire subtree for free.

So the answer to "can a custom intersection function fetch the LOD error and
reject the subtree" is: **yes, and the way to express it is to accept the
coarse node rather than to reject the fine ones.** "This node is good enough"
is a commit, and the commit is what prunes.

##### Measured: the two-test cut driving hardware traversal

A second probe put a 4-level octree - 1 + 8 + 64 + 512 = 585 nodes, every level
present as a primitive in one BLAS, `selfError` 16/8/4/2 per level and
`parentError` set to the parent's `selfError` - behind an `intersection_query`
loop that evaluates the real two-test cut with camera-projected error:

```metal
const float  dist       = max(distance(cameraPos, nodeCentre), 1e-3f);
const float  projSelf   = m->selfError   / dist * pixelScale;
const float  projParent = m->parentError / dist * pixelScale;
if (projSelf < threshold && projParent >= threshold)
    q.commit_bounding_box_intersection(t);
```

One ray, sweeping the camera distance:

| camera distance | L0 | L1 | L2 | L3 | total candidates | committed |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| 20,000 m | 1 | 0 | 0 | 0 | **1** | L0 |
| 4,000 m | 1 | 1 | 2 | 0 | **4** | L2 |
| 1,000 m | 1 | 2 | 4 | 6 | **13** | L3 |
| 300 m | 1 | 2 | 4 | 8 | 15 | none |

This is the behaviour the design wants, and the hardware delivers it:

- distant geometry costs **one candidate out of 585**;
- cost scales with how deep the cut has to go, not with tree size;
- the depth is chosen per ray, per node, from the hierarchy's own error values;
- at 300 m no level satisfies both tests, because even the leaf error projects
  above threshold. The cut correctly reports *nothing admissible* rather than a
  wrong answer - which is exactly the `UnknownResidency` case, and the trigger
  for a page request for a finer page.

One number is worth reading carefully. A no-LOD baseline that always descends
to the leaf level saw 5 candidates at 1,000 m, while the two-test cut saw 13.
Rejections do not shorten the interval, so descending through a rejected
ancestor chain costs more than going straight to a leaf. The cut wins massively
where it commits early (1 candidate at 20 km) and loses modestly where it has to
descend to the bottom anyway. That is the right trade for terrain, where most
of the scene is far away, but it means the flat all-levels encoding should not
extend deeper than the cut realistically reaches.

##### The correctness constraint on the commit distance

Committing at the *bounding box entry* reports a hit on the box face, which is
not where the geometry is. That is fine for the probe and wrong for a renderer.

An accepted node has to commit at the distance of its **representative
geometry**, not its bounds: the node's tight occupied extent, or a short DDA
against its coarse proxy, computed inside the intersection function before
committing. Otherwise LOD-selected hits land on bounding-box faces and every
distance, normal and shading position is wrong by up to a box diagonal.

This is the real cost of the scheme and it is where the 175 Mray/s custom-AABB
number will move: that measurement committed at a slab-test distance with no
proxy intersection. A node whose acceptance requires an in-node DDA pays for it.
Budget the refinement view accordingly, and keep the exact triangle view - 924
Mray/s through the hardware intersector, hitting real geometry - as the path for
primary and reflection rays.

##### Where this leaves residency

With camera-driven error, the per-candidate cut and the residency cut compute
the same answer. That is a feature: it means they cannot disagree. It also
means the per-candidate test is not what *selects* the LOD in steady state -
residency already did that - and its real value is elsewhere:

- one structure spans several LOD levels, so the TLAS does not churn every time
  the camera moves a metre;
- when the desired level is not resident, the ray degrades to the resident
  ancestor by the same two tests, instead of missing;
- offscreen geometry, whose residency was chosen for the screen cut, still gets
  a defensible LOD decision at trace time.

So the four pruning mechanisms, strongest first:

```text
1. commit    accepting a coarse node collapses its whole subtree
             measured 585 primitives -> 1 candidate
2. residency the subtree is not in the structure at all
             the streaming cut; this engine already has it
3. instance  8 hardware-enforced mask bands, free per ray
   mask      measured 2.24 ms -> 1.46 ms at 4 of 8 bands, non-monotonic
4. reject    filters the answer, prunes nothing
             use it only where no commit is possible
```

##### Does the commit trick survive on cluster geometry?

The pruning result above was measured on **nested volumes**, where a child box
is strictly inside its parent so `t_child >= t_parent` always holds. A
Nanite-style cluster DAG is not that. Its LOD levels are **coincident surfaces**:
a simplified parent cluster occupies the same space as the children it replaces,
and simplification displaces vertices both toward and away from the viewer. So
the ordering the pruning relies on is not guaranteed, and the question had to be
re-measured on triangles rather than assumed.

It does survive, at a measurable price.

A probe built one surface patch at three coincident LOD levels - 512 / 2,048 /
8,192 triangles, `selfError` 0.5 / 0.25 / 0.125, `parentError` set to the
parent's `selfError` - instanced 512 times, and compared three configurations at
1920x1080 over a TLAS whose instances span camera distances from 60 to 800:

| configuration | triangles in BLAS | GPU | throughput | hits |
| --- | ---: | ---: | ---: | ---: |
| A pre-resolved cut, **opaque**, selected level only | 2,048 | 1.885 ms | 1,100 Mray/s | 452,199 |
| B per-ray two-test cut, non-opaque, **all** levels | 10,752 | 3.734 ms | 555 Mray/s | 452,550 |
| C non-opaque, all levels, commit first candidate | 10,752 | 1.742 ms | 1,190 Mray/s | 461,294 |

Hit counts for A and B agree to within 0.1%, so the two are answering the same
question.

Three things fall out, and C is the one that matters most:

- **Non-opacity is not the cost.** C runs the same all-LOD structure
  non-opaque, with an intersection shader invoked on every candidate, and is
  *faster* than the opaque pre-resolved case. Turning off `opaque` to get an
  intersection function does not forfeit the hardware fast path. This was the
  outcome that was least obvious in advance.
- **The per-ray cut costs about 2x**, not 5x. B traverses 5.25x more triangles
  than A and pays 2.0x. The commit-prunes-the-subtree mechanism therefore *is*
  working on coincident surfaces: committing the admissible level shortens the
  interval and the bulk of the finer geometry lies beyond it. It is a weaker
  guarantee than the nested-volume case, but it is not a wash.
- **The cost is rejection, not shading.** The gap between B and C is entirely
  that B rejects candidates. A rejected candidate does not shorten the interval,
  so the ray keeps traversing until it finds an admissible level.

That last point has a sharp corollary.

##### The no-admissible-level cliff

An earlier run of the same probe, with the error scale set so that most rays
found *no* level satisfying `selfError < t && parentError >= t`, measured **11.46
ms instead of 3.73 ms** - a 3x cliff, and 5.8x slower than the pre-resolved cut.
A ray that never commits traverses the entire structure to `tMax`.

This is not an exotic case. It is the normal transient in a streaming system:
the camera moves in, the desired finer level is not resident yet, and until it
arrives every ray through that region finds nothing admissible and pays full
traversal. Left unguarded it is a frame-time spike exactly when the streaming
system is already under load.

The guard is the one the raster cut already uses on roots: **the coarsest
resident level must always be admissible.** Publish `parentError = +inf` on the
coarsest resident node of every subtree, so the two tests always have an answer,
and let the miss be reported as a coarse hit plus a page request rather than as
a full-traversal miss. `SparseVoxelBrickNodeGPU` already initialises
`parentError` to `float max` for roots; the RT path needs the same treatment
applied to the *resident* frontier, not just the true root.

##### What this means for the Mega Geometry model

The pre-resolved cut - resolve the DAG cut per frame, put only the selected
clusters in the acceleration structure, keep triangles opaque - is the model
NVIDIA's RTX Mega Geometry and UE5's Nanite ray tracing both use. It is
measurably the fastest option here too (1.885 ms vs 3.734 ms), and this plan's
exact view already follows it.

But the reason that model is unambiguously correct on NVIDIA does not transfer.
Mega Geometry ships **CLAS** - cluster-level acceleration structures that are
built once per cluster and then assembled into a BLAS cheaply whenever the cut
changes. That makes "rebuild the BLAS when the cut moves" nearly free.

**Metal has no CLAS equivalent.** There is no way to pre-build a cluster
acceleration structure and cheaply relink it into a different BLAS. On Metal,
changing the cut means a full rebuild of the affected group BLAS, measured at
1.89 ms per full page. So the trade is genuinely different:

```text
NVIDIA + CLAS     cut changes are cheap        -> pre-resolved cut wins outright
Metal, no CLAS    cut changes cost a rebuild   -> per-ray LOD's 2x traversal may
                                                  be cheaper than the churn it
                                                  avoids
```

The break-even is a rebuild-rate question, and it is the one number that decides
the exact view's design:

```text
per-ray LOD wins when
    (rebuild cost) x (pages whose cut changes per frame)
  > (2x - 1x) x (trace cost of the affected rays)
```

That framing assumed a cut change costs a BLAS rebuild. **Two-level instancing
removes the assumption**: with one BLAS per cluster and a per-group instance
acceleration structure, a cut change rebuilds a 14 KB group AS in 0.230 ms and
touches no BLAS at all. That recovers the CLAS property Metal lacks, and it
makes the pre-resolved cut the right default after all - the Mega Geometry
playbook does transfer, just through a different mechanism than NVIDIA uses.

The per-ray LOD path therefore stops being a candidate for the exact view. It
keeps a narrower and still valuable job in the bounding-box refinement view,
where geometry is coarse, memory is cheap, and the alternative is no answer at
all.

The likely end state is a hybrid split by distance band rather than one choice:

```text
near field   few pages, LOD changes constantly under camera motion
             -> all-LOD encoding, per-ray two-test cut, no rebuild churn

far field    most of the scene, LOD stable for many frames
             -> pre-resolved cut, opaque, 1,100 Mray/s
```

The remaining cost of the all-LOD encoding is memory, and it is linear where the
speed penalty was sublinear: 5.25x the triangles for three levels, at ~109 bytes
of AS per triangle. That, not throughput, is what bounds how many levels the
near-field encoding can span.

### Phase 13: Optional smoother representatives

Only after exact cubical triangles work:

- try marching cubes / dual contouring for terrain surface only;
- compute measured geometric error against the dense heightfield;
- seam-certify chunk and group boundaries;
- keep Tree64 trace fallback until triangle trace parity is proven.

Tests:

- continuous heightfield oracle error in metres and projected pixels;
- crack-free neighbour LOD boundaries;
- silhouette parity under conservative error thresholds;
- shadow/raster mismatch detection.

## Expected performance

For terrain raster specifically, this should be faster than fragment-DDA brick
raster once cluster culling is in place. The fixture test shows roughly 7.5x
fewer raster primitives than naive exposed cube faces, and far less empty-box
fragment traversal than the current brick proxy path.

The realistic expectation:

- brick raster draw passes can plausibly drop by 2x or more on terrain-heavy
  views if HZB/cluster culling works;
- full-frame speedup will be smaller because lighting, streaming publication,
  and scheduling still consume time;
- the first CPU sidecar path may be good for offline/static terrain, but
  full-GPU generated terrain needs Phase 6 to avoid moving the bottleneck to
  CPU meshing/upload.

The key win is not just fewer triangles. It is that the G-buffer becomes normal
hardware raster work again: early-Z, HZB, and cluster culling all fit the shape
better than fragment DDA over large voxel boxes. The trace path stays on the
existing streamed trace payloads, so raster performance work does not destabilize
lighting and shadows.

### Phase 19: voxel-native virtual geometry build (the octree is the DAG)

Phase 19 replaces the LOD0-sidecar-through-meshoptimizer route. The measurement
that forced it: on the Cliff fixture the old bridge reported
`source_clusters=164 encoded_meshlets=164`, every meshlet at LOD0 and every
`parentError` at infinity. Exposed-face meshing emits a quad soup whose corners
are not shared, so edge collapse has nothing to collapse; the "LOD chain" was one
level deep and the renderer could only ever draw the finest geometry.

The brick octree already carries everything the DAG needs, so nothing is
simplified at all:

- a node is 8^3 cells and its eight children tile exactly the same volume;
- a parent cell is the OR reduction of the eight child cells under it;
- `selfError`/`parentError` are measured deviations and are monotonic.

What landed:

- `SparseVoxelBrickSurfaceMesh` - bitmask exposed-face meshing. A brick is eight
  64-bit z slabs, so a whole face direction is a shift and a mask, including the
  cross-brick neighbour test. Greedy merge is bounded to octree-aligned blocks.
- `VoxelOctreeVirtualGeometryBuilder` - meshes every octree level and emits the
  virtual geometry DAG directly. A cluster is one octree-aligned *block* of one
  brick; a group is one parent block against the eight child blocks under it, so
  volume correspondence is exact and a cut can never open a hole. The global
  block depth is raised until every block fits the 128-triangle / 192-vertex
  cluster budget; depth 3 is a single cell, so the search always terminates.
- `buildSparseVoxelTerrainOctreeVirtualGeometryAsset` - terrain wrapper that
  resolves the page node span and origin and encodes streamable pages.
- `SparseVoxelTerrainVirtualGeometryPageCache` - revision-tracked per-chunk cache
  with a bounded per-update rebuild budget and nearest-first ordering.
- The sample now builds and refreshes terrain VG assets through that cache.

Measured on the terrain fixtures (Release):

| | old bridge | octree build |
| --- | ---: | ---: |
| Cliff meshing | 8.4-13.0 ms (LOD0 only) | 0.17-0.27 ms (all 5 levels) |
| Cliff clusters | 164, all LOD0 | 130 over levels 94/16/10/6/4 |
| parent error | infinity everywhere | finite on 95%+ of clusters |

The cut sweep on Mountain selects 268 / 148 / 56 / 8 clusters as the projected
error budget loosens, with zero paths selecting two clusters and zero paths
selecting none.

Three defects were found and fixed on the way:

1. `buildToLinearizedClusterIndex` was not remapped by the page-ordering pass in
   `VirtualGeometryHierarchyRewrites`, so every consumer that used it to tie a
   meshlet back to its source read a different cluster after finalization.
2. The terrain sidecar's default cluster capacity was 64 face-local quads = 256
   vertices. `Meshlet::vertex_count` is a `uint8`: the count wrapped to zero and
   the encoded meshlet decoded as empty. The capacity is now 48 quads, and the
   virtual-geometry compatibility check now tests the vertex budget as well as
   the triangle budget.
3. The build's per-cluster stdout logging ran on every streamed page build. It
   is now behind `setVirtualGeometryBuildLogging`, off for runtime callers.

Phase 19 validation:

```bash
cmake --build build-release --target SparseVoxelOctreeVirtualGeometryTests SparseVoxelTerrainVirtualGeometryRefinementTests -j 10
ctest --test-dir build-release -R 'SparseVoxelOctreeVirtualGeometryBuild|SparseVoxelTerrainVirtualGeometryRuntimeRefinement|SparseVoxelTerrainTriangle' --output-on-failure
```

What the tests pin:

- exposed-face masks against an independent oracle rebuilt from the occupancy
  words: every exterior voxel face is meshed exactly once, at every octree
  level, on all nine fixtures - no missing faces, no faces on occluded sides, no
  duplicates;
- each level is a valid coarsening: every fine cell has an occupied parent cell,
  no coarse cell is invented, and each coarser level carries strictly fewer
  triangles;
- cluster cuts are brick block bounds: one cluster per block, every vertex on
  the closed block box, and each group's eight fine blocks tiling its coarse
  block exactly;
- the DAG cut is crack-free across an error sweep and refines monotonically;
- normal cones are only tightened for genuinely single-direction blocks;
- runtime refinement: a refined chunk is detected by revision, rebuilt, and
  never served stale; bounded budgets converge with no page starved; a focused
  update rebuilds the page under the camera first; a chunk that loses its bricks
  is evicted.

Still open:

- the renderer graph takes the terrain VG asset set at construction, so a
  refreshed page reaches the screen through a scene rebuild rather than a live
  `VirtualGeometryScene` page install. The Phase 14 runtime-install API is the
  destination; the cache's `generation` field is what a live installer should
  compare against.
- per-face material is not carried through the virtual geometry vertex format.
  Greedy merging only merges faces with equal palette index, so the information
  exists on the quads and is dropped at the `virtualgeometry::Vertex` boundary.

### Phase 20: the terrain path had no LOD cut at all

Terrain does not stream its virtual geometry from a file. It hands the whole
encoded asset to `VirtualGeometryScene::installRuntimeEncodedObject`, which
installs every page at once. That path did this for each installed page:

```cpp
const uint8_t enabledMask = fullEnabledMaskForNode(node);
setNodeClusterMasks(node, enabledMask, enabledMask);   // streamingMask = full
```

The first argument is the **streaming-leaf** mask. In `culling_multipass.wgsl` a
streaming leaf has its self error forced to zero and skips the parent test
entirely:

```wgsl
let isStreamingLeaf = ... ((streamingMask >> localBit) & 1u) != 0u;
let effectiveSelfError = select(rawSelfError, 0.0, isStreamingLeaf);
if (projSelfError >= selfErrorThreshold) { return; }        // never fires
if (!isStreamingLeaf) { ...parent test... }                 // never runs
```

So every cluster on every installed page drew at every distance: the LOD cut did
not exist, and all levels rasterised on top of each other. That is the "terrain
never refines" symptom, and the overdraw with it.

The encoded asset already carries the per-page hierarchy rewrites that describe
the real residency state. The fix threads them through
`RuntimeEncodedPageInstallInfo` / `RuntimePageInstallInfo` into
`installRuntimeObjectPage`, which now applies them after writing the page's own
node, and only falls back to the old always-resident behaviour when a caller
supplies none.

Measured on the 64-cell voxel chunk, runtime-installed, clusters selected by the
GPU culler:

| camera | before | after |
| --- | ---: | ---: |
| 2000 m | 18 | 11 |
| 500 m | 57 | 42 |
| 160 m | 90 | 64 |
| 10 m | 92 | 66 |

The DAG has 91 leaf clusters. A cut holds one cluster per root-to-leaf path and
distinct cut clusters cover disjoint leaf sets, so **a valid cut can never hold
more clusters than the DAG has leaves**. Before the fix the cut held 92 - proof
that parents were drawn on top of their children. That bound is the regression
assertion in `SparseVoxelOctreeStreamingGPUTests`; it fails at 92 > 91 without
the fix and passes at 66 <= 91 with it.

What the error metric does *not* explain, and what was checked first: terrain
error structure is sound. On the Mountain fixture the brick errors are 0.25 /
0.5-1 / 2 for voxel sizes 1 / 2 / 4, cluster self errors are 0 at the leaves and
rise monotonically, and a CPU cut sweep gives 268 / 268 / 268 / 56 / 8 clusters
at 5 / 40 / 300 / 1000 / 4000 m. Coarsening genuinely starts at
`projectionScale * parentError` metres - about 311 m for 1 m voxels at a
one-pixel budget - so a page of 1 m voxels is *supposed* to stay at LOD0 inside
300 m. With 20 cm terrain voxels the first step lands near 62 m.

Phase 20 validation:

```bash
cmake --build build-release --target SparseVoxelOctreeStreamingGPUTests -j 10
ctest --test-dir build-release -R 'SparseVoxelOctreeStreamingGPU' --output-on-failure
```

What the GPU test pins, reading back the hierarchy buffer and page table rather
than any CPU mirror:

- the root page installs alone and holds a single parent group;
- its hierarchy leaf is marked installed, has clusters enabled, and is marked as
  the residency border while its children are absent;
- every other leaf still carries the not-installed bit, and the bit agrees with
  the page table entry at every step;
- LOD errors are finite and never inverted, at every residency step;
- the GPU writes priorities for the pages under the root, and installs drain
  them one at a time;
- newly installed pages are picked by culling, and the cut never exceeds the
  leaf-cluster bound;
- the border is empty exactly when nothing is missing.

Open, not fixed:

- the culler can emit the same cluster more than once in the visible list at the
  coarsest cuts (2 duplicate entries out of 311 across the sweep). The distinct
  cluster set is still a valid cut, so this is wasted draw work rather than a
  broken cut, but it has not been diagnosed.
- `VisibleClusterInfo::pageLocalClusterIndex` does not index a page's build-time
  cluster range the way a per-path GPU coverage check needs (page 1 of 16
  clusters reports index 20), so that check was removed rather than left
  asserting on a mapping that cannot be trusted.

### Phase 21: page rewrites under a huge octree, and the streaming-leaf rule

Phase 21 tests the install/uninstall rewrites directly, on a fixture built to be
hard: a single floor of the finest voxels, wide enough that the brick octree runs
six levels and the page builder has to split the DAG across many pages with real
cross-page references.

```text
floor 256x256  cells=131072  bricks=1365  levels=6
clusters=6820  groups=1368  pages=172
rewrites: sameOwner=1368  crossPage=21889
```

`SparseVoxelOctreeRewriteTests` drives residency purely through the rewrites -
reset, apply the install updates of every resident page, full-enable installed
nodes no rewrite mentions - and inspects the state after every install and every
uninstall. It pins:

- cross-page rewrites: 21889 of the 23257 updates name a hierarchy node owned by
  a *different* page, and every one of those names a page inside the emitting
  page's transitive dependency closure, so an update is never applied against a
  page that is not resident;
- no rewrite names a node that owns no page, and no page emits two updates for
  the same node;
- at every install step: nothing enabled on an absent page, no streaming mark
  without an enabled mark, and the enabled set closed under DAG parents;
- the renderable cut across an error sweep: exactly one cluster per root-to-leaf
  path that has anything resident on it (a path with nothing resident is not a
  hole - that geometry has not been streamed yet);
- every cluster available once every page is resident (6820/6820) with nothing
  left on the residency border;
- two different legal install orders reach identical hierarchy state, and
  uninstalling retraces every install state bit for bit;
- dependency-closed prefixes - what a budgeted streamer actually produces - all
  render a complete cut.

#### The streaming-leaf selection rule

Those checks found a third defect, this time in the culler. `culling_multipass`
did:

```wgsl
let effectiveSelfError = select(rawSelfError, 0.0, isStreamingLeaf);
...
var projParentError: f32 = -1.0;
if (!isStreamingLeaf) { ...parent test... }   // skipped for streaming leaves
```

The streaming-leaf mark answers one question - *can this be refined past?* Its
children are absent, so forcing its self error to zero is right: it must never be
rejected for being too coarse. Coarsening is a different question, and skipping
the parent test meant a streaming leaf drew at **every** distance, on top of the
coarse cluster that also passed its own test. The streaming border rasterised
twice, at two resolutions, wherever a page boundary sat.

Applying the parent test to streaming leaves as well is safe precisely because
the enabled set is closed under DAG parents - the property the same test suite
verifies at every install step. A fine cluster is only enabled while every
coarser cluster standing in for it is enabled too, so rejecting a streaming leaf
as too fine always leaves its stand-in resident and drawable.

Measured over a 59-page install sequence with an error sweep at every step:

| rule | holes | double draws |
| --- | ---: | ---: |
| streaming leaf skips the parent test | 0 | 225360 |
| streaming leaf keeps the parent test | 0 | 0 |

On the GPU the far cuts tighten accordingly, with no other behaviour change:

| camera | before | after |
| --- | ---: | ---: |
| 2000 m | 11 | 6 |
| 500 m | 42 | 31 |

Phase 21 validation:

```bash
cmake --build build-release --target SparseVoxelOctreeRewriteTests -j 10
ctest --test-dir build-release -R 'SparseVoxelOctreePageRewrites|SparseVoxelOctreeStreamingGPU|VirtualGeometryPageInstalls|VirtualGeometryRenderer' --output-on-failure
```

### Phase 22: verifying the runtime install on the GPU, and what is still open

The Phase 20 fix is now verified on the GPU rather than only in the CPU model.
`SparseVoxelOctreeStreamingGPUTests` installs an encoded asset through
`installRuntimeEncodedObject` - the one-shot path terrain uses - then reads the
hierarchy buffer back and inspects the masks the install left behind:

```text
runtime-installed hierarchy: leaves=22 notInstalled=0 noEnabledCluster=0
                             onStreamingBorder=0 enabledClusters=112/112
```

That is exactly the state a working LOD cut requires: every page resident, every
cluster enabled, and nothing left marked as a streaming border. Before the fix
every cluster on every page carried the streaming mark, which forced its self
error to zero and skipped its parent test, so every level drew at once.

#### Terrain error arithmetic, for reference

The sample gives the terrain culler `max(0.25, lodPixelThreshold)` with
`lodPixelThreshold = 1.5`. With a 720-tall target and a 60 degree vertical field
of view the projection scale is `tan(30)^-1 * 0.5 * 720 = 623`, so a cluster is
coarsened once `error / distance * 623 < 1.5`, i.e. beyond `415 * error` metres.

For a 31 m page of 20 cm voxels the octree runs five levels and the cluster
errors are roughly 0, 0.1, 0.2, 0.4 and 0.8 metres, which puts the transitions at
about 41 m, 83 m, 166 m and 332 m. A chunk seen from beyond ~330 m therefore
draws its root cluster and nothing finer, and that is the metric behaving
correctly rather than a defect. Inside that range the finer levels are selected -
at 50 m the level-1 clusters pass both tests while the root and levels 2-4 are
rejected.

#### Resolved: the GPU wall image test renders

`SparseVoxelOctreeWallGPUTests` renders a 64x64 wall of voxels through page
decode, culling, hardware draw and a one-dispatch id readback, at one finest
voxel per pixel, and inspects the image after every page install. It is
registered as a ctest and passes.

The bug that kept it dark: `math::Mat4f` is **column major**
(`data[col * 4 + row]`), and the passes take that raw storage. The test rebuilt
the uniform arrays as `[row * 4 + col] = at(row, col)`, which transposes them.

The culling pass survives a transposed matrix, because its projected-error term
only reads `proj[1][1]` - unchanged by transposition - so clusters were selected
normally, the counters looked healthy, and the meshlet vertex shader then
transformed every vertex off screen. Culling reporting work while the frame stays
empty is the signature to remember. The fix is to pass
`camera.getViewMatrix().data` straight through.

What the test shows, streaming one page per update at an error budget just above
zero so the cut wants the finest level everywhere:

```text
visible clusters:   7 7 7 19 31 43 55 67 ... 247 256 256
covered pixels:     4096 at every single step
distinct surfaces:  14 14 14 32 50 78 94 139 336 572 ... 2093 2118 2154
```

Coverage is complete from the first frame - the root page's streaming leaves
stretch over the whole wall - and the detail inside it grows by two orders of
magnitude as pages arrive. Three PPMs are written next to the executable
(`wall-gpu/wall-000-root-page.ppm`, `-001-half-streamed`, `-002-settled`) so the
picture can be looked at: 37 distinct colours in the root-page frame against 2314
in the settled one.

Two corrections the fix forced elsewhere, both because the transposed matrix had
been suppressing frustum culling in `SparseVoxelOctreeStreamingGPUTests` as well:

- residency now settles at 7/7 pages rather than 5/7, and detail peaks in the
  middle of a distance sweep (2000 m = 7, 160 m = 300, 10 m = 120) rather than
  rising monotonically - approaching a chunk wider than the frustum takes most of
  it off screen. The assertions were calibrated against the buggy numbers and are
  now written against the real behaviour.
- the cut-size bound added in Phase 20 was removed. The intent was sound - a cut
  can never hold more clusters than the DAG has leaves - but the visible-cluster
  list cannot supply that quantity: it reports 300 entries resolving to 148
  distinct `(pageIndex, pageLocalClusterIndex)` pairs for a DAG of 112 clusters,
  so that pair is not the cluster identity it was assumed to be. The Phase 20
  regression now rests on a direct hierarchy-mask readback instead, which is
  exact:

```text
runtime-installed hierarchy: leaves=22 notInstalled=0 noEnabledCluster=0
                             onStreamingBorder=0 enabledClusters=112/112
```

Still unexplained and worth a look: the culler emits the same cluster more than
once in the visible list (300 entries for 148 distinct pairs).

#### Page packing, measured

Pages are packed to the ABI limit already; the earlier "one level per page"
observation was an artefact of tests forcing `maxGroupsPerPage = 8`.

```text
groupsPerPage=8   pages=172  meanGroups=7.95  meanClusters=39.7   full=170/172
groupsPerPage=64  pages=23   meanGroups=59.5  meanClusters=296.5  full=21/23
```

`maxGroupsPerPage` is clamped to `MAX_GROUPS_PER_PAGE` (64), the page group-table
limit, and binds before the 448-cluster cap. Groups are never split across pages.
The remaining weakness is ordering rather than fill: groups are collected in
group-id order, so pages come out as strips of median 2.5:1 and worst 10.7:1
aspect. Sorting `collectVisibleUnassignedGroups()` by a Morton code of the
group's block origin would square them up without touching the rewrite algebra.

### Phase 23: signed brick keys truncate the LOD chain across the origin

Chasing the wall render turned up a defect that has nothing to do with rendering.

`buildSparseVoxelBrickHierarchy` coarsens by halving signed brick keys. Integer
floor division sends -1 to -1, so a brick key of -1 stays -1 at every level and
never merges with key 0. Coarsening stalls with several permanent roots rather
than converging on one, and the octree ends up a level shorter.

Measured on identical geometry, a 64x64 two-voxel slab, built once in positive
cell coordinates and once shifted to straddle the origin:

```text
positive origin:   levels=4  clusters=340
straddling origin: levels=3  clusters=336
```

A page that crosses the world origin on any axis therefore has one fewer level
to coarsen through than the same page placed away from it. For terrain, whose
pages sit at world coordinates that routinely cross the origin, that directly
shortens the LOD chain available to the cut.

This is characterised rather than fixed:
`SparseVoxelOctreeRewriteTests::testStraddlingTheOriginShortensTheLODChain`
asserts the current behaviour and says so, so the fact is recorded and the check
fails if someone fixes it. The fix belongs in the brick builder's key
coarsening - biasing keys to be non-negative before halving - and touches the
brick raster path as well as virtual geometry, so it wants its own validation
pass rather than being folded in here.

Workaround available today: build a page in non-negative cell coordinates and
recentre it with `SparseVoxelOctreeVirtualGeometrySettings::originOffset`, which
only shifts emitted vertex positions and leaves the key derivation alone. That is
what the terrain bridge already does, using the page minimum as the offset.

### Phase 24: the culler's duplicate clusters were a runtime-install defect

The duplicate visible-cluster entries left open at the end of Phase 22 were not a
culling defect at all. `VirtualGeometryScene::installRuntimeObjectPage` did this
for every hierarchy leaf belonging to the page being installed:

```cpp
const uint32_t advertisedClusterCount =
    install.pageTableEntry.clusterCount != 0u
    ? install.pageTableEntry.clusterCount
    : install.pageAllocation.clusterCount;
if (advertisedClusterCount != 0u)
{
  if (node.child_start == UINT32_MAX) node.child_start = 0u;
  node.child_count = advertisedClusterCount;   // the *page's* count
}
```

A page owns many hierarchy leaves - one per group, at most eight clusters each -
and each leaf's `child_start`/`child_count` already names its own slice. Writing
the page's whole cluster count into every leaf makes each one claim the entire
page **starting at its own offset**, so the ranges overlap and the tail runs off
the end:

```text
leaf A: child_start=0  child_count=32  -> clusters 0..31
leaf B: child_start=8  child_count=32  -> clusters 8..39   (8..31 duplicate A,
                                                            32..39 do not exist)
```

Measured on a 7-page object at 160 m:

| | before | after |
| --- | ---: | ---: |
| visible-cluster entries | 300 | 83 |
| distinct clusters | 148 | 83 |
| entries naming a cluster past the page end | 104 | 0 |
| repeated entries | 94 | 0 |

So the raster was doing 3.6x the work it needed to, drawing many clusters several
times and some clusters that do not exist. This is the runtime-object path, which
is the path terrain uses.

The override is now gated on `install.overrideHierarchyLeafMetadata` - the same
flag that already gates the bounds and error override, and which means "the
caller owns this node's layout". That is the generated-page path, where one node
covers one page and the counts arrive with the install. An encoded asset carries
correct per-node counts from the build and must keep them.

With entries now distinct and in range, the cut-size bound is sound again and is
reinstated in `SparseVoxelOctreeStreamingGPUTests`, alongside two new checks that
would have caught this directly:

```text
no visible-cluster entry names a cluster past the end of its page: 0
the culler emits each visible cluster exactly once: 0 duplicates
the runtime-installed cut never exceeds the leaf count (83 vs 91)
```

### Phase 25: the two install paths must agree, and terrain end to end

Two tests close out the path, both on the GPU.

#### Install-path equivalence

A file-backed object rebuilds its hierarchy with
`rebuildHierarchyFromInstalledPages`, which touches only the install bit and the
two per-cluster masks and **never rewrites `child_start` or `child_count`**. A
runtime-installed object updates the same state incrementally, one page at a
time. From the same encoded asset, fully resident, the two must land on identical
hierarchy nodes.

`SparseVoxelOctreeStreamingGPUTests` now installs the asset both ways in the same
run, reads both hierarchy buffers back, and compares every node field by field:

```text
path comparison over 27 nodes: childRange=0 masks=0 installBit=0 errors=0 bounds=0
```

This is the check that would have caught the Phase 24 `child_count` overwrite
immediately, and it pins the invariant that an incremental install converges on
exactly what a full rebuild produces.

#### Terrain end to end

`SparseVoxelTerrainVirtualGeometryEndToEndTests` starts from a real generated
terrain chunk and follows it to pixels through the passes the sample uses:

```text
buildWorldMapVoxelTerrain
  -> buildSparseVoxelTerrainOctreeVirtualGeometryAsset
  -> installRuntimeEncodedObject          (the sample's one-shot install)
  -> registerObjectForStreaming           (the incremental path, for comparison)
  -> page decode -> culling -> hardware draw -> id readback
```

The Mountain fixture, 32 m chunk of 1 m voxels, 19 pages at four groups each:

```text
d=1108m  clusters=8    pixels=59     L2=8
d=443m   clusters=8    pixels=407    L2=8
d=221m   clusters=62   pixels=1600   L0=26 L1=36
d=110m   clusters=130  pixels=4603   L0=120 L1=10
d=55m    clusters=99   pixels=10496  L0=99
d=33m    clusters=83   pixels=21969  L0=83
```

That is the LOD refinement that was missing: the coarsest level alone at
kilometre range, genuinely **mixed** L0/L1 cuts in the middle - which is what
proves per-cluster selection rather than a whole-page flip - and pure level zero
close up. Pixel coverage rises with proximity as the chunk fills more of the
frame.

Streaming the same chunk in one page per update:

```text
1p/4c 1p/4c 2p/4c 3p/8c 4p/14c 5p/26c 6p/27c 7p/27c 8p/27c 9p/29c 10p/36c
11p/40c 12p/42c 13p/46c 14p/46c 15p/46c 16p/54c 17p/63c 18p/76c 19p/83c
```

Residency only grows, no frame is ever empty, and the cluster count climbs with
it. Every frame is also checked for the Phase 24 defects: no cluster emitted
twice, none emitted outside its page, and the cut never exceeds the DAG's leaf
count.

### Phase 26: CPU chunk generation is the renderer default, and the route that hid it

The "huge chunks" symptom had nothing to do with the LOD cut. The sample was
never running the terrain virtual-geometry route at all:

```text
Terrain G-buffer resolver request=bricks selected=brick hierarchy
  route=brick-fallback reason=forced brick terrain fallback route
visibleBricks=597
```

Two changes:

1. **CPU chunk generation is now the renderer's default lane.** The sample
   started in `GPUFull` / `TerrainGPUV2RequestedMode::GPUV2`; it now starts in
   `TerrainGenerationMode::CPU` / `TerrainGPUV2RequestedMode::CPU`. The GPU-v2
   lane remains available with `--terrain-generation gpu-v2`, which is what the
   GPU publication tests request explicitly.

2. **CPU terrain no longer forces the brick G-buffer.** The sample contained:

```cpp
if (terrainMode && terrainBuilderMode == CPU && !gbufferResolverProvided)
  gbufferResolver = "bricks";   // "the CPU path publishes brick/Tree64
                                //  payloads, not generated-VG decoded pages"
```

   That was true before this work. The CPU lane now builds and installs
   generated virtual geometry pages per chunk through the terrain page cache, so
   the override meant CPU terrain always rendered as coarse brick boxes with no
   per-cluster LOD cut. It now selects `auto`, which prefers the virtual
   geometry G-buffer once a chunk's pages are installed and falls back to bricks
   otherwise.

Measured headless over 30 frames against `world-data`, before and after:

```text
before: request=bricks   route=brick-fallback   visibleBricks=597  coveredPixels=418281
after:  request=auto     route=virtual-geometry visibleBricks=0    coveredPixels=454258
        Terrain virtual-geometry asset bridge: chunks=5 assets=5 pages=13
                                               meshlets=2831 build=0.01s
        Terrain virtual-geometry scene: objects=5 pages=13 meshlets=2831 decodeQueued=13
```

`SparseVoxelTerrainVirtualGeometryPhase17Tests` asserted the old forcing and now
asserts the new contract, plus that no path silently forces bricks for CPU
terrain and that CPU is the default lane.

#### Reading the rendered surface across page installs

`SparseVoxelTerrainVirtualGeometryEndToEndTests` now gives its streaming scene
the full draw path and an id readback, so it inspects the rendered surface rather
than counters as pages arrive:

```text
streaming progression (pages/clusters/surfaces):
  1p/4c/27s  1p/4c/27s  2p/4c/27s  3p/8c/25s  4p/14c/41s  5p/26c/98s ...
  19p/83c/301s
page installs=18, cut changes=13 (image moved on 8),
installs that changed the cut=13, unexplained image changes=0
```

Two assertions were written too strongly first and corrected against this data:

- **not every page install changes the picture.** A page can install whose
  clusters the error budget does not select at this distance: residency grows,
  the cut does not, and the image must stay identical.
- **detail can fall on a refinement.** Distinct visible surfaces went 27 -> 25
  when the cut refined from 4 to 8 clusters, because finer faces land sub-pixel
  and fewer of them win a pixel. "Detail never decreases" is not an invariant at
  a fixed resolution.
- **not every cut change moves the image either.** Occlusion culling is off in
  this test, so a page can add clusters that are selected, drawn, and then lose
  the depth test behind terrain already on screen - 8 of 13 cut changes moved the
  picture.

What is asserted, and holds: a frame that installs nothing renders a bit-identical
image; the surface never moves without residency or the cut moving; most cut
changes move the picture and several separate installs do; and streaming
multiplies the distinct surfaces on screen 27 -> 301.

### Phase 27: the cut that never settled, and the three defects behind it

The reported symptom was one: bricks blinking in and out on a static camera,
some never subdividing, some vanishing. Measured on the elevated slope scene
(`--test-world-terrain-slope`, 640x360, budget 1.5 px) it was three defects,
each with its own signature in the per-frame coverage signature.

**1. Steady-state blink was the software raster hand-off.** Every 35-45 frames
the whole cut lost its LAST-visibility bit at once (`p1hw=0`), phase two
re-routed ~15k clusters to the Metal compute rasterizer, which covered 83k of
the 130k pixels the hardware path had, and the next two frames drew a stale
software payload over-covering to 229k. The software rasterizer is now off by
default (`--vg-software-raster` turns it on; `--disable-vg-software-raster`
is the explicit off). With it off, coverage holds within 127706-128188 px on a
static camera and the phase-two re-emits are harmless hardware redraws.
`SparseVoxelVGCutStabilityGPU` pins this: 24 frames of an identical cluster set
and image, phase two never re-emitting, every drawn cluster satisfying
`projSelf < T <= projParent` with its voxel within one octave of the budget.

**2. The LOD stall was demand idempotence against a per-frame request feed.**
Refinement demand was published once per chunk generation, but the terrain
request feed is a snapshot replaced every frame, so a demand not admitted in
its frame was gone for good. The cut froze at 107 chunks with demand published
for 213 pages and `refine=0`. Demand now expires after 60 frames and re-issues,
the demand metric is the projected leaf voxel (`parentError * 0.5` at the
cluster's distance) rather than a bounding-box guess, and coarsen requests are
dropped when the coarsened voxel would project above 70% of the budget - the
hysteresis that stops refine/coarsen ping-pong. Same scene: 179+ chunks,
22.7k cut clusters, max triangle 49 px.

**3. The 12% dip on every refresh was scene identity by cache slot.** The page
cache is keyed by clipmap slot and converges a bounded number of slots per
frame; the terrain cut moves as a whole. A refinement rebuilds the parent's slot
with one child and lands the other three over later frames; a slot move puts
the same ground in a new slot and something else in the old one. Keying scene
objects by slot meant the ground a slot used to draw had no object until its
new slot was meshed - 136576 -> 120714 px, on every refresh frame, and no
amount of deferred release, serialization or decode delay touched it (each was
tried; each is still available as an `SVR_TERRAIN_VG_*` diagnostic).

`TerrainVirtualGeometrySceneSchedule` now keys objects by ground (world centre
and LOD) and only ever swaps coverage atomically: finer entries under a
registered coarser object are held back until the coarser footprint is fully
covered (an exact quadtree coverage test, so mixed-level refinements work),
then install together while the coarser object retires in the same apply;
a coarser entry over finer objects installs and retires them at once; a slot
move is a no-op; ground that left the cut retires only once covered or once
nothing in the cut overlaps it. Installs run before retires, and a failed
install keeps the ground it was meant to replace on screen.

Result on the same 100-frame slope run: 0 coverage drops (was 4), every
refinement logged as `installed=4 removed=1`, `heldBack` non-zero only while a
replacement set is incomplete. `TerrainVirtualGeometrySceneSchedule` (CPU)
drives the scheduler through refine, coarsen, slot move, version replace,
eviction, a mixed two-level refinement and a 400-frame random quadtree walk,
asserting no hole under the root and no overlap on every frame.

Also in this phase: replaced runtime objects release their hierarchy, page
table and page-buffer ranges only after every in-flight frame that could
reference them has completed (`setRuntimeReleaseDelayFrames`); uninstalling a
page rebuilds the object's hierarchy masks so leaves and the page table agree
under LRU eviction (`[evict]` section of the GPU test); and the frame-local
culling readbacks are attributed to the slot of the frame that produced them,
which the previous code got wrong every other frame.

### Phase 28: one visibility key for both rasterizers, and the draw capacity

Two more defects and the piece that was missing for the software path.

**The draw capacity was the bootstrap meshlet count.** `maxDrawnClusters` was
clamped to the meshlets the scene held at construction (4148 for the slope
scene). The flat direct draw and the indirect-count draw both stop at that
slot, and which clusters land past it depends on the atomic emission order, so
a settled 15k-cluster cut lost a different 11k every frame: on the 1200-frame
slope run, coverage moved by up to 3.75% on 35 frames where nothing changed.
The overflow diagnostic never fired because it read phase two's count while
phase one draws the bulk under two-pass occlusion. Now: capacity is
`MAX_VISIBLE_CLUSTERS`, the draw is the single indirect command with the
culler's vertex count, and the check covers both phases. Same run: zero
coverage jumps on quiet frames and a bit-constant late window.
`[scale]` in `SparseVoxelVGCutStabilityGPU` pins it with 144 chunks on a 512²
target (7302 clusters), both draw paths bit-identical over ten frames and
covering the same pixels, and adds the culling contract: frustum or occlusion
culling off draws more clusters and the identical pixels.

**The unified visibility path.** The hardware meshlet draw kept writing a full
G-buffer while the software rasterizer, when it ran at all, produced its own
payload that an adapter merged after the fact. Both now produce the same 64-bit
key per pixel - reverse-Z depth bits in the high word,
`packVisibilityPayload(globalCluster, triangle)` in the low word, so nearest
wins by an unsigned max (`virtualgeometry-visibility.wgsl`):

- hardware: depth attachment plus the payload attachment, through the depth
  test;
- software (`virtualgeometry-software-raster-cs.wgsl`, one workgroup per
  routed cluster, one thread per triangle): `rasterizeDepth` does an
  `atomicMax` of the depth bits, `rasterizePayload` an `atomicMax` of the
  payload where the depth equals the winner - two passes because MoltenVK
  reports no 64-bit buffer atomics to WGSL. One instance per occlusion phase
  with its own buffers, cleared at the start of the phase.
- `VirtualGeometryVisibilityMergePass` folds the hardware attachments and the
  software phases into one key buffer; `VirtualGeometryVisibilityResolvePass`
  is a fullscreen pass that reconstructs depth, ids, material, UV, geometry
  normal and motion for every pixel from its key, with the same transform and
  edge functions the rasterizers used. Phase one's resolve runs before the
  depth pyramid, so software surfaces occlude phase two.

The culler keeps near-plane-crossing and skinned clusters on hardware; the
software path does not clip and does not skin. Both culling passes route
clusters when the software rasterizer is on.

**What broke on the way.** A per-triangle bounding-box cap in the software
rasterizer (128 px "for safety") truncated the large flat quads a
small-average cluster still contains, and read as the surface behind them
showing through on 6% of pixels. The per-triangle attribution written to find
it blamed neighbouring slivers, because it compared depth bits strictly and
coplanar neighbours differ by an ulp; a CPU replica of the rasterizer inside
the test, matching the hardware on every pixel, is what separated the
algorithm from its GPU execution. The cap is gone; the routing threshold is the
only size control.

`SparseVoxelVGVisibilityGPU` is the contract for the path: with software off,
the resolve reproduces the hardware G-buffer bit-exactly in depth, ids and
material and within interpolation tolerance in normal and UV; with every
cluster routed to software, coverage is identical and depth agrees on every
covered pixel; with a threshold that splits the cut, the merged picture
matches, settles bit-identical over 24 frames with phase two re-emitting
nothing, every drawn cluster on either rasterizer passes the projected-error
test, and disabling occlusion or frustum culling never changes a pixel.

The sample renders through this path by default; `--vg-software-raster` turns
the compute rasterizer on and `--vg-metal-raster` selects the dormant native
Metal encoder for bring-up only.

**The sample's empty frames, and what they taught.** Wired into the sample, the
unified two-phase chain produced whole frames with zero coverage every few
frames - the culler emitted nothing in either phase - while single-phase, or
the same chain without the final merge and resolve, did not, and no GPU harness
configuration (replacement, growth, two frames in flight) reproduced it.
Per-phase rejection counters (words 26-31 of the culling statistics, now
copied whole; the sample's earlier copy stopped at word 16, so anything past it
had been garbage) showed the mechanism: within one frame, phase one saw last
frame's visibility bits still clear while phase two saw them set. The advance
kernel that produces those bits was the first dispatch of the pre-culling pass,
and on MoltenVK it intermittently ran unordered against the cluster stage. It
now follows the counter clear, and reports the size of the set it produced in
the pre-pass's word 31; phase one's hardware plus software count equals it on
every frame without a retirement (a retired object's bits linger until its
cluster slots are reused, so the count can only exceed the draw), and the
1200-frame run shows zero coverage drops.

Two diagnostics stay: `RG_DUMP_ORDER=1` prints the graph's node order per run
(which also showed that a `cmdMemoryBarrier` recorded between child passes of
a composite becomes its own level-0 node - it does not order anything where it
was written; the graph's dependency analysis is the only ordering), and
`SVR_WAIT_IDLE_EACH_FRAME=1` drains the GPU after every submission to separate
in-frame defects from races between frames in flight.

### Phase 29: why chunks were slow to arrive, and where the geometry time went

The question was whether chunk and page building could move to the GPU, why
some chunks took many frames to update or never divided, why there were gaps
between chunks, and why the grid looked misaligned. Every claim below is a
measurement on the slope test (`--test-world-terrain-slope`, 640x360, static
camera, `--lod-threshold 1.5`, 700-900 frames).

**Where the CPU time goes.** Per streamer commit the heightmap sampling costs
92 ms for about 4.2 chunks (22 ms per chunk) and the brick hierarchy 8.8 ms;
commits land every 17 frames under the 750/1500 us budget, so the streamer
produces 0.24 chunks per frame. Meshing a page for virtual geometry costs
7.7 ms on a worker, four per frame. That would be adequate - 170 pages is
1.3 s of worker time - except that the page cache was meshing every ground
five to six times: 944 rebuilds for 158 grounds, and 786 of the successive
rebuilds carried the *same* payload revision in a *different* slot. The cache
is keyed by slot, the streamer moves chunks between slots as the cut changes,
and a slot with a new chunk in it read as "new geometry". The brick node
revision made it worse: it hashes the physical node base and capacity, so a
relocation with identical content also changed identity. The fix is a
pre-pass in `collectStaleChunks` that adopts the page another slot holds for
the same ground, payload and level (`adoptedChunks` in the update), resolved
against the pre-update entries so two chunks swapping slots each get the
other's page. After it every ground on the run is meshed exactly once (170 of
170) and total worker time drops from 10.1 s to 1.3 s. The deferred queue,
which had reached 63 pages, is gone: stale detections fall from 6723 to 18.
`testSlotMoveAdoptsExistingPage` pins this on the CPU.

**"Some chunks never divide."** Two causes, both in the scene scheduler. The
first (Phase 27's rule) waited for a coarser object's *whole* footprint to be
covered by finer entries; a quadrant outside the cut never arrives, and 46
entries were held back forever. The rule now covers only ground the cut
holds or wants. The second is the opposite error: with the relaxed rule a
parent retired the moment its *resident* children had pages, while a fourth
child was still generating - a chunk-sized hole for 61-71 frames (the
generation time), which is what "gaps between chunks" looked like. The
scheduler takes a `pending` list of ground the cut has asked for and does not
hold; the sample builds it from the page directory (`Generating`, `Cached`,
`Installing` states - the chunk entries carry no queued state under spatial
streaming, which is why a first attempt from them produced an empty list).
`testPendingChildHoldsParent` pins both halves: a pending quadrant holds the
parent, an absent one does not.

**Gaps and alignment.** Two coverage metrics were added to the frame
signature: `holes` (uncovered pixels with three or four covered neighbours:
seams and pinholes) and `enclosed` (uncovered pixels in regions touching no
image border: a missing chunk). Seams are essentially absent - at most 22 of
230,400 pixels on any frame, with or without boundary stitching - so the
`boundaryStitchSubdivisions` experiment (2 on spatially coarse pages) is kept
only as a runtime override, `SVR_TERRAIN_STITCH=<n>`. The visible coverage
drops on the static run (-5.6% at one frame, never recovering) are silhouette
changes when a 102 m-voxel root page refines, not holes. The grid is one
nested lattice anchored at the world minimum corner (-64 km): every level's
tiles nest exactly and never overlap, but 64,000 m is a multiple of the tile
edge 6.4 m * 2^L only for L <= 4, so from LOD 5 up tile borders sit half a
tile off the multiples of their own edge measured from the world origin. That
is the "misalignment": a real offset from the origin, not a defect between
chunks. Anchoring the lattice at the origin instead would move every address
by 10,000 base tiles and is a data-format change.

**Fine-to-coarse pops.** The streamer's coalesce statistics on the static run
show 180 accepted merge requests against 259 refines: the distance-scheduled
brick feedback still asks for merges the rendered-error demand then undoes.
None of them reached the scene on the static camera (`coarsened=0` in the
refresh line, which now counts coarser entries installed over finer objects),
but under motion the same requests commit; the next step is to silence the
brick-feedback merge path entirely while the virtual-geometry route is the
resolver, rather than filter it with hysteresis.

**Moving generation to the GPU.** Yes, and most of the pieces exist. The
gpu-v2 lane already samples the heightmap and builds columns, fine raster
bricks and the joint publication on the GPU, and
`TerrainGPUV2GeneratedVGEmitPass` emits page-space voxel faces straight into
the decoded cluster buffers. What it lacks is exactly what the question
proposes: (1) a per-level pipeline - every octree level is derivable from a
heightmap mip in parallel, so a page at level L is one dispatch over its
64x64 footprint with no dependence on its children; (2) a page budget per
frame, root first, descending only where the cut's demand band asks (the
priority feedback already exists); (3) cluster formation on the GPU: today
the emit lane writes 48 faces per cluster with no simplification or error
bound, so it can only serve leaf pages, while the cut rule needs
`selfError`/`parentError` per cluster - the voxel level gives both directly
(a level-L voxel's error is its voxel size, Phase 26). (4) The reservation:
the sample reserves `MAX_CLUSTERS_PER_PAGE * 48 * 4` decoded vertices for
every page regardless of content and fails with "scene buffers cannot fit the
page reservation" once a few hundred pages are wanted; a per-page count from
the columns pass (faces are known after the height raster) sizes it exactly.
With those four, chunk generation stops being 22 ms of CPU per chunk and the
CPU keeps only the cut, the scheduler and the page table.

Diagnostics added: `SVR_DUMP_REBUILDS=1` logs every meshed page with its
ground, level and revisions; the refresh line carries the stale reasons
(`new/payload/nodeOnly/slotOnly/adopted`), the scene's `coarsened` count and
`pendingGround`.

### Phase 30: terrain pages are generated on the GPU

The CPU no longer meshes terrain. A page's geometry is produced by the same
GPU lane that samples the heightmap: columns, fine raster bricks, then a
virtual-geometry emit that writes decoded clusters straight into the scene's
decoded buffers. What the CPU keeps is the cut, the page set and the page
table.

**Two transactions per page.** A blind reservation (the maximum cluster count
for every page) exhausted the scene after a few hundred pages, which is why
the lane never ran. Generation is now a *count* transaction and an *emit*
transaction over the same columns and bricks: the count enumerates the exposed
faces per tile and returns them, the CPU turns those into a cluster plan
(`terrainGPUV2GeneratedVGClusterPlan`: clusters tile by tile, decoded ranges
packed by face), reserves exactly that through the payload manager, and the
emit writes into it. Both transactions carry their mode in the request flags,
so a batch may mix pages being counted with pages being emitted, and the emit
lane keeps the column and raster stages idle (`reuseSource`). A page is 8x8
tiles; each cluster holds up to 48 faces of one tile, so a cluster is
spatially coherent and belongs to one quadrant of the page.

**The cut is decided per page.** Every cluster of a page carries the page's
sphere and its level's voxel as self error, and the parent *page's* sphere and
voxel as parent error, so a child's clusters and the parent clusters they
replace evaluate identical numbers on both sides of the test: no level draws
twice, and none drops out. Residency is per quadrant: a cluster whose quadrant
child page is not installed carries
`DECODED_CLUSTER_FLAG_STREAMING_LEAF` in the decoded cull data and draws at
any distance until the child lands. The flag lives in the cull data rather
than the hierarchy node's 8-bit masks because a generated page has hundreds of
clusters and its children arrive one quadrant at a time; the leaf carries
`HIERARCHY_DECODED_CLUSTER_FLAGS_FLAG` and the culler reads the per-cluster
flag instead of the mask. A sixth emit kernel rewrites those flags when a
child comes or goes, with no regeneration.

**The page set.** The scene registers one runtime object with a fixed number
of parked page slots (4096); `TerrainGPUVirtualGeometryPageSet` maps the
streamer's page directory onto them, keeps parent and child links by quadrant,
and fixes a page's bounds when its slot is assigned so the spheres never move
under an installed page. A slot's leaf is rewritten as it becomes requestable
(`VirtualGeometryScene::updateRuntimeObjectPageLeaf`) and again at install; a
parked leaf projects zero parent error, so the culler never traverses or
requests it. Its CPU contract is `TerrainGPUVirtualGeometryPageSetTests`.

**Measured on the slope test** (700 frames, 640x360, static camera): 494 pages
generated across LOD 10 down to LOD 3, every ground exactly once (no churn),
zero holes and zero enclosed gaps on every frame, no coverage drop over 3%.
The CPU spends no time meshing at all. Four defects were found on the way and
each is now pinned by a test:

- the emitted clusters had a cone axis with cutoff -1, which the culler reads
  as "always backfacing": every cluster was camera-culled and the frame was
  empty. Voxel faces point every way inside one cluster, so the cutoff is 1.
- a root page's leaf inherited a finite parent error and failed its own LOD
  test at distance, so the terrain vanished; a root's leaf and its clusters
  both project infinity now.
- uninstalling one page rewrote the whole object's hierarchy, which raced the
  frame in flight and read every page as not installed for one frame (a whole
  black frame). A page whose clusters carry their own flags has one leaf to
  mark and no masks to replay.
- a parent was uninstalled the moment its record left the cut, while its
  children were still generating, which opened a quadrant-sized hole for the
  whole generation time (1342 pixels for two frames). A page whose record left
  because the cut *refined* past it keeps drawing until a child is installed;
  only a page nothing below claims is uninstalled.

**The brick-feedback merge path is load-bearing, not noise.** Phase 29 read
the 180 accepted merges against 259 refines on a static camera as two feeds
fighting, and proposed silencing the merges under this resolver. Measured:
dropping them changes the CPU route's cut error not at all (64.07 px either
way, 422 versus 424 oversized true leaves) and costs the GPU route 85% of its
coverage - 130169 pixels down to 18136, with the cut error improving to 1.03
px. That is the mechanism, not a bug: merges are the only lever that trades
far detail for near detail inside the residency cap, so without them the cut
refines near the camera until the cap evicts the distant pages outright
instead of coarsening them. What keeps the two feeds from fighting is the
demand filter and the coarsen hysteresis, which stay.

**A resolution ceiling, and where it is.** With terrain drawn on this route
for the first time, the sample loses the device to the watchdog at 1220x720:
about 15 s of GPU per frame against 1 s at 960x540, for 1.8x the pixels. The
cost is per-pixel and downstream of generation (the same 20 frames generate 22
pages at 540p), so it is the sample's lighting on pixels that were empty
before, not the emit. The G-buffer smoke test runs at 960x540 and now requires
pages to actually install; it passed at 720p before only because the route
installed nothing.

### Phase 31: the cut is decided per tile, and the arena is budgeted

**Per-tile LOD.** Deciding a whole page's level from its centre made voxels
visibly coarse at a page's near edge and fine at its far edge, worst from a
height where one page spans a wide depth range. The decision is now per tile:
a cluster's self sphere is its tile's square with the page's height range, and
its parent sphere is the parent page tile covering the same ground (a page
occupies one quadrant of its parent, so a child tile (tx,tz) sits inside
parent tile (4qx + tx/2, 4qz + tz/2)). Both sides compute that sphere by the
same rule, so the parent's own self test and the child's parent test are the
identical number and the cut stays exact - this is the Nanite grouping with
the group being the parent tile's ground and the parent's coarser voxels as
the simplified mesh. Residency stays per page; a page whose near tiles are too
coarse simply yields those tiles to its child while keeping the far ones.
`SparseVoxelTerrainGPUGenerationTests` checks the invariant directly: for every
tile of the parent's ground exactly one level draws, and with one child
missing the parent covers that whole quadrant (16 of 16 tiles).

**The decoded arena is a budget, not a side effect.** The scene derived
decoded capacity from the pages buffer, which is right for encoded assets and
meaningless here: this route streams no encoded bytes. `Settings` now takes
explicit decoded capacities, the pages buffer shrinks to 32 MiB, and
`--terrain-vg-memory-mb` (2048 by default, 216 bytes per emitted face) sizes
the arena. The reservation failure names the allocator and how much was free.

**Publishing leaves one at a time was seconds of scene update.** The commit
that first names two thousand directory pages published two thousand hierarchy
leaves, each its own buffer upload: 9.5 s in one frame. They now share one
upload (`updateRuntimeObjectPageLeaves`), and the per-slot directory scan in
the page-set sync became a hash lookup.

**Measured back to back, 200 frames, same conditions:**

| | GPU route | CPU route |
|---|---|---|
| Pages produced | 202 | 16 |
| Median frame | 164 ms | 130 ms |
| Wall clock | 89 s | 39 s |

So generation is 12.6x more productive per frame and about 5.5x cheaper per
page, while each frame is about a quarter heavier because generation rides in
the render submission. Absolute times on this machine drift upward over hours
of back-to-back GPU runs (the identical run measured 89 s and later 219 s), so
only same-session pairs are comparable.

**What remains.** Generation is not asynchronous: the count and emit
transactions ride in the frame's own submission, so a batch inflates that
frame (the worst is 3.3 s, half of it scene update and half GPU wait). The
shape that fixes it is one workgroup per cluster writing into a fixed page
slice, with atomic counters marking a page ready, so the CPU publishes what is
finished instead of pacing two round trips per page. That also removes the
count transaction: a fixed slice trades arena for latency, which the budget
above now makes affordable.

**The older reservation wall.** Before the budget, the arena was a hard wall: at ~370 resident pages the
decoded arena is full and refinement stops (`Terrain generated-VG reservation
failed`). Packing the decoded ranges by face rather than by whole clusters
buys about a quarter of it back; a page budget that evicts installed pages the
culler is not asking for is the next step, and is what makes the resident set
a policy rather than an arena size.

### Phase 32: the CPU cut advances one refinement per publication cycle

With the camera on a ridge the CPU route reaches twelve chunks and 120 px
triangles in three hundred frames. Every stage was measured rather than
guessed:

| stage | measurement |
|---|---|
| Demand published by the culler | 4 to 14 pages, every frame |
| Requests the streaming job receives | 1 |
| Chunks a job builds | 4 |
| Job body (plan, generate, publish) | 300 to 840 ms |
| Full cycle between jobs | about 4.5 s |
| Generation inside a job | 90 to 200 ms |

So the generator is idle roughly 98% of the time, and the cut advances by one
refinement per cycle. Two independent limits produce that:

- **The job cannot start while the previous result is publishing.** The gate is
  `!terrainUploadInFlight && !terrainGPUPageBuildPending()`, so generation and
  publication are strictly serialized even though they use different hardware.
- **A job receives one request.** `take()` returns one however large the cap,
  and calling it repeatedly returns nothing more: the coalesce that revalidates
  the queue is only ever handed one or two requests, though four to fourteen
  were published on that same frame. The loss is inside the queue's
  replace/enqueue/update path and is not yet explained.

Three real defects were fixed on the way, each measured on that camera:

- Refinement priority accumulated one demand band per oversized cluster, so it
  ranked a page by *how much* of it was coarse rather than *how wrong* the
  worst part was, and a large distant chunk outranked the chunk underfoot. It
  is now the worst projected voxel on the page, written with an atomic max.
- A chunk's demand was published for one frame and then suppressed for sixty.
  The request queue is a per-frame snapshot and jobs are seconds apart, so
  almost every demand was thrown away unread. Demand is republished every
  frame; the streamer already drops a slot it is building.
- The demand carried the render side's view of a chunk's level and generation,
  which lags the streamer by a commit, so requests were dropped as stale or
  same-level. Identity now comes from the streamer's own published page state,
  and the request asks for the finest level rather than naming one.

Together those took the same three hundred frames from twelve resident chunks
to fifteen, and the drawn cut from 1523 to 2029 clusters. They do not change
the cycle, which is the ceiling.

**`TerrainPageStreamingPipeline`** is the replacement for that cycle, built and
tested (`TerrainPageStreamingPipelineTests`, 28 checks): the frame's request
snapshot goes in, requests are ordered by rendered error so the nearest and
most wrong ground is built first, a ground already building or already built at
that generation absorbs further requests, N workers build whole pages off the
render thread, finished pages wait in a bounded queue and come back under a
per-drain byte budget, and admission stops when that queue is full. Payloads
move as pointers. The queue is a mutex-protected heap rather than lock-free:
the measured rates are hundreds of requests and single-digit completions per
frame against a 4.5 s stall, so the critical sections are nanoseconds.

It is not wired in. Wiring it means the streamer's whole-cut transaction
(`updateWorldVoxelChunkCache`) has to be callable per page, which is the
remaining work.

### Phase 33: the queue was cleared on the frame its demand arrived

Phase 32 left one link unexplained: four to fourteen pages were demanded every
frame, and the streaming job received one request with priority 1. The answer
is an ordering, not a policy. Every commit ran
`worldTerrainGPURefinementRequests.clear()` with the comment "wait for feedback
from the newly published cut" - but the culler's demand for that frame had
already been published into the queue moments earlier in the same finalize. So
the demand was discarded on every commit and the queue a job drained held
nothing but the behind-near-plane camera-owner fallback, which is what
priority 1 was.

The clear is now a revalidation. The coalesce already drops a request whose
slot generation the commit moved, which is exactly what the clear was
protecting against, while a request for ground the commit did not touch
survives and is still correct.

Measured on the world map, same camera, same 300 frames:

| | before | after |
|---|---|---|
| Requests a job receives | 1 | 4 to 8 |
| Chunks a job builds | 4 | 16 to 32 |
| Resident chunks | 12 | 57 |
| Drawn clusters | 1523 | 7458 |
| Job body | 300-840 ms | 561-619 ms |
| Median frame | 12.3 ms | 12.9 ms |

On the slope test at 700 frames the cut error fell from 64.07 px to 20.99 px,
and `SparseVoxelTerrainVGCutErrorContract`, which had been failing against its
56 px threshold, passes.

**What the speed exposed.** Both routes now refine fast enough to outrun their
own page installs, and both open holes doing it. The CPU route leaves thin
seams (142 enclosed pixels at worst, 0.06% of the image, on 561 of 698 frames;
boundary stitching makes them worse, so they are not shell sampling). The GPU
route is worse: with the demand flowing it reaches 1.12 px of cut error and
19245 drawn clusters, but leaves up to 10481 enclosed pixels on two thirds of
frames. Keeping every page the directory still names, rather than uninstalling
one whose record moved to a finer level, recovered coverage from 80065 to
115587 and is the right rule regardless - a page is the stand-in for every
level below it - but it did not close the holes.

Raising the GPU transaction from four pages to eight or sixteen is not the
cause: it multiplies generation (664 pages in 300 frames against 202, cut error
0.93 px) and the holes appear at four as well. The transaction size is left at
four until the transition is understood.

**Where the hole is not.** The per-tile cut cannot double-reject: a page whose
child is absent carries `DECODED_CLUSTER_FLAG_STREAMING_LEAF` on every cluster
of that quadrant, so its self error is zero and it draws at any distance. The
next step is to instrument which pixels are uncovered - by page and by tile -
rather than reasoning about the rule, because the rule as written cannot
produce them.

### Phase 34: the face tests read a neighbour table, and what churn costs

**Triangulating from the occupancy bits.** A voxel face is exposed when the
cell is set and its neighbour is not, so the whole face set of a brick is a
function of its 512 occupancy bits and its six neighbouring bricks. The
neighbour was the expensive half: the fine raster compacts only occupied
bricks, so a neighbour is not addressable by dense index and every boundary
face searched the page's brick list for a matching canonical origin. A page
holds roughly 150 bricks and about 1800 boundary faces each, so the search ran
tens of millions of comparisons per page and it is why the emit was the
slowest stage.

A stage now builds the six neighbour indices per brick once per transaction -
one thread per brick and face, the same search run 900 times instead of
millions - and both face passes read them. The extraction is otherwise
unchanged, which the GPU oracle test proves: it compares every emitted quad
against a CPU enumeration of the same height field and still matches exactly.

The next step from here is the bit-parallel form the table makes possible:
with the neighbour bricks known, all six exposed-face masks of a brick are
shifts and and-nots over its 16 occupancy words (`occ & ~(occ >> 1)` for +X,
`>> 8` for +Y, `>> 64` for +Z), face counts are popcounts, and the emission
offsets come from a prefix sum instead of atomics. That replaces 3072 threads
per brick with 16 words of bit work.

**What churn costs, measured three ways.** Every change that raises the rate of
page installs raises enclosed holes, and by the same mechanism:

| change | pages | cut error | worst enclosed | coverage |
|---|---|---|---|---|
| Baseline, four lanes | 202 / 200 frames | 2.65 px | 0 | 126172 |
| Eight lanes | - | - | 19453 | 90674 |
| Sixteen lanes | 664 / 300 frames | 0.93 px | 2010 | 82327 |
| Demand flowing, four lanes | 294 / 300 frames | 1.12 px | 10481 | 115587 |
| Plus neighbour table | 190 / 196 frames | - | 171 | 121865 |
| Plus prefetch at half budget | 246 / 252 frames | - | 17976 | 88934 |

Prefetching - demanding a page one level before it is needed, so the finer
geometry is resident by the time the cut asks for it - is reverted for that
reason, not because the idea is wrong. It is the right thing to do once a page
transition cannot open a hole.

Keeping every page the directory still names is now the retention rule: a page
is the stand-in for every level below it, and uninstalling one whose record
moved to a finer level opened quadrant-sized holes for as long as generation
took. That is also the cache behaviour to build on - a page that leaves the
cut keeps its geometry until the directory drops it, which is what makes a
future request free.

### Phase 35: the transaction was never generation-bound

The cut advanced once every 200 frames and residency crawled, which reads as
chunks that never reach their voxel size. Profiling the streaming job end to
end - not the sampler - found page building was under 5% of it.

| stage of one four-page transaction | before | after |
|---|---|---|
| Pack the material/palette arenas into words | 2100-2400 ms | 0 |
| Allocate and zero the arena mirror | 600-1200 ms | 120-260 ms |
| Build the upload plan | 625 ms at 111 pages | 30-48 ms |
| Bytes uploaded | 56 MiB | 2-16 MiB |
| Page workgroup (sampling, shell, hierarchy) | 30-70 ms | 24-50 ms |

**The repack.** `prepareTerrainBrickUploadPlan` packed all 203,726,448 material
bytes into words, copied all 203,726,448 micro-surface words, and built a
palette-descriptor array whose only use was a size comparison - to publish four
pages. Only ranges belonging to changed pages are ever read out of those
arrays, so only those ranges are packed now, straight into the batch.

**The arena.** The CPU route resizes a dense mirror of the whole arena every
transaction, about 1.9 GiB, and almost all of that cost is first-touch page
faults on a fresh allocation. The allocation is now recycled from the cut the
commit retires and zeroed in parallel. Clearing only the previous cut's page
ranges - which the induction says is equivalent - is not: staged spatial
entries publish into ranges the records do not name, and the terrain G-buffer
catches it as 37 pixels of non-zero motion on a static camera.

**The root offset.** 87 of 95 resident pages were republished every
transaction. Every one of them was a page whose node, child, material and
palette ranges were byte-identical and whose *root index* range had moved,
because root indices are packed by publication order. `samePhysicalRange`
compared all five at once, so a four-word move republished a 40,000-entry
material range. Each arena is now gated on its own range; a payload revision
change still republishes everything.

**Sampling.** Column samples are independent, so a page samples its grid in row
blocks, and a transaction with fewer pages than cores hands each page the
remainder. The build pool runs batch items on the submitting thread, which is
what makes that nested request safe. Two of the four full grids each page
sampled were the elevation anchor's, and when the anchor LOD is the page's own
LOD they are the column grid and the corner grid position for position, so
their minimum is already known. Halo columns no longer sample the six material
fields they then discard.

Same command and frame count as the baseline: 18 transactions instead of 6,
228 resident chunks by frame 800 instead of 51 by frame 900, median frame 17.7
ms instead of 21.6. The remaining main-thread cost on a commit frame is
`refreshTerrainVirtualGeometryPages`, 2 ms at 30 pages rising to 79 ms at 150.

**The GPU-owned route** (`--terrain-generation gpu-v2`) skips the arena, the
repack and the upload plan entirely: its generate stage measures 0-2 ms and its
whole cache update 7-53 ms. That is the answer to sub-millisecond generation,
and it is a routing choice, not a new implementation.

### Phase 36: map sampling was resolving its field on every sample

Page generation was 65 ms of map sampling per 6.4 m page, which is why a
transaction that built four pages could not keep up with a camera. None of it
was arithmetic. `sampleMapExportFieldAtWorld` resolves, on **every single
sample**:

- the field, by a linear scan over the bundle's fields comparing names, and
- its page, by a linear scan over that field's pages, repeated once per level
  `bestPage` has to walk up when the requested tile is not resident.

A page samples roughly ten thousand positions from twenty fields, and all of
them fall inside the same export page, so that resolution is identical every
time and costs more than the bilinear filter it precedes.

`MapExportFieldCursor` and `MapExportBiomeCursor` hold the resolution across a
batch. Reuse is exact - the cached page is taken only when the requested address
matches the one that produced it - and each parallel row block owns its own
cursors. Terrain resolves nineteen fields plus the biome pair once per block
instead of once per sample.

| 32-page transaction | before | after |
|---|---|---|
| Map sampling, summed | 1641-2562 ms | 271-444 ms |
| Page workgroup, wall | 315-363 ms | 72-125 ms |
| Per page | 65 ms | 8.5 ms |

Residency reaches 210 pages in 250 frames, against 99 before this change and 51
in 900 frames at the start of the session.

**And that moves the wall.** Generation is now 2.25 ms per page, and the
transaction is 457-2314 ms of arena zeroing and whole-cut publication: costs
proportional to the 1.9 GiB dense mirror and to total residency, not to the
pages built. Frame times got worse, not better - median 53 ms, p95 1521 ms -
because throughput rose and those per-transaction costs now land far more often.

The next thing is therefore architectural, not another optimisation: the CPU
should generate page payloads and publish them, with no dense arena mirror and
no cut. The pieces exist - `installRuntimeObjectPage` /
`installSparseVoxelTerrainVGScenePage` install one page into a slot of a single
runtime object, and `TerrainGPUVirtualGeometryPageSet` already maps the
directory onto slots with parent links and per-quadrant streaming-leaf flags.

### Phase 37: the transaction was maintaining a dense mirror, not building pages

With generation at 2.25 ms per page, a 32-page transaction over 210 resident
pages still cost 2314 ms. None of it was the pages:

| stage | ms |
|---|---|
| `generate` - building the 32 pages | 80 |
| `arenaShape` - zeroing the 1.9 GiB dense CPU mirror | 601 |
| `publishEntries` - re-translating **all 210** resident pages into it | 1300 |
| everything else | 333 |

82% was maintaining a dense CPU copy of the whole GPU arena: wipe it, rewrite
every resident page into it including the 178 unchanged ones, then diff page
revisions to decide what to upload. Virtual geometry reserves a range and
uploads the page; this reserved a range and rebuilt the world around it.

Two changes, no ABI move:

**The arena is never zeroed.** No consumer reads a byte outside a published
page's range - every read goes through a record - and inside a range the bytes
are either rewritten now or already correct from whichever transaction wrote
them. Each written range still clears its tail to the full reservation, because
the upload publishes the whole reservation.

**A page already written at those exact offsets from that exact payload is not
translated again.** The commit hands the next job the records of the cut that
last published into that arena; a page matching on mapping id, payload revision
and all four range offsets reuses its stored node revision and only has its root
index range rebased, because roots are packed by publication order and move for
almost every page.

| 32-page transaction | before | after |
|---|---|---|
| `arenaShape` | 601 ms | 0 ms |
| `publishEntries` | 1300 ms | 43-266 ms |
| Total | 2314 ms | 87-319 ms |
| Pages translated | all 210 | 11-64 of 336 |

Residency reaches 336 pages in 250 frames. The G-buffer contract's geometry
fields - coverage, surface, face, instance id, voxel id, material id, normals,
depth - are all zero, so reusing the arena does not corrupt geometry.

**The wall is now the install, on the main thread.**
`refreshTerrainVirtualGeometryPages` costs 443-1480 ms at every commit and grows
with page count, because the CPU route still registers one virtual-geometry
object per (ground, level) and re-schedules them. That is the piece to replace
with page-slot installs into a single runtime object.

### Phase 38: the install was copying meshed pages, not installing them

With the transaction down to 87-319 ms on a worker, the main thread was still
spiking to 2.5 s. It was not the meshing, which is already off-thread, and not
the scene apply, which is under 16 ms. It was `collectStaleChunks`, at up to
**1568 ms** in a single refresh.

The slot-adoption step snapshots the entries it is about to overwrite, because
two chunks swapping slots each adopt the other's page and writing one in place
first would lose the other. It took that snapshot by **copying** the entries -
and an entry owns its encoded virtual-geometry page, so every adoption
duplicated megabytes of geometry.

Moving is both faster and more correct. `regionMappingId` is the ground's
identity and is unique, so a source matches exactly one destination; the page
belongs to the ground that is now at the destination slot, and the source slot
now holds different ground whose own entry is stale and is rebuilt or adopted in
the same pass. The source is cleared so a moved-from entry can never read as
valid with no geometry.

| main thread | before | after |
|---|---|---|
| `collectStaleChunks`, worst | 1568 ms | 140 ms |
| Whole VG refresh, worst | 1480 ms | 140 ms |
| Frame median | 35 ms | 18.6 ms |
| Frame p95 | 1584 ms | 200 ms |
| Frame max | 2501 ms | 886 ms |

### Phase 39: the refresh was freeing geometry on the render thread

`refreshTerrainVirtualGeometryPages` sat at 83 ms. Splitting it found none of it
was meshing (already off-thread) and almost none was the scene apply:

| phase of one refresh | ms |
|---|---|
| `resize` | 0.0 |
| adoption `detect` | 0.5 |
| adoption `apply` (119 adoptions) | **43.8** |
| `scan` | 0.2 |
| `sort` | 0.8 |

The adoption apply overwrites the entry at a destination slot, and an entry owns
its encoded virtual-geometry page - so every adoption was a multi-megabyte
`free()` on the thread that has to present a frame. Two changes:

**The adoption search is indexed.** It was a linear pass over every entry for
every stale chunk, quadratic in resident pages. It is now a hash of
`(regionMappingId, clipmapLOD, brickPayloadRevision)` built once per refresh.

**Displaced pages are released off-thread.** The cache collects entries
displaced by adoption or eviction into `pendingRelease_`; the caller takes them
and destroys them on a worker. The free is unavoidable; being on the render
thread was not.

| main thread | before | after |
|---|---|---|
| Adoption apply | 43.8 ms | 0.1 ms |
| Whole VG refresh, worst | 79.5 ms | **4.1 ms** |
| Scene apply, worst | 123 ms | 16-62 ms |
| Frame median | 19.9 ms | 16-19 ms |
| Frame p95 | 234 ms | 68-294 ms (varies with how far streaming gets) |

**What is left on the render thread** is the per-transaction publication:
`File I/O / streaming / uploads` peaks at 300-720 ms on commit frames at high
residency, of which upload staging is 17-60 ms and the rest is the Tree64 page
retirement, field publication and page-table work. That block is per
*transaction*, not per page, which is exactly what the page-slot architecture
removes: a page is generated, written to its reserved slice, and installed on
its own.
