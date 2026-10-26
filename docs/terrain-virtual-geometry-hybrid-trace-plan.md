# Terrain / Virtual Geometry Hybrid Trace Hierarchy Plan

## Short feasibility answer

Yes, it is feasible to merge the useful parts of CWBVH, octree-style traversal
and Tree64 into one installed-geometry hierarchy, but the likely winning shape
is not a literal CWBVH replacement for Tree64.

The current Tree64 node is already extremely compact for cubic occupancy:

```text
u32 childBaseAndFlags
u32 populationMaskLo
u32 populationMaskHi
```

Children are fixed as a 4x4x4 grid, so each node covers 64 possible child
cubes. Child bounds are implicit from `(nodeOrigin, nodeSide, childIndex)`.
That means a populated child costs only one bit plus a compact child offset
computed with `countOneBits`. CWBVH pays for something Tree64 gets for free:
quantized child bounds.

CWBVH is still useful as a source of ideas:

- compressed per-child metadata;
- quantized conservative child bounds;
- ray-sign-aware child ordering;
- compact child groups with internal/leaf masks;
- explicit benchmarkable variants rather than one assumed best path.

For cubic terrain/voxel pages, the strongest candidate is therefore:

```text
installed geometry page
  -> trace view: compact implicit cubic occupancy, Tree64-like
  -> culling/refinement view: errors, request state, page residency metadata
  -> optional bounds sidecar: only for non-cubic/cluster geometry
```

The trace path should read only the trace view unless an LOD/error early-return
mode is enabled. Culling and streaming can read the heavier sidecar buffers.
This preserves the bandwidth profile that makes Tree64 attractive while still
letting virtual geometry convergence use the same installed pages.

## What the current code already gives us

Current Tree64 facts:

- `SparseVoxelTree64NodeGPU` is 12 bytes, or 16 bytes in padded variants.
- Children are indexed as `x + 4 * y + 16 * z`.
- Internal nodes store a compact child base; terminal nodes use only the
  population mask.
- Terrain page descriptors already publish immutable node/material pages with
  generation checks.
- Dynamic terrain instances already validate terrain generation, descriptor
  identity, node page generation and material page generation before tracing.
- Traversal already has an LOD cutoff:

```text
cutoff(t) = lodWorldSize + lodDistanceSlope * (t - lodStartDistance)
```

So a conservative early hit below a selected world-size threshold already fits
the traversal model.

Current CWBVH facts:

- The repo CWBVH builder creates BVH8 nodes.
- Each compressed node stores parent origin, per-axis exponent scales, child
  base indices, child metadata bytes and 8-bit quantized child bounds.
- The trace shader tests 8 child AABBs in two groups of four and builds hit
  masks that are ordered by ray octant.
- The packed CWBVH node is 80 bytes for 8 arbitrary child bounds. That is
  excellent for triangle/cluster BVHs, but expensive for fixed cubic grids
  where child bounds are implicit.

## Node-shape recommendation

### Keep 64-way cubic occupancy as the baseline

For terrain and cubic voxel bricks, Tree64's 4x4x4 node is still the baseline
to beat. An octree-like 2x2x2 node has cheaper masks but more levels and more
node visits. A 16-child or 32-child node is tempting because it fits in a
`vec4<u32>`, but it is not naturally cubic:

- 8 children: 2x2x2, isotropic, deeper tree.
- 16 children: usually 4x2x2 or 4x4x1, anisotropic.
- 32 children: usually 4x4x2, anisotropic.
- 64 children: 4x4x4, isotropic, current Tree64.

The current 64-child node already fits in three `u32`s. The padded path already
makes it a `vec4<u32>` when backend alignment makes that better. So reducing
branching only to fit a float4 is probably a trap.

### Add a sidecar rather than bloating the trace node

The trace node should stay hot and tiny:

```cpp
struct HybridTraceNode64
{
  uint32_t childBaseAndFlags;
  uint32_t occupancyMaskLo;
  uint32_t occupancyMaskHi;
  // optional/padded:
  uint32_t sidecarIndexOrFormat;
};
```

The culling/error sidecar can be colder:

```cpp
struct HybridCullNode64
{
  uint32_t errorRange;       // min/max or exponent/scale encoding
  uint32_t childErrorQ0;     // 4x u8
  uint32_t childErrorQ1;     // 4x u8
  // ...
  // up to 16 words if storing all 64 child errors as q8
};
```

Do not put 64 child errors in the trace node unless benchmark data proves the
early-return saves more than the bandwidth costs. Most rays should avoid this
buffer.

### Use bounds sidecars only when geometry is not cubic

For installed virtual-geometry clusters, CWBVH-style quantized bounds can make
sense. For terrain bricks/cells, it duplicates implicit information.

Recommended split:

```text
format = Cubic64
  trace: implicit child cubes + occupancy mask
  cull: projected error / residency sidecar

format = Bounds8 or Bounds16
  trace: quantized child bounds + masks
  cull: error / residency sidecar
```

This lets arbitrary installed geometry use CWBVH-like compression without
penalizing terrain pages.

## Error and LOD early-return design

There are two different early-return meanings, and they must not be blurred:

1. Conservative any-hit early return:
   - valid for shadows, AO, GI visibility and "is something there?" rays;
   - can report an occluder earlier/coarser than the true surface;
   - must not miss occupied geometry.

2. Exact/primary early return:
   - valid only when the selected render LOD says this node is the surface;
   - must preserve material, identity and depth contracts;
   - cannot simply return on coarse occupancy unless the renderer accepts that
     as the installed geometry for this pixel.

For visibility rays, use a camera/projected-error rule:

```text
projectedError = nodeError / max(distance(camera, nodeBounds), epsilon)
if projectedError <= traceErrorThreshold:
    return conservative hit
```

For scene lighting rays that start at a receiver and go toward a light, the
camera is still the right source for visible-quality error. Ray `t` alone is
not enough because a shadow ray may move away from the camera while still
affecting a camera-visible receiver.

Suggested node error encoding:

```text
pageErrorMin, pageErrorMax as f16/f32 in descriptor
per-node or per-child q8 error = round((error - min) / (max - min) * 255)
```

For culling queues:

- use max error to decide "must expand";
- use min error to decide "definitely do not expand";
- keep exact f32 error for borderline cases only if needed.

The user idea of local min/max quantization makes sense, but "local to the
queue" should probably mean local to the page or dispatch batch, not a mutable
per-frame queue encoding in the node itself. Immutable pages are much safer
with the current streaming/publication model.

## Traversal strategy

### Baseline: region/DDA Tree64 traversal

Keep the current region traversal as the control. It marches sparse regions
instead of testing all children and sorting candidates. This is well matched to
fixed cubic occupancy and shallow pages.

### Add ordered-child experiments

Borrow from CWBVH only where it helps:

- ray-octant ordering for children within a node;
- compact hit masks;
- subgroup sharing of upper nodes;
- small local stack only when testing multiple child candidates beats region
  marching.

For Tree64/Cubic64, an ordered stack path should be an experiment, not the
default. Testing all 64 child boxes is likely too expensive. A cheaper variant
is to group children into 8 octants of 2x2x2 children, intersect the octant
groups, then region-march inside the selected group.

### Stack recommendation

Use three traversal variants:

```text
region
  current default; no explicit per-ray candidate stack

grouped-octant
  test 8 coarse groups inside a Cubic64 node; push only hit groups

bounds-wide
  CWBVH-like stack for Bounds8/Bounds16 arbitrary child bounds
```

For local stacks, prefer a fixed small stack in function memory first. Only
move to workgroup/shared stack if benchmarks show register pressure or spills.
The current CWBVH shader already has a TODO for shared stack; it should be
measured, not assumed.

### Error-ordered traversal policy

For conservative visibility traversal, candidate quality is at least as
important as `t` ordering. A lower-error hit should override an earlier or
nearer higher-error coarse hit, because the goal is "best acceptable occluder
under the current camera error budget", not simply "first coarse box touched".

Recommended rule:

```text
candidate key = (knownError, nearT, childIndex)

if bestHit exists and candidate has known error > bestHit.error:
    prune candidate

if candidate hit has lower error than bestHit:
    replace bestHit even when it is farther along the ray

if candidate hit has equal error:
    prefer smaller nearT
```

Exact/material/primary traversal should not use this rule by default. Exact
mode remains nearest-hit ordered unless the caller explicitly opts into coarse
fallback. Unknown-error candidates cannot be pruned by this rule; they may be
sorted behind known low-error candidates but still need an exact fallback path.

## Installed-geometry integration

The goal should be "one installed page, multiple views", not "no structure at
all". Rays still need a structure; the win is making that structure one of the
published installed-geometry views instead of a separately rebuilt BLAS.

Proposed installed page:

```text
InstalledGeometryPage
  descriptor:
    page id
    generation
    geometry revision
    format
    root node
    trace node range
    material range
    culling sidecar range
    bounds sidecar range, optional

  trace nodes:
    Cubic64 or Bounds8/Bounds16

  materials:
    existing material/micro payload references

  culling sidecars:
    error, request bits, residency state, feedback counters
```

Publication remains double-buffered/immutable:

```text
active page descriptor -> currently traceable geometry
staging page payload    -> written by CPU/GPU installer
publish CAS             -> swaps descriptor only after all views are complete
retire old payload      -> after frame completion fence
```

The culling system may read both active and staging bookkeeping, but the trace
shader must read only the active committed descriptor. That preserves the
current no-holes streaming invariant.

## Direct virtual-hierarchy tracing model

The end state should not require an external Tree64 sidecar that is generated,
scheduled and published as a second representation of the same geometry. The
virtual voxel hierarchy itself should own the traceable representation.

The important distinction is that "no external Tree64" does not mean "no ray
hierarchy". It means the ray hierarchy is one installed view of the same
virtual page payload that rasterization, culling and streaming already consume.

Recommended page-local shape:

```cpp
struct VoxelTracePageGPU
{
  uint32_t pageId;
  uint32_t generation;
  uint32_t geometryRevisionLo;
  uint32_t geometryRevisionHi;

  uint32_t format;        // Cubic64, Brick8, Bounds8, Bounds16, ...
  uint32_t rootNode;
  uint32_t traceNodeOffset;
  uint32_t traceNodeCount;

  uint32_t brickOffset;
  uint32_t brickCount;
  uint32_t materialOffset;
  uint32_t materialCount;

  float localOriginX;
  float localOriginY;
  float localOriginZ;
  float rootSide;

  float errorMin;
  float errorMax;
  uint32_t errorSidecarOffset;
  uint32_t flags;
};
```

The hot node for cubic voxel/terrain pages should remain Tree64-shaped:

```cpp
struct VoxelTraceNode64GPU
{
  uint32_t childBaseAndFlags;
  uint32_t occupancyMaskLo;
  uint32_t occupancyMaskHi;
  uint32_t payloadOrSidecar;
};
```

This is deliberately a Tree64-like encoding, but it is not an external Tree64
system. It is the trace view embedded in the installed virtual page. A page
install publishes this trace view together with its raster brick payload,
material payload, culling sidecars and dependency rewrites.

The current virtual geometry hierarchy already carries culling bounds, child
counts, page indices and LOD errors. That hierarchy should remain the cut and
streaming authority. The ray path should enter through the selected resident
page/fallback, then traverse the page-local `VoxelTraceNode64GPU` or
non-cubic bounds format.

```text
shared virtual hierarchy / page DAG
  -> choose resident page or fallback
  -> trace page-local microtree
  -> resolve brick / material / microgeometry payload
```

This avoids a second asynchronously generated Tree64 sidecar while preserving
the bandwidth and traversal properties that made Tree64 useful.

## Screen cut vs trace cut

The screen cut and trace cut must share identity, generation and fallback
rules, but they must not be forced to be the exact same leaf set.

```text
shared page table / virtual hierarchy
  -> screen cut: pages selected for visible rasterization
  -> trace cut: pages selected by rays, including offscreen occluders
```

If tracing only sees the pages selected for the screen, offscreen occluders
disappear. Shadows, GI and reflections then become camera-frustum dependent:
turning the camera can change lighting from geometry that should still exist.
That is not acceptable.

The invariant is:

```text
Every renderable page has a conservative traceable ancestor resident.
```

For in-view receiver geometry:

- the G-buffer stores the selected page id, generation, local cell/brick id and
  selected render LOD;
- rays spawned from that receiver start from the same page identity;
- self-shadow rejection can skip the receiver cell or receiver page generation;
- exact reflections may require the same page/LOD as the screen cut or report
  `Unresolved` until it is resident.

For offscreen occluder geometry:

- tracing starts from scene roots or virtual page roots, not from visible
  raster pages only;
- when a ray reaches a missing desired page, it emits a trace page request;
- while the desired page is missing, visibility rays trace the resident
  conservative parent/fallback;
- exact rays may trace a marked coarse fallback only when the caller accepts
  coarse results, otherwise they report `Unknown`/`Unresolved`.

So traces "match the screen cut" in the sense that visible receivers use the
same selected page identity and generation. Occluders may be coarser or
different from the screen cut, but must be conservative, non-stale and selected
from the same shared hierarchy.

## Trace-driven page requests

Trace traversal must be able to request pages that are not currently visible.
The request system should merge three producers:

```text
1. Raster feedback
   visible pages needed for the screen cut

2. Trace feedback
   offscreen pages touched by shadows, GI, reflections or primary fallbacks

3. Stability feedback
   parent/fallback pages that must remain resident to prevent holes
```

Trace requests should be bounded, deduplicated and prioritized before they
reach CPU/GPU streaming. A proposed request record:

```cpp
struct SparseVoxelTracePageRequestGPU
{
  uint32_t pageId;
  uint32_t producerId;
  uint32_t desiredLOD;
  uint32_t rayClass;        // shadow, GI, reflection, primary fallback

  uint32_t receiverPageId;
  uint32_t receiverGeneration;
  uint32_t flags;
  uint32_t priority;

  float desiredError;
  float rayConeWidth;
  float minimumT;
  float maximumT;
};
```

Missing trace pages must not silently become misses:

```text
visibility ray:
  desired fine page missing
    -> request fine page
    -> trace conservative resident parent

exact/reflection ray:
  desired fine page missing
    -> request fine page
    -> return unresolved or explicitly trace a coarse fallback
```

The scheduler should retain roots and conservative parent pages as pinned or
high-priority dependencies. A fine page can be evicted only when its parent
fallback remains resident, unless no ray/raster consumer can reference that
part of the hierarchy.

## Matching and mismatch policy

Trace and raster agree through identity, not through incidental physical
buffer addresses. Every hit or fallback should carry enough identity to reject
stale data:

```text
producer id
logical page id
page generation
geometry revision
selected LOD / trace format
material payload generation
```

Failure policy:

- stale generation: reject, request current page, use conservative ancestor if
  valid;
- missing root/fallback: report traversal failure, never emit "empty";
- missing fine occluder for visibility: conservative fallback;
- missing fine receiver for exact ray: unresolved;
- non-conservative fallback: invalid for implicit visibility fallback.

This is the rule that prevents the new direct hierarchy from reintroducing the
holes/blackout class we just tested around Tree64 page streaming.

## Benchmark plan

Do not decide this by taste. The plan needs microbenchmarks and real scene
benchmarks.

### 1. Microbenchmarks

Extend the existing Tree64 GPU benchmark coverage with compile-time traversal
variants:

- `tree64-region` baseline;
- `tree64-padded-region` baseline alignment check;
- `tree64-mirrored-ancestor` existing fast-path comparison;
- `hybrid64-error-sidecar-off`;
- `hybrid64-error-sidecar-on`;
- `hybrid64-grouped-octant`;
- `bounds8-cwbvh-style` for non-cubic clusters;
- optional `octree8` only as a sanity baseline.

Measure:

- GPU dispatch time;
- rays/sec;
- node visits/ray;
- queries/ray;
- bytes/ray, estimated from node and sidecar loads;
- max stack depth;
- query-limit rate;
- hit/miss parity against current Tree64;
- conservative false-hit rate for any-hit LOD mode.

Use synthetic cases:

- dense cube;
- sparse random occupancy;
- diagonal terrain-like surface;
- thin wall / one-voxel bridge;
- checkerboard worst case;
- nested LOD cut with mixed coarse/fine pages.

### 2. Real renderer benchmarks

Use existing benchmark entry points:

```sh
./SparseVoxelRendererSample \
  --shadow-backend tree64 \
  --benchmark-shadow tree64-static.json \
  --benchmark-sequence static \
  --benchmark-warmup 120 \
  --benchmark-frames 600 \
  --benchmark-resolution 1280x720

./SparseVoxelRendererSample \
  --benchmark-lighting hybrid64-camera-walk.json \
  --benchmark-sequence camera-walk \
  --benchmark-warmup 120 \
  --benchmark-frames 600 \
  --benchmark-resolution 1200x720 \
  --lod-threshold 0.75 \
  --tree64-lod-threshold 1.5 \
  --render-scale 1.0 \
  --no-taa \
  --disable-metalfx-upscaler \
  --disable-frame-generation \
  --disable-vsync
```

Add variant switches such as:

```text
--tree64-node-format cubic64|hybrid64|bounds8
--tree64-traversal region|mirrored-ancestor|grouped-octant|wide-bounds
--tree64-error-sidecar off|cull-only|trace-lod
--tree64-error-quantization none|q8-page|q8-batch
```

Real workloads:

- `static`;
- `camera-walk`;
- `camera-yaw`;
- `interior-static`;
- `roof-down`;
- generated terrain with active page streaming;
- terrain lighting with the no-regression harness used for holes/residency.

Collect:

- benchmark JSON CPU/GPU totals;
- `tree64_planning_ms`;
- `tree64_upload_ms`;
- virtual trace page request count;
- offscreen trace request count;
- fallback trace count;
- unresolved exact trace count;
- committed Tree64 page count;
- upload bytes;
- scene-ray stats;
- Tree64 invalid reason counters;
- terrain hole diagnostics;
- output hashes for correctness modes.

Run Release under Metal System Trace for final timing. The README already warns
that diagnostic readbacks and built-in timestamps can perturb renderer
benchmarks; keep performance captures clean.

### 3. Acceptance criteria

Keep the new path only if it satisfies all correctness gates and at least one
performance gate:

Correctness:

- exact primary rays match current Tree64 when LOD/error early-return is off;
- any-hit conservative mode never misses an occupied baseline hit;
- no `UNKNOWN_RESIDENCY` caused by descriptor/page generation mismatch;
- offscreen occluders remain traceable when outside the camera frustum;
- missing trace pages request refinement instead of producing false misses;
- screen receivers and trace receivers agree on page id/generation;
- terrain close-slope hole checks remain zero for strict fixtures;
- moving terrain lighting residency remains clean.

Performance:

- at least 10% faster scene-ray/tree64 GPU time on one real workload with no
  more than 5% regression on the others; or
- at least 20% lower trace node memory/upload bandwidth with neutral GPU time;
  or
- significantly lower culling/refinement queue pressure with neutral tracing.

If a variant only helps culling but slows tracing, keep it as a culling sidecar
and do not put it on the hot trace path.

## Implementation phases and tests

### Phase 0: document invariants and baseline numbers

- Add this plan to the convergence docs.
- Capture current Tree64 benchmark JSON for static, camera-walk and terrain
  lighting.
- Archive node count, upload bytes, node visits/ray and scene-ray stats.

Tests:

- doc-only phase; no runtime tests required;
- preserve the current terrain/Tree64 regression sweep as the baseline.

Status: complete for the initial local baseline capture.

Captured on 2026-08-31 from a local Debug/Vulkan build at 640x360, 30 warmup
frames and 60 measured frames. These numbers are good regression anchors for
the convergence work, but they are not final performance acceptance numbers.
Final timing still needs Release captures with diagnostic readbacks disabled.

Raw benchmark JSON:

```text
build/terrain-virtual-geometry-phase0-baselines/tree64-static-shadow.json
build/terrain-virtual-geometry-phase0-baselines/tree64-camera-walk-shadow.json
build/terrain-virtual-geometry-phase0-baselines/tree64-terrain-camera-walk-lighting.json
```

Commands used:

```sh
build/tests/SparseVoxelRendererSample \
  --no-terrain \
  --gbuffer-resolver svraster \
  --shadow-backend tree64 \
  --benchmark-shadow build/terrain-virtual-geometry-phase0-baselines/tree64-static-shadow.json \
  --benchmark-sequence static \
  --benchmark-warmup 30 \
  --benchmark-frames 60 \
  --benchmark-resolution 640x360 \
  --occupancy-extent 512 \
  --no-taa \
  --disable-metalfx-upscaler \
  --disable-frame-generation \
  --disable-vsync

build/tests/SparseVoxelRendererSample \
  --no-terrain \
  --gbuffer-resolver svraster \
  --shadow-backend tree64 \
  --benchmark-shadow build/terrain-virtual-geometry-phase0-baselines/tree64-camera-walk-shadow.json \
  --benchmark-sequence camera-walk \
  --benchmark-warmup 30 \
  --benchmark-frames 60 \
  --benchmark-resolution 640x360 \
  --occupancy-extent 512 \
  --no-taa \
  --disable-metalfx-upscaler \
  --disable-frame-generation \
  --disable-vsync

build/tests/SparseVoxelRendererSample \
  --world-map world-data \
  --terrain-render-distance-km 5 \
  --shadow-backend tree64 \
  --benchmark-lighting build/terrain-virtual-geometry-phase0-baselines/tree64-terrain-camera-walk-lighting.json \
  --benchmark-sequence camera-walk \
  --benchmark-warmup 30 \
  --benchmark-frames 60 \
  --benchmark-resolution 640x360 \
  --lod-threshold 0.75 \
  --tree64-lod-threshold 1.5 \
  --render-scale 1.0 \
  --no-taa \
  --disable-metalfx-upscaler \
  --disable-frame-generation \
  --disable-vsync
```

Baseline summary:

| Capture | Workload | Sequence | Terrain streaming | CPU avg/max ms | GPU avg/max ms | Tree64 plan avg/max ms | Tree64 upload avg/max ms | Trace rays | Trace status counts |
| --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- |
| `tree64-static-shadow.json` | shadow diagnostic | static | none | 10.369 / 17.084 | 7.258 / 8.215 | 0.002339 / 0.006791 | 0.000 / 0.000 | 57,600 | `[38279, 19321, 0, 0]` |
| `tree64-camera-walk-shadow.json` | shadow diagnostic | camera-walk | none | 11.524 / 16.896 | 8.510 / 9.592 | 0.002701 / 0.006875 | 0.000 / 0.000 | 57,600 | `[27905, 29695, 0, 0]` |
| `tree64-terrain-camera-walk-lighting.json` | scene lighting | camera-walk | progressive chunks | 14.720 / 23.328 | not serialized | 0.005258 / 0.008500 | 0.000 / 0.000 | 0 | `[0, 0, 0, 0]` |

Memory snapshot shared by these captures:

| Field | Bytes |
| --- | ---: |
| Occupancy base persistent | 33,554,432 |
| Occupancy pyramid persistent | 5,543,256 |
| Occupancy total persistent | 39,097,688 |
| Tree64 directory | 23,440 |
| Tree64 node pool reserved | 12 |
| Tree64 build scratch | 0 |
| Tree64 total persistent | 23,452 |
| Sky guide total | 176,640 |
| Brick micro total buffer | 0 |

Terrain lighting startup log baseline:

```text
Generated world terrain: source=world-data resident=6/512 renderDiameter=10.00km finestVoxel=0.100m
Terrain Tree64 build: models=6 instances=6 boxes=0 nodes=6 finestSide=256 worstCellScale=1 worstPageNodes=8192 slot=8192 revision=1
Terrain GPU runtime lane: queued=6 atlas=561.17MiB denseBricks=4096 treeCandidates=266305
```

Known Phase 0 instrumentation gaps:

- the current benchmark JSON does not serialize node visits/ray or bytes/ray;
- scene-lighting benchmark JSON does not serialize scene-ray trace hashes,
  diagnostic ray counts or GPU total timing in this capture;
- terrain streaming counters are visible in runtime logs but not persisted into
  the benchmark JSON;
- offscreen trace requests, fallback trace counts and unresolved exact trace
  counts do not exist yet.

These gaps are promoted into Phase 1/4/12 instrumentation work. Until then,
the focused terrain and scene-ray regression tests remain the correctness
baseline for holes, stale Tree64 descriptors and ray tracing parity under the
new streaming model.

### Phase 1: direct trace-page ABI prototypes

- Add CPU structs for `VoxelTracePageGPU`, `VoxelTraceNode64GPU` and optional
  `VoxelTraceErrorSidecarGPU`.
- Add pack/unpack tests for occupancy masks, child bases and q8 errors.
- Add shader decode tests mirroring the CPU ABI.
- Keep current `SparseVoxelTree64NodeGPU` unchanged.

Tests:

- CPU ABI size/alignment tests;
- CPU pack/unpack round-trip tests for all 64 child bits;
- shader readback tests for node decode, terminal flags and compact child
  offsets;
- q8 error quantization tests for zero range, saturated range and normal range.

Status: complete for the initial ABI prototype.

Implemented:

- `VoxelTracePageGPU`, `VoxelTraceNode64GPU` and
  `VoxelTraceErrorSidecarGPU` CPU ABI definitions;
- Cubic64 child-index, occupancy-mask, child-base, terminal-node and q8
  error pack/unpack helpers;
- a WGSL mirror in `sparse_voxel_virtual_trace_common.wgsl`;
- a tiny WGSL ABI smoke shader that compiles the mirror helpers to SPIR-V;
- `SparseVoxelVirtualTraceABIContractTests` covering CPU size/alignment,
  field offsets, all 64 child bits, compact child offsets and q8 edge cases.

Intentional Phase 1 boundary:

- `SparseVoxelTree64NodeGPU` remains unchanged;
- no renderer or traversal routing uses the new ABI yet;
- GPU readback tests for actual node traversal/decode are promoted to Phase 5,
  where the direct Cubic64 trace shader exists and can be compared against the
  current Tree64 oracle.

### Phase 2: shared page-table trace identity

- Extend the shared page table entry with trace-view ranges and trace format.
- Add active/staging trace payload ranges to the physical allocator.
- Publish trace view only with the same page generation and geometry revision
  as the render/culling payload.
- Add uninstall rewrites that restore parent/fallback traceability before
  removing child pages.

Tests:

- installing a page publishes hierarchy, trace, brick and material ranges
  atomically;
- stale trace generation is rejected;
- missing fallback root prevents page selection rather than producing empty
  space;
- uninstalling a child retains or restores a resident parent trace fallback;
- retired trace ranges are not reused before the completion fence.

Status: complete for the initial shared page-table trace identity plumbing.

Implemented:

- optional `traceNodes` and `traceErrorWords` physical ranges in
  `SparseVoxelPhysicalPageAllocation`;
- shared `geometryRevision` and `traceFormat` metadata on the allocation and
  shared page-table entry;
- trace allocator capacities and stats through `TraceNodes` and
  `TraceErrorWords` buffer kinds;
- up-front validation that trace allocations carry a non-empty trace node
  range, valid trace format and nonzero geometry revision;
- install publication that exposes trace format/revision only when the page
  becomes resident;
- fenced retirement/reclaim for trace-node and trace-error ranges alongside
  hierarchy, brick, material and Tree64 ranges;
- produced-payload reservation checks that reject mismatched trace geometry
  revisions before allocation.

Tests added/extended:

- Phase 2 shared-page-table test now installs a child page with both current
  Tree64 sidecar payload and the new direct trace view;
- verifies resident traceability requires matching logical page generation,
  geometry revision and trace format;
- verifies evict-pending trace views are not selectable;
- verifies trace ranges are not reused before the completion fence and are
  reused only after reclaim;
- verifies malformed trace payloads do not reserve allocator ranges.

Intentional Phase 2 boundary:

- the renderer still traces through the existing Tree64 path;
- direct Cubic64 traversal does not consume `traceNodes` until Phase 5;
- page-table identity is now ready for Phase 3 culling sidecars and Phase 4
  trace-driven requests.

### Phase 3: sidecar-only culling experiment

- Generate culling sidecars beside current Tree64 pages.
- Use q8 page-local error encoding for expansion decisions.
- Do not change trace shaders yet.
- Benchmark queue size, expansion rate and CPU/GPU culling cost.

Tests:

- culling decisions match f32 reference decisions within the declared q8 error
  tolerance;
- page requests are stable/deduplicated for unchanged camera input;
- min/max error ranges reject "must expand" and "definitely skip" cases
  correctly;
- sidecar absence falls back to exact f32/current path.

Status: complete for the initial sidecar-only streaming experiment.

Implemented:

- page-local q8 child-error packing helpers and CPU cull-decision helpers that
  return `DefinitelySkip`, `MustExpand` or `ExactFallback` using the declared
  quantization uncertainty;
- a WGSL mirror for the q8 cull-decision helper, compiled by the virtual-trace
  ABI smoke shader;
- an independent `cullErrorWords` physical range in
  `SparseVoxelPhysicalPageAllocation`, separate from direct trace nodes and
  separate from the current Tree64 payload;
- shared page-table capacities, allocation validation, install publication,
  retirement and fenced reclaim for the culling sidecar range;
- produced terrain page payloads now carry a geometry revision so cull sidecars
  can be tied to the same immutable geometry identity as Tree64/direct-trace
  payloads.

Tests added/extended:

- `SparseVoxelVirtualTraceABIContractTests` now verifies q8 culling sidecar
  word counts, encoded endpoints, decoded-error tolerance, skip/expand/fallback
  decisions, null/out-of-range fallback, zero-range behavior and malformed
  input rejection;
- Phase 3 convergence test installs a terrain page with a current Tree64
  payload plus culling sidecar, while leaving direct trace disabled, proving the
  sidecar can live beside the installed geometry without changing traceability;
- Phase 3 convergence test verifies malformed cull sidecars with missing
  geometry revision are rejected before allocation;
- Phase 3 convergence test verifies sidecar ranges retire with the page and are
  not reused before the completion fence, then are reused after reclaim.

Intentional Phase 3 boundary:

- trace shaders still do not consume the culling sidecar;
- the renderer culling path is not switched to sidecar decisions yet;
- benchmark counters for queue size, expansion rate and CPU/GPU culling cost
  still need the Phase 12 metrics hooks once the renderer path consumes the
  sidecar;
- direct Cubic64 traversal remains Phase 5 work, with this sidecar range ready
  to support Phase 6 trace LOD early-return experiments.

### Phase 4: trace-driven request feedback

- Add a trace request buffer and per-ray request emission path.
- Deduplicate requests by `(producer, pageId, desiredLOD, rayClass)`.
- Merge raster, trace and stability feedback into one scheduler queue.
- Carry receiver page id/generation in trace requests for diagnostics and
  self-shadow policy.

Tests:

- offscreen shadow ray touching an unloaded fine page emits a trace request;
- repeated rays to the same missing page emit one deduplicated request;
- trace requests do not starve visible raster requests;
- stability requests keep conservative parents resident while children stream;
- bounded request buffers report overflow without changing ray hits.

Status: complete for the initial CPU request-feedback bridge.

Implemented:

- `SparseVoxelTracePageRequestGPU`, a 64-byte/16-byte-aligned request ABI with
  split 64-bit producer/page IDs, desired LOD, ray class, receiver page
  generation, flags, priority and ray/error diagnostics;
- trace ray classes and request flags covering shadows, GI, reflections,
  exact/primary use, raster feedback, stability feedback, offscreen rays,
  conservative fallback and exact unresolved results;
- `SparseVoxelTraceRequestQueue`, a bounded queue that deduplicates by
  `(producer, pageId, desiredLOD, rayClass)`, merges diagnostics, keeps the
  highest priority duplicate, and replaces lower-priority entries when a
  higher-priority raster/stability/trace request arrives under pressure;
- CPU trace feedback selection that requests missing desired pages while
  either tracing the resident conservative parent for visibility rays or
  reporting exact unresolved for exact rays that do not allow coarse fallback;
- overflow reporting that does not alter the ray result: visibility rays still
  use the resident parent fallback, and exact rays still report unresolved.

Tests added/extended:

- Phase 4 convergence test verifies repeated rays to the same missing page
  produce one deduplicated queue entry;
- verifies visible raster feedback can replace a lower-priority trace request
  in a full bounded queue, so trace feedback cannot starve screen-cut requests;
- verifies receiver generation and offscreen/fallback diagnostics survive
  request packing;
- verifies offscreen shadow rays touching an unloaded fine page request
  refinement and trace the resident root fallback rather than producing a false
  miss;
- verifies exact reflection-style rays touching an unloaded fine page request
  refinement and report unresolved instead of silently accepting empty space;
- verifies a saturated request buffer reports overflow without changing the
  conservative fallback hit policy.

Intentional Phase 4 boundary:

- request emission is represented by CPU-side trace feedback helpers and tests;
  renderer/GPU ray shaders do not emit these request records yet;
- the queue is ready to merge raster, trace and stability producers, but the
  production terrain scheduler still needs the later integration pass that
  consumes these records;
- stability feedback is represented as a first-class request class/flag in the
  shared queue; automatic parent pinning/eviction-policy integration remains
  part of the renderer scheduler work;
- direct Cubic64 traversal remains Phase 5, which can now request missing
  offscreen pages through this bridge instead of inventing a separate feedback
  path.

### Phase 5: direct Cubic64 trace traversal

- Implement a trace shader that traverses `VoxelTracePageGPU` +
  `VoxelTraceNode64GPU` directly.
- Route a test fixture through the shared page table instead of the external
  Tree64 descriptor.
- Preserve the current Tree64 trace shader as the oracle.
- Support exact mode and conservative visibility mode separately.

Tests:

- CPU reference traversal parity for hits, misses and query limits;
- GPU parity against current Tree64 with LOD/error early-return disabled;
- receiver-cell skip parity for terrain/self-shadow rays;
- page-generation mismatch returns invalid/unknown, never stale hit;
- exact mode returns the same cell/material identity as the raster-selected
  page.

Status: complete for the initial direct Cubic64 traversal oracle.

Implemented:

- CPU reference ray/AABB testing, child-coordinate decoding and implicit
  Cubic64 child-bound generation for `VoxelTraceNode64GPU`;
- CPU direct Cubic64 traversal over `VoxelTracePageGPU` +
  `VoxelTraceNode64GPU`, including page validation, generation checks, nearest
  occupied-child hit selection, query-limit reporting and invalid-child-pointer
  rejection;
- receiver/self payload skip support so terrain self-shadow tests can verify
  "skip the receiver cell, keep tracing" semantics before GPU integration;
- WGSL mirror helpers for Cubic64 child origins and ray/AABB tests, compiled by
  the virtual-trace ABI smoke shader;
- shared page-table test coverage for installing a fine terrain page with a
  direct Cubic64 trace-node range and no external Tree64 sidecar.

Tests added/extended:

- `SparseVoxelVirtualTraceABIContractTests` now covers direct Cubic64 CPU
  traversal hits, misses, nearest-hit ordering, query-limit termination,
  receiver payload skip, page-generation mismatch and malformed child-base
  rejection;
- Phase 5 convergence test verifies a direct trace view installs through the
  shared page table, becomes `traceable()` only when trace format and geometry
  revision match, and exposes the same selected page identity used by the
  screen cut;
- Phase 5 convergence test verifies a malformed direct-trace payload without
  matching geometry revision is rejected before physical allocation.

Intentional Phase 5 boundary:

- production renderer/ray shaders are not switched to direct Cubic64 traversal
  yet;
- the WGSL work is a compile-checked helper/prototype path, not a GPU readback
  parity test against the current Tree64 shader;
- direct traversal currently uses a correctness-first CPU oracle with sorted
  child candidates, not the final optimized region/DDA or subgroup traversal;
- exact material/brick resolution is represented by terminal payload identity
  in the oracle; full material payload lookup remains part of renderer
  integration.

### Phase 6: trace LOD error early-return

- Add optional trace-side sidecar reads for visibility rays only.
- Early-return on conservative node hits when projected error is below the
  caller threshold.
- Keep exact primary/material rays on the existing path.
- Add parity tests proving disabled mode is bit-identical.

Tests:

- disabled early-return is bit-identical to exact direct traversal;
- conservative mode never misses an exact occupied hit;
- coarse conservative hits are marked as coarse/fallback in diagnostics;
- exact reflections/primary rays refuse implicit coarse fallback unless
  explicitly enabled;
- camera-distance projected error, not ray `t` alone, controls quality.

Status: complete for the initial conservative LOD/error oracle.

Implemented:

- explicit `VoxelTraceTraversalMode` with exact traversal as the default and
  opt-in `ConservativeVisibility` traversal for visibility rays;
- optional per-node q8 error sidecar lookup for direct Cubic64 traversal;
- camera-distance projected-error thresholding:
  `absoluteThreshold = projectedErrorThreshold * distance(camera, childBounds)`,
  clamped by a minimum camera distance, so quality is tied to the camera rather
  than to the ray's current `t`;
- conservative coarse-hit reporting on occupied internal children whose q8
  error interval is definitely below the projected threshold;
- error-ordered conservative traversal: candidate stack entries carry decoded
  q8 error when known, lower-error hits override higher-error hits, and known
  worse candidates are pruned after a better hit exists;
- exact-mode protection: exact traversal ignores sidecar early-return unless
  `allowExactCoarseFallback` is explicitly enabled;
- `VoxelTraceHitCPU::coarseFallback` and `VoxelTraceHitCPU::error`
  diagnostics to distinguish conservative coarse hits from exact terminal hits
  and to report the quality that won traversal;
- WGSL helper mirror for distance-to-AABB and projected-error distance
  threshold, compile-checked by the virtual-trace ABI smoke shader.

Tests added/extended:

- `SparseVoxelVirtualTraceABIContractTests` proves sidecar-present disabled
  traversal is bit-identical to exact direct traversal;
- verifies conservative visibility mode reports a coarse fallback hit on an
  occupied internal child and does not miss the exact occupied hit;
- verifies exact mode refuses implicit coarse fallback by default and only
  accepts it when explicitly enabled;
- verifies moving only the camera position changes the LOD early-return
  decision while the ray and geometric hit remain unchanged;
- verifies a farther lower-error coarse hit overrides a nearer higher-error
  coarse hit;
- verifies error-sorted stack traversal can visit the lower-error branch first
  and prune a known-worse subtree after a better hit exists;
- existing Phase 6 convergence tests continue to cover traversal-cut feedback,
  parent fallback residency and no-hole behavior.

Intentional Phase 6 boundary:

- production renderer/ray shaders still do not execute the conservative
  early-return path;
- GPU readback parity for direct traversal with early-return disabled is still
  pending renderer integration;
- the q8 sidecar lookup is page-local/per-node in the CPU oracle; final GPU
  layout and cache behavior still need benchmarking before this becomes the
  default visibility path;
- exact material/primary rays remain exact unless a caller deliberately opts
  into coarse fallback.

### Phase 7: screen-cut / trace-cut consistency

- Store selected page id/generation/LOD in the receiver metadata used by
  scene-ray creation.
- Let trace traversal start from shared roots/fallbacks so it can reach
  offscreen occluders.
- Add cut diagnostics that report screen-selected page, trace-selected page,
  fallback page and request page.

Tests:

- visible receiver rays start from the same page generation as the G-buffer;
- offscreen occluder outside the camera frustum still shadows the receiver;
- rotating the camera without changing geometry does not toggle offscreen
  shadowing incorrectly;
- missing fine offscreen page requests refinement and traces resident parent;
- missing fine visible receiver page reports unresolved for exact rays rather
  than inventing a hit.

Status: complete for the initial receiver/trace-cut consistency bridge.

Implemented:

- `SparseVoxelTraceReceiverGPU`, a 32-byte/16-byte-aligned receiver metadata
  ABI carrying the screen-selected producer, page id, page generation, selected
  LOD and fallback flag;
- receiver pack/unpack helpers so scene-ray creation can carry the same shared
  page-table identity selected by the screen cut;
- `SparseVoxelTraceCutDiagnostics`, reporting screen-selected page/generation,
  screen LOD, trace desired page, trace selected page, fallback page, request
  page, receiver-generation validity, offscreen state, conservative fallback,
  exact unresolved and request overflow;
- `evaluateSparseVoxelTraceCutConsistency`, which validates receiver metadata
  against the shared page table before forwarding to trace feedback request
  selection;
- stale receiver generation rejection, preventing G-buffer/page-table mismatch
  from becoming a stale hit or false empty result.

Tests added/extended:

- Phase 7 convergence test verifies visible receiver rays carry the same page
  id and generation as the screen-selected/G-buffer receiver metadata;
- verifies an offscreen missing fine occluder emits a refinement request while
  tracing its resident conservative root fallback;
- verifies repeated "camera rotation" style feedback with unchanged geometry
  keeps the same offscreen fallback/request pages and deduplicates the request
  while raising priority;
- verifies stale receiver generation is rejected before trace selection;
- verifies a missing fine visible receiver in exact/primary mode reports
  unresolved and requests refinement instead of inventing a hit or falling back
  implicitly.

Intentional Phase 7 boundary:

- receiver metadata is represented and validated on the CPU side; production
  G-buffer packing and GPU scene-ray creation still need renderer integration;
- offscreen shadowing is proven at the cut/request-policy level, not yet by a
  full GPU shadow-ray image comparison;
- trace and screen cuts now share identity and diagnostics, but direct
  renderer traversal still remains on the later integration path.

### Phase 8: grouped-octant traversal experiment

- Implement a Cubic64 traversal variant that intersects 8 fixed 2x2x2 child
  groups before descending.
- Compare against region traversal and mirrored-ancestor traversal.
- Keep only if it reduces node visits or branch divergence enough to beat the
  extra ALU.

Tests:

- grouped traversal returns the same exact hits as direct region traversal;
- maximum stack depth remains within the fixed shader limit;
- diagonal/thin-wall/checkerboard fixtures retain parity;
- benchmark reports node visits, group tests and GPU time.

Status: complete for the initial grouped-octant CPU/reference experiment.

Implemented:

- `VoxelTraceTraversalVariant::GroupedOctant`, opt-in only so the exact
  Child64 path remains the default baseline;
- CPU and WGSL helper mirrors for Cubic64 child-octant selection, octant masks
  and octant bounds/origins;
- a traversal prepass that intersects 8 fixed 2x2x2 child groups before testing
  individual children;
- `groupTests` and `skippedChildGroups` diagnostics so benchmarks can separate
  extra group ALU from reduced child tests.

Tests added/extended:

- `SparseVoxelVirtualTraceABIContractTests` verifies the 8 octant masks
  partition all 64 children with no overlap;
- verifies grouped-octant traversal returns the same hit contract as the
  Child64 baseline on terminal and internal-node fixtures;
- verifies a full terminal root ray that only crosses two octant slabs performs
  8 group tests, skips 48 grouped children and reduces child tests from 64 to
  16;
- the WGSL ABI smoke shader compile-checks the octant helper mirror.

Intentional Phase 8 boundary:

- this is still a CPU/reference and shader-helper compile-smoke experiment, not
  the production renderer traversal path;
- thin-wall/checkerboard parity currently rides on the exact traversal
  regression suite; dedicated GPU image fixtures remain Phase 11/12 work;
- keep/prune still depends on real GPU counters for node visits, group tests,
  branch divergence and total trace time.

### Phase 9: Bounds8/Bounds16 installed cluster experiment

- For non-cubic virtual-geometry clusters, build a CWBVH-style bounds node
  format with q8 child bounds.
- Reuse the existing CWBVH quantization approach: parent origin, exponent
  scale per axis, conservative lower/upper bounds.
- Keep this path separate from cubic terrain pages.

Tests:

- quantized bounds are conservative for all children;
- bounds format parity against CWBVH/triangle fallback fixtures;
- mixed Cubic64 terrain and Bounds8 cluster pages can coexist in one trace
  dispatch;
- non-cubic bounds pages never become the implicit fallback for cubic terrain
  unless marked conservative.

Status: complete for the initial Bounds8/Bounds16 installed-cluster
CPU/reference experiment.

Implemented:

- `VoxelTraceBounds8NodeGPU`, an 80-byte/16-byte-aligned CWBVH-style node for
  eight arbitrary child bounds;
- `VoxelTraceBounds16NodeGPU`, a 128-byte widened version for experiments where
  sixteen children may reduce hierarchy depth enough to pay for the extra
  per-node bandwidth;
- shared q8 conservative bounds quantization using parent origin, per-axis
  signed exponent scale, floor-lower/ceil-upper expansion and compact child
  masks;
- CPU reference traversal for Bounds8 and Bounds16 pages, kept separate from
  the Cubic64 terrain traversal;
- a trace-format fallback policy helper that allows exact same-format fallback
  and only allows Bounds8/Bounds16 to stand in for Cubic64 when the candidate
  page is explicitly marked `ConservativeFallback`;
- WGSL ABI/helper mirrors compile-checked by the virtual trace smoke shader.

Tests added/extended:

- `SparseVoxelVirtualTraceABIContractTests` now freezes Bounds8/Bounds16 struct
  size, alignment and field offsets;
- verifies decoded q8 bounds conservatively enclose all occupied source child
  boxes and reject child boxes outside the quantized parent range;
- verifies Bounds8 traversal matches an AABB fallback fixture and supports
  payload skipping;
- verifies Bounds8 internal-node traversal descends through compact internal
  child ordering;
- verifies Bounds16 traversal reaches high child slots correctly;
- verifies mixed Cubic64 and Bounds8 pages can be traced side-by-side while
  wrong-format dispatches are rejected as invalid;
- verifies non-cubic Bounds8/Bounds16 pages cannot implicitly replace Cubic64
  terrain fallback unless marked conservative.

Intentional Phase 9 boundary:

- this is still a CPU/reference traversal and WGSL ABI compile-smoke path, not
  a production scene-ray shader route;
- parity is against conservative AABB fixtures in this phase; full
  CWBVH/triangle image/readback parity remains Phase 11/12 work;
- Bounds8/Bounds16 are for non-cubic installed virtual-geometry clusters only;
  cubic terrain keeps the Cubic64 hot path unless an explicit conservative
  fallback page says otherwise.

### Phase 10: installed geometry descriptor unification

- Extend installed page descriptors so raster/culling/tracing all name the
  same geometry revision and generation.
- Publish trace view, material view and culling sidecar atomically.
- Preserve immutable active pages and deferred retirement.

Tests:

- descriptor publication updates all views or none;
- material generation mismatch rejects trace material resolution;
- raster page resident + missing trace view is invalid and keeps fallback;
- trace page resident + missing raster view is invalid for visible cut;
- fuzz install/uninstall order with dependency and fence checks.

Status: complete for the initial shared descriptor identity and publication
contract.

Implemented:

- `materialRevision` now travels with produced payloads, physical allocations
  and shared page-table entries;
- prebuilt packages default material identity to the same payload revision used
  for geometry identity;
- allocation/reservation validation rejects explicit material/geometry revision
  mismatches before physical ranges are reserved;
- committed entries expose separate descriptor predicates:
  `rasterVisible()`, `traceable()`, `materialTraceable()` and
  `hasCullErrorSidecar()`;
- failed descriptor publication remains upload-queued and publishes no partial
  generation, geometry, material, trace or cull sidecar view;
- direct trace feedback can opt into `requireDirectTraceView`, so a resident
  raster-only desired page is treated as missing trace data and emits a trace
  request instead of becoming a false hit/miss;
- trace-only descriptors are valid for direct tracing but rejected by the new
  raster-visible predicate, keeping visible cuts from selecting pages without a
  raster payload.

Tests added/extended:

- Phase 10 verifies unified descriptor publication commits raster, trace,
  material and cull views atomically;
- verifies rewrite-count mismatch leaves no partially published descriptor;
- verifies material revision mismatch is rejected before trace/material storage
  allocation;
- verifies direct trace feedback distinguishes `desiredResident` from
  `desiredTraceable`, falls back to the traceable root for conservative rays
  and reports unresolved for exact rays;
- verifies trace-only installed descriptors remain traceable but not
  raster-visible;
- keeps the existing Tree64 LOD-generation tests so multiple Tree64 payload
  versions per logical page still coexist safely while newer geometry revisions
  supersede stale jobs.

Intentional Phase 10 boundary:

- the predicates and CPU feedback path now express the unified descriptor
  contract, but renderer/G-buffer publication still needs Phase 11 integration;
- legacy Tree64/root fallback paths remain compatible unless direct virtual
  trace explicitly requests `requireDirectTraceView`;
- material resolution is represented by descriptor identity and material range
  presence here; full shader-side exact material lookup remains renderer work.

### Phase 11: renderer integration

- Add runtime switches for node format, traversal variant and error-sidecar
  mode.
- Route terrain, virtual geometry and dynamic Tree64 instances through the
  unified descriptor where possible.
- Keep current Tree64 as the default until benchmark data justifies a flip.

Status: complete for the initial runtime routing contract.

Implemented:

- `VoxelTraceRuntimeSettingsCPU` and `VoxelTraceRuntimeRouteCPU` define the
  renderer-facing switch surface for backend, node format, traversal variant,
  error-sidecar mode, screen traces and exact material resolution;
- legacy Tree64 remains the default route and does not require the direct
  virtual trace view when selecting resident raster pages;
- direct virtual tracing is opt-in and feeds `requireDirectTraceView` into the
  streaming feedback path so raster-only pages are treated as missing trace
  payloads and request the desired trace page;
- unsafe combinations are rejected at the policy boundary:
  - Tree64 cannot silently select Bounds8/Bounds16;
  - grouped-octant is Cubic64-only;
  - `trace-lod` sidecars require the direct virtual backend;
- string parsers/name helpers now freeze the intended CLI/config spellings:
  `tree64`, `direct-virtual`, `cubic64`, `bounds8`, `bounds16`,
  `child64`, `grouped-octant`, `off`, `cull-only`, `trace-lod`.

Tests:

- Phase 11 placement tests continue to validate streamed terrain/object
  instance boundaries;
- Phase 11 runtime tests validate Tree64 default routing, direct virtual
  routing, unsafe-combination rejection and streaming feedback behavior when a
  desired page is raster-visible but not directly traceable;
- virtual trace ABI tests continue to validate the direct trace descriptor and
  traversal contracts.

Remaining renderer work:

- wire these settings into the sample/runtime CLI and shader dispatch selection;
- run scene-ray GPU validation in both Tree64 and direct virtual trace modes;
- validate terrain shadow parity with screen traces enabled and disabled;
- keep terrain G-buffer, lighting residency and captured/camera-walk no-hole
  regressions in the Phase 12 benchmark sweep.

### Phase 12: benchmark and prune

- Run microbenchmarks and renderer benchmarks.
- Remove losing variants quickly. This space can get swampy if every cute idea
  becomes permanent. Tiny goblin zoo, big maintenance bill.
- Promote only the variant that wins under real terrain/lighting workloads.

Status: complete for the initial benchmark/prune smoke gate.

Implemented:

- `SparseVoxelVirtualTraceBenchmarkSmoke` now runs as a named regression test
  using the Phase 12 convergence binary;
- the smoke benchmark emits and validates JSON-shaped records for retained
  variants:
  - `tree64-legacy`;
  - `direct-cubic64-child64`;
  - `direct-cubic64-grouped-octant`;
  - `direct-bounds8`;
  - `direct-bounds16`;
- each record carries the counters required for pruning decisions:
  `trace_requests`, `offscreen_trace_requests`, `fallback_traces`,
  `unresolved_exact`, `node_visits`, `child_tests`, `group_tests`,
  `skipped_child_groups`, `estimated_node_bytes` and elapsed time;
- the smoke test asserts grouped-octant reports fewer child tests than the
  Cubic64 child64 control on the fixed ray set;
- removed/unimplemented switch names such as `hybrid64`, `wide-bounds` and
  `q8-batch` are rejected so stale variants do not remain exposed.

Tests:

- benchmark smoke emits JSON-shaped telemetry for every retained variant;
- no retained variant regresses exact correctness or streaming safety in the
  CPU/ABI convergence suite;
- removed variants leave no exposed runtime switches;
- real GPU renderer captures remain a data-collection step before flipping the
  default from Tree64 to direct virtual tracing.

### Phase 13: 1 MiB packed page slots and dependency sidecar

Current generated terrain Tree64 pages reserve `8192` Tree64 nodes per page.
That is about `96 KiB` for the raw three-word node format and `128 KiB` for
the padded `vec4<u32>` dynamic-trace path, before material and error sidecars.
The renderer therefore streams many small logical page payloads even when the
GPU producer could fill a larger upload/granularity unit efficiently.

Add a packed physical page slot policy on top of the shared page table:

```text
physical slot budget: 1024 KiB
  pagelet 0: logical page / lod version / trace node range / sidecar ranges
  pagelet 1: logical page / lod version / trace node range / sidecar ranges
  ...
  dependency records: parent slot/pagelet, required fallback pages, sidecars
```

Rules:

- keep logical pages and LOD versions distinct; the 1 MiB slot is a packing
  unit, not the logical residency identity;
- pack generated terrain bricks/LOD payloads until the slot would exceed
  `1024 KiB`, then close the slot and publish it atomically;
- pagelets carry the same generation, geometry revision, trace format and
  sidecar ranges that standalone pages carry today;
- every pagelet records parent/fallback dependencies in the existing shared
  dependency graph, so a packed child cannot become selectable before its
  conservative parent/fallback pagelet is resident;
- eviction is dependency-aware at pagelet granularity, but physical slot memory
  is reusable only after all pagelets in the slot are non-resident or copied
  into a compacted slot;
- GPU builders should write full packed slots when possible, but must still be
  allowed to publish partially filled slots for latency-critical near-camera
  requests.

Tests:

- CPU allocator oracle packs synthetic pagelets into `1024 KiB` slots without
  crossing the limit and preserves logical page ids/generations;
- dependency oracle rejects installing a child pagelet when its parent/fallback
  pagelet is absent, even if both would fit in the same physical slot;
- eviction oracle prevents freeing a slot containing any pagelet with resident
  dependents;
- generated terrain smoke verifies packed slots produce the same screen cut and
  trace cut as one-page-per-logical-page publication;
- streaming benchmark reports bytes/pagelet, bytes/slot, fill ratio,
  pagelets/slot, dependency misses and slot compaction/copy cost;
- long camera-walk regression has zero terrain holes and no false trace misses
  while slots are partially filled or being retired.

Implementation note:

- `SparseVoxelSharedPageTable` now stamps each allocation with packed-slot /
  pagelet placement metadata and exposes slot reuse decisions for CPU oracles;
- `collectPageRequestClosure` expands trace/offscreen requests into
  dependency-first missing page chains so CPU scheduling can request parent
  fallbacks before children;
- the first implementation is append-only within a live slot and reuses a slot
  only after all pagelets in it have retired behind their fences;
- the renderer still needs the next wiring step: terrain GPU publication should
  batch pagelet copies into these 1 MiB slots and report slot fill ratio /
  pagelets per slot in benchmark JSON.

## Regression test matrix

Add or extend these named tests as implementation lands:

```text
SparseVoxelVirtualTraceABIContractTests
  CPU/shader ABI for VoxelTracePageGPU and VoxelTraceNode64GPU.

SparseVoxelVirtualTraceCPUReferenceTests
  CPU reference traversal for Cubic64 pages and fallback cuts.

SparseVoxelVirtualTraceGPUParityTests
  Direct virtual trace vs current Tree64 with exact traversal.

SparseVoxelVirtualTraceOffscreenRequestGPU
  Offscreen occluder rays emit trace page requests and use fallback.

SparseVoxelVirtualTraceScreenCutConsistencyGPU
  G-buffer receiver page identity matches trace receiver identity.

SparseVoxelVirtualTraceFallbackSafetyGPU
  Missing fine pages never become false misses for visibility rays.

SparseVoxelVirtualTraceExactUnresolvedGPU
  Exact rays report unresolved when required fine receiver/occluder pages are
  absent and coarse fallback is not allowed.

SparseVoxelVirtualTraceGenerationSafetyGPU
  Stale page/material/geometry generations reject before trace.

SparseVoxelVirtualTraceConservativeLODGPU
  Error early-return never misses exact occupied hits.

SparseVoxelVirtualTraceBounds8GPU
  Optional CWBVH-style bounds pages are conservative and coexist with Cubic64.

SparseVoxelWorldTerrainShadowParity
  Existing close-slope screen/world parity continues to pass.

SparseVoxelWorldTerrainLightingResidencyRegression
  Existing moving terrain Tree64/streaming blackout and hole regression guard.

SparseVoxelTerrainGBufferContract
  Receiver identity/material/depth contract remains valid.

SparseVoxelVirtualTraceBenchmarkSmoke
  Benchmark JSON contains request, fallback, node-visit and timing counters.
```

Minimum regression gates before flipping the default:

- exact direct virtual tracing is bit-identical to current Tree64 on synthetic
  Cubic64 fixtures;
- direct virtual tracing produces no stale-generation invalids in terrain
  streaming;
- offscreen occluders remain visible to trace when outside the camera frustum;
- missing desired pages produce requests and conservative fallback, not misses;
- screen receiver identity and trace receiver identity match;
- strict close-terrain fixtures have zero isolated holes and zero page-sized
  gaps;
- benchmark data is collected in Release with diagnostic readbacks disabled.

## Expected outcome

The most likely winner is:

```text
Cubic64 trace nodes remain Tree64-like and tiny, but live inside the installed
virtual page rather than an external Tree64 sidecar.
Culling/error data moves to optional sidecars on the same page.
Visibility rays get conservative projected-error early return.
Non-cubic installed geometry gets a separate CWBVH-like Bounds8/Bounds16 path.
Screen and trace cuts share page identity/generation but may choose different
resident fallback leaves.
Trace rays can request offscreen pages without creating false misses.
All page views publish atomically under the existing generation/streaming
contract.
```

That gives us convergence without throwing away the thing Tree64 is already
very good at: tracing fixed cubic occupancy with almost no node bandwidth.
