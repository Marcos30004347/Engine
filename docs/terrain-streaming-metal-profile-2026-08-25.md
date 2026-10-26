# Terrain streaming Metal profile - 2026-08-25

## Scope

- Host: Apple M4, 10-core GPU, Metal 4
- Xcode: 26.6 (17F113)
- Renderer: Release `SparseVoxelRendererSample`, generated `world-data` at
  `0.5,0.5`, 16 km render distance
- Output: 1920x1080, 960x540 internal MetalFX source
- Terrain defaults: 0.1 m finest voxel, 4 px spatial error, 32 pages/update,
  32 MiB upload budget, five raster LOD levels in benchmark mode
- Camera: built-in `camera-walk` loop. Captures at warm-up phases 180, 480,
  and 540 cover the reverse-cliff/high-clearance, spawn/surface, and
  forward/crest portions of the loop. The generated terrain logs report a
  1.8 m spawn clearance and about 59 m clearance on the reverse-cliff portion.

The benchmark JSON is used for source-frame pacing. GPU pass attribution comes
only from Xcode Instruments `Metal System Trace` with `--profile-passes`; no
Vulkan GPU timestamps were enabled. `--profile-passes` prevents RenderGraph
command-buffer merging, so trace pass shares are useful but traced absolute
frame time is not a shipping frame time.

## Source-frame pacing

All rows use the same 180 warm-up / 120 measured-frame camera walk unless noted.

| Arm | CPU p50 | p95 | p99 | max | Delta p50 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Original default | 17.435 ms | 18.141 ms | 19.162 ms | 22.507 ms | - |
| Frame generation disabled | 13.585 ms | 16.625 ms | 16.735 ms | 16.991 ms | -3.849 ms |
| Compute route <= 64 px | 18.417 ms | 19.153 ms | 20.104 ms | 20.759 ms | +0.982 ms |
| All bricks on compute | 18.853 ms | 19.652 ms | 31.755 ms | 33.696 ms | +1.418 ms |

The default frame's CPU p50 is split between RenderGraph submit/back-pressure
(7.925 ms) and present (8.740 ms). File I/O / streaming / upload work on the
render thread is 0.0025 ms p50, 0.0077 ms p95, and 1.796 ms max. Terrain
feedback readback is 0.0146 ms p50 and 0.0256 ms p95. Tree64 CPU planning is
0.0014 ms p50 and no Tree64 CPU upload is recorded in the measured window.

Disabling frame generation changes the pacing shape: present falls to 0.171 ms
p50 and submit to 1.271 ms p50, while the explicit GPU-frame wait becomes
11.257 ms p50. The source frame is 3.85 ms faster and its worst measured frame
falls by 5.52 ms. This does not account for the extra displayed interpolated
frames produced by frame generation; it measures responsiveness and source
frame stability.

After making native frame generation opt-in and restoring exact-leaf raster
for generated spatial pages, a 120 warm-up / 120 measured-frame static default
validation reported 13.410 ms p50, 16.659 ms p95, and 17.002 ms max. That is
about 74.6 source FPS at the median and 60.0 FPS at p95. All 46 requested pages
were resident throughout the measured window. The stricter terrain raster cut
therefore did not give back the source-frame pacing recovered by disabling
frame generation.

## Metal System Trace

The settled default capture retained a 673 ms window. Xcode recorded 31 drawable
waits totaling 196.2 ms: 5.55 ms p50, 13.96 ms p95, 14.85 ms max. With frame
generation disabled, a 531 ms window contained 11 drawable waits totaling
13.6 ms: 0.99 ms p50, 3.38 ms p95, 3.85 ms max. Drawable starvation is the
largest directly observed default pacing stall.

Across the three camera-depth windows, `SceneScreenTrace` is consistently the
largest or one of the largest named Metal encoders (15.5-37.4% of named encoder
time). Ray worklist/reset/finalize, HZB, GBuffer, and brick culling move with
the view. Terrain upload is small: 1.1% in the reverse-cliff capture, absent
from the settled spawn/crest measured windows, and never a leading encoder.
Only 34-43% of retained encoder time is named in the short depth windows;
MetalFX, presentation, MoltenVK, and other Metal-owned command buffers remain
unattributed and are not charged to adjacent Vulkan passes.

The small-brick compute arm adds `BrickTileRasterRaster` at 8.2% of named Metal
encoder time. Bin count/reset/scatter add further work. Drawable waits remain
essentially unchanged (198.0 ms total versus 196.2 ms), so compute routing adds
GPU work without reducing the active pacing stall.

Xcode's `Metal GPU Counters` add-on was also attempted. Instruments reported
`Selected counter profile is not supported on target device` for this M4, so no
bandwidth/occupancy counter values are claimed here.

## Terrain streaming

During the default measured window, residency advanced from 43 to 46 pages.
Three upload frames submitted 1,361,008 bytes / 42 sections, 144,960 bytes / 16
sections, and 190,780 bytes / 21 sections. Pending upload data peaked at
1,696,748 bytes, far below the 32 MiB frame budget. The worker logs show page
generation dominated by map sampling (roughly 55-175 ms for the observed
batches), followed by hierarchy construction (17-50 ms); that work stayed off
the render thread.

The diagnostic `upload_queue_latency_frames` reached 143 even after pending
bytes returned to zero. It is the last completed submission delta, not a live
queue-depth measurement, so it should not be interpreted as 143 frames of
current GPU backlog.

## Conclusions

1. Keep the compute brick rasterizer disabled. Small-brick routing regresses
   p50 by 0.98 ms; all-compute regresses p50 by 1.42 ms and introduces a 33.7 ms
   outlier. Early-Z on the hardware path is cheaper than the fixed bin,
   full-tile raster, and full-screen merge entry cost.
2. Keep native frame generation opt-in. It improves displayed frame count, but
   in this workload it creates long drawable waits and costs 3.85 ms of
   source-frame p50. MetalFX upscaling remains enabled by default.
3. Optimize `SceneScreenTrace` before brick rasterization. It is the only named
   GPU hotspot that persists across all tested camera depths.
4. Terrain CPU generation and GPU installation are no longer render-thread
   bottlenecks. Future terrain work should target request churn/time-to-detail
   and map-sampling throughput, not larger upload batches.

## Post-correctness validation

After correcting the startup camera to target an actual in-range height sample
and replacing the cancellation-prone terrain-patch quadratic formula, the
deterministic world-terrain image contract now runs three arms: an in-range
horizon, a close streamed brick slope, and the same close view through the
independent smooth-mesh contract. The close view reports 171,239 brick pixels
versus 172,025 mesh pixels (0.457%), with zero deep coverage holes, near-black
terrain pixels, steep closure normals, or heightmap-slope mismatches.

The corrected 180/120-frame shipping camera walk currently measures 34.87 ms
CPU p50 and 40.00 ms p95, almost entirely in GPU completion wait; terrain file
I/O, streaming, and uploads remain negligible on the render thread. A fresh
`--profile-passes` Metal trace attributes only 35% of retained encoder time and
labels 48.5% of the named share as `Atmosphere.Transmittance`. Do not optimize
from that label: disabling the complete atmosphere changes shipping p50 by only
about 1.1 ms, and changing internal render scale does not scale the short-run
wait. This capture is dominated by profiling/display scheduling perturbation,
so the earlier multi-depth conclusion and shipping A/Bs remain the useful
evidence.

The corrected close-slope compute parity run matches coverage exactly but has
69 nearest-depth disagreements, with a 1.42 km worst delta in overlapping
streamed terrain. Together with the Metal and shipping timing regressions above,
that keeps compute brick routing experimental and disabled by default.

### Gap-only correctness fallback

A later 10 cm regression isolated one hardware miss and a different compute
miss. Full hardware-first compute merge closed both, but its Metal span was
11.85 ms p50 / 18.20 ms p95 versus 11.59 / 15.49 ms for hardware-only, and it
added about 1.47 ms of compute work per frame. Full compute merge and brick
routing therefore remain disabled.

Generated terrain now enables a narrower gap-only merge. Pixels already
covered by hardware skip candidate traversal, and fully covered 16x16 tiles
exit uniformly before loading candidates. It closes the deterministic
close-slope hole and measured 10.69 ms p50 / 14.76 ms p95 Metal GPU span in
the same 252-frame window. Traced host-visible p50 was 11.67 ms versus 10.54 ms
for pure hardware, so this remains a correctness-first cost. Non-terrain scenes
stay on hardware-only raster, and `--disable-brick-tile-merge` preserves the
pure hardware comparison arm.

### Eviction and final regression contracts

The delayed disappearance was scheduling, not memory exhaustion. In the
bounded 300-frame default command, the terrain GPU allocation remained fixed at
222.27 MiB of the 2048 MiB budget through generations 2-9; the CPU page cache
was 42.63 MiB. Residency converged from 4 bootstrap pages through 253 pages to
216 pages without allocation growth. The feedback-driven cut had suppressed
CPU convergence after camera motion, even though an absent page cannot emit GPU
feedback. Camera movement now always refreshes the world cut, and missing or
pending pages keep CPU convergence active until the moved camera has a resident
owner.

The final regression set adds a feedback-driven camera move/owner contract, a
120-frame streaming round trip, and a four-view raster contract including the
default 10 cm voxel scale. The image diagnostics independently reject deep
coverage holes, incorrect heightmap slope normals, excessive near-black area,
and isolated hard-black pixels. The complete raster suite passes in 71.87 s and
the streaming round trip in 2.70 s on the profiled M4 system.

## Artifacts

- `/tmp/svr-default-terrain-camera-walk.json`
- `/tmp/svr-default-terrain-camera-walk-profile.trace`
- `/tmp/svr-default-terrain-low-profile.trace`
- `/tmp/svr-default-terrain-mid-profile.trace`
- `/tmp/svr-default-terrain-compute-route64-profile.trace`
- `/tmp/svr-default-terrain-no-framegen-profile.trace`
- `/tmp/svr-default-terrain-metal-counters.trace`
- `/tmp/svr-holefix-default.json`
- `/private/tmp/svr-hardware.trace`
- `/private/tmp/svr-merge.trace`
- `/private/tmp/svr-gapfill-vote.trace`
