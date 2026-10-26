# Distant Tree64 shadows and the 1080p frame budget

**Status:** plan, 2026-08-22. Nothing in Parts 2-5 is implemented yet.

Two goals, tracked separately because they pull in opposite directions:

1. Distant terrain must cast shadows, without a coarse page inventing occlusion.
2. `SparseVoxelRendererSample --world-map world-data --world-map-center 0.5,0.5
   --terrain-render-distance-km 16` must default to 1920x1080 and hold 60 fps
   with frame generation, at least 30 fps and ideally 40 fps without.

## 0. Corrections to the starting picture

Two things I said earlier were wrong, and the plan changes because of them.

**The cut already reaches the full render distance.** I read `spatialLevels=1`
in the log as "one clipmap ring" and multiplied 314 resident pages by the 12.8 m
*finest* page edge to get ~330 m. That count is wrong: `lodCount=1` is the
**internal** page LOD (voxels per page edge), while the **spatial** quadtree cut
runs to `maximumSpatialLOD` and is what carries distance. `buildWorldVoxelChunkLeafCut`
(`PlanetVoxelChunk.cpp:3163`) descends purely on distance and projected error,
with no frustum term, so pages exist behind the camera too. At 16 km the cut is
433 leaves, and solving its split test
(`projectionScalePixels * voxel / distance / (1 + 0.1 * km) <= 4 px`) gives
roughly:

| distance | spatial LOD | page edge | page voxel |
| --- | --- | --- | --- |
| 100 m | 2 | 51 m | 0.4 m |
| 1 km | 5 | 410 m | 3.2 m |
| 8 km | 9 | 6.5 km | 51 m |
| 16 km | 10 | 13.1 km | 102 m |

So reach is not the missing piece. **Fidelity and sign of error are.**

**The distant pages are biased outward, not inward.** A column's top cell is
`floor((topHeight - origin) / voxelSize)` where
`topHeight = max` of the four corner heights (`PlanetVoxelChunk.cpp:2616`), and
the emitted voxel occupies the *whole* cell above that floor. Both steps push
the surface **up**. That is correct for the raster — an outward-conservative
shell is what keeps cracks and holes out of the G-buffer at LOD seams — and it
is exactly backwards for a shadow caster. At 8-16 km the page voxel is 51-102 m,
so the shadow geometry can stand up to a coarse voxel above the real ground,
over the whole 13 km page. `SparseVoxelTerrainTree64.cpp` then OR-reduces that
into the tree, which adds a second outward step whenever `cellScale > 1`.

This is the thing to fix. It is not a reach problem; it is that the shadow tree
and the raster shell want opposite rounding and currently share one build.

## 1. Measured baseline

`--world-map world-data --world-map-center 0.5,0.5 --terrain-render-distance-km 16
--resolution 1920x1080 --disable-vsync --frames 400 --skip-output-signature`,
after the page cut converges (`resident=433`):

```
CPU frame summary: samples=176 average=51.927ms median=51.572ms p95=55.071ms
display resolution: requested=1920x1080 window=960x540 drawable=1920x1080
Sparse voxel resolution: output=1920x1080 internal=960x540 scale=0.500
Direct illumination: per-pixel ReSTIR (0.52 Mpx internal, under the 8.5 Mpx probe threshold)
```

**51.9 ms, ~19 fps.** Frames before convergence are not comparable: the same
command at `--frames 60` reports 31.9 ms with only 4 of 433 pages resident. Any
future measurement has to run past the `World terrain GPU commit: ... resident=433`
line or it is measuring an empty world.

Targets against that baseline:

| target | frame time | speedup needed |
| --- | --- | --- |
| 30 fps floor | 33.3 ms | 1.56x |
| 40 fps ideal | 25.0 ms | 2.08x |
| 60 fps with 2x frame generation | ~33 ms real | 1.56x |

The 60-with-framegen target and the 30 fps floor are the same engineering
problem. 40 fps native is the one that needs real cuts.

## Part 2 — Trustworthy pass attribution

`--gpu-timings` cannot answer where the 51.9 ms goes, and the numbers it printed
during this investigation are a live example: it billed 27.5 ms to
`SceneRayScheduleFinalize`, a finalize kernel, while the schedule pass that
precedes it showed 0.102 ms. Vulkan timestamps bracket Vulkan passes; a MoltenVK
stall lands on whichever pass follows it. `docs/xcode-gpu-profiling.md` already
documents this, and `svraster-pass-timeline-misattribution` records a previous
instance of the same trap.

**2.1 The working recipe.**

```bash
xcrun xctrace record --template 'Metal System Trace' \
  --output frame.trace --launch -- \
  ./build-release/tests/SparseVoxelRendererSample <args> --frames 25 --profile-passes
scripts/metal-trace-passes.py frame.trace --frames 25
```

Three pieces had to be built for this, all now in place:

- `RHI::setCommandBufferName` / `VulkanRHI::setCommandBufferName`, which
  MoltenVK forwards to `MTLCommandBuffer.label`. This is the **only** string
  that reaches the Instruments encoder table. Debug-utils *labels* and *groups*
  do not — verified against recorded traces, with and without
  `MVK_CONFIG_DEBUG_MODE`. `RenderGraph` now names each buffer, and it must do
  so **before** `vkBeginCommandBuffer`: MoltenVK prefills, so that call is where
  it creates and labels the Metal buffer, and a later name is ignored. Moving
  the call earlier took the unnamed share from 27.7% to 10.2%.
- `--profile-passes` on the sample, which turns off
  `mergeSingleQueueCommandBuffers` so each pass lands in its own named buffer.
  It costs extra submissions, so a profiled frame time is not a shippable frame
  time — read the share column, not the milliseconds.
- `scripts/metal-trace-passes.py`, which exports the `metal-gpu-intervals`
  table and aggregates per pass. The format interns every value by id and
  repeats element tags within a row, so columns are matched positionally
  against the schema header.

`RenderGraph` also wraps each node body in `cmdBeginDebugLabel`. That does not
show in Instruments but is the right hook for an Xcode `.gputrace` hierarchy;
it is kept for that path and costs nothing.

**2.2 Findings — the converged frame.**

Capture: 400 frames, `--profile-passes`, reading the settled second half
(`--skip-fraction 0.5`). Two independent captures were taken, one with
per-pass command buffers and one with the shipping merged layout, so the
command-buffer structure is not what produces the result.

**Correction: the frame IS GPU-bound.** The reading below came from a
400-frame capture that dropped 60% of its events — Instruments records deferred
and a 20+ second trace wraps its buffer. A 25-frame capture of the same scene
records 11.4 encoders/frame and 12.1 ms against the long one's 4.9 and 2.9.
Behaviour agrees with the short capture: `--render-scale 0.25` and
`--ray-resolution-divisor 4` both move frame time in proportion to GPU work.
**Capture short, on a converged scene.** The CPU does wait inside
`vkQueueSubmit`, which is what GPU-pacing looks like from the CPU; MoltenVK
prefill style is not involved (0/1/3 within 0.6 ms) and drawable waits are
~2 ms/frame.

The original, now-retracted reading:

```
RenderGraph run detail: total=47.414ms submissions=1 commandBuffers=1
  Prepare=0.000  PrepareResources=0.000  PrepareCommands=0.000
  AllocCmdBuffers=0.000  BuildWaits=0.000  Barriers=0.000
  Commands=0.000  EndCmdBuffers=0.000    Submit=47.414
```

**47.4 ms of a 53.0 ms frame is inside a single blocking `Submit`**, and every
recording phase is zero. Whether that is the CPU waiting on the GPU or MoltenVK
doing deferred encoding inside `vkQueueSubmit` (prefill is configured to mode 3)
is the one question that decides Part 5, and it is the next thing to answer.

**2.3 Findings — the dial audit, and a retraction.**

Frame time, converged, 400 frames, median:

| configuration | median | vs baseline | ray count |
| --- | --- | --- | --- |
| baseline | 51.2 ms | — | 446812 |
| `--lighting-scale 2` | 50.9 ms | -0.5% | — |
| `--lighting-scale 4` | 51.0 ms | -0.4% | **446830** |
| `--ray-resolution-divisor 2` | 51.0 ms | -0.3% | **446835** |
| `--ray-resolution-divisor 4` | 44.7 ms | -13% | **446831** |
| `--disable-svgf` | 49.8 ms | -2.6% | — |
| `--disable-screen-traces` | 59.8 ms | **+17%** | — |
| `--render-scale 0.25` | 33.5 ms | -35% | **36886** |
| `--disable-lighting` | 18.6 ms | -64% | — |

The ray-count column is why this table is not what it first looks like.
**`--lighting-scale` and `--ray-resolution-divisor` are inert on this path**:
the traced ray count is identical at 1, 2 and 4. They are not "measured as
ineffective", they do not reach the ray domain at all, and the -13% attributed
to divisor 4 is therefore not a domain reduction. Only `--render-scale` moves
the ray domain, by 12x.

This retracts the `lighting-scale-ray-domain` note, which recorded that dial
reaching 30 fps at native. It does not reproduce here, and the ray count says it
cannot have.

`--disable-screen-traces` making the frame **17% slower** is the other result
worth keeping: the screen trace saves more world-trace work than it costs, so it
is not a candidate for removal.

**2.4 What this means for Part 5.** The quality-dial list that Part 5 was built
on is mostly dead. Two of its three entries do nothing, and the frame is
dominated by a single blocking submit rather than by shader cost. Optimising
the ReSTIR path means reducing what the frame *submits* or unblocking that wait
— not turning down sample counts. Fixing the two inert dials is worthwhile on
its own, since they are the documented way to trade quality for speed and they
currently lie.

## Part 3 — Conservative-inside shadow geometry — IMPLEMENTED

**3.1 Done.** `SparseVoxelTerrainPageTree64Source::conservativeInside` lowers
every column until its cells are contained by the terrain. A column's exclusive
top `T` only proves `surface >= T - 1`, so a cell `c` is known fully below the
surface across its own footprint when `c <= min(T) - 2`, minimised over the four
edge neighbours — which is what makes it hold on a slope, where a column says
nothing about the ground under its far edge. Runs keep their thickness and slide
down, so node cost is unchanged; columns that reach the page floor are dropped.
Page-edge neighbours are treated as unknown rather than absent, so a page does
not erode inward from all four sides.

**3.2 Done.** The power-of-four reduction snaps boxes inward (`ceil` the
minimum, `floor` the maximum) instead of voxelising the fractional box, so a
reduced page stays inside exactly as its unreduced form was.

**3.3 Wired.** `conservativeInsideCoarsePages` applies it to every page coarser
than the set's finest voxel. The finest level stays exact — it is voxel-for-voxel
the raster geometry, its voxel is 10 cm, and the near field is where the exact
match matters.

**3.4 Done**, in `SparseVoxelTerrainTree64Tests.cpp`:

- `testInwardPageNeverInventsOccluders` — containment at cell scale 1 and after
  reduction to scale 4.
- `testInwardTraceNeverOccludesAnUnoccludedRay` — **0 false occlusions in 4096
  rays**, 1024 of 2332 raster occlusions given up. The reference here had to
  become a *solid* heightfield: the raster shell is a few voxels thick, so rays
  pass underneath it through ground that is solid in reality, and the shell
  blames the inward page for occlusions that are real.
- `testInwardSeamNeverGainsOcclusion` — a coarse page invents **6** occlusions
  against a finer page's surface, down from **55** outward.

**3.5 Measured**, `--terrain-render-distance-km 16` at 1080p, converged, 400
frames, identical flags:

| | outward | inward |
| --- | --- | --- |
| worldHits | 15576 | 10639 |
| skyHits | 15744 | 12765 |
| escapedTreeVolume | 365 | 460 |
| meanLuminance | 0.16849 | 0.16821 |
| frame median | 51.2 ms | 51.9 ms |

Cost is flat. Mean luminance barely moves (-0.17%), so the scene is not visibly
washed out. But **a third of world occlusions and a fifth of sky occlusions are
gone**, and the seam test says only a small part of that was false. The rule is
more conservative than it needs to be.

**3.6 Remaining — the half that lives in generation.** The seam test does not
reach zero, and it structurally cannot: a coarse page is sampled with
`max`-of-corners in `PlanetVoxelChunk.cpp` *before* this builder sees it, so
the builder can only be inside the page's own sampling, not inside a finer
page's. Sampling coarse pages with `min`-of-corners for shadow use would both
close the seam and let the builder lower by less, recovering most of the lost
occlusion. That is the next piece of work here.

## Part 4 — Effective shadow reach

With Part 3 in place, distant pages become usable as casters. Two follow-ups,
both cheap, both to be measured before keeping:

**4.1** Confirm the ray span actually reaches them. The per-pixel span is now
`sceneProbeTraceDistance` (56.8 km on this fixture), so it does. What to check
is `escapedTreeVolume`: the directory covers 32.8 km against a 32 km scene here,
which is tight. If that status climbs above a fraction of a percent, the
directory needs `--tree64-trace-extent-scale`, not a new mechanism.

**4.2** Consider decoupling the shadow cut from the raster cut. The raster
splits on projected pixel error, which is the wrong metric for an occluder: a
mountain 12 km away occupies few pixels and casts kilometres of shadow. A
shadow-specific split term — height above the local surface, or projected
*shadow* footprint rather than projected screen footprint — would keep large
distant relief at a finer LOD than its screen size justifies, while flat distant
ground coarsens further than it does today. This is a policy change inside
`buildWorldVoxelChunkLeafCut` and should come **after** Part 3, since it is
worthless while coarse pages are still outward-biased.

## Part 5 — The 1080p frame budget

**5.1 Default resolution.** Change the sample default from 1280x720 to
1920x1080 (`windowWidth`/`windowHeight`, `SparseVoxelRendererSample.cpp:17846`).
Mechanical; do it first so every later measurement is at the target resolution.

**5.2 Dials that already exist.** Part 2 measured these; the results are in
2.3 and most of this list is now closed:

- `--lighting-scale 2` — shrinks the reservoir and worklist domain.
  `lighting-scale-ray-domain` records this reaching 30 fps at native 1080p.
- `--ray-resolution-divisor 2|4` — fewer traced rays, reconstructed from
  neighbours.
- `--svgf-atrous-iterations 0|1` and `--disable-svgf` — the denoiser is three
  passes at internal resolution.
- `--probe-tile`, `--probe-max-density-stride` if the probe path wins.
**Probe mode is out of scope.** The optimisation happens inside the ReSTIR
path; `--di-mode probe` is not a fallback for missing the target. That also
means the automatic switch at 8.5 Mpx should be reviewed: it can move a pinned
ReSTIR configuration onto the probe path without being asked.

**5.3 On the light-field upscale.** The proposal was to use normal and depth in
the reconstruction. That is already what it does:
`sparse_voxel_light_field.wgsl:1479` gathers a 3x3 neighbourhood and accepts a
neighbour only after instance/face, geometry-normal, and depth tests. So this is
not missing work — it is an existing mechanism whose *reduction factor* has
never been pushed. The actionable version is to raise
`--ray-resolution-divisor` and measure where the reconstruction visibly fails,
rather than to build a second upscaler. If it fails early, the interesting
question is the footprint (3x3 is small for a 4x reduction), not the metric.

**5.4 What not to do.** `svraster-overdraw-is-not-the-cost` closed the overdraw
line of attack with four measurements, and `restir-native-floor` measured a
45.6 ms floor at native with every quality dial off — meaning the cost is domain
work, not rays. Both point the same way: reduce the *domain*, not the per-sample
work.

**5.5 Frame generation** is the last step, not the first. It doubles the
presented rate and halves nothing; a 33 ms frame with framegen presents at 60 and
still responds at 30. Enable it only once the underlying frame is at or under
33 ms, so the 60 fps figure is not hiding a 20 fps input latency.

## Acceptance

- Distant terrain casts shadows, and no ray that the LOD-0 geometry leaves
  unoccluded is occluded by a coarse page (Part 3 tests, plus a visual check on
  flat ground under a distant ridge).
- `worldHits` and `skyHits` rise relative to today's converged 16 km run;
  `escapedTreeVolume` stays under 1% of rays.
- The command in the header runs at 1920x1080 by default, at or under 33.3 ms
  median converged, with 25.0 ms as the target, measured over at least 150
  frames after `resident=433`, without `--gpu-timings` or output signatures.
- The whole Tree64 CPU suite stays green.

## Sequencing

Part 2 gates everything in Part 5. Part 3 gates Part 4. Parts 3 and 5 are
independent of each other and can proceed in parallel. Part 5.1 is mechanical
and can land immediately.

## Part 6 — BVH4 TLAS for the shadow trace

**Corrected scope.** I first assessed this against `SparseVoxelTLASNodeGPU` (the
voxel side's culling BVH) and concluded the traversal kernel and the streaming
story both had to be written. That was wrong: `src/runtime/softwarebvh` already
contains a production GPU ray tracer, and it has almost everything.

**What exists in `softwarebvh`:**

- `BVH4NodeGPU` in `assets/shaders/softwarebvh/wgsl/software-raytrace.wgsl` —
  SoA layout (`childMinX/Y/Z`, `childMaxX/Y/Z` as `vec4`), 4-wide SIMD-friendly.
- A **BVH4 TLAS ray traversal**, selected by `SWRT_TLAS_BVH4`, with
  `tlasBvh4Nodes` bound and per-instance `transform` applied at the TLAS leaf —
  already once per instance entry, not per node.
- **CWBVH** nodes behind `SWRT_WORLD_CWBVH`, plus DOBB conversion settings, so
  the CWBVH endpoint is reachable later without another rewrite.
- `SoftwareTLASManager` with `INSTANCE_FLAG_DYNAMIC`, **partial rebuild**
  (`partialRebuildDirtyRatioThreshold`, `partialRebuildSahGrowthThreshold`) and
  **refit with SAH-growth monitoring** (`bvh4RefitSahGrowthThreshold`), over a
  SAH binned builder. This is the streaming/update story I previously listed as
  the piece that decides whether the idea works. It is already built.
- **Ray sorting** (`raySortBinCounts`, `raySortBinOffsets`, `raySortIds`), a
  two-stage BLAS-candidate path, and a workgroup queue — the divergence
  mitigation, already present.
- Extensive traversal statistics, including TLAS vs BLAS node visits separately
  (`trace_stat_add_tlas_node_visit`, `trace_stat_add_blas_node_visit`).
- **Primitive type is already a compile-time define** (`SWRT_TRIANGLE_PRIMITIVES`),
  so the BLAS is an existing extension point rather than a hardcoded assumption.

**What is actually missing:**

- **6.1** A Tree64 BLAS primitive variant — the analogue of
  `SWRT_TRIANGLE_PRIMITIVES` — entering the existing Tree64 traversal at the
  TLAS leaf instead of `intersect_triangle_transform`.
- **6.2** Feed terrain page instances into
  `SoftwareTLASManager::InstanceBuildInput`. Terrain instances are
  translation-only, which the TLAS leaf transform can exploit exactly as
  `kSparseVoxelTree64DynamicTranslationOnly` does today.
- **6.3** Dispatch this pass from the voxel scene-ray chain in place of
  `SparseVoxelTree64DynamicTracePass`, behind the existing `Traversal` enum so
  grid and BVH4 are one flag apart on the same scene and build.

**Why it is worth doing.** The grid's structural cost is measured: 930 instances
produce **677970 references, 729 per instance**, because one directory cell size
(64-128 m) serves pages from 12.8 m to 13 km. A BVH has no replication term, and
the gap widens as instance sizes diversify.

**Caution.** Two structural optimisations on this trace have measured neutral
(the translation-only transform; the pass-timeline attribution work). The
softwarebvh tracer already reports TLAS and BLAS node visits separately — read
those on both paths rather than trusting frame time alone to explain the result.
