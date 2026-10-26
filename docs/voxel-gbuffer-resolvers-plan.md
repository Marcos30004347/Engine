# Voxel G-buffer resolvers plan

## Outcome

The sample gains a **G-buffer resolver selector**. Everything upstream (TLAS/BLAS
culling, LOD, streaming) and everything downstream (material G-buffer, ReSTIR DI,
light field, compose, TAA) is untouched: each resolver's only contract is to fill
`resolvedSurfaces`, `coverage` and `linearDepth` for every viewport pixel, plus the
depth target that feeds the HZB.

```
resolver ∈ { SvRaster (default, existing), Tree64Primary, BrickHierarchy }
```

Three resolvers, in the order they land:

1. **SvRaster** — the existing project/duplicate/bin/resolve path, optimized. This
   is the correctness reference; the other two are compared against it.
2. **Tree64Primary** — primary rays marched through the existing camera-centred
   Tree64 clipmap, no binning at all. Cheapest to build because the traversal
   already exists for shadow and GI rays.
3. **BrickHierarchy** — a Virtual-Geometry-shaped cluster hierarchy whose leaves
   are 8³ voxel bricks. One culling pass does frustum + occlusion + LOD selection,
   emits an indirect multi-draw, and a fragment shader DDAs each brick behind the
   three camera-facing faces of its bounding box.

The three must agree on visibility to within their representation. They will not
agree bit-exactly — a Tree64 cell and an octree leaf are different primitives — so
equivalence tests compare coverage and depth within tolerance, not signatures.

## Why this order

`resolveSolidVoxels` is 36% of the frame, `scatterTileCandidates` 9.6%,
`prefixTileBuckets` 4%. Roughly half the frame is in the SvRaster binning/resolve
chain. Part A pays for itself immediately and, because the alternative resolvers
are measured against the optimized baseline, it also stops us from declaring a win
that is really just "we replaced an unoptimized pass".

---

## Part A — optimize the existing SvRaster resolver

### A1. `prefixTileBuckets` is single-threaded (4% of frame)

`@compute @workgroup_size(1, 1, 1)` running a serial loop over
`tileRangeCapacity` tiles. At 1920x1080 with `SPARSE_VOXEL_TILE_SIZE = 4` that is
480 × 270 = **129,600 serial iterations on one lane**, each with a dependent
`atomicLoad` — a full serialization point between count and scatter, twice per
frame (pre-phase and final phase).

Replace with the three-kernel exclusive scan the same shader already uses for
duplicate offsets (`scanDuplicateBlocks` / `scanDuplicateBlockSums` /
`addDuplicateBlockOffsets`):

- `scanTileBucketBlocks` — 256-wide Blelloch scan per block, writes
  `tileRanges[tile].begin` (block-local) and the block total.
- `scanTileBucketBlockSums` — scan of ~507 block totals; also publishes
  `candidateCount`, `duplicateCount` and the overflow state that the serial
  kernel published.
- `addTileBucketBlockOffsets` — adds the block offset and writes the final
  `begin`/`end` pair, clamped to `candidateCapacity` exactly as today.

The clamp semantics must be preserved literally: `begin = min(runningTotal,
candidateCapacity)`, `end = min(runningTotal + count, candidateCapacity)`, so an
overflowing frame still degrades to a deterministic prefix instead of failing
closed. Expected: 4% → well under 0.5%.

### A2. `countTileCandidates` + `scatterTileCandidates` (9.6% for scatter alone)

Both kernels re-read `projectedVoxels[index]` (32 B), re-derive the same
`tileRectangle`, and walk the same tile loop. Two changes:

1. **Fold the count into `preprocessVisibleVoxels`.** Preprocess already has the
   projected bounds and computes `tileCount` for `duplicateCount`; adding the
   per-tile `atomicAdd` there removes an entire dispatch plus a full re-read of
   the projected array. `countTileCandidates` stays in the file for the tests that
   drive it standalone, but the render graph stops scheduling it.
2. **Cut scatter's per-tile dependent load.** `scatterTileCandidates` loads
   `tileRanges[tile]` *inside* the inner loop to get `range.begin`. The row of
   tiles a voxel covers is contiguous, so the load can be hoisted per row, and
   `range.end` is only needed for the bound check — which `begin + slot <
   candidateCapacity` already implies once A1 guarantees a monotone prefix.

Atomic contention is the other half of scatter's cost. A voxel spanning a wide
tile rectangle serializes on one lane; large voxels dominate. Mitigation, measured
before committing to it: have each thread emit its rectangle row-major and
`atomicAdd` the whole row's count once per row into a per-row cursor, rather than
once per tile. This only helps when a voxel spans several tiles horizontally, so it
is gated behind a measurement, not assumed.

### A3. `resolveSolidVoxels` (36% of frame)

Four independent wins, ordered by confidence:

**(a) Hoist the ULP padding out of the per-pixel loop.** `voxelSegmentForPixel`
calls `nextFloatDown`/`nextFloatUp` six times per candidate *per pixel*, but the
padded box depends only on the voxel. The staging loop already loads each
candidate once per bucket (16 lanes, 16 candidates); compute the padded
`minimum`/`maximum` there and put those in shared memory instead of
`centerLength`. 16 pixels × 6 ULP ops collapses to 6. **Bit-identical** — the same
values, computed once.

**(b) Do the preferred-candidate search only in the first batch.** The search loop
runs whenever `bestVoxel == SPARSE_VOXEL_INVALID_INDEX`, which is *every batch*
for a pixel that never hits anything. Sky and background pixels therefore pay a
full extra scan of every batch. The comment says the intent is "establish a close
hit as early as possible in the first batch" — restricting it to `batch == 0u`
matches the stated intent and removes the miss-pixel penalty. Resolved output is
unchanged because the search only reorders candidate visitation, and
`hitPrecedes()` is a total order.

**(c) Cache the reciprocal direction per instance.** `voxelSegmentForPixel` does
six divides per candidate. `cachedLocalDirection` is already cached per instance;
cache `1.0 / direction` alongside it and multiply. **This changes results** —
`(a - o) * (1/d)` is not `(a - o) / d` — so it is gated behind the
`SparseVoxelRasterReference` equivalence tests and, if they reject it, kept as an
opt-in fast path rather than forced.

**(d) Sort each tile bucket by `minimumHitDistance` so the resolver can `break`.**
Today a candidate whose conservative lower bound exceeds `bestT` costs a
`continue`; the list is in arbitrary atomic order so nothing can be skipped
wholesale. If the scatter pass emitted per-tile lists sorted by the packed lower
bound, the resolver would stop at the first candidate whose bound loses, turning
the tail of every deep tile into zero work. Cost is a small per-tile sort (typical
buckets are tens of entries). This is the largest potential win and the largest
amount of new code, so it lands last and only if (a)–(c) leave resolve dominant.

### A4. Measurement protocol

`make gputrace` before and after each change, comparing the per-pass timings that
`SparseVoxelPassTimeline` already collects, on the same Sponza camera
(`--test-camera-roof-down`). A change that does not move its own pass by more than
noise gets reverted rather than kept "because it should be faster".

---

## Part B — Tree64 primary-ray resolver

### Status: landed and rendering

Behind `--gbuffer-resolver tree64`. On the 128³ shell it produces **exactly the
same coverage as SVRaster** — 174724 pixels both ways — while reporting
`candidates=0 duplicates=0`, confirming the binning chain is genuinely skipped
rather than merely unused.

**Static vs dynamic layer — the trap.** `tree64DynamicScene` is
`shadowBackend == Tree64 && !instances.empty()`, so every instanced scene puts
its geometry in the *dynamic* layer and leaves the static chunk directory empty
by design. The sample's `occupied=0 liveNodes=0` readout is that expected state,
**not** a build regression: it reads the static directory, which the dynamic path
never populates. A resolver that picks `SparseVoxelTree64TracePass` therefore
marches an empty tree and renders nothing. The trace pass must be chosen the same
way the shadow path chooses it — `SparseVoxelTree64DynamicTracePass` when
`tree64DynamicScene`, and its committed model/instance/reference/node counts need
the same per-frame `update()` the shadow trace gets.

This also explains `SparseVoxelTree64CutoverT9`: its regex asserts
`occupied=[1-9][0-9]*` on the static directory, which the current architecture
can never satisfy for a scene with instances. It looks like a stale assertion
rather than a live regression, but confirming that is a separate call.

The traversal was deliberately not refactored. The plan below called for folding
ray generation into `traceTree64`, which would have meant editing a 1395-line
shader whose own test suite is red — four Tree64 GPU tests segfault on `main`.
Instead the resolver brackets the stock trace pass: generate rays → trace
(untouched) → resolve surfaces.

**That composition is now measurably the resolver's dominant cost.** At 720p on
the shell scene, median per pass:

| Pass | ms |
| --- | --- |
| `Tree64PrimaryRays` | 0.157 |
| `Tree64PrimaryTrace` | 0.056 |
| `Tree64PrimarySurfaces` | 0.003 |

Ray generation is 73% of the chain, and all it does is write 29 MB of rays that
the very next pass reads back. Folding generation into the trace entry point
would remove nearly all of it. That is now the highest-value follow-up here, and
the Tree64 GPU tests that guard the traversal are green again, so it is unblocked.

Comparing totals on this scene would be misleading: the SVRaster chain measures
0.05 ms here, which is the dispatch-overhead floor for a scene with almost no
voxels (see the measurement note below). Which resolver wins has to be settled on
a scene where the SVRaster chain does real work.

### Shape

One compute pass, one thread per pixel, no binning, no candidate buffers:

```
SparseVoxelTree64PrimaryResolvePass
  in : treeState, chunks, nodes (+ optional dynamic layer), raster uniforms
  out: resolvedSurfaces, coverage, linearDepth
```

It reuses `tree64TraceChunk` / `tree64IntersectVolume` from
`sparse_voxel_tree64_trace.wgsl` verbatim by `#include`, generating the ray inline
from `focalPrincipal` + `cameraToWorld` instead of reading a ray buffer. That
avoids materializing a width × height ray array, which at 1080p is 66 MB of
round-tripped bandwidth for data the shader can compute in four ALU ops.

### The material problem — read this before implementing

**Tree64 carries occupancy only.** `SparseVoxelTree64Node` is
`{childBaseAndFlags, populationMaskLo, populationMaskHi}`; the trace returns a
`globalCell` and a distance. `SparseVoxelMaterial` is indexed by *octree*
`voxelIndex`, and nothing maps a Tree64 cell back to it. Tree64 has been used for
shadow and GI visibility, where a boolean is all that is needed.

So this resolver cannot fill `ResolvedVoxelSurface::voxelIndex` with anything the
material G-buffer can use. Three options, in preference order:

1. **Add a per-terminal-node attribute payload to Tree64** — a parallel
   `array<u32>` of material indices, allocated alongside the node slots and
   written by the batch build from the source octree's material array. This is the
   correct fix and makes Tree64Primary a real renderer, but touches the build,
   publish, streaming and residency paths.
2. **Ship the resolver material-free first**, writing a sentinel material index
   and a face normal, and drive it through the depth-only path
   (`--depth-only`) plus a debug present mode. This validates traversal cost and
   visibility against SvRaster without touching the build chain, and is what the
   first commit does.
3. Hash the `globalCell` into a debug palette. Diagnostic only, never shipped.

Option 2 is what landed. Hits are flagged `SPARSE_VOXEL_SURFACE_NO_MATERIAL`
(bit 2, additive — `HIT` and `CAMERA_INSIDE` keep their values), and the G-buffer
shader skips the material lookup for those pixels instead of indexing the array
with a voxel index that does not address it. Geometry is exact; albedo is neutral.

Face normal: rather than adding a field to `SparseVoxelTree64Result` and touching
the traversal, the resolve kernel recovers the entry face with one slab test
against the single cell the trace reports. Same answer, no change to a shader
other consumers share.

### Expected behaviour

Tree64 is a camera-centred clipmap sized from camera-to-scene reach, not scene
extent, so this resolver is exact only inside the clipmap and degrades to
`TREE64_STATUS_INVALID` outside it. The selector must present it as what it is: a
fast approximate primary path, not a drop-in replacement for SvRaster at all
camera distances.

---

## Part C — brick-hierarchy resolver

### Status: rendering

Selectable with `--gbuffer-resolver bricks`. All stages verified on the
128-cubed shell:

| Stage | State |
| --- | --- |
| In-memory builder | 1712 nodes, 4 levels, 127712 material entries, 8 roots, finest voxel 0.015625. Unit-checked separately on a 16-cubed solid block. |
| Culling + LOD cut | Selects 8 root bricks at distance, 1352 leaf bricks under `--disable-lod`. The cut descends correctly. |
| Rasterize + DDA | Renders. Coverage 174724 against SVRaster's 174306. |
| Attachment resolve | Feeds `resolvedSurfaces` / `coverage` / `linearDepth` unchanged downstream. |

### The bug that hid all of this: a Y-flip

Every fragment was being discarded, which is indistinguishable from "the draw
never ran". The cause was a projection convention, not the brick code.

`worldToClip` in this renderer is **not** Vulkan-Y-flipped — the pipeline treats
clip +Y as the top of the image and uses top-left image coordinates, as the
`clipToUv` comment in `sparse_voxel_gbuffer.wgsl` states. Vulkan's rasterizer
takes NDC +Y as downward. Nothing had ever noticed, because every other pass
reaches the screen through compute or a full-screen triangle. The brick draw is
the first pass to put **world-space geometry** through the rasterizer.

The consequence: the ray reconstructed from `focalPrincipal` + `cameraToWorld`
did not point at the fragment the rasterizer had produced, so the ray/box slab
test collapsed (`tExit` ≈ 0 everywhere) and discarded 100% of fragments. The fix
is one line in the vertex shader:

```wgsl
output.position = vec4<f32>(clip.x, -clip.y, clip.z, clip.w);
```

The bisect that found it, in order, since each step ruled out a whole layer:
indirect args readback (18 verts × 8 instances — correct), unconditional
fragment (full coverage — raster, depth, attachments and resolve all fine),
unconditional fragment after the node fetch (fine — flat varyings and buffer
loads fine), classify-instead-of-discard on the slab test (all pixels failing
`tEnter > tExit`), magnitude probes (`tExit` ≈ 0, `tEnter` > 0), then comparing
the two ray formulations directly (they disagreed).

Two things worth keeping from that: the fragment now derives its ray from its
own interpolated face position rather than reconstructing it from the pixel —
cheaper and self-consistent — and the brick bounds travel as flat varyings
rather than being re-fetched. A probe confirmed the flat `u32` brick index
agrees with those bounds on every fragment, so material lookups are trustworthy.

### Artifact fixes

Two classes of black artifact came out of testing, both mine, both from
incomplete changes:

1. **Whole scene black under the Tree64 resolver.** `SPARSE_VOXEL_SURFACE_NO_MATERIAL`
   was honoured in `sparse_voxel_gbuffer.wgsl` but not in
   `sparse_voxel_material_gbuffer.wgsl`, which kept indexing
   `materials[voxelIndex]` — landing on material 0 for every pixel. The material
   pass now shades a neutral surface for occupancy-only resolvers.
2. **Huge black boxes spanning the scene under the brick resolver.** The
   renderer is **reverse-Z** — the G-buffer clears depth to 0.0 — but the brick
   draw cleared to 1.0 and tested `LessOrEqual`, so *farther* fragments won and
   distant coarse bricks painted over near geometry. Now clears to 0.0 and tests
   `GreaterOrEqual`. Coverage could never have revealed this, only ordering.
   The fragment also rejects depths outside [0,1] rather than letting a clamped
   depth win against the far clear, and clamps the ray entry to the near plane
   so a camera sitting inside a coarse brick cannot report a hit at t=0 with a
   degenerate clip w.

### Camera inside solid geometry

Flying the camera into a wall - or simply into a coarse brick whose OR-reduced
occupancy is solid where the eye is - made the DDA report a hit at the eye
itself. In reverse-Z that is the nearest possible depth, so it won every pixel
and turned the brick's whole silhouette into a black wall. It only showed up
after moving around, which is why launching straight into the brick resolver
never reproduced it.

The DDA now skips the run of occupied cells the ray *starts inside*, and reports
the next surface it actually enters. The guard is conditioned on the eye being
inside the box: a ray entering from outside lands on an occupied cell constantly
and that is the front surface, not something to skip. Conditioning it on the
entry cell alone instead cost a third of all coverage.

SVRaster solves the same problem with an explicit `CAMERA_INSIDE` flag that
resolves from the exit face; the brick path skips instead, which is cheaper and
close enough given exact parity is not required here.

### Streaming: bricks only cover the resident page set

Symptom: load a streamed scene and the bricks show "the whole Sponza, but only
within a radius of the camera" - which reads as aggressive distance culling and
is not culling at all.

Measured directly, same scene and camera, only residency differing:

| | brick hierarchy | visible bricks |
| --- | --- | --- |
| `--test-vsvo-roundtrip` (all pages) | 70410 nodes | 6496 |
| `--test-vsvo-selective` (partial) | **8 nodes** | 5 |

Five enormous root bricks covering the screen - that is both the "radius" and the
"huge boxes". Coverage stayed at 518400 in both cases, which is exactly why no
headless check caught it.

Two separate defects fed it. First, the builder read `lodHierarchy`
unconditionally, but a file-backed scene uploads `resident->physical*()` instead,
so after a scene swap the bricks described the *previous* scene or nothing.
Second - and this is the real limit - once the builder does read the resident
arrays, the resident set is a **distance-prioritized subset**. A hierarchy built
from it covers a radius around wherever the camera stood when the graph was
built, and nothing exists as a brick beyond that.

Bricks have no streaming support, by decision. The only way to get whole-scene
coverage today is to make the whole scene resident: open with **Load all pages**
(`--load-all-pages`). The sample now **refuses** a partially resident scene
outright - it logs why and falls back to SVRaster - rather than rendering a
handful of giant boxes that look like a depth or culling bug. Same for an empty
hierarchy.

Proper streaming means rebuilding or incrementally patching the brick hierarchy
as residency changes, which is the natural follow-on to a `.vsvb` format.

### Far-plane clipping ate the scene

Symptom: with bricks on Sponza, only geometry close to the camera is visible -
the scene appears cut off well before it should be.

The brick pass draws the three *back* faces of each brick, because front faces
are clipped away whenever the camera is inside a brick's box. But a back face is
by definition farther than the surface the fragment will report, and the far
plane here is derived from the **initial** camera framing and never grows
(`cameraFarPlane`). So a brick whose front surface is comfortably in view gets
its back faces clipped, and the whole brick disappears.

Once again only this pass is affected: SVRaster and Tree64 reach the screen
through compute plus a full-screen triangle and never present world geometry to
the clipper.

The fix is one line in the vertex shader. The triangle only needs to *cover the
right pixels* - its interpolated depth is discarded, because the fragment writes
`frag_depth` from the DDA hit. Clamping clip-space z into the accepted range
stops the clip without moving a pixel, since screen position is x/w and y/w:

```wgsl
let clippedZ = select(clip.z, clamp(clip.z, 0.0, clip.w), clip.w > 0.0);
```

Vertices behind the eye (`w <= 0`) are left alone; those must still clip.

Demonstrated with a new `--camera-far` override, exterior Sponza, coverage:

| far plane | without the clamp | with the clamp |
| --- | --- | --- |
| 20000 | 18345 | 18345 |
| 8000 | **nothing rendered** | 18345 |
| 4000 | **nothing rendered** | 18345 |
| 2000 | - | 18345 |

### Two-phase brick culling

Single-phase culling can only test against the *previous* frame's pyramid, which
a camera rotation invalidates completely - measured as `visible=0 occluded=2368`
on a yawing camera, leaving only the screen-edge bricks that bail out of the
test for being partially offscreen.

Two-phase fixes that: phase one redraws last frame's visible set with frustum
culling only, a pyramid is built from *that* depth so it describes this frame's
view, and phase two culls everything else against it. A per-brick counter in
`brickPreviouslyVisible` is the handover between frames, seeded to all-visible so
the first frame draws everything.

Two bugs surfaced getting there:

1. The phase-two counters were never read back, so occlusion looked dead when it
   was merely unreported.
2. **The visibility flag was monotonic.** Phase two skipped the occlusion test
   for bricks phase one had already drawn, so once visible a brick stayed
   visible forever, phase one kept redrawing the whole scene, and occlusion
   could never take effect. Phase two now evaluates occlusion for every brick
   even when it does not re-emit it.

**Hysteresis.** A plain visible/hidden flag makes phase one's set flip on a
single frame's test. Under fast rotation phase one is seeded from a stale set, so
a brick could test occluded once, drop out, and take several frames to be
re-admitted - which measured as 5.8% missing coverage on the yaw sequence. The
flag is now a count of consecutive occluded frames, and a brick only leaves
phase one after two of them. Out-of-frustum resets the count rather than
accumulating, so re-entering the frustum does not inherit a history from a
different view.

Coverage against the same scene with occlusion disabled:

| camera | occluded | coverage |
| --- | --- | --- |
| static interior | 56 | exact |
| roof-down | 8 | exact |
| yaw (2.4 rad/frame) | 0 | **exact** (was -5.8%) |
| exterior | 5 | -0.09% |

The isolation that found it: forcing the flag to ignore occlusion, and separately
forbidding demotion, both restored exact coverage *and* dropped culling to zero -
which located the error precisely in the demotion decision rather than in the
projection, the depth comparison or the HZB test itself.

### Per-brick HZB culling - working, on by default

Enabled behind the experimental occlusion toggle (`--enable-blas-occlusion` or
the UI checkbox). On Sponza interior it rejects 735 of 6496 bricks. Three
distinct bugs had to be fixed to get there, each of which alone made it reject
the entire scene or large parts of it:

1. **The brick path never produced an HZB.** `FinalHZB` was only registered when
   occlusion or screen tracing was on, and this resolver disables both, so the
   cull sampled undefined memory (`visible=0 occluded=6496`). It is now
   registered for the brick resolver too.
2. **No first-frame validity flag.** The pyramid is written at the end of a
   frame, so frame zero has none. Culling against it rejects everything.
3. **Two errors in the test itself.** It compared the box's *far* corner
   against the occluder - "nearest" is the minimum depth in a forward projection
   but the **maximum** in reverse-Z - and it sampled a single texel at the
   rectangle's centre, which is not conservative when the rectangle straddles
   texels. It now samples the four corners and keeps the farthest occluder.
   It also sampled mip 0, which is the reduce-to-power-of-two staging level
   rather than a reduction; the usable chain starts at `hzbFirstValidMip`.

Over-cull went 34% -> 4% -> 0.29% -> 0% as each of those was fixed, and the
remaining accuracy came from the SPD race fix and two-phase culling above.

### Early-Z: the face configuration, and what still blocks it

The original objection was that back faces make the DDA hit *nearer* than the
rasterized depth, so depth only increases in reverse-Z (`DepthGreater`), and a
`GreaterOrEqual` test can then early-accept but never early-reject.

That is fixable in principle. Early reject needs `DepthLess`, meaning the shader
may only ever push depth *away*, which holds if the rasterized geometry is at or
in front of every possible hit:

- **Camera outside the brick:** draw the three *front* faces. The hit is at or
  behind the front face, so depth decreases. The clip-space z clamp already
  added for far-plane survival also keeps a front face that crosses the near
  plane from being clipped.
- **Camera inside the brick:** front faces have vertices behind the eye
  (`w <= 0`), which no z clamp can rescue. Emit a full-screen triangle pinned to
  the near plane instead - it covers the brick conservatively, and being the
  nearest possible depth it satisfies `DepthLess` as well.

Both branches then satisfy the same guarantee, so the declaration would be
sound. What still blocks it is purely toolchain: `wgslc-naga` is naga 29 with
`wgsl-in`/`spv-out`, WGSL has no conservative-depth attribute, and nothing emits
`OpExecutionMode ... DepthLess`. Injecting it needs a SPIR-V post-process step in
`wgsl2spirv` - a small binary edit inserting one instruction after the entry
point's existing execution modes. That is the remaining work; the face
restructuring is only worth doing together with it, since on its own it changes
no fragment count.

### The shared depth pyramid race - found and fixed

`SparseVoxelSharedHZBG20Tests` failed **9 runs in 10**; it now passes **10 in
10**. The bug was in `assets/shaders/common/wgsl/spd_core.wgsl`, shared by
virtual geometry and every voxel HZB consumer.

Each reduction level reads `spdLds[2 * lid]` and writes `spdLds[lid]`. Those
alias whenever one lane's destination is another lane's source (`lid == 2*lid'`),
so with nothing between the read and the write a lane could overwrite a
neighbour's input before that neighbour consumed it. Hence a handful of wrong
reductions that moved from run to run - the measured signature was 12 wrong
values out of 43691.

No barrier was there because `workgroupBarrier()` must be reached uniformly and
the levels are guarded by `if (lid < N)`. The fix restructures every level to
read into a register under the guard, synchronise *outside* it, and only then
write back:

```wgsl
let active8 = all(lid < vec2<u32>(8u, 8u));
if (active8) { reduced = spdReduce2x2(...); }   // read
workgroupBarrier();                              // uniform
if (active8) { spdLds[...] = reduced; ... }      // write
```

The first reduction had the same defect without any guard at all - all 256 lanes
read and wrote in sequence - and is fixed the same way.

This was invisible in Release until the `assert`/NDEBUG fix, and it degraded the
real renderer's HZB, not just the test.

### Measurement warning: this machine drifts under sustained load

Every absolute timing in this document was taken on a machine that had been
benchmarking continuously for hours, and it drifts badly. The control:
**SVRaster, with completely unchanged code, measured 36.98 ms early in the
session and 58.48 ms later.** After a three-minute cooldown it was still 59.76 ms
with a p10/p90 spread of 38.9/84.0.

Consequences, which apply to every number below:

- **Cross-batch comparisons are meaningless.** A change that appears to halve a
  pass may simply have been measured while the machine was cooler.
- **Per-pass attribution is worse than the frame total.** After the DDA rewrite
  `BrickResolve` appeared to improve 10x - a pass that was not touched at all.
  That is barrier/wait time migrating between timer windows, not work.
- The only comparison worth anything is **within a single back-to-back batch**,
  and even then only the ordering, not the magnitudes.

What survives: two independent batches agree on the ordering - **bricks fastest
or tied, Tree64 slowest**. The size of the brick advantage does not survive
(large when cool, negligible when throttled, which is what one expects when
everything converges on a thermal ceiling).

Optimizations here are therefore justified by **identical output plus enumerable
work removed**, not by a timing delta. Real numbers need a Metal capture on a
cold machine.

### Optimization: the fragment DDA

Bit-identical output (`ff0314ec866b4095` before and after on the shell, matching
coverage on both Sponza views), so this is pure work removal:

- **One reciprocal per fragment.** The slab test, the DDA setup and the
  entry-face pass each recomputed `1/direction` per axis - about nine divisions
  where three suffice.
- **Deleted the entry-face pass entirely.** It was a third full pass over the
  three slabs to find which one produced the entry; the slab test already
  computes exactly that, so the answer is one comparison on values in registers.
- **Vectorized the slab test.** Substituting a tiny direction for a
  parallel axis makes the interval either unconstrained or empty on its own,
  which is what the per-axis branch was hand-coding.
- **Hoisted `nodes[brickIndex].occupancyIndex` out of the DDA loop.** It was a
  storage load *per step* for a value constant across the fragment.
- **Cached the occupancy word across steps.** One word covers 32 consecutive
  cells - four full rows along x - so a ray stepping in x reuses it repeatedly.
- **Loop bound from the sub-box span** rather than a fixed 24.

### Optimization: draw the occupied sub-box

Each node now stores its inclusive occupied cell range packed into six 3-bit
fields. Bricks that sit on a surface occupy a thin slice of their 8-cubed box,
so both the drawn geometry and the DDA were mostly empty space. Drawing the
tight box removes the fragments that could only ever miss and the leading empty
span of every march that hits:

| | before | after |
| --- | --- | --- |
| `BrickDraw` | 2.805 ms | **1.877 ms** |
| brick chain | 4.115 ms | **3.187 ms** |
| frame GPU p50 | 29.98 ms | **28.79 ms** |

Coverage is unchanged, so this is pure work removal.

Next candidates, in order of expected value:

1. **Early-Z is disabled** because the fragment writes `frag_depth`, so every
   fragment runs the full DDA before the depth test can reject it. The hit is
   always at or behind the rasterized front face, which in reverse-Z means depth
   only ever decreases - exactly the condition a conservative-depth declaration
   (`DepthLess` in SPIR-V) needs to re-enable early-Z. WGSL has no attribute for
   it, so this needs either a SPIR-V post-process or a translator that emits it.
   This is the single biggest remaining win: it would let overlapping bricks
   reject before shading rather than after.
2. **`BrickResolve` is 1.2 ms** and unchanged by any of this - it is a
   full-screen pass reading three attachments. Folding it into the G-buffer pass
   would remove a full-resolution round trip.
3. Front-to-back brick submission, which only pays off once early-Z works.

### The HZB over-cull was a flag-numbering mismatch

Symptom, as reported: "HZB culling is culling lots of wrong bricks that should
not be culled, and the results change from frame to frame."

The cause was neither the test nor the pyramid. The sample passed
`svr::kSparseVoxelCullingReverseZ` into the brick culling uniform, which is
**bit 4**. `sparse_voxel_brick_cull.wgsl` reads reverse-Z as **bit 2** - which
is `kSparseVoxelCullingPreviousFrame` in the SVRaster numbering. So `reverseZ`
was false on every invocation and the test ran its forward-Z branch against a
reverse-Z pyramid: it reduced the pyramid with `max` (the *nearest* occluder
rather than the farthest) and compared it against the box's *farthest* corner.
That rejects a large fraction of genuinely visible geometry, and it changes its
mind every frame because both sides of the comparison move with the depth
buffer.

The fix is to stop borrowing the SVRaster constants. `kSparseVoxelBrickCull*`
now live in `SparseVoxelBrick.hpp` beside the shader that reads them.

Measured on the 128-cubed shell, `--disable-lod` so the cut is all leaves:

| | visible bricks | covered pixels | output hash |
| --- | --- | --- | --- |
| `--disable-occlusion` | 1352 | 174722 | `6a18956bf81607ce` |
| occlusion on | **626** | 174722 | `6a18956bf81607ce` |

54% of bricks rejected, **bit-identical output**. That is what a conservative
occlusion test is supposed to look like, and it is the first time this one has.

An `occlusionDilationVoxels` margin was added at the same time, defaulting to
one voxel. The pyramid is rasterized from bricks chosen by the same LOD cut, and
a coarse brick's OR-reduced occupancy dilates its surface by up to one of its
own voxels - so the depth it contributes is *nearer* than the geometry it stands
for. The margin is applied by dilating the world-space box before projecting it,
not by biasing the depth value, which keeps it exact under any near/far
convention. Sweeping it on the shell (a single-level cut, where it is not
needed) gives 549 / 626 / 760 visible at 0 / 1 / 2 voxels, all bit-identical -
so it is a pure conservativeness dial. The mixed-cut case it exists for needs a
scene like Sponza to exercise.

### The brick pyramid was 1x1 whenever screen tracing was off

Found while making bricks the default. `fullResolutionHZB` was
`occlusionEnabled || sceneScreenTraceEnabled`, and `occlusionEnabled` is
deliberately cleared for every non-SVRaster resolver a few hundred lines earlier
- the two-phase prepass exists for per-voxel culling, which these resolvers do
not do. So the brick path's depth pyramid collapsed to a **1x1 texture with one
mip** unless screen tracing happened to be enabled and propped it up.

The default configuration (`--shadow-backend tree64` with screen tracing on) hid
this completely. `--shadow-backend packed-dda`, `--disable-screen-trace` or
`--depth-only` exposed it, and the failure is silent - the test still runs, it
just compares every brick against a single depth value:

| | occluded bricks |
| --- | --- |
| `--shadow-backend packed-dda --disable-lod`, before | **0** |
| same, after | **726** of 1352, output hash unchanged |

The brick requirement is now captured in `brickOcclusionEnabled` *before* the
prepass bypass clears the flag it was riding on, and both `fullResolutionHZB`
and the `FinalHZB` registration read that one flag so they cannot drift apart.

### Representation: two-level occupancy and inline materials

Three changes to what a brick stores, all of them removing memory traffic from
the fragment shader rather than changing what it computes. Coverage on the shell
is **174724 before and after** with LOD on, 174722 with `--disable-lod`, so
these are pure work removal.

**A 4-cubed coarse mask in the node (64 bits).** Bit `x + 4y + 16z` is set when
any of the eight voxels under it is occupied. It lives in the node rather than
beside the occupancy words so the vertex shader forwards it as two flat
varyings: the DDA is now two-level, the coarse walk runs entirely in registers,
and the occupancy buffer is only touched inside a coarse cell that has something
in it. A surface-fitted brick is mostly empty, so most of the march is now free.

**Occupancy words carry their prefix popcount.** `array<vec2<u32>, 16>`, `.x`
the bits and `.y` the popcount of every bit before that word. The material
lookup used to sum up to fifteen words at the end of every hit - fifteen
dependent storage loads and fifteen popcounts per fragment. It is now one masked
popcount on a value already in a register, because the DDA loaded that word to
test occupancy anyway.

**Eight-byte inline materials.** A voxel used to cost a 4-byte index plus a
dependent, scattered 32-byte `SparseVoxelMaterialGPU` load. Voxels need very
little of that record, so it is packed inline instead:

```
packed.x : R (0..7) | G (8..15) | B (16..23) | roughness (24..31)
packed.y : emissive (0..7) | metallic (8..15) | reserved (16..31)
```

The indirection disappears and the record is 4x smaller. Emissive collapses to
one scalar tinting the surface's own albedo, which is what voxel emitters were
doing anyway. `SPARSE_VOXEL_SURFACE_PACKED_MATERIAL` carries it through
`ResolvedVoxelSurface`'s reserved fields, so the material G-buffer decodes it
directly and bricks never index the scene material array. `sparse_voxel_gbuffer.wgsl`
had to learn the same flag: it was gating its material fetch on
`NO_MATERIAL` alone, and a packed-material surface has an invalid `voxelIndex`.

**Two render targets instead of three.** The reserved high half of `packed.y`
holds the hit face and surface flags, so the separate `faceFlags` R32Uint target
is gone - 8 MB written and 8 MB read back per frame at 1080p, for four bits.

### Stale light on newly disoccluded surfaces - two identity bugs

Symptom: walk the camera into the shell and interior pixels stay lit for a few
frames before settling to dark.

Both SVGF (`history_surface_keep`) and the light field
(`light_field_reproject_history`) accept a reprojected history sample only when
its `(instanceId, voxelId)` matches **exactly**. Bricks are built in world space
as one implicit instance, so `instanceId` is 0 on every brick pixel and
`voxelId` carries the whole test.

**Regression, mine.** Packing materials inline left bricks with no voxel index,
and `voxelId` was pointed at the packed base colour and roughness instead. On a
uniformly coloured surface that is the *same value on every pixel*, so history
validation degenerated to "always accept" and the exterior's lighting reprojected
straight onto the interior wall. The draw pass now emits a real per-voxel
identity - `occupancyIndex * 512 + cellIndex` - which is unique, stable under
camera motion, and unlike the source octree's voxel index does not alias across
the cells of an LOD-coarse brick.

Carrying it cost no bandwidth. The three colour targets became **one**
`RGBA32Uint`: packed material in x and y with the hit face and flags in y's
reserved half, the identity in z, and the ray distance bit-cast into w. Depth is
the only value the hardware insists on owning separately, and `textureLoad` with
no filtering and no blending makes the float round trip exact. That is one
target and one full-screen read where the pass started with three of each.

**Pre-existing, and it affects SVRaster too.** Instance plus voxel does not
identify a *surface*. A shell one voxel thick shows the same voxel from both
sides, so moving through it keeps the identity while the visible face flips. SVGF
rejects that anyway because it also compares normals; the light field has no
history normal to compare against, so it accepted it. The face class - dominant
axis of the geometry normal plus sign, three bits - now travels in the spare high
bits of the identity's instance channel, which makes the existing exact-equality
test reject a face change at no extra cost and with no new binding.

Neither of these can be confirmed headlessly - "settles to dark over a few
frames" is not something the frame signature captures. Coverage and the suite are
unchanged; the visual check is interactive.

### Culling and vertex path

- **The parent node is no longer fetched.** The LOD cut needs the parent's box,
  which was a dependent, spatially scattered 64-byte load for every node in the
  dispatch - and nodes are emitted in spatial-key order *per level*, so a node
  and its parent never coalesce. Bricks sit on a regular grid, so the parent box
  is arithmetic on the node's own fields: `floor(round(min / (8v)) / 2) * 16v`.
- **The box is transformed once, not eight times.** The frustum test, the
  occlusion rectangle and the area metric each walked the eight corners with a
  full `mat4` product. They now share one `brickProject()`, and the corners come
  from one transformed origin plus three transformed edge vectors - four matrix
  products for the whole box instead of eight (or twenty-four).
- **Phase one usually skips the projection entirely.** It runs no occlusion test
  and, with the area metric off, needs no rectangle.
- **The area metric is opt-in** (`--brick-measure-area`, or the UI checkbox). It
  costs a contended global atomic for every surviving brick, which was the most
  expensive thing in the pass, and it only means anything during a profiling
  session. It reads 801146 area pixels against 174722 covered on the shell, a
  4.6x overdraw factor.
- **The HZB texel window is bounded to 4x4.** The mip chain runs out of levels
  before the largest bricks run out of screen area, and at the coarsest level a
  near brick can span the whole mip - hundreds of taps for the least useful test
  in the pass. Beyond the window it returns visible, which is conservative.
- **The vertex shader's face corner is bit arithmetic** rather than an indexed
  lookup table, and the fragment reads its brick data from flat varyings instead
  of re-fetching the node - `occupancyIndex` and `materialBase` were two
  dependent loads per pixel for values the vertex shader already had.
- **The fragment's final depth is two dot products**, not a `mat4` product: it
  needs clip z and w, and it already has its screen position.

### Measured at last: Sponza interior, 1920x1080

The headless benchmark *can* resolve these passes. The earlier conclusion that it
could not was measuring the wrong frame, not hitting a tool limit.

Two settings were wrong, and either alone makes the capture meaningless:

- **The LOD threshold.** `--frames N` overrides the interactive default with 64
  pixels so the bounded smoke check exercises a coarse cut. At 64 the whole of
  Sponza resolves to **eight root bricks** covering 25k pixels. `--lod-threshold
  1.5` - the interactive default - gives ~3200 visible bricks.
- **The camera.** The OBJ preview framing sits outside the model and covers 18k
  pixels. `--test-camera-interior` puts it in the hall: 518400 covered, the full
  screen.

`make bench-sponza` runs it; `make gputrace-sponza` captures the same frame for
Xcode. Sponza, `--voxel-size-meters 5 --lod-threshold 1.5`, interior-yaw, 60
measured frames after 20 warmup, 1920x1080:

| | p50 |
| --- | --- |
| **frame GPU** | **28.20 ms** |
| frame CPU | 30.17 ms |

Top passes:

| pass | p50 ms | share |
| --- | --- | --- |
| TemporalResolve | 10.252 | 36.9% |
| SceneDIReservoir | 7.049 | 25.4% |
| OccupancyShadowTrace | 3.799 | 13.7% |
| SceneReservoirWorklistReset | 1.681 | 6.1% |
| **BrickResolve** | 1.105 | 4.0% |
| **BrickPhase0.Draw** | 1.048 | 3.8% |
| GBuffer | 0.613 | 2.2% |

**The whole brick chain is 2.53 ms of 28.20 - nine percent of the frame.** Within
it, culling is negligible: both phases of cull, both DrawArgs and the mid-frame
pyramid together come to 0.11 ms, against 1.08 ms of rasterization and 1.11 ms of
resolve. Brick culling at 3190 visible / 3455 occluded costs 0.036 ms.

That reframes the remaining work. The 40 ms target is already met at 28.2 ms, and
further optimization of culling, the vertex path or the DDA cannot move the frame
by more than about a millisecond in total - the parent-fetch removal, the shared
projection and the opt-in area metric were worth doing for their own sake, but
they are not where the frame is. What is:

1. **TemporalResolve at 10.3 ms** is over a third of the frame on its own, and it
   is an upscaler running after everything else. `--disable-lighting` removes it
   along with the rest of the shading chain, which is the fastest way to confirm
   what the resolver actually costs.
2. **ReSTIR DI at 7.0 ms**, plus 1.7 ms in a worklist *reset* pass, which is a
   suspicious amount of time for a pass that only clears state.
3. **BrickResolve at 1.1 ms** is now the largest single brick pass, and it is a
   full-screen round trip that exists only because the draw writes attachments.
   Folding it into the G-buffer pass is the one brick optimization still worth
   its cost - and it is worth about a millisecond.

Timings taken with the machine already warm; treat the ordering as solid and the
absolute values as approximate. The thermal drift warning further up still
applies.

### Not measured per change

Everything above is justified by identical output plus enumerable work removed;
no individual change is backed by a before/after timing delta. The frame total
*is* now measured - see the section above - and it says the whole chain these
changes touch is nine percent of the frame, so per-change deltas would be inside
the noise even with a perfect timer.

### Remaining fidelity gap

Bricks cover 174724 pixels against SVRaster's 174306, a 0.24% dilation that is
stable across LOD levels (a hollow shell's silhouette is LOD-invariant, so this
is not the LOD cut). The likely cause is the builder's fill rule: a leaf whose
bounds land a floating-point epsilon below a grid multiple takes
`floor(low / voxelSize)` into the previous cell, dilating the shell by one voxel
in places. SVRaster instead pads by exactly one ULP for watertightness. Snapping
the fill bounds with the same ULP discipline is the fix; it is a fidelity
refinement, not a blocker.

### Representation as built

`SparseVoxelBrick.hpp` / `.cpp`. A brick is 8x8x8 stored as 512 occupancy bits
plus one material index per occupied voxel, addressed by the popcount of the
bits before it. That keeps appearance **exact** — the thing the Tree64 resolver
cannot do — at one word per voxel, the same cost the octree material array
already pays.

Bricks form an octree: a level-(L+1) node covers the same volume as eight
level-L children at half the voxel size, its occupancy being their OR-reduction.
Because that makes the LOD metric monotonic down the tree, the cut needs no
traversal at all: a node is in the cut when its own projected voxel size is
small enough and its parent's was not. One dispatch, no queue, no barriers, and
crack-free by construction. This replaces the queue-based level-by-level
traversal the plan originally borrowed from Virtual Geometry.

Deliberately out of scope, per the current instruction: no `.vsvb` file format,
no importer flag, no streaming. Bricks are built in memory from the resident
octree leaves with instance transforms baked into world space, which is why a
non-uniform instance scale is rejected rather than supported.

## Benchmark: Sponza, camera in the hall, looking around

`--benchmark-sequence interior-yaw` (new): places the camera at the horizontal
centre of the *occupied* voxels at 12% of scene height, and sweeps the yaw
through grazing and axial views. The previous `interior-static` used the octree
root's centre, which for a wide flat scene like Sponza is the centre of the
enclosing **cube** — well above the roof.

1920x1080, `--lod-threshold 1.8 --tree64-lod-threshold 1.8`, voxel size 5,
150 measured frames after 30 warmup:

| resolver | GPU p50 | p10 | p90 | resolver chain only |
| --- | --- | --- | --- | --- |
| SVRaster | 36.98 ms | 32.05 | 38.54 | 0.028 ms |
| Tree64 primary | 38.69 ms | 35.52 | 40.29 | 0.007 ms |
| Brick hierarchy | **29.98 ms** | 27.77 | 31.08 | 4.115 ms |

Read these as directional only. The per-pass attribution in this report is not
trustworthy — it puts 10.7 ms in `TLASInitialize` for one resolver and 20.6 ms in
`FinalHZB` for another, which is barrier/wait time being charged to whichever
pass happens to precede it. The **frame totals** are the usable signal, and they
are consistent across runs.

Two things do stand out and are worth confirming under a real capture:

- The brick path wins the frame by ~7 ms despite its own chain being by far the
  most expensive (4.1 ms of cull + raster + resolve against 0.03 ms of binning).
  That is the trade the design predicted: it does more work in its own passes and
  less everywhere downstream, because hardware rasterization and early-Z replace
  the tile binning and the per-candidate depth resolution.
- The resolver chains themselves are all small relative to the frame here.
  Whether that holds at a voxel size fine enough to stress the binning chain is
  the open question, and the one the original 36% profile was measuring.

## Selector plumbing — landed

- `enum class GBufferResolver { SvRaster, Tree64Primary, BrickHierarchy }`,
  **defaulting to `BrickHierarchy`**. A scene that cannot be bricked - anything
  only partially resident - falls back to SVRaster with a log line, so the
  default is always renderable.
- Every sample-driven ctest now names its resolver explicitly. They were all
  calibrated against SVRaster - several are occlusion- or frustum-equivalence
  runs over the per-voxel path, which bricks do not have - so a default change
  would have silently repointed them at a different renderer. The one
  exception is `SparseVoxelRendererSampleSmoke`, left unpinned on purpose so
  the default path keeps coverage.
- The default LOD error is **1.5 px**, down from 8. The brick resolver
  rasterizes its cut with early-Z instead of binning it, so a finer cut costs
  draw instances rather than candidate records. On the shell that cut is all
  leaves: 1352 bricks, of which occlusion rejects 726.
- CLI: `--gbuffer-resolver svraster|tree64|bricks`. `bricks` fails with a message
  pointing at part C; `tree64` requires `--shadow-backend tree64`, since it
  marches the clipmap that backend builds.
- ImGui: a live combo in the main panel. Switching rebuilds the render graph
  through the same path the occupancy and occlusion toggles already use, and the
  choice lives in `SampleState` so it survives the rebuild. Tree64 is refused
  unless the Tree64 shadow backend is active, since it marches that clipmap.
- Non-SvRaster resolvers skip the binning and resolve registrations entirely and
  force the two-phase prepass off: that prepass exists to build a provisional HZB
  for per-voxel occlusion culling, which these resolvers do not do. Their scratch
  buffers are only allocated when they are selected.

## Test plan

| Area | Test |
| --- | --- |
| A1 | Existing `SparseVoxelBinningTests` — tile ranges must be identical to the serial prefix, including the overflow-clamp case |
| A3(a,b) | `SparseVoxelOcclusionGBufferEquivalenceG22` and the output-signature tests — must be bit-identical |
| A3(c) | Same, but expected to differ; if it does, the change becomes opt-in |
| A3(d) | New test: shuffled candidate order must resolve to the same surface |
| B | Coverage/depth agreement with SvRaster within the clipmap, tolerance-based |
| C | Same, plus a brick-vs-octree occupancy round-trip test on import |

## Order of work

1. ~~A1 (parallel prefix)~~ — **done**.
2. ~~A3 (a) and (b)~~ — **done**, plus the shared per-bucket batch bound.
3. ~~A4 (slim bucket entries)~~ — **done**. Landed ahead of A2 because it hits
   both named hot passes at once.
4. A2 (fuse count into preprocess) — see the note below on why it is now the
   smallest remaining item.
5. ~~A2 (fuse count into preprocess)~~ — **done**. Projection now counts while
   the tile rectangle is still in registers, and the bucket clear moved ahead of
   it.
6. ~~Selector plumbing + Part B material-free~~ — **done and rendering**, coverage
   matches SVRaster exactly on the shell scene.
7. Re-trace on a scene that actually loads the raster chain. A3 (c)/(d) only if
   resolve is still dominant. This is also what tells us whether the Tree64
   resolver is actually faster than the optimized SVRaster chain.
8. Tree64 attribute payload, promoting Part B to a full-appearance resolver.
9. Part C, in its own milestone: format → importer → streaming → culling → draw.

## The voxel tests did not run in Release — fixed

Eight svraster tests failed in `build-release` while passing in `build` (Debug).
None were engine bugs.

**Seven were the same harness bug.** The tests use `assert()` as their check
mechanism and wrap the call under test in it — `assert(volume.configure(...))`.
Release compiles with `-O3 -DNDEBUG`, so `assert` expands to `((void)0)` and
*the call disappears with the assertion*, leaving objects unconfigured and the
suite dereferencing null (`SegFault`) or tripping `[RHI] Src overflow`. Every
`tests/voxel/*.cpp` that includes `<cassert>` now does `#undef NDEBUG` first;
`cassert` re-reads `NDEBUG` on each include, so this restores `assert` regardless
of what included it earlier. 26 files.

This was not cosmetic: in Release those suites were not testing anything, and had
been silently passing over their own setup for as long as `-DNDEBUG` has been set.

**The eighth was a real coverage regression.** `SparseVoxelTree64CutoverT9`
asserted `occupied=[1-9][0-9]*` on the static chunk directory. That assertion was
added in `030dc4db`; the dynamic layer landed in `a9759b2a` (HEAD) and made the
static directory permanently empty for any instanced scene, so the test could no
longer pass and was guarding nothing. The sample's Tree64 state log now also
reports the layer that actually holds the geometry
(`dynamic=1 dynModels=4 dynInstances=1 dynNodes=26692`), and the test asserts
readiness plus a populated dynamic layer — its original intent, against the right
layer.

The full `svraster` label went from 8 failures to 0 in Release.

**One intermittent failure has since appeared:** `SparseVoxelSharedHZBG20Tests`
asserts that the GPU depth pyramid matches its CPU reference. It passed
repeatedly after the NDEBUG fix and now fails consistently. It is **not** caused
by the resolver work - reverting every source change while keeping the asserts
active still fails, so this is a pre-existing GPU/CPU mismatch that the NDEBUG
fix made visible, and it is timing or state dependent rather than deterministic.
Worth its own investigation.

### A4 (landed): slim bucket entries

`scatterTileCandidates` wrote a 32-byte `TileCandidate` per (voxel, tile) pair
in which `instanceIndex`, `voxelIndex`, `quadrant` and the conservative hit
lower bound were all copies of fields in `projectedVoxels[index]` — which the
resolver loads anyway for the pixel bounds. The bucket path now stores a single
`u32` projected index:

- scatter writes 4 bytes per entry instead of 32;
- resolve reads 4 + 32 bytes per candidate instead of 32 + 32.

The legacy emit/sort chain still produces full records, so the resolver keeps
both paths and picks between them with `resolveControls.y`, which is uniform for
the whole dispatch. Values are identical either way, so resolved surfaces are
unchanged — the G-buffer and depth equivalence tests confirm it.

Follow-up left on the table: the renderer no longer reads the 8M-entry
`TileCandidate` array at all, so its 256 MB allocation can shrink to a stub once
the legacy chain is confirmed dead. That is a memory win, not a speed one.

## Measurement status — read before claiming a number

The headless benchmark (`--benchmark-shadow`, per-pass `gpu_passes` timings)
**cannot currently resolve these passes**. On every scene this tree can build:

- Sponza only imports at `--voxel-size-meters 5` or coarser; 1 and 2 both fail
  the importer's `maximumEstimatedCells` budget, which has no CLI override. At
  5 m the whole raster chain sits at the 4–6 µs dispatch-overhead floor.
- The synthetic 128³ shell is no better; the frame is 48% ReSTIR DI, 25%
  compose, 18% shadow trace, and the raster chain does not reach the top 14.

At that floor the numbers are noise: across two runs of the *same* build,
`Final.TLASInitialize` moved by 1.2 ms and the frame total by 7 ms, both far
larger than anything these passes cost. The pre-change serial `prefixTileBuckets`
also measured 4 µs, which is impossible for 129600 dependent serial atomic loads
— further evidence the timer is reporting overhead, not work.

So A1/A3/A4 are justified structurally and verified for correctness, not
measured. Confirming the win needs a Metal GPU trace on a scene with a real
voxel count — the same setup that produced the original 36% / 9.6% / 4% split.
Raising `maximumEstimatedCells` behind a CLI flag would make this measurable
headlessly and is worth doing before the next optimization pass.

## The Xcode "hang" was a Debug build, and why that is not obvious

Symptom: running from Xcode stops at `building hierarchy 0%` and never
finishes. Running `make xcode-release` and waiting does not help.

**Cause.** At `-O0`, Apple's libc++ enables `_LIBCPP_ASSERT_INTERNAL`, and
`std::__tree_remove` asserts `__tree_invariant(__root)` - a full validation of
the entire red-black tree - on **every** `map::erase`. The OBJ importer erases
per level at `SparseVoxelOBJBuilder.cpp:447`, so the loop goes from O(k log n) to
O(k n). On Sponza that is not slow, it is unfinishable. A `sample` of the process
shows 100% of stacks inside `__tree_sub_invariant`.

Confirmed by symbol, not by inference:

| object | `__tree_invariant` |
| --- | --- |
| `editor.build/Release/.../SparseVoxelOBJBuilder.o` | 0 |
| `editor.build/Debug/.../SparseVoxelOBJBuilder.o` | 1 |

The same import in Release finishes the hierarchy in **5.95s** and the whole run
exits cleanly.

**Why `make xcode-release` did not fix it.** Two independent traps.

1. *The scheme owns what Run does.* CMake generated the scheme with
   `LaunchAction buildConfiguration="Debug"`, so Cmd-R rebuilt and launched Debug
   no matter what had been built from the command line. Now pinned with
   `XCODE_SCHEME_LAUNCH_CONFIGURATION "Release"`.
2. *Both configurations link to the same path.* The binary has to sit beside the
   generated `assets/` tree, so `RUNTIME_OUTPUT_DIRECTORY_DEBUG` and
   `_RELEASE` both point at `${CMAKE_BINARY_DIR}/tests`. Nothing on disk
   distinguishes them and building either one silently replaces the other -
   which is how a Debug binary ends up at the path you believe is Release. This
   is still true; it is the reason for the mitigation below.

**Mitigation.** The sample now logs its configuration as its first line, and
warns explicitly on Debug. `SPARSE_VOXEL_BENCHMARK_BUILD_TYPE` was derived from
`CMAKE_BUILD_TYPE`, which multi-config generators leave empty - so the Xcode tree
reported `""` in the one place the distinction mattered. It now uses `$<CONFIG>`.

```
INFO Sparse voxel sample: Release build
```

If that first line says Debug, nothing else in a profiling session is worth
reading.
