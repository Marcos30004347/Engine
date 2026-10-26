# Sparse voxel 1080p performance plan

Status: active, 2026-08-14. Goal: good frame rates at 1920x1080 output on the
default terrain package.

Everything below is measured on this machine with the protocol in
`sparse-voxel-raster-ddgi-implementation-plan.md`: `--benchmark-lighting`
aggregates, built-in GPU timestamps **disabled**, default terrain package,
`--benchmark-sequence camera-walk`, LOD 0.75/1.5, 90 warmup and 240 measured
frames, two interleaved repeats per arm. Run spreads are quoted with every
delta; nothing under about 0.4 ms is a result here.

## 1. Where 1080p stands today

**The default configuration already renders 1920x1080 at 35.2 FPS.** It renders
internally at 960x540 (`kPerformanceRenderScale = 0.50`) and reconstructs with
the portable temporal upscaler.

| Configuration | internal | p50 | p95 | FPS p50 | spread |
| --- | ---: | ---: | ---: | ---: | ---: |
| **default (TSR, scale 0.50)** | 960x540 | **28.45 ms** | 31.11 ms | **35.2** | 0.04 |
| `--metalfx-upscaler` (scale 0.50) | 960x540 | 28.39 ms | 31.11 ms | 35.2 | 0.10 |
| MetalFX, scale 0.58 | 1114x626 | 31.76 ms | 34.54 ms | 31.5 | 0.31 |
| MetalFX, scale 0.667 | 1280x720 | 36.17 ms | 38.82 ms | 27.6 | 0.03 |
| MetalFX, scale 0.75 | 1440x810 | 39.16 ms | 41.77 ms | 25.5 | 0.40 |
| native 1080p + TSR | 1920x1080 | 71.93 ms | 75.30 ms | 13.9 | 0.11 |
| native 1080p, no temporal | 1920x1080 | 54.05 ms | 57.44 ms | 18.5 | 0.02 |

Two immediate consequences:

- **MetalFX and the portable TSR cost the same at the default scale**
  (28.39 vs 28.45 ms, inside spread). MetalFX is not a lever here. It only wins
  at high internal resolutions, where TSR's cost scales with render pixels and
  MetalFX's does not: at 1080p internal, TSR adds 17.89 ms and MetalFX 3.83 ms.
- **Below scale 0.50 MetalFX silently falls back to the portable TSR**
  ("MetalFX does not support the 4x scale"). A `--render-scale 0.3334` arm
  measured *slower* than 0.50 for this reason, not because of a real curve
  inversion. Any scale sweep must check the log for that fallback line.

## 2. What the 28.45 ms is made of

Measured by rendering the same 960x540 internally but presenting at 540p rather
than 1080p, which isolates the reconstruction and output-resolution work:

| Block | Cost | Share |
| --- | ---: | ---: |
| Render at 960x540 (cull, raster, G-buffer, lighting, SVGF, compose) | 15.70 ms | 55% |
| Temporal resolve at 1:1 | 4.47 ms | 16% |
| Reconstruction to 1080p plus present | 8.28 ms | 29% |
| **Total** | **28.45 ms** | |

Derivation: 960x540 output with no temporal is 15.70 ms; with the temporal
resolver at 1:1 it is 20.17 ms; the shipping 1080p default is 28.45 ms.

**The temporal and output-resolution block is 12.75 ms, 45% of the frame - more
than the entire render.** That is the headline. A temporal upscaler producing
1080p should cost single-digit milliseconds in total, and this one costs more
than everything the renderer does at 540p.

Inside the 15.70 ms render half, the proportions measured at 720p carry over:
`--depth-only` is 37% of the frame, so roughly 5.8 ms is cull plus brick raster
plus depth and roughly 9.9 ms is everything after depth. At 720p the same
ablations gave SVGF 2.79 ms, atmosphere 0.93 ms, GTAO 0.05 ms, volumetric fog
0.07 ms, and **exactly zero** for both halving the scene-ray grid and disabling
spatial ReSTIR. Ray tracing is not the cost; do not start there.

The frame is also **~94% pixel-bound**: 720p native no-temporal is 25.48 ms and
1080p native no-temporal is 54.05 ms, against a 2.25x pixel ratio predicting
57.3 ms. Internal resolution is therefore the strongest single lever, which is
exactly why the default already uses 0.50.

## 3. Targets

From 28.45 ms:

- **40 FPS needs 25.0 ms: -3.45 ms.** Close, and plausible from a single stage.
- **60 FPS needs 16.7 ms: -11.75 ms.** Requires roughly halving the frame. The
  only block large enough is the 12.75 ms temporal/output block.

## 3a. What the statistics changed

Full measurement record in `sparse-voxel-1080p-statistics.md`. Three results
reshape the stage list:

- **The frame is `2.93 ms + 24.83 ms/Mpx`, R^2 = 1.00000 over a 9x pixel
  range.** Fixed cost is 2.93 ms in total, so no amount of pass merging or
  dispatch reduction can reach a target. Only per-pixel cost matters.
- **Post-depth is 65% of the per-pixel cost**, 16.04 of 24.83 ms/Mpx.
- **`--di-mode per-pixel` costs +74.94 ms.** The probe system is not optional
  and is already the large win; it is the thing to make cheaper, not to remove.

Attributed at 1080p by register-twice, min-of-3, each neutrality-checked:
ProbeInterpolate **4.79 ms**, BrickResolve 0.97 ms (control, predicted 1.12),
probe history copies 0.90 ms, Compose 0.38 ms, and the four full-resolution SVGF
history copies **free at -0.02 ms**. About 18.8 ms of the 34.3 ms post-depth is
still unattributed, concentrated in the G-buffer and the probe ray chain -
exactly the two places this technique cannot reach, because both are
non-idempotent.

**Dead ideas, killed by measurement rather than argument:** double-buffering the
SVGF history textures to remove four full-resolution copies per frame (they cost
nothing), and any plan resting on reducing pass count or encoder boundaries
(2.93 ms of fixed cost in total).

## 4. Stages

### P0. Probe interpolation and probe density

The largest attributed pass in the frame and the largest single lever now known.

- **ProbeInterpolate is 4.79 ms at 1080p**, 2.31 ms/Mpx, confirmed by two
  independent register-twice runs (4.574 and 4.793 ms, the second with a
  0.014 ms spread).
- **`--probe-tile 16` is worth -3.24 ms.** Probe grid density is a real dial
  with real milliseconds on it, and it has a direct quality cost that needs
  judging on the moving-camera sequences.

The renderer's own DDGI plan (`sparse-voxel-raster-ddgi-implementation-plan.md`,
part II) is the structural version of this. Its stated integration rule is
already the right one: **query DDGI inside the existing `ProbeInterpolate` path
or at a lower-resolution lighting stage so the new cache replaces work rather
than adding to it.** With ProbeInterpolate now measured at 4.79 ms rather than
estimated at 4 ms, that rule has a number behind it, and D11's budget of no more
than 3.20 ms incremental should be read against it.

A world-anchored cache that lets the surface probes thin out - fewer probes, or
a coarser interpolation tile, backed by DDGI for the low-frequency term - is
the one change that could cut both the 4.79 ms interpolation and part of the
unattributed probe ray chain at once. That makes DDGI a performance stage here,
not only a quality one. It must still be measured as one: the budget is
whole-frame neutral or better, not "the probe passes got cheaper".

### P1. Size and cut the 12.75 ms temporal and output block

This is now the largest item in the frame and the only one big enough to reach
60 FPS. It is 45% of the frame at the shipping configuration.

It splits into a 4.47 ms resolve at internal resolution and an 8.28 ms
reconstruction-plus-present at output resolution. Neither has been attributed
to a specific pass yet, and MetalFX costing the same total says the expense may
not be the upscaler kernel at all - it may be the surrounding output-resolution
work (compose, blit, present, ring copies) that both paths share.

Steps, in order:

1. **Attribute it.** Use register-twice sizing on every idempotent pass in the
   block and an Xcode Metal System Trace for encoder intervals. Do **not** use
   `--gpu-timings`: Vulkan timestamps bracket Vulkan passes rather than Metal
   encoders, so a MoltenVK stall is billed to the next pass; that instrument
   already overstated `BrickResolve` by 4.7x and produced a 4.5 ms phantom in a
   duplicated depth-pyramid pass.
2. **Check for a redundant full-resolution copy.** MetalFX writes into a
   dedicated `svr.sample.metalfx.upscaled` ring at display resolution which is
   then transferred; a `TransferSrc` blit of a 1920x1080 RGBA16F image is about
   16 MB per frame each way. If the present path can consume the upscaled
   texture directly, that is pure profit.
3. **Audit the portable resolver's feature stack.** The TSR path has
   accumulated accumulation-weight, rectification-box, shading-change,
   luma-instability, robust-sharpen and reconstructed-depth stages, several of
   which were added while chasing quality bugs. At least one of them - the
   temporal edge metric - has already been shown to self-cancel and move
   nothing across a full 0-to-full sweep. Re-measure each against its own
   whole-frame delta and delete what does not pay.

Accept if the block drops below 8 ms with no new flicker on the moving-camera
sequences. Judge flicker on the dolly and camera-walk sequences, never a static
camera: flicker in this renderer tracks camera velocity.

### P2. The post-depth render work, about 9.9 ms at 540p

Second largest pool, and a black box: every dial that exists for it moved the
frame by roughly nothing, which means the cost sits in stages with no dial -
the material G-buffer, probe interpolation, compose, and present.

Structural suspects, to be sized with the same register-twice technique:

- The G-buffer render pass binds **seven** colour attachments at internal
  resolution, and the brick raster writes four integer MRTs at 40 bytes per
  covered pixel.
- Probe interpolation was previously estimated near 4 ms and has never been
  sized with a trustworthy instrument.
- `BrickResolve` is already known at 0.467 ms p50 marginal. It is not the
  problem.

### P3. SVGF a-trous iterations

2.41 ms of the 2.79 ms SVGF total at 720p, and `--svgf-atrous-iterations 0|1|2`
already exists. Going from 2 to 1 is a quality-versus-cost dial worth roughly
1.2 ms at 720p, less at 540p. Take it after P1 and P2, and only if the quality
cost is acceptable on moving-camera sequences. Mind the noise floor: identical
runs in this tree have varied 3x on some metrics, and four "inert" dial sweeps
were once declared void for exactly that reason.

### P4. Replace baked per-voxel slope patches with a heightmap read

Approved direction. The evidence supports it strongly on **memory** before any
timing claim is made, and the intent is explicitly to replace the staircase
pattern with a simple heightmap check per microvoxel.

**What exists today.** Every occupied voxel carries a `microSurfaceData` word:
a context byte plus a 24-bit four-corner coverage patch, six bits per corner.
`PlanetVoxelChunk` builds it from the page's height raster by reconstructing a
signed distance at the four parent corners, dividing by the local gradient, and
quantizing. The brick fragment then re-derives a bilinear height per visited
column, quantizes it to whole microvoxels, and produces a staircase **confined
inside that one parent voxel**. Continuity across parents exists only because
the builder arranges for adjacent parents to share corner values.

**What it costs.** On the default terrain package at the intended LOD:

```text
materialIndices   27,016,020 x 1 byte  =  27.0 MB
microSurfaceData  27,016,020 x 4 bytes = 108.1 MB   <-- 80% of the total
palette               45,568 x 8 bytes =   0.4 MB
                                   total 135.4 MB   (logged as materialBytes)
```

`microSurfaceData` is **108.1 MB, 80% of the brick material footprint**, and it
is mostly zeros: interior voxels store `surface.flags = 0`. The fragment also
performs one dependent descriptor load per eligible parent - 6,603,134 loads in
a single frame, a 1:1 ratio with eligible parents, so there is no reuse across
the DDA walk even though consecutive parents in a brick almost always share a
descriptor.

**The proposal.** Upload the page height raster itself - it is O(area), not
O(voxels) - and intersect the ray against it directly, giving a genuinely
continuous slope across parent and brick boundaries instead of a staircase
clamped inside each voxel strip.

Expected gains, in confidence order:

1. **Memory**: ~108 MB of the 135 MB brick material footprint replaced by a
   per-page 2D raster. Near-certain and large.
2. **Quality**: continuous slopes, no per-voxel staircase, no reliance on
   neighbouring parents agreeing about shared corner values.
3. **Bandwidth and fragment work**: removes the per-voxel dependent load and the
   per-column bilinear-plus-quantize. Size unknown; must be measured.

Risks that make this a spike before it is a stage:

- Ray-versus-heightfield is a root find along the ray, not a point sample. Done
  naively it misses thin features or self-intersects. A maximum-mipmap or
  fixed-step-plus-refinement scheme is required and may cost more in the inner
  loop than the arithmetic it replaces.
- The heightmap must be addressable in world space through the existing planet
  page and clipmap system, on a spherical cube-face parameterization. The data
  exists in the package; the GPU-resident, LOD-consistent addressing does not.
- **This is a quality change, not a byte-exact refactor.** Every exact-output
  gate covering terrain micro-geometry needs a deliberate, reviewed rebaseline.
- The R3 column cache becomes dead if this lands. Retire it explicitly rather
  than leaving two sources of truth for column depth.

Sequence: spike the intersection in isolation against the existing staircase as
reference, measure the fragment cost at equal quality, and only then touch the
chunk builder or the material stream.

### P5. Anisotropic brick shapes, if P2 says the raster still matters

The brick DDA's coarse empty-space skip barely earns its keep: at 1280x720 the
scale-4 test skipped **118,878 of 858,006 groups, 13.9%**. A surface-fitted
8-cubed brick on terrain is mostly occupied, so the coarse mask has little to
reject. Overdraw is **16.4x** - 8,721,169 rasterizer fragments over 530,762
covered pixels.

R7 in the raster plan specifies the fix: keep 512 logical cells and every axis
a power of two, but let the brick follow the surface - `16 x 16 x 2` for
near-horizontal terrain rather than `8 x 8 x 8`. That raises the skip rate and
cuts the vertical proxy extent producing the overdraw.

Largest remaining structural raster idea, and the most expensive to build. Do
not start it until P2 shows the raster half is still worth attacking: at the
shipping 540p configuration it is roughly 5.8 ms of a 28.45 ms frame, and two
cheaper structural raster hypotheses have already failed (section 5).

## 5. Refuted - do not re-propose without new evidence

**Two temporal upscalers running in series.** An earlier revision of this plan
claimed the upscaled path stacked the portable TSR under MetalFX and put 7.83 ms
on decoupling them. **That was wrong and is retracted.** The renderer already
skips the TSR chain when MetalFX is active:

```cpp
if (temporalEnabled && !metalFxUpscalerEnabled)   // TSR chain
```

The 7.83 ms figure came from comparing 720p with and without the temporal
resolver at `internal = output`, which measures TSR at full render resolution,
not the shipping configuration. At the default 0.50 scale the two upscalers cost
the same to within 0.06 ms. The trap that produced the error is real and worth
keeping: **`--no-taa` silently forces `internal = output`**, because the render
scale is gated on `taaEnabled && tsrEnabled`. An arm that adds `--no-taa` to an
upscaled configuration is not measuring what its name says - it renders native.
Always confirm with the `Sparse voxel resolution: output=... internal=...` line.

**The R2 split's three render passes.** Merging them into a single render pass -
verified by the R2 gate, which requires the merged graph to register
`MergedMicroDraw` and none of the three split passes - measured **+0.074 ms p50
/ -0.149 ms p95**, inside spreads and opposite in sign. Attachment traffic is
not where that gap lives. Retained behind `--brick-micro-merged-draw` because
R9's hybrid visibility raster needs multi-source draws anyway.

**Front-to-back brick ordering.** The visible list is built by
`atomicAdd`-append, so bricks rasterize in race order and conservative-depth
early-Z has no nearer fragment to reject against. A 256-bucket counting sort
over quantized reverse-Z depth produced byte-identical output and measured
**+0.113 ms p50 / +0.047 ms p95** - neutral. **Reverted.** `DepthReplacing` and
`DepthLess` are present on all eighteen compiled brick-draw variants, verified
by parsing the SPIR-V execution modes, so early-Z is already declared and
already doing whatever it can. Note also that the overdraw and traversal
counters **cannot** measure this: the diagnostics build's atomic writes force
late-Z, and its counters were bit-identical between sorted and unsorted arms.

The broader lesson: **the brick rasterizer is not obviously fill-bound.** Two
independent fill-oriented hypotheses moved nothing. Raster work should be
justified by P2's numbers, not by the 16.4x overdraw figure on its own.

## Delivery order

0. **P0** - probe interpolation and density, and DDGI as the structural version
   of it. Largest attributed pass, 4.79 ms, plus a measured 3.24 ms density
   dial.
1. **P1** - attribute and cut the 12.75 ms temporal/output block.
2. **P2** - the ~18.8 ms unattributed post-depth work. Needs an Xcode Metal
   System Trace: the G-buffer and the probe ray chain are both non-idempotent,
   so register-twice cannot reach them.
3. **P3** - a-trous iteration count, as a tuning decision on P1/P2 evidence.
4. **P4** - heightmap slope spike, then stage. Memory and quality justify it
   independently of frame time.
5. **P5** - anisotropic bricks, only if P2 says the raster half still matters.

Rebaseline after every retained stage. Report source-renderer throughput and
upscaled throughput as separate numbers, and never quote a generated or
upscaled figure as source cadence.
