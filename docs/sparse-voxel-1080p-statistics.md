# Sparse voxel 1080p cost statistics

Status: measurement record, 2026-08-14. Companion to
`sparse-voxel-hd-upscaled-performance-plan.md`, which holds the staged work.
This file holds only what was measured and how, so a later claim can be checked
against it.

Protocol: `--benchmark-lighting` aggregates, built-in GPU timestamps
**disabled**, default terrain package, `--benchmark-sequence camera-walk`, LOD
0.75/1.5, 90 warmup and 240 measured frames, `--render-scale 1.0 --no-taa` for
the native arms. Two or three interleaved repeats per arm.

**Thermal drift is real on this machine.** A 3-repeat run without cooldown had
every arm's third repeat systematically high, producing a 6.07 ms within-arm
spread. Repeats with a 25 s cooldown between runs gave spreads of 0.01-0.68 ms.
**Use min-of-N across runs, and put a cooldown between them.**

## 1. The frame is almost purely per-pixel

Five resolutions, full frame and `--depth-only`, native, no temporal:

| Resolution | Mpx | full | depth-only | post-depth |
| --- | ---: | ---: | ---: | ---: |
| 640x360 | 0.23 | 8.672 ms | 3.894 ms | 4.778 ms |
| 960x540 | 0.52 | 15.755 ms | 6.194 ms | 9.562 ms |
| 1280x720 | 0.92 | 25.844 ms | 9.609 ms | 16.236 ms |
| 1600x900 | 1.44 | 38.633 ms | 14.057 ms | 24.576 ms |
| 1920x1080 | 2.07 | 54.429 ms | 20.114 ms | 34.315 ms |

Linear fits:

```text
full        = 2.926 ms + 24.827 ms/Mpx      R^2 = 1.00000
depth-only  = 1.664 ms +  8.786 ms/Mpx      R^2 = 0.99889
post-depth  = 1.262 ms + 16.041 ms/Mpx
```

An R^2 of 1.00000 over a 9x pixel range is as clean as this kind of measurement
gets. Two consequences follow directly:

- **Fixed cost is 2.93 ms.** Pass-count reduction, encoder merging and dispatch
  overhead cannot deliver a target; there is only 2.93 ms of it in total, and
  the merged-encoder experiment already confirmed that independently.
- **Post-depth is 65% of the per-pixel cost** (16.04 of 24.83 ms/Mpx). The brick
  raster and culling together are 35%.

### What a target requires

| Target at native 1920x1080 | Frame budget | Per-pixel cost must be |
| --- | ---: | ---: |
| 35 FPS | 28.6 ms | x0.50, a **50% cut** |
| 40 FPS | 25.0 ms | x0.43, a **57% cut** |
| 60 FPS | 16.7 ms | x0.27, a **73% cut** |

At 1280x720 the same model gives 25.8 ms, 38.7 FPS, so **720p native already
clears 35 FPS** with no work at all.

## 2. Register-twice pass attribution

An idempotent pass registered a second time rewrites identical values, so the
output signature is unchanged and the whole-frame delta is that pass's true
marginal cost. Exposed as `--profile-duplicate-pass <group>`.

**Every group is neutrality-checked first**: the exact-output signature must be
bit-identical with and without the duplicate. That check is what makes the
number trustworthy, and it is not a formality - see the G-buffer below.

Measured at 1920x1080 native, min-of-3:

| Group | Marginal | ms/Mpx | Note |
| --- | ---: | ---: | --- |
| `probe-interpolate` | **+4.79 ms** | 2.31 | Largest attributed pass. Confirmed twice: 4.574 ms and 4.793 ms in separate sessions, the second with a 0.014 ms spread. |
| `brick-resolve` | +0.97 ms | 0.47 | **Control.** Independently measured at 0.467 ms at 1200x720, predicting 1.121 ms here. 13% agreement validates the instrument. |
| `probe-copies` | +0.90 ms | 0.43 | Three probe history buffer copies. |
| `compose` | +0.38 ms | 0.19 | |
| `svgf-copies` | **-0.02 ms** | -0.01 | Four full-resolution texture copies, and they are **free**. |

### Passes this technique cannot size

- **The G-buffer.** Duplicating it **changed the exact-output hash**, so it is
  not idempotent and its number would have been meaningless. Suspected cause is
  the depth attachment load or the motion-vector target. The group was removed
  rather than reported. It remains the single largest unattributed suspect: 12
  full-resolution render targets at **72 bytes per pixel**, which is 149 MB
  written per frame at 1080p before any downstream read.
- **ProbeRays, ProbeResolve, ProbeShare.** All blend into history buffers, so a
  duplicate is a different render, not the same one twice.

These need an Xcode Metal System Trace. Do not reach for `--gpu-timings`: it
brackets Vulkan passes rather than Metal encoders, so a MoltenVK stall is billed
to whichever pass runs next; it already overstated `BrickResolve` by 4.7x.

## 3. Whole-frame ablations

Valid for anything with a flag, including non-idempotent passes.

At 1920x1080 native, min-of-3 with cooldown:

| Ablation | Delta | Note |
| --- | ---: | --- |
| `--probe-tile 16` | **-3.24 ms** | Probe grid density is a real dial. |
| `--di-mode per-pixel` | **+74.94 ms** | Probe mode is not optional. Per-pixel direct lighting costs 129 ms against 54 ms. |

At 1280x720 native:

| Ablation | Delta |
| --- | ---: |
| `--disable-svgf` | -2.79 ms |
| `--svgf-atrous-iterations 0` | -2.41 ms |
| `--disable-atmosphere` | -0.93 ms |
| `--disable-volumetric-fog` | -0.07 ms |
| `--no-gtao` | -0.05 ms |
| `--ray-resolution-divisor 2` | **0.00 ms** |
| `--disable-spatial-restir` | **0.00 ms** |

**Scene-ray tracing and reservoir work are not the cost.** Halving the ray grid
and disabling spatial ReSTIR each moved the frame by nothing against spreads of
0.21-0.24 ms.

## 4. Post-depth budget at 1080p

Post-depth is 34.3 ms. Attributed so far:

| Item | Cost | Source |
| --- | ---: | --- |
| SVGF total | ~6.3 ms | 720p ablation scaled by 2.25 |
| ProbeInterpolate | 4.79 ms | register-twice |
| Atmosphere | ~2.1 ms | 720p ablation scaled |
| BrickResolve | 0.97 ms | register-twice |
| Probe history copies | 0.90 ms | register-twice |
| Compose | 0.38 ms | register-twice |
| GTAO | ~0.1 ms | 720p ablation scaled |
| SVGF history copies | 0.00 ms | register-twice |
| **Attributed** | **~15.5 ms** | |
| **Unattributed** | **~18.8 ms** | G-buffer plus the probe ray/trace chain |

The unattributed remainder is larger than everything attributed. It is
concentrated in exactly the two places register-twice cannot reach.

## 5. Raster-side counters

At 1280x720, from `--brick-raster-diagnostics --brick-measure-area`. These
perturb timing and never supply performance numbers; they are workload
statistics only.

| Counter | Value |
| --- | ---: |
| Visible bricks, phase 0 | 14,599 |
| Rasterizer fragments | 8,721,169 |
| Covered pixels | 530,762 |
| **Overdraw** | **16.4x** (peak 255) |
| Scale-4 groups tested / skipped | 858,006 / 118,878 = **13.9% skipped** |
| Scale-2 cells visited | 9,266,261 |
| Fine occupancy probes | 9,858,330 |
| First-hit exits / misses | 6,772,833 / 1,558,277 |
| Micro descriptor loads | 6,603,134, **1:1 with eligible parents** |

The 13.9% coarse skip rate is the argument for anisotropic brick shapes (R7):
a surface-fitted 8-cubed brick on terrain is mostly occupied, so the coarse mask
has almost nothing to reject.

**The overdraw figure is not by itself an argument for fill-rate work.** Two
independent fill-oriented changes - merging the R2 split's render passes, and
front-to-back depth ordering of the visible list - both measured neutral. See
section 5 of the plan document.

## 6. Memory

From the brick hierarchy build on the default terrain package:

```text
materialIndices   27,016,020 x 1 byte  =  27.0 MB
microSurfaceData  27,016,020 x 4 bytes = 108.1 MB   <-- 80% of the total
palette               45,568 x 8 bytes =   0.4 MB
                                   total 135.4 MB   (logged as materialBytes)
```

`microSurfaceData` is one word per occupied voxel carrying a context byte and a
24-bit four-corner slope patch. Interior voxels store zero, so most of those
108 MB are zeros. This is the memory argument for the heightmap microvoxel path.

## 7. Reproducing

```sh
# resolution regression
for res in 640x360 960x540 1280x720 1600x900 1920x1080; do
  for mode in "" "--depth-only"; do
    ./SparseVoxelRendererSample \
      --terrain-package "$PWD/polygon-terrain-default/planet.planetpkg" \
      --terrain-lod-levels 5 --lod-threshold 0.75 --tree64-lod-threshold 1.5 \
      --benchmark-sequence camera-walk --benchmark-warmup 90 --benchmark-frames 240 \
      --benchmark-resolution $res --render-scale 1.0 --no-taa \
      --disable-metalfx-upscaler --disable-frame-generation --disable-vsync $mode \
      --benchmark-lighting out.json
  done
done

# pass attribution, with the neutrality check first
./SparseVoxelRendererSample --gbuffer-resolver bricks --frames 3 --no-terrain \
  --test-frame-signatures --resolution 320x180 --no-taa \
  --profile-duplicate-pass probe-interpolate      # hash must match the base run
```

Groups: `brick-resolve` (control), `svgf-copies`, `probe-copies`,
`probe-interpolate`, `compose`, or `all`.
