# Sparse voxel terrain FullHD and 4K optimization plan

**Status:** Milestones A-D implemented and Metal-instrumented, 2026-08-22.
Part 0 contains measured findings from the current generated-world terrain
path. Parts 1-5 are implementation work.

**Profiling rule:** optimization decisions in this plan require Xcode Metal
instrumentation. App-side Vulkan timestamp timings, default frame breakdowns,
and pass timings collected without Metal System Trace are not sufficient on
macOS/MoltenVK, because command-buffer merging and deferred MoltenVK work can
move GPU cost onto the wrong pass. Benchmark JSON may be used for frame pacing
only; pass attribution must come from Xcode Instruments / Metal System Trace.

Goal scene:

```text
SparseVoxelRendererSample
  --world-map world-data
  --world-map-center 0.5,0.5
  --terrain-render-distance-km 16
  --terrain-lod-levels 5
  --lod-threshold 0.75
  --tree64-lod-threshold 1.5
```

Targets:

| output | source-frame target | frame generation target |
| --- | ---: | ---: |
| 1920x1080 | stable 30 fps in all terrain views, preferably 60 fps source when settled | 60 fps presented |
| 3840x2160 | 20 fps if possible | framegen can smooth presentation, but source frame still must approach 50 ms |

The important constraint is measurement quality: Vulkan timestamp timings are
not authoritative on MoltenVK because stalls and merged dispatches can be billed
to the wrong pass. Every claimed optimization must include an Xcode Metal
System Trace with `--profile-passes` for pass attribution. Use benchmark JSON
only for frame pacing and regression gates.

## 0. Current measured picture

### 0.1 Correction already applied

The original 4K spike was not a shader explosion. Generated-world terrain chose
its spatial leaf cut from the **output/window height**, even when MetalFX was
rendering internally at half resolution. That requested a native-4K terrain cut
while shading at half rate, which pushed the generated-world page target from
roughly 930 to 2346 pages and made staging/upload memory enormous.

`SparseVoxelRendererSample.cpp` now derives generated-world
`projectionScalePixels` from the **internal raster height** when upscaling is
active. After that, the 4K path returns to the same 930-page terrain cut as the
1080 half-scale path.

### 0.2 Baseline pacing after the correction

Commands used the generated-world path above, `--render-scale 0.5`,
`--metalfx-upscaler`, and `--disable-vsync`.

| sequence | output drawable | median | p95 | notes |
| --- | ---: | ---: | ---: | --- |
| `camera-walk` | 1920x1080 | 20-26 ms in early runs, 16.6 ms in fresh settled runs | 17-132 ms depending on streaming state | target is reachable; spikes are the risk |
| `roof-down` | 1920x1080 | 16.5-24.4 ms | 17.5-43 ms | sky/downward view is not the hard case after settling |
| `camera-walk` | 3840x2084 on this Mac display | 59-60 ms | 61-63 ms | blocker for 20 fps |
| `roof-down` | 3840x2084 on this Mac display | 36-37 ms | 39-41 ms | already clears 20 fps |

The 4K requested mode was `3840x2160`, but SDL/Metal created a `3840x2084`
drawable on this display. Keep recording the actual drawable in all reports.

### 0.3 Xcode Metal trace findings

1080 roof-down, `--profile-passes`, settled half of the trace:

```text
named encoder time: 66.9 ms, unattributed: 105.6 ms
top named pass: svr.sample.SceneScreenTrace, 49.5% of named work
next passes: SceneDISpatialReservoir, GTAO, LightField, GBuffer, SVGF.ATrous2
```

4K camera-walk, `--profile-passes`, settled half of the trace:

```text
named encoder time: 108.8 ms, unattributed: 340.3 ms
top named passes:
  svr.sample.Final.BrickCopyTerrainPageRequestBits
  svr.sample.SceneScreenTrace
  svr.sample.SVGF.CopyNormalHistory
  svr.sample.Atmosphere.SkyView
  svr.sample.TerrainUpload
```

The parser's `ms/frame` divisor is not reliable for launch-to-exit traces unless
the retained frame count is known exactly. Treat the trace as pass ranking and
attribution. Use benchmark JSON for frame pacing.

### 0.4 Candidate dial audit

All 4K numbers below are `camera-walk`, MetalFX, render scale 0.5 unless noted.

| candidate | median | result |
| --- | ---: | --- |
| baseline | 59-60 ms | current blocker |
| `--tree64-traversal bvh4` | ~60.5 ms | not a 4K pacing win |
| `--terrain-pages-per-update 16` | ~59.5 ms | helps 1080 streaming stability, not 4K median |
| `--svgf-atrous-iterations 1` | ~57.4 ms | small useful quality/perf knob |
| `--svgf-atrous-iterations 0` | ~55.6 ms | larger but visible quality trade |
| `--disable-svgf` | ~54.8 ms | quality-risky, still short of 50 ms |
| `--disable-spatial-restir` | ~59.3 ms | not useful |
| `--probe-tile 24` | ~59.4 ms | no meaningful gain on this path |
| `--screen-trace-cell-span 4` | ~59.6 ms | no meaningful gain |
| `--disable-screen-traces` | failed validation | unsafe as a preset |
| `--terrain-render-distance-km 8` | ~58.4 ms | distant coverage is not the limiter |
| `--disable-gtao` | ~59.4 ms | not useful |
| `--disable-atmosphere` | ~59.1 ms | not useful |
| no MetalFX at 0.5 scale | 109-112 ms | do not use |
| `--render-scale 0.45` with MetalFX requested | ~101 ms | falls off the useful MetalFX path |

Frame generation did not change source-frame pacing in this benchmark: 1080
remained ~16.6 ms and 4K remained ~59 ms. It can improve presentation only after
the source frame is within budget.

## Part 1 - Terrain feedback/readback pressure

**Priority:** highest for 4K `camera-walk`.

### 1.1 Problem

When terrain is active, the graph schedules four independent readback copies:

- `BrickCopyTerrainPageRequestBits`
- `BrickCopyTerrainPageRequestPriorities`
- `BrickCopyTerrainPageRequestGenerations`
- `BrickCopyTerrainPageRequestDesiredLODs`

The world-terrain finalizer then reads all four every eligible frame to build
`WorldVoxelChunkRequest`s. The buffers are small, so the cost showing up in the
Metal trace is likely synchronization/command-buffer/readback overhead rather
than raw bandwidth.

This is exactly the kind of work virtual geometry avoids structurally: feedback
is aggregated, scored, and admitted under a bounded upload budget rather than
letting every frame force a broad feedback/readback loop.

### 1.2 Work

- **1.2.1 Add readback cadence controls.**
  Add a terrain feedback cadence, defaulting to every frame at first:
  `--terrain-feedback-interval N` where `1` is current behaviour. When
  `pendingWorldChunks != 0`, skip GPU feedback readback unless the last feedback
  age exceeds a small ceiling. Do not skip CPU publication status needed to make
  uploads visible.

  **Implemented:** `--terrain-feedback-interval` and
  `--terrain-feedback-pending-max-stale` now select between normal render-graph
  handles and handles recorded without the four terrain feedback copy commands.
  The finalizer consumes world-terrain feedback only for frames that submitted
  those copies, and benchmark schema 16 reports per-frame feedback cadence,
  back-pressure, request count, and queued-request state. Validation must use
  Xcode Instruments / Metal System Trace with `--profile-passes`; a short
  1280x720 trace confirmed the cadenced path submits one feedback readback for
  interval 4 instead of one per frame.

- **1.2.2 Pack world-terrain feedback.**
  Replace four copy/readback passes with one packed readback buffer for world
  terrain:

  ```text
  slot, generation, priority, desiredLOD, flags
  ```

  The CPU loop can then read one contiguous buffer. Keep the old buffers behind
  a debug/compatibility path until validation proves the packed path identical.

  **Implemented for Milestone B:** generated-world terrain now runs
  `svr.sample.Final.TerrainFeedbackPack` after brick culling and copies a
  single `SparseVoxelTerrainFeedbackRecordGPU` array back to the CPU. Packaged
  terrain keeps the legacy request-bit/priority readback path. The old
  generated-world `BrickCopyTerrainPageRequestBits`,
  `BrickCopyTerrainPageRequestPriorities`,
  `BrickCopyTerrainPageRequestGenerations`, and
  `BrickCopyTerrainPageRequestDesiredLODs` copies no longer appear in the 4K
  `--profile-passes` Metal trace.

- **1.2.3 Move cheap filtering to GPU.**
  Add a compacted request list/count buffer so the CPU reads only requested or
  coarsening-eligible slots rather than scanning `terrainPageRequestCapacity`.
  The first version can be fixed-capacity and conservative; overflow should set
  a flag and fall back to the old full scan.

- **1.2.4 Borrow virtual texture scoring.**
  Mirror the virtual texture request admission pattern:

  - aggregate duplicate requests;
  - boost pages requested across consecutive feedback frames;
  - prefer children whose parent is already resident;
  - enforce a top-N admission budget independent of the number of visible slots.

  This should live in the terrain request coalescing/admission layer, not in the
  raster pass.

### 1.3 Acceptance

- 4K `camera-walk` median improves by at least 5 ms or named/unattributed Metal
  trace time for terrain feedback falls clearly.
- 1080 `camera-walk` and `roof-down` p95 stay under 33.3 ms in settled runs.
- No visible refinement starvation during camera movement.
- Request output matches the old path on a one-frame capture when cadence is 1
  and packing is in compatibility mode.

## Part 2 - Terrain upload smoothing

**Priority:** high for p95 stability and streaming spikes.

### 2.1 Problem

`--terrain-pages-per-update 16` made short 1080 settled runs cleanly hit
~16.6 ms, but it did not move the 4K median. That means smaller batches help
stability but do not solve the 4K source-frame cost. Upload smoothing is still
needed so FullHD is stable across camera motion and new terrain cuts.

### 2.2 Work

- **2.2.1 Default generated-world streaming budget by resolution.**
  Consider changing the generated-world default from 64 pages/update to a
  dynamic value:

  | mode | candidate budget |
  | --- | ---: |
  | 1080 interactive | 16-32 pages/update |
  | 4K interactive | 16 pages/update |
  | spatial-cut or CLI-authored LOD change | current unbounded/large one-shot path |

  **Implemented for Milestone A:** generated-world terrain now chooses
  resolution-aware defaults when the CLI does not override them: 32
  pages/update and 32 MiB/frame below 2560x1440, 16 pages/update and 16
  MiB/frame at 1440p/4K-class output. Explicit spatial-cut or authored LOD
  changes still use the existing unbounded reset path so the complete cut can
  publish quickly.

- **2.2.2 Cap bytes, not only pages.**
  Page count is an indirect proxy. Use `maximumUploadBytesPerFrame` as the
  primary throttle and let page count be the secondary limit. Record both in the
  benchmark report.

  **Implemented for Milestone A:** `--terrain-upload-budget-mb 0..1024` feeds
  `maximumUploadBytesPerFrame` for generated-world jobs and packaged terrain
  uploads when explicitly provided. Benchmark schema 17 records both
  `terrain_pages_per_update` and `terrain_upload_budget_bytes`.

- **2.2.3 Avoid many small upload command buffers.**
  Inspect `SparseVoxelTerrainUploadPass` batch formation and the number of
  upload sections emitted per frame. Merge adjacent/range-compatible sections
  more aggressively before the GPU upload pass.

- **2.2.4 Add streaming convergence metrics.**
  Extend benchmark JSON with:

  - resident terrain page count;
  - pending refinement request count;
  - upload bytes submitted this frame;
  - upload queue latency frames;
  - feedback age in frames.

  **Implemented for Milestone A:** benchmark schema 17 adds per-frame
  `terrain_streaming` and `terrain_feedback` objects with resident capacity,
  pending refinement count, submitted upload bytes/sections/publications,
  pending upload bytes/batches, upload queue latency, committed Tree64 page
  count, feedback submission/skip state, feedback age, and request counts.

### 2.3 Acceptance

- FullHD p95 remains under 33.3 ms while walking after terrain has begun
  refining, not only when fully settled.
- No single generated-world upload transaction exceeds the configured byte
  budget except for explicit spatial-cut resets.
- 4K p95 does not regress while upload smoothing is enabled.

## Part 3 - 4K source-frame cost

**Priority:** medium-high after Parts 1 and 2, because current feature dials only
recover 2-5 ms.

### 3.1 Keep MetalFX at render scale 0.5

Measured data says this is non-negotiable for now:

- MetalFX at 0.5 scale: ~59 ms in 4K walking.
- no MetalFX at 0.5 scale: ~109-112 ms.
- `--render-scale 0.45`: ~101 ms because it falls off the useful MetalFX path.

Do not build presets below 0.5 unless the portable upscaler path is separately
optimized.

### 3.2 SVGF quality ladder

`--svgf-atrous-iterations 1` is the only currently useful low-risk 4K quality
dial. It saves roughly 2-3 ms. `0` or `--disable-svgf` saves up to 5 ms but is
not enough alone and should be treated as an emergency/perf mode.

Work:

- Add a named 4K performance preset that sets a-trous to 1, but keeps MetalFX,
  atmosphere, GTAO, and spatial ReSTIR enabled.

  **Implemented for Milestone C:** `--terrain-performance-preset
  4k-performance` and the alias `--terrain-4k-performance-preset` select the
  Performance render-scale path, enable the native MetalFX path where
  available, keep the default lighting features enabled, and reduce SVGF to one
  a-trous pass. Benchmark schema 18 records
  `terrain_performance_preset`.
- Add visual validation captures for:
  `camera-walk`, `roof-down`, horizon, near-ground grazing, and high-altitude
  downward views.
- If a-trous 1 is visually acceptable, make it automatic only for 4K output
  with MetalFX, not for FullHD.

Acceptance:

- 4K `camera-walk` median improves by at least 2 ms.
- No obvious temporal instability increase in moving captures.
- FullHD defaults stay visually unchanged unless explicitly using the preset.

### 3.3 Screen trace optimization, not removal

1080 Metal trace shows `SceneScreenTrace` as the top named pass. However:

- disabling screen traces failed the world-only validation path;
- halving `screenTraceCellSpan` from 8 to 4 did not move 4K walking;
- earlier measurements showed removing screen traces can make world tracing
  slower.

Work:

- Add counters for active screen-trace rays, leaf visits, max visits, hits,
  `MISS_TMAX`, and `ESCAPED_SCENE`.
- Add a dynamic maximum leaf-visit budget based on projected segment length.
  The C++ path currently requests 128 leaf visits per ray; the shader can spend
  up to 1024 total visits internally.
- Add a terrain-specific early-out for rays whose end distance is below the
  receiver-cell bias or whose projected segment is below a subpixel threshold.
- Fix the disabled-screen-trace validation path so it can be used as an A/B
  test, but do not ship it as a performance preset without quality review.

  **Implemented for Milestone D:** benchmark schema 19 records optional
  `screen_trace` counters behind `--screen-trace-statistics`: active rays, node
  visits, leaf visits, max leaf visits, hits, `MISS_TMAX`, `ESCAPED_SCENE`, and
  subpixel/short segments. `SceneScreenTrace` now uses a projected-segment leaf
  cap instead of always allowing one leaf per screen-trace step, and it early
  outs projected subpixel segments. A full receiver-cell-bias early-out was
  tested and rejected because the counters showed it short-circuited the trace
  instead of trimming only unproductive work. The disabled-screen-trace
  validation path was also fixed so benchmark-mode A/B runs collect the
  fallback scene-ray statistics they need.

Acceptance:

- Xcode trace shows lower `SceneScreenTrace` share at 1080 without increasing
  world trace share by more than the saved time.
- 4K walking improves only if screen trace was actually present in the hot
  window; otherwise mark as FullHD-specific cleanup.

## Part 4 - What not to chase first

These were measured and should not be first-line optimization work for this
specific 4K issue:

- **BVH4 TLAS as the 4K fix.** It removes the directory replication term and is
  still the right structural broad phase, but the measured 4K walking frame did
  not improve. Keep it for correctness/scalability, not this source-frame miss.
- **Virtual geometry BVH8 drop-in.** Virtual geometry's BVH8/page-table model
  is not directly reusable for Tree64 terrain because terrain needs voxel
  occupancy for raster and lighting. Borrow request admission and bounded
  streaming, not the meshlet BVH itself.
- **Reducing terrain render distance.** 16 km -> 8 km barely moved 4K walking.
- **Atmosphere/GTAO/spatial ReSTIR removal.** Each was too small or in the noise.
- **Probe tile 24 for this scene.** It was useful in older terrain measurements,
  but not in the current generated-world 4K walking path.
- **Render scale below 0.5 with MetalFX requested.** It currently falls off the
  useful path and gets much slower.

## Part 5 - Measurement protocol

This part is mandatory. No part of this plan is considered validated from
default timings alone. A change can be merged as an experiment with benchmark
JSON, but it cannot be called an optimization until it has an Xcode
Instruments / Metal System Trace capture showing where the time moved.

### 5.1 Benchmark commands

FullHD settled walking:

```bash
./build-release/tests/SparseVoxelRendererSample \
  --world-map /Users/marcosvinicius/workspace/Engine/world-data \
  --world-map-center 0.5,0.5 \
  --terrain-render-distance-km 16 \
  --terrain-lod-levels 5 \
  --lod-threshold 0.75 \
  --tree64-lod-threshold 1.5 \
  --benchmark-lighting perf-1080-walk.json \
  --benchmark-sequence camera-walk \
  --benchmark-warmup 45 \
  --benchmark-frames 60 \
  --benchmark-resolution 1920x1080 \
  --render-scale 0.5 \
  --metalfx-upscaler \
  --disable-vsync
```

FullHD sky/downward view:

```bash
./build-release/tests/SparseVoxelRendererSample <same scene args> \
  --benchmark-lighting perf-1080-roof.json \
  --benchmark-sequence roof-down \
  --benchmark-warmup 45 \
  --benchmark-frames 60 \
  --benchmark-resolution 1920x1080 \
  --render-scale 0.5 \
  --metalfx-upscaler \
  --disable-vsync
```

4K walking:

```bash
./build-release/tests/SparseVoxelRendererSample <same scene args> \
  --benchmark-lighting perf-4k-walk.json \
  --benchmark-sequence camera-walk \
  --benchmark-warmup 45 \
  --benchmark-frames 45 \
  --benchmark-resolution 3840x2160 \
  --render-scale 0.5 \
  --metalfx-upscaler \
  --disable-vsync
```

4K sky/downward view:

```bash
./build-release/tests/SparseVoxelRendererSample <same scene args> \
  --benchmark-lighting perf-4k-roof.json \
  --benchmark-sequence roof-down \
  --benchmark-warmup 45 \
  --benchmark-frames 45 \
  --benchmark-resolution 3840x2160 \
  --render-scale 0.5 \
  --metalfx-upscaler \
  --disable-vsync
```

### 5.2 Xcode Metal attribution

Use short captures after confirming the scene is representative. `--profile-passes`
is required for pass names; it disables render-graph command-buffer merging, so
do not use the profiled frame time as shipping frame time.

Required capture source:

- Xcode Instruments template: **Metal System Trace**.
- Capture tool: `xcrun xctrace record --template 'Metal System Trace'`.
- Renderer flag: `--profile-passes`, otherwise render-graph command buffers are
  merged and pass labels are not attributable.
- Parser: `scripts/metal-trace-passes.py`.
- Interpretation: use pass shares and named/unattributed movement, not the raw
  profiled frame time.

```bash
xcrun xctrace record --template 'Metal System Trace' \
  --output perf-4k-walk-metal.trace \
  --launch -- \
  ./build-release/tests/SparseVoxelRendererSample <same scene args> \
  --benchmark-lighting perf-4k-walk-metal.json \
  --benchmark-sequence camera-walk \
  --benchmark-warmup 45 \
  --benchmark-frames 30 \
  --benchmark-resolution 3840x2160 \
  --render-scale 0.5 \
  --metalfx-upscaler \
  --profile-passes \
  --disable-vsync

python3 scripts/metal-trace-passes.py \
  build-release/tests/perf-4k-walk-metal.trace \
  --skip-fraction 0.5 \
  --top 35
```

### 5.3 Reporting requirements

Every optimization report should include:

- command line;
- requested resolution and actual drawable resolution;
- render scale and upscaler path;
- frame p50, p95, max, and mean from benchmark JSON;
- Xcode Instruments / Metal System Trace file path;
- Xcode trace top named passes, plus unattributed share;
- explicit note that the trace was captured with `--profile-passes`;
- terrain resident count and upload/refinement counters once Part 2 adds them;
- quality notes or screenshots for moving camera and roof-down views.

Reports that omit the Xcode/Metal trace are allowed to say "pacing changed",
but must not claim "pass X got faster" or "this optimization fixed the 4K
bottleneck".

## Part 6 - Milestones

### Milestone A - Stabilize FullHD

**Status:** done for the current `camera-walk` and `roof-down` generated-world
views. Validation used Xcode Instruments / Metal System Trace captures as
required; benchmark JSON was used only for frame pacing and terrain counters.

- Implemented upload smoothing defaults from Part 2:
  32 pages/update and 32 MiB/frame at 1920x1080, 16 pages/update and 16
  MiB/frame for 1440p/4K-class output, with `--terrain-upload-budget-mb` as the
  explicit byte-budget override.
- Confirmed 1080 `camera-walk` after convergence under the 33.3 ms p95 budget
  with MetalFX frame generation enabled:
  `/tmp/svr_milestone_a/1080-walk-framegen-shipping-metal-20260822-190230.trace`
  and `/tmp/svr_milestone_a/1080-walk-framegen-shipping-metal.json`.
  The 30-frame measured window reported p50 16.63 ms, p95 17.13 ms, max
  17.36 ms, schema 17, `terrain_pages_per_update=32`, and
  `terrain_upload_budget_bytes=33554432`.
- Confirmed 1080 `roof-down` after convergence under the 33.3 ms p95 budget
  with MetalFX frame generation enabled:
  `/tmp/svr_milestone_a/1080-roof-framegen-shipping-metal-20260822-190253.trace`
  and `/tmp/svr_milestone_a/1080-roof-framegen-shipping-metal.json`.
  The 30-frame measured window reported p50 16.63 ms, p95 17.40 ms, max
  17.49 ms, schema 17, `terrain_pages_per_update=32`, and
  `terrain_upload_budget_bytes=33554432`.
- Captured short `--profile-passes` Metal System Trace attribution runs for
  both 1080 views. The traces confirm pass labels remain attributable with the
  new telemetry path:
  `/tmp/svr_milestone_a/1080-walk-framegen-metal-20260822-190051.trace` and
  `/tmp/svr_milestone_a/1080-roof-framegen-metal-20260822-190135.trace`.
  In both traces `SceneScreenTrace` remained the top named encoder, so
  Milestone A did not move the primary named GPU hotspot; it stabilized
  FullHD streaming/pacing and added the counters needed for later milestones.

### Milestone B - Remove 4K feedback overhead

**Status:** packed/cadenced feedback implemented and Metal-instrumented, but
the 55 ms 4K pacing target is not met by this milestone alone.

- Implemented packed generated-world terrain feedback:
  `SparseVoxelTerrainFeedbackRecordGPU` records carry
  `slot, generation, priority, desiredLOD, flags`, populated by
  `sparse_voxel_terrain_feedback_pack.wgsl` and consumed by
  `readWorldTerrainPageRequests`.
- 4K generated-world feedback now defaults to cadence 4 unless explicitly
  overridden with `--terrain-feedback-interval`; 1080 remains cadence 1.
- Re-ran 4K `camera-walk` with Xcode Instruments / Metal System Trace:
  `/tmp/svr_milestone_b/4k-walk-packed-shipping-metal-20260822-191619.trace`
  and `/tmp/svr_milestone_b/4k-walk-packed-shipping-metal.json`.
  Requested `3840x2160`, actual drawable `3840x2084`, MetalFX render scale
  0.5. The 30-frame measured window reported p50 60.17 ms, p95 67.54 ms, max
  72.93 ms, `terrain_pages_per_update=16`,
  `terrain_upload_budget_bytes=16777216`, and
  `terrain_feedback_interval=4`; 7 feedback readbacks were submitted and 23
  were skipped.
- Re-ran 4K `camera-walk` with `--profile-passes` for pass attribution:
  `/tmp/svr_milestone_b/4k-walk-packed-profile-metal-20260822-191702.trace`
  and `/tmp/svr_milestone_b/4k-walk-packed-profile-metal.json`.
  The retained trace window showed `TerrainFeedbackPack` at 1.4% of named
  encoder time and no old `BrickCopyTerrainPageRequest*` passes. The top named
  work moved elsewhere, led by `SceneReceiverWorklistReset`, `TerrainUpload`,
  `SceneRaySchedule`, atmosphere, and `SceneScreenTrace`, while unattributed
  encoder time remains large.
- Conclusion: Milestone B removes the specific broad terrain feedback readback
  overhead, but 4K still needs Milestone C/D source-frame work to get below
  55 ms and toward the 50 ms source-frame budget.

### Milestone C - 4K performance preset

**Status:** preset implemented and Metal-instrumented, but the 50 ms 4K
`camera-walk` source-frame target is not met by this milestone alone.

- Added `--terrain-performance-preset 4k-performance` plus the convenience
  alias `--terrain-4k-performance-preset`. The preset resolves to MetalFX at
  render scale 0.5 on Apple, frame generation enabled, and SVGF a-trous 1. It
  does not disable atmosphere, GTAO, spatial ReSTIR, or screen traces.
- Benchmark schema 18 now records `terrain_performance_preset`, so C captures
  are self-describing.
- Re-ran 4K `camera-walk` with Xcode Instruments / Metal System Trace:
  `/tmp/svr_milestone_c/4k-walk-preset-shipping-metal-20260822-192402.trace`
  and `/tmp/svr_milestone_c/4k-walk-preset-shipping-metal.json`.
  Requested `3840x2160`, actual drawable `3840x2084`, preset
  `4k-performance`, SVGF a-trous 1, MetalFX render scale 0.5. The 30-frame
  measured window reported p50 58.61 ms, p95 66.36 ms, max 66.60 ms,
  `terrain_pages_per_update=16`, `terrain_upload_budget_bytes=16777216`, and
  `terrain_feedback_interval=4`. This improves over Milestone B's p50
  60.17 ms, but by only 1.56 ms, below the 2 ms acceptance target and still
  above the 50 ms 4K source-frame goal.
- Re-ran 4K `camera-walk` with `--profile-passes` for pass attribution:
  `/tmp/svr_milestone_c/4k-walk-preset-profile-metal-20260822-192438.trace`
  and `/tmp/svr_milestone_c/4k-walk-preset-profile-metal.json`.
  The retained trace window reported 157.9 ms in named encoders and
  380.2 ms unattributed. Top named work was `SceneScreenTrace` and `Blit`
  at about 24.5% each, followed by `Final.BrickPhaseHZB` at 16.1% and
  `TerrainUpload` at 7.4%. `SVGF.ATrous2` is gone from the named pass list.
- Captured two additional 4K preset smoke views with Xcode Metal traces:
  `/tmp/svr_milestone_c/4k-roof-preset-shipping-metal-20260822-192647.trace`
  and `/tmp/svr_milestone_c/4k-roof-preset-shipping-metal.json`
  reported p50 37.46 ms, p95 40.95 ms, max 41.10 ms for `roof-down`;
  `/tmp/svr_milestone_c/4k-yaw-preset-shipping-metal-20260822-192720.trace`
  and `/tmp/svr_milestone_c/4k-yaw-preset-shipping-metal.json`
  reported p50 51.84 ms, p95 57.82 ms, max 59.06 ms for `camera-yaw`.
- Conclusion: Milestone C removes one SVGF graph pass and is worth keeping as
  an explicit 4K quality/performance mode, but the walking case is now dominated
  by screen trace, blit/MetalFX/present-unattributed work, BrickPhaseHZB, and
  upload. Continue with Milestone D before making this automatic for all 4K
  MetalFX runs.

### Milestone D - Screen-trace cleanup

**Status:** implemented and Metal-instrumented. This milestone adds the needed
visibility and gives a small 4K cleanup, but it does not satisfy the 1080
screen-trace acceptance gate.

- Added `--screen-trace-statistics` and schema 19 `screen_trace` frame records.
  The counters cover active rays, node visits, leaf visits, max leaf visits,
  hits, `MISS_TMAX`, `ESCAPED_SCENE`, and short projected segments.
- Added a projected-segment dynamic leaf cap in `SceneScreenTrace` and a
  conservative subpixel projected-segment early-out.
- Fixed disabled-screen-trace validation for benchmark mode. The world-only
  validation run completed and wrote
  `/tmp/svr_milestone_d/1080-world-only-validation.json`.
- Rejected the full receiver-cell-bias early-out. The diagnostic capture
  `/tmp/svr_milestone_d/4k-walk-screenstats-metal-20260822-193624.trace`
  showed active rays all terminating as short `MISS_TMAX` with zero node/leaf
  visits, so that path was correctness-risky and was removed.
- 4K `camera-walk` shipping with the 4K preset:
  `/tmp/svr_milestone_d/4k-walk-d2-shipping-metal-20260822-194229.trace` and
  `/tmp/svr_milestone_d/4k-walk-d2-shipping-metal.json`. Requested
  `3840x2160`, actual drawable `3840x2084`, p50 57.80 ms, p95 64.72 ms,
  max 65.02 ms. Compared with Milestone C's p50 58.61 ms and p95 66.36 ms,
  this is a modest 0.81 ms median and 1.64 ms p95 improvement.
- 4K `camera-walk` pass profile:
  `/tmp/svr_milestone_d/4k-walk-d2-profile-metal-20260822-194305.trace` and
  `/tmp/svr_milestone_d/4k-walk-d2-profile-metal.json`. The retained window
  reported 84.7 ms named encoder time and 386.1 ms unattributed. Top named
  work was `TerrainUpload` 13.8%, `Blit` 13.2%, `SceneScreenTrace` 9.0%,
  `SceneRaySchedule` 6.0%, and atmosphere/compose passes around 4%.
- 1080 `camera-walk` pass profile:
  `/tmp/svr_milestone_d/1080-walk-d2-profile-metal-20260822-194448.trace` and
  `/tmp/svr_milestone_d/1080-walk-d2-profile-metal.json`. Frame pacing remains
  good at p50 16.60 ms, p95 17.02 ms, max 17.40 ms, but the Metal trace still
  shows `SceneScreenTrace` as 48.5% of named encoder work. This means the
  acceptance criterion, lower 1080 `SceneScreenTrace` share without payback in
  world tracing, is not met.
- Counter captures:
  `/tmp/svr_milestone_d/4k-walk-screenstats-fixed-metal-20260822-193731.trace`
  and `/tmp/svr_milestone_d/4k-walk-screenstats-fixed.json` reported about
  102.4k active rays/frame, 9.24 node visits/active ray, 0.0326 leaf
  visits/active ray, 18.3 max leaf visits on average, 0.13% hits, and 26.0%
  short segments. `/tmp/svr_milestone_d/1080-walk-screenstats-subpixel1-metal-20260822-194158.trace`
  and `/tmp/svr_milestone_d/1080-walk-screenstats-subpixel1.json` reported
  about 18.4k active rays/frame, 10.59 node visits/active ray, 0.0220 leaf
  visits/active ray, 13.4 max leaf visits on average, 0.05% hits, and 16.3%
  short segments.
- Conclusion: leaf pressure is not the dominant screen-trace problem. Most
  cost is in active-ray count and hierarchy stepping, while the hit rate is very
  low. The next screen-trace work should focus on reducing rays entering the
  pass, using a better screen hierarchy traversal, or scheduling fewer
  low-value rays, not tightening the leaf cap further. 4K remains above the
  50 ms source-frame goal, with large unattributed Metal/MoltenVK/present work
  still visible in every trace.

## Part 7 - Open questions

- How much of the 4K unattributed encoder time is MetalFX/present/drawable
  scheduling, and how much is still hidden MoltenVK work? A dedicated short
  trace with fewer warmup frames may separate this better.
- Can terrain feedback be handled fully on GPU until a compact top-N list is
  ready, avoiding full CPU readback most frames?
- Does a-trous 1 hold up visually in shadowed or high-frequency terrain, or only
  in the current open generated-world fixture?
- Should the sample expose a user-facing "4K terrain performance" preset, or
  should the heuristics be automatic from output resolution and upscaler path?
