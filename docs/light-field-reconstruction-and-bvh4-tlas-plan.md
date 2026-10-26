# Light-field reconstruction and the BVH4 TLAS

**Status:** Part A, Part B, and Part C complete 2026-08-22.

Two independent pieces of work. They share one motivation: `--lighting-scale 2`
now takes the 1080p terrain frame from 51.5 ms to 26.7 ms (19 -> 37 fps), so the
quality of sparse-ray reconstruction has become the thing that decides whether
that speed is usable.

## Part A — Light-field reconstruction

### A.0 What is actually there (correcting the premise)

The reconstruction is **not** bilinear, and it **does** use normals and depth.
`sparse_voxel_light_field.wgsl` already implements a joint bilateral upsample:

- a bilinear footprint over the ray grid, each tap multiplied by
  `light_field_slot_weight(rayPixel, identity, geometryNormal, depth)`;
- that weight is **continuous**, not a boolean accept: normal alignment and
  relative depth each ramp from one at an exact match to zero at the threshold
  that would have rejected the tap, with a comment recording that this replaced
  a boolean test specifically to remove the filter seam;
- plus an explicit 8-tap ring gather over the ray grid, because at full ray
  resolution the bilinear footprint degenerates to a single tap and contributes
  no reuse;
- plus a fallback 8-neighbour search when the accumulated weight is ~0.

There is also **no separate upscale pass**: `lightField` and friends are
allocated at full raster `width`/`height`. `sceneRayWidth`/`sceneRayHeight` is
the *ray* domain only. Sparse rays are reconstructed into a full-resolution
accumulator in place.

So the work is not "add normals and depth". It is that the existing filter does
not adapt to how sparse the rays have become.

### A.1 The real weaknesses

1. **Fixed 3x3 ray-space footprint at every reduction factor.** At
   `--lighting-scale 4` the ray grid is 240x135 for a 960x540 raster — 16 raster
   pixels per ray — and the filter still gathers the same 3x3. Valid-sample
   density per raster pixel falls 16x while the footprint stays constant.
2. **Fixed bilateral thresholds** (`settings.thresholds.x/y`). Sparser ray grids
   put the nearest compatible sample further away, across more surface
   curvature, so a threshold tuned at full rate rejects more taps exactly when
   samples are scarcest. Rejection compounds with reduction instead of relaxing.
3. **Unmeasured fallback.** When everything rejects, the pixel falls through the
   ~0-weight path. What it lands on — stale history, a neighbour ignoring
   compatibility, or nothing — is the visible artifact at high reduction, and it
   is currently not instrumented.

### A.1b Measured (A.2.1 done, 2026-08-22)

`--terrain-render-distance-km 16` at 1080p, 60 frames, covered pixels 254379:

| configuration | zero-tap pixels | mean accepted taps |
| --- | --- | --- |
| `--ray-resolution-divisor 1` | 0.01% | 6.58 |
| baseline (divisor 2, default) | 0.93% | 4.91 |
| `--ray-resolution-divisor 4` | 6.04% | 3.68 |
| `--lighting-scale 2` | **6.45%** | 6.79 |
| `--lighting-scale 4` | **14.79%** | 5.92 |
| `--lighting-scale 4 --ray-resolution-divisor 4` | 16.40% | 4.52 |

**`--lighting-scale 2` — the 26.7 ms / 37 fps path — leaves one covered pixel in
fifteen with no measurement at all.** Those keep history, which is fine standing
still and is nothing under disocclusion. That is the quantified answer to
whether that speed is shippable: there is a real gap, and it is bounded.

**The failure is bimodal, and that reorders the work.** Lowering the ray
resolution *raises* mean accepted taps (4.91 -> 6.79 at scale 2) while also
raising the zero-tap count. A coarser ray grid spreads each ray's bilinear
footprint over more raster pixels, so the pixels that succeed succeed *more*
easily — and a growing minority, at edges and curvature where no nearby ray
shares the surface, get nothing. Widening the footprint globally (A.2.2) would
therefore spend work on pixels that already have 6-7 taps, and would not
obviously help the ones that have zero.

So **A.2.4 becomes the first change to try, not the last**, and A.2.2 is
demoted to "only if the guided fill turns out to need a wider search".

### A.2 Work

- **A.2.1 DONE.** Counters live in `sparse_voxel_light_field.wgsl` (binding 29),
  reported as `Sparse voxel light field reconstruction: covered=... zeroTap=...
  meanTaps=...` under `--test-frame-signatures`. Originally specified as: Add two counters to the light-field pass: fraction
  of raster pixels reconstructed with zero accepted taps, and mean accepted taps
  per pixel. Report per `--lighting-scale`. Without this, "the upscaler is weak"
  cannot be separated from "the rays are too sparse for any filter". This is the
  measurement that tells us whether A.2.2 and A.2.3 are worth anything.
- **A.2.2 Scale the footprint with the reduction factor.** Gather radius derived
  from `rasterDimensions / rayDimensions` rather than fixed at one ray cell.
  Cost is quadratic in radius, so this must be measured against A.2.1's
  accepted-tap count, not adopted blindly.
- **A.2.3 Relax thresholds with the reduction factor.** Same derivation. A tap
  one ray cell away at quarter rate spans four raster pixels of surface, so the
  normal and depth tolerances should widen proportionally.
- **A.2.4 Only if A.2.1 shows a significant zero-tap population:** a
  guided fill for pixels with no compatible sample, using the raster's own
  normal/depth as the guide and the nearest accepted tap as the source. This is
  the piece closest to "a better upscaler", and it should be built last, because
  A.2.2/A.2.3 may remove most of its input.

### A.2b Implemented (A.2.4 + A.2.3, 2026-08-22)

A **recovery pass**, entered only by pixels that accepted nothing under the
ordinary gates. `light_field_slot_weight` is now a thin wrapper over
`light_field_slot_weight_relaxed(..., relaxation)`, which slides the normal gate
toward -1 and scales the depth gate; the ordinary filter passes `1.0`. The
recovery searches ring radius 1 at relaxation 3, then ring radius 2 at
relaxation 6, and stops at the first ring that yields anything. A recovered
estimate is credited as a **single** observation however many taps it merged, so
the accumulator can move off invalid history without a biased fill converging as
if it had been measured.

This is A.2.3 and A.2.4 folded together: relaxing the gates *is* the guided
fill, and only the failing minority pays for it. A.2.2 was not implemented and
should stay unimplemented — the measurement in A.1b showed the successful pixels
already average six to seven taps, so widening the main gather spends its cost
where nothing is wrong.

| configuration | zero-tap before | zero-tap after | frame median before | after |
| --- | --- | --- | --- | --- |
| baseline (divisor 2) | 0.93% | **0.00%** | 51.86 ms | 51.85 ms |
| `--lighting-scale 2` | 6.45% | **0.01%** | 26.69 ms | 26.71 ms |
| `--lighting-scale 4` | 14.79% | **0.23%** | 18.98 ms | 19.38 ms |

Cost is flat at baseline and at scale 2, and +0.4 ms at scale 4, which carries
the most recovery work. Mean accepted taps rise (4.91 -> 4.94, 6.79 -> 7.23,
5.92 -> 6.79) because recovered pixels now contribute.

Quality, `--lighting-scale 2` against baseline, both with recovery: `instability`
0.000000 on both, `edgeEnergy` 0.1158 vs 0.1140, `meanLuminance` 0.16827 vs
0.16832. The fill is not injecting visible noise or shifting brightness.

### A.3 Acceptance

Zero-tap pixel fraction at `--lighting-scale 2` below what it is today, frame
time still under 33.3 ms, and no regression in the temporal metrics
(`instability`, `edgeEnergy`) that the sample already reports.

## Part B — BVH4 TLAS

### B.0 What is already there

`src/runtime/softwarebvh` is a production GPU ray tracer and has almost
everything this needs:

- `BVH4NodeGPU` in `software-raytrace.wgsl`, SoA layout (`childMinX/Y/Z`,
  `childMaxX/Y/Z` as `vec4`), 4-wide SIMD-friendly.
- A **BVH4 TLAS ray traversal** behind `SWRT_TLAS_BVH4`, applying the
  per-instance `transform` at the TLAS leaf — already once per instance entry.
- **CWBVH** behind `SWRT_WORLD_CWBVH` plus DOBB conversion, so that endpoint is
  reachable later without a second rewrite.
- `SoftwareTLASManager` with `INSTANCE_FLAG_DYNAMIC`, partial rebuild
  (dirty-ratio and SAH-growth thresholds) and refit with SAH-growth monitoring,
  over a SAH binned builder — the streaming/update story.
- **Ray sorting** (`raySortBinCounts`/`Offsets`/`Ids`), a two-stage
  BLAS-candidate path, and a workgroup queue — divergence mitigation.
- Traversal statistics counting **TLAS and BLAS node visits separately**.
- **Correction:** `SWRT_TRIANGLE_PRIMITIVES` is passed by CMake but referenced
  by no shader — it is a dead define, not the abstraction. The real extension
  point is **file substitution**: `software-raytrace-primitives-triangle.wgsl`
  and `software-raytrace-primitives-rbvh-card.wgsl` are two existing primitive
  implementations of one interface
  (`load_trace_primitive_metadata`, `trace_blas_primitive_leaf_local`,
  `..._local_cached`, `..._any_hit`, `..._any_hit_cached`). A third file is the
  way in. `localOrigin`/`localDirection` arrive already transformed at the TLAS
  leaf, so no per-node matrix work is involved.

### B.1 Why replace the grid

The current TLAS is the Tree64 clipmap directory, a uniform grid. Its structural
cost is measured: **930 instances produce 677970 references, 729 per instance**,
because one cell size (64-128 m) serves pages from 12.8 m to 13 km. A BVH has no
replication term, and the gap widens as instance sizes diversify beyond terrain.

### B.2 Work

- **B.2.1 DONE.** `tree64TraceTlasBvh4` in `sparse_voxel_tree64_trace.wgsl`,
  behind `TREE64_TLAS_BVH4`, with a matching build variant. It is an
  **ordered** stack descent: each node slab-tests its four children, sorts the
  intersected children by entry distance, and pushes them in reverse order so
  the closest candidate is consumed first. `dispatch.tlasRootNode`/
  `tlasNodeCount` were added to the dispatch struct on both sides; a zero node
  count keeps the directory march, so both broad phases ship in one build and
  compare with a flag rather than a rebuild. Node layout mirrors
  `SoftwareTLASManager::BVH4NodeGPU` exactly. Fails closed on an untrustworthy
  publication and on stack overflow, matching the directory path.

  Superseded plan text: a Tree64 BLAS primitive variant — the analogue of
  `SWRT_TRIANGLE_PRIMITIVES` — entering the existing Tree64 traversal at the
  TLAS leaf instead of `intersect_triangle_transform`.
- **B.2.2 DONE.** `SparseVoxelTree64TLASBridge` converts committed Tree64
  dynamic instances to `InstanceBuildInput`. Two conventions differ and are
  handled there: Tree64 stores `worldToModel` while the TLAS wants
  object-to-world (inverted; exact negation for the translation-only terrain
  case), and Tree64's `modelId` becomes the TLAS `blasHandle`, so a BLAS handle
  addresses a Tree64 model rather than a triangle mesh. Terrain generations mark
  the instance dynamic.

  `TlasOverTerrainPageCut` in `SoftwareBVHRuntimeTests` builds the real 930-page
  cut and measures the thing the whole part is for:

  ```
  terrain cut TLAS: 930 pages, 1859 canonical nodes, 488 bvh4 nodes, 930 leaves
  terrain cut refit: 30 moved pages, refitted=1 partial=0 rebuilt=0
  ```

  **930 leaves for 930 instances against the grid directory's 677970
  references** — the replication term is gone, which is the argument stated as a
  test rather than as prose. Streaming churn takes the refit path without
  falling back to a rebuild.
- **B.2.3 DONE.** `SparseVoxelRendererSample` owns a Tree64
  `SoftwareTLASManager`, publishes a TLAS leaf-to-dynamic-slot map, rebuilds or
  refits the TLAS at dynamic/terrain publication points, binds the BVH4 node and
  slot buffers into `SparseVoxelTree64DynamicTracePass`, publishes
  `tlasRootNode`/`tlasNodeCount`/`tlasInstanceCount`, and selects the variant
  through `--tree64-traversal bvh4` (alias `mirrored-ancestor-bvh4`). Original text: dispatch from the voxel scene-ray chain in place of
  `SparseVoxelTree64DynamicTracePass`, behind the existing `Traversal` enum, so
  grid and BVH4 are one flag apart on the same scene and build.

### B.2c Design correction for B.2.1 (2026-08-22)

**Do not port the Tree64 into the softwarebvh kernel.** The replaceable seam is
inside `sparse_voxel_tree64_trace.wgsl`, and it is small and specific.

The grid broad phase is lines ~1906-1960 of that shader, and it is exactly three
steps: the ray marches directory cells; each cell reads
`dynamicMemberships[directoryIndex]` for a reference range; each reference reads
`dynamicReferences[referenceIndex]` for an instance index; and then the existing
`tree64TraceDynamicInstance(ray, instanceIndex, ...)` runs unchanged.

**A BVH4 broad phase replaces those three steps and nothing else.** The BLAS
traversal, the mirroring, the ancestor stack, the LOD cutoff, the receiver-cell
skip and the fail-closed residency handling are all downstream of the instance
index and are untouched. This removes every hard part the earlier framing
implied:

- no marcher extraction (it was going to be feasible — the marcher touches only
  `dynamicNodes`, `terrainTree64PageNodes`, `dispatch` and the workgroup
  `tree64AncestorNodes` — but it is not needed);
- no new primitives file, and no `SWRT_*` variant;
- no binding of the whole Tree64 state into the softwarebvh pass, which was the
  largest integration cost.

**What B.2.1 becomes:**

1. An **ordered** BVH4 descent in `sparse_voxel_tree64_trace.wgsl` enumerating
   candidate instances front to back. Ordering is not optional: the grid march
   gives it for free via DDA, and the trace relies on it to stop early. Children
   must be sorted by entry distance, as the softwarebvh traversal already does.
2. Binding `tlasBvh4Nodes` and the leaf instance list into the existing trace
   pass.
3. CPU: run `SoftwareTLASManager` over `sparseVoxelTree64TLASInstances` each
   frame and expose its buffers, under the same publication contract terrain
   page streaming already uses.
4. A `Traversal::BVH4` enum value and shader variant — which is B.2.3, and now
   falls out of the same switch rather than needing a separate dispatch path.

The per-frame build now sits at the existing publication seams:
`publishTree64TlasBvh4` is called after ordinary dynamic Tree64 updates and
after planet terrain commits, then the trace pass consumes the most recently
published root/node/leaf counts.

### B.2d Note on the softwarebvh BLAS shape

The softwarebvh BLAS is itself a BVH whose *leaves* call
`trace_blas_primitive_leaf_*`. A Tree64 replaces the whole BLAS traversal, not
just a leaf test, so the clean fit is a **degenerate one-primitive BLAS per
page** whose primitive intersection marches the Tree64. The BLAS descent is then
a single node and costs nothing, and no existing traversal code has to change.
The real cost of B.2.1 is binding the Tree64 model/node buffers into the
softwarebvh pass, which currently binds an entirely different resource set.

### B.3 Acceptance

Grid and BVH4 traced on the same converged scene; compare TLAS and BLAS node
visits (both paths report them) as well as frame time. Keep BVH4 if traversal
work is not materially worse — the reference-replication win stands on its own
for heterogeneous scenes.

## Sequencing

A and B are independent. A.2.1 gates the rest of A. B.2.1 and B.2.2 are
independent of each other and both gate B.2.3.

**Caution carried from earlier work.** Two structural optimisations on this
trace measured neutral (the translation-only transform; the pass-timeline
attribution work found the cost was never where the instrumentation said), and a
dial audit produced a wrong "everything is inert" verdict because it read a
counter that counts ray *slots* rather than traced rays. Instrument first in
both parts, and check the counter measures what its name suggests.

## Part C — 1080p defaults (2026-08-22)

`automaticLightingScale` keyed off the **internal** raster, which at 1080p with
the default 0.5 render scale is 0.52 Mpx — below every threshold — so 1080p
always ran lighting at full internal resolution. It now keys off the **output**
resolution, which is what states the intent.

| output | light field | frame median | before |
| --- | --- | --- | --- |
| 1280x720 | 320x180 | 12.89 ms (78 fps) | — |
| 1920x1080 | 240x135 | **19.49 ms (51 fps)** | 52.35 ms (19 fps) |

**2.7x faster at 1080p**, comfortably past the 30 fps base requirement and over
60 with frame generation. Quality at the new default against the old, same
fixture: `zeroTap` 0.23%, `instability` 0.000000, `edgeEnergy` 0.1137 vs 0.1140,
`meanLuminance` 0.16818 vs 0.16832. The recovery pass from Part A is what makes
this setting shippable — scale 4 measured 14.79% zero-tap pixels before it.

**Headroom.** The terrain fixture covers 49% of internal pixels; there is no
full-screen aerial terrain camera (`--test-camera-roof-down` and
`--test-camera-dolly` both target the Sponza model and report
`coveredPixels=0`). Doubling the covered lighting work from 19.5 ms still lands
inside 33.3 ms, so a screen filled with terrain should hold the 30 fps base — but
that is headroom arithmetic, not a measurement. **A full-coverage terrain camera
is worth adding as a fixture.**

**4K is broken**, separately: 3840x2160 measures 492 ms median with a 2508 ms
p95, which is thrashing rather than lighting cost. Out of scope here and not
investigated.
