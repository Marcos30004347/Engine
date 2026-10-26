# Sparse voxel optimizations

Working list, 2026-08-14. Ranked by measured size. Evidence lives in
`sparse-voxel-1080p-statistics.md`; staged detail in
`sparse-voxel-hd-upscaled-performance-plan.md`.

## The fact that ranks everything

```text
frame = 2.93 ms + 24.83 ms/Mpx          R^2 = 1.00000  (5 resolutions, 9x range)
  depth-only  = 1.66 ms +  8.79 ms/Mpx   (35% of per-pixel)
  post-depth  = 1.26 ms + 16.04 ms/Mpx   (65% of per-pixel)
```

Fixed cost is **2.93 ms in total**. Anything that only removes passes, encoders
or dispatches is competing for under 3 ms and cannot reach a target. Only
per-pixel cost matters.

Native 1920x1080 is 54.4 ms. 35 FPS needs a **50% per-pixel cut**, 60 FPS needs
**73%**. Native 1280x720 is 25.8 ms = 38.7 FPS, so **720p already clears 35 FPS**.

## Ranked

| # | Optimization | Size | Confidence | State |
| --- | --- | ---: | --- | --- |
| 0 | **Micro-geometry LOD handoff** | **10.1 ms, half of raster+cull** | measured, full curves | **next** |
| 1 | **Surface probe density -> DDGI** | **12.2 ms, 21% of frame** | measured, full curve | planned |
| 2 | **Internal render scale** | 18.5 -> 35.2 FPS at 1080p | measured | already shipping at 0.50 |
| 3 | **SVGF a-trous iterations** | ~5 ms at 1080p | measured, flag exists | tuning decision |
| 4 | **Temporal/output block** | 12.75 ms of the upscaled frame | measured | only if shipping upscaled |
| 5 | **Anisotropic bricks (R7)** | raster is 35% of per-pixel; coarse skip only 13.9% | structural | large work |
| 6 | **Heightmap microvoxel** | 108 MB memory, quality; speed unknown | high on memory | approved direction |

### 1. Surface probe density, and DDGI as its structural fix

**The largest lever in the renderer by a wide margin.** `--probe-tile` sweep at
1920x1080 native, min-of-2 with cooldown:

| probe tile | p50 | FPS | vs default | probes (relative) |
| ---: | ---: | ---: | ---: | ---: |
| 8 | 74.92 ms | 13.3 | +17.41 ms | 2.25x |
| **12 (default)** | **57.51 ms** | **17.4** | - | 1.00x |
| 16 | 50.76 ms | 19.7 | **-6.76 ms** | 0.56x |
| 24 | 46.48 ms | 21.5 | **-11.03 ms** | 0.25x |
| 32 | 45.31 ms | 22.1 | **-12.21 ms** | 0.14x |
| 48 | 45.01 ms | 22.2 | -12.51 ms | 0.06x |
| 64 | 45.30 ms | 22.1 | -12.22 ms | 0.04x |

Cost tracks probe count (1/tile^2) and **saturates at tile 32**: beyond it
nothing more is recovered, so the density-dependent probe work is
**12.2 ms of a 57.5 ms frame, 21%**, and the rest of the probe system is fixed.

`ProbeInterpolate` alone is **4.79 ms** (register-twice, confirmed twice,
0.014 ms spread); probe history copies are 0.90 ms. The remainder of the 12.2 ms
is the place/share/ray/resolve/filter chain.

`--di-mode per-pixel` costs **+74.9 ms**, so the probe system is already an
enormous win. The goal is to *thin* it, not remove it.

**This is the DDGI case, in numbers.** A world-anchored low-frequency cache that
lets the surface probe tile coarsen from 12 to 24 while holding quality recovers
about **11 ms**. Query DDGI inside the existing `ProbeInterpolate` path so the
cache *replaces* work rather than adding a pass; its D11 budget of no more than
3.20 ms incremental has to be read against the 11 ms it is meant to unlock.
Budget: whole-frame neutral or better, not "the probe passes got cheaper".

Coarsening the tile on its own is available today and free to try, but it is a
straight quality trade - judge it on the moving-camera sequences, not a static
camera.

### 1a. DDGI re-sized on terrain, 2026-08-16: the premise does not hold here

The case above assumes coarsening the surface probe tile costs quality, and that
DDGI is what buys the coarsening back. Re-measured in the 42 km horizon
configuration (background off, micro retired, riser artifact fixed), all arms
interleaved in one session:

```text
default (terrain uses tile 14)   36.530 ms   spread 0.318
--probe-tile 12                  39.860 ms   spread 0.279
--probe-tile 24                  29.267 ms   spread 0.114
```

**First: terrain does not use tile 12.** Line ~20459 sets 14 for terrain scenes
deliberately, so passing the "default" 12 explicitly is 3.3 ms *slower* than
passing nothing. Any earlier prize quoted against a tile-12 baseline overstates
by that much. The real spread from the shipping terrain default to saturation is
**7.26 ms of a 36.5 ms frame**.

**Second, and this is what changes the plan: coarsening costs almost nothing on
terrain.** Tile 14 against tile 24, same frame:

```text
mean relative luminance difference   0.71%
p50 0.03%   p90 1.72%   p99 11.11%
pixels differing more than 10%       1.29%
probe flicker           mean 0.029 -> 0.021, severeFraction 0.008 -> 0.005
```

The images are visually indistinguishable, and the coarser grid is *more*
temporally stable. DDGI exists to protect a quality loss that this scene does
not suffer: **take the 7.26 ms by coarsening and do not build the cache for
terrain.**

**Scope this claim honestly.** An open dune field has almost no local GI
structure - no contact shadows, no near-field bounce - which is the best
possible case for coarse probes. The code comment at the tile default says to
keep 12 for compact and interior scenes, and those are exactly where coarsening
should hurt. DDGI's case has to be re-made on a scene with local GI detail; it
is not dead, it is unproven on the scene that was being optimised.

### 1b. World irradiance cache (DDGI) - BUILT, populating, not yet consumed

A world-anchored spherical-harmonic irradiance cache now exists as a lighting
path in its own right: `--ddgi`.

```text
volumes    4 cascades x 12^3 probes = 6,912 probes, spacing 4/8/16/32 m
storage    9 RGB SH coefficients per probe, ~3 MB total
rays       32 per probe, spherical Fibonacci, rotated per refresh
tracing    the existing SparseVoxelTree64TracePass, pointed at the cache's own
           ray and result buffers - no second tracer
shading    a hit is fully blocking, a miss takes analytic sun and sky, so the
           cache holds sun-and-sky irradiance with real occlusion
blending   hysteresis 0.97, and a probe with no history takes its estimate
           outright rather than climbing from black
```

Reusing the Tree64 pass is the important structural decision: the cache
inherits the dynamic layer, the material geometry and every traversal fix
rather than carrying a second tracer that would drift from the first.

**Cost, measured against the 42 km horizon configuration:**

```text
no cache                                  29.417 ms
cache, 1/8 of probes per frame (27.6k rays)  29.503   +0.086
cache, every probe per frame (221k rays)     29.898   +0.481
```

Even a full refresh of all 6,912 probes every frame is under half a
millisecond, against the 7.26 ms that coarsening the surface probe tile is
worth. The ray budget is not the constraint on this idea.

**Query, and why it changed nothing on terrain.** The cache is now wired into
`ProbeInterpolate` (bindings 18/19, cascade count zero when absent) and answers
the *failed-gather* path: when no screen probe passes the strict test, a
trilinear eight-probe lookup replaces the 81-tile last-resort search rather than
adding to it. Measured on the 42 km terrain fixture at probe tile 24:

```text
                    no cache      with cache
blackFraction       0.000000      0.000000
widenedFraction     0.000000      0.000000
fallbackFraction    0.028077      0.028477
probe flicker mean  0.0196        0.0196
```

**The failure it targets does not occur on this scene.** The 81-tile search
never fires, nothing goes black, and the 2.8% of pixels on the fallback path
always find a neighbouring screen probe - so the cache branch, which only runs
when no fallback probe exists at all, almost never executes. This is the same
result as the coarsening measurement above and has the same cause: open terrain
has no local GI structure to lose. The shader's own comments locate this failure
on the interior fixture ("mean probe irradiance collapsed by 2.9x"), which is
where the cache should be evaluated. An attempt to measure it there was invalid
- `blackFraction` read 1.0 in *both* arms, meaning that fixture was not
rendering a scene, not that the cache failed.

**Rays per probe beat probe scheduling.** The incremental refresh defect is
moot: at full refresh, fewer rays per probe is both cheaper and more uniform
than refreshing a fraction of probes with more rays, because every probe stays
fresh every frame and the hysteresis integrates the rest.

```text
cache-primary, full refresh      frame     vs screen-probe gather
  32 rays/probe                  28.990         -0.547
  16 rays/probe                  28.430         -1.107   <- best
   8 rays/probe                  28.440         -1.097  (spread 0.893)
```

**Cache-primary: the main gather replaced. `--ddgi-primary`.**

```text
                      ProbeInterpolate   frame
screen-probe gather        1.819 ms      29.537
cache-primary              0.633 ms      28.500   -1.037 ms
```

**ProbeInterpolate drops 65%, and the frame is a full millisecond faster** even
while paying 0.481 ms to refresh every probe every frame. The pass is now well
inside a 1 ms budget. Two things got it there:

- Replacing a *search* with a *fetch*. The screen gather walks a 4x4 tile
  neighbourhood with validity, normal and plane-distance weighting plus each
  tile's refined probes; the query is eight probes at fixed offsets.
- **First-order SH.** At nine coefficients the query is bandwidth bound - eight
  probes x nine vec4 is 1,152 bytes per pixel - and cost 1.268 ms. Loading four
  instead of nine took it to 0.706. The second-order bands carry little of a
  sky-dominated cache's energy.

**Three correctness defects had to be fixed first, and each was invisible in
timings.** The first version rendered the near field black while distant terrain
- outside the cache's 192 m reach, where the query falls through - stayed lit:

1. **No sun.** Sampling a sun disc with 32 uniform rays essentially never hits
   it, so the cache carried sky only and came out **46x** darker than the path
   it replaces. Outdoor irradiance is sun dominated. Ray zero is now dedicated
   to sun visibility and the sun is added analytically as a delta lobe.
2. **Radiance where irradiance was expected.** The convolution folded in 1/pi;
   the screen path publishes irradiance (its fallback multiplies the DC term by
   pi for exactly this reason). A factor of pi reads as a plausible but
   uniformly dim image, not an obvious error.
3. **Buried probes.** With 4 m spacing, half of a ground receiver's eight probes
   sit below the surface and store zero. Backface weighting and skipping
   zero-energy probes handle it.

Result: near-field mean luminance 0.4001 against the screen path's 0.5150
(**0.78x**), no black regions, and probe flicker unchanged (mean 0.0193 vs
0.0198). The residual 22% is the analytic sky model, not a structural error.

**What this does not establish, and it is more serious than a spacing
argument.** The screen-probe field carries the **direct sun**, not just the
indirect term: removing the sun from the cache leaves it **13x darker** than the
path it replaces (0.0386 against 0.5150). So a world cache substituting for it
must carry the sun as well - and a 4 m cache resolves the sun's shadow at 4 m.
Each probe answers "is the sun visible" with a single ray, so on a shadowed
surface adjacent probes disagree and the trilinear blend renders that
disagreement as blotchy, dithered shading where a per-pixel shadow would be
smooth. Two artifact reports from the shadowed-geometry case are consistent with
exactly this.

**This is therefore not a like-for-like substitution.** The 1.1 ms is obtained
by replacing sharp direct lighting with 4 m-blurred direct lighting, which is
invisible on open sunlit dunes - almost nothing there is shadowed - and would be
plainly visible on shadowed geometry. `--ddgi-no-sun-in-cache` isolates the
term for A/B testing.

The architecture this points to: **the cache carries indirect only, and the
direct sun stays per-pixel.** That requires knowing how the existing chain
combines the probe field with direct lighting, which has not been established
here. Until it is, cache-primary should be treated as a measured *possibility*,
not a shippable path.

Neither could the artifacts be reproduced in the terrain fixture - it has almost
no shadowed geometry - so both diagnoses above are mechanism-level, not
observed.

**Populating state:** With
`--ddgi-update-fraction 1.0` every probe is visited and carries plausible sky
irradiance (`visited=6912`, `lit=100%`). The consumer side - a trilinear probe
lookup in the surface-probe path, so the cache *replaces* work rather than
adding to it - is not written, so today it costs 0.086 ms and delivers nothing.

**Known defect: incremental refresh.** At the default 1/8 per frame only the
first batch of 864 probes ever retains data. The refresh cursor is written by
the advance pass and read as zero by the ray generation and refresh passes;
the same passes also read the settings uniform as its frame-zero contents. The
advance pass accumulates its own generation counter across frames correctly, so
storage persists - the disagreement is specifically about buffers written by one
pass and read by another in this chain. Three separate attempts to route around
it (frame-local versus persistent uniforms, moving the write out of the recorded
command buffer, a GPU-owned cursor, and finally carrying the probe slot on the
ray itself) did not change the symptom, so the cause is in how these passes are
wired rather than in where the value lives.

Until that is understood, `--ddgi-update-fraction 1.0` is the working
configuration and costs 0.481 ms.

### 0. Micro-geometry LOD handoff - LANDED

`--micro-handoff-scale` divides the projected microvoxel size before the exact
handoff, so values above one retire exact carving at proportionally greater
distance. **Default is now 2.0**; 1.0 is bit-exact with the previous behaviour.

Why it was needed: the per-material `lodHandoff` byte is clamped to [0.25, 1.0]
and multiplies a 0.5..1.0 pixel fade band, so it could only ever retire exact
microgeometry *later*. There was no way to express "retire it sooner". Applied
identically in the raster fragment and in MicroPrepare so the R2 prepared path
stays consistent.

Measured, 1920x1080, min-of-2:

| scale | raster+cull | vs 1.0 | full frame | FPS |
| ---: | ---: | ---: | ---: | ---: |
| 1.0 (previous) | 19.80 ms | - | 53.53 ms | 18.7 |
| **2.0 (default)** | **14.57 ms** | **-5.23 ms** | **51.37 ms** | **19.5** |
| 4.0 | 12.97 ms | -6.83 ms | 50.09 ms | 20.0 |
| 8.0 | 11.81 ms | -7.99 ms | 47.32 ms | 21.1 |
| micro off (floor) | 10.02 ms | -10.12 ms | 43.23 ms | 23.1 |

Scale 2 recovers about half of everything exact microgeometry costs, for detail
that projects below two pixels. Higher scales keep paying but the quality cost
grows; 2.0 is the conservative pick and the dial is there for a quality tier.

The R2 gate's `COLUMNS` case is pinned to scale 1.0. The shipping default
retires every staircase parent at the dolly framing, which left the R3 column
cache assertion testing an empty population - the gate caught that immediately
when the default moved, which is what it is for.

### 0b. Why the handoff was worth landing first

**Exact micro-geometry carving is 10.12 ms of the 20.13 ms raster+cull, and
10.78 ms of the whole 1080p frame.** Disabling it with
`--disable-terrain-micro-materials` lands raster+cull at **10.02 ms** and the
frame at 43.23 ms (23.1 FPS from 18.5).

This is the largest lever inside the raster by a factor of seven over every
geometry knob combined, and none of the LOD/clipmap/voxel-size dials touch it.

**Coarsening microvoxels barely helps.** Terrain presets use subdivision 5, so
at a 0.195 m voxel a microvoxel is 3.9 cm. Sweeping it (`--terrain-micro-subdivision`):

| subdivision | microvoxel | raster+cull | vs default |
| ---: | ---: | ---: | ---: |
| 5 (default) | 3.9 cm | 20.19 ms | - |
| 4 | 4.9 cm | 20.48 ms | +0.29 ms |
| 3 | 6.5 cm | 19.56 ms | -0.63 ms |
| 2 | 9.8 cm | 17.73 ms | **-2.46 ms** |
| off | - | 10.02 ms | **-10.12 ms** |

Going from 3.9 cm to 9.8 cm microvoxels recovers only 2.46 ms of the 10.12 ms.
**The cost is not per-column stepping** - it is the fixed per-fragment cost of
*entering* the micro path: the dependent descriptor load (6,603,134 per frame,
exactly 1:1 with eligible parents, so no reuse across the DDA walk), the context
decode, the eligibility test, and the register pressure of a 140 KiB divergent
fragment shader.

**Distance is where it is being spent.** `--brick-exact-micro-distance` already
exists and already gates exact carving by distance, but **defaults to off**:

| exact micro cutoff | raster+cull | vs default |
| ---: | ---: | ---: |
| none (default) | 20.15 ms | - |
| 200 m | 19.70 ms | -0.45 ms |
| 100 m | 18.28 ms | -1.87 ms |
| 50 m | 16.04 ms | **-4.11 ms** |
| 25 m | 14.40 ms | **-5.76 ms** |
| 10 m | 12.63 ms | **-7.52 ms** |

Distant terrain pays full price for exact microgeometry whose microvoxels are
far below a pixel. That is a straightforward LOD failure with a large, measured
prize.

**The fix, in order of value:**

1. **A projected-size handoff in the incumbent fragment.** MicroPrepare already
   computes one - `microPrepareHandoffWeight` fades on projected microvoxel
   pixels - but the incumbent raster fragment has no equivalent; it has only the
   opt-in raw-distance cutoff. Gate exact carving on projected microvoxel size
   (below about 1 px, use the parent voxel) and enable it by default. Worth up
   to ~7.5 ms, and unlike a raw distance it is resolution- and FOV-correct.
2. **Make entering the micro path cheap** - the heightmap path (section 6)
   removes the descriptor load and context decode outright for terrain surfaces,
   and R5's specialized pipelines replace the one divergent fragment shader.

Both are worth more than R7 and R9, and cost far less.

### 5. Anisotropic bricks - and why raster+cull cannot be tuned

**Raster+cull is per-pixel bound, not geometry bound.** Every density and LOD
knob was swept at 1920x1080 with `--depth-only` (which isolates cull + brick
raster + depth), min-of-2 with cooldown, against a 20.1 ms baseline:

| Knob | Delta |
| --- | ---: |
| `--lod-threshold` 0.5 -> 3.0 px | **0.19 ms across the whole range** |
| `--terrain-lod-levels` 3 -> 6 | 0.15 ms across the whole range |
| `--terrain-chunk-radius 1` | -0.05 ms |
| `--disable-occlusion` | +0.22 ms |
| `--terrain-voxel-size 0.4` (8x coarser volume) | **-1.44 ms**, the best of all |
| `--terrain-voxel-size 0.1` | +2.42 ms |
| `--terrain-render-distance-km 4` | +4.27 ms (**slower**) |
| `--terrain-chunk-radius 2` / `4` | +6.48 / +3.97 ms (**slower**) |

Everything available adds up to about **1.4 ms of 20.1**. There is no
combination of LOD, clipmap, render-distance or voxel-size tuning that reaches
a 10 ms raster.

**`--lod-threshold` does nothing at all on terrain.** At 0.75 px and 3.0 px the
diagnostic counters are *byte-identical*: `phase0=14649`, `visibleAreaPixels=
18086395`, `fragments=18096640`. Terrain LOD is positional - spatial clipmap
rings carry an explicit finer-ownership clip tag and `brickHasFinerTerrainClip`
decides ownership - so the screen-error threshold is bypassed. It is not that
coarse LOD costs the same; the flag is simply not in the terrain path.

**Why coarsening geometry does not help.** Going from 0.2 m to 0.4 m voxels cuts
bricks 14,649 -> 12,702 (-13%) but fragments only 18.10 M -> 17.27 M (-5%), and
covered pixels are unchanged at 1.193 M -> 1.196 M. The terrain covers the same
screen either way; each covered pixel still walks a DDA through whatever bricks
overlap it. **Brick count is not the objective - covered pixels x overdraw x DDA
steps is.** This is R6's premise, now proven across the whole knob range.

So the cost is 18.1 M fragments for 1.19 M covered pixels: **overdraw 15.2x**,
about 1.1 ns per fragment. Halving raster means halving fragment count, and only
two things can:

1. **Anisotropic bricks (R7).** Coarse scale-4 skip rate is **13.9%** - a
   surface-fitted 8-cubed brick on terrain is mostly occupied, so the mask has
   nothing to reject. A `16 x 16 x 2` slab for near-horizontal terrain keeps the
   512-cell budget while cutting the vertical proxy extent, which is exactly
   what a grazing view ray pierces 15 times over.
2. **Compute/visibility raster (R9).** One visibility record per pixel instead
   of 15x overdraw each writing four integer MRTs.

Overdraw of 15x is the argument *for these two specific changes*, and not for
fill-rate work generally: two fill-oriented changes (merged render passes,
front-to-back ordering) already measured neutral because early-Z is already
declared and working.

### 0c. Microvoxel size and the handoff are ONE dial, not two - measured

**Coarsening microvoxels on its own makes the frame slower, not faster.** The
handoff retires exact carving on *projected* microvoxel size,
`(voxelSize / subdivision) * focal * outputScale / (distance * handoffScale)`,
so a coarser microvoxel projects **larger** and stays exact much further out:

| `--terrain-micro-subdivision` | eligible | exactTraces |
| ---: | ---: | ---: |
| 5 (3.9 cm, shipping) | 15,718,061 | 4,615,690 |
| 2 (9.8 cm), scale unchanged | 16,436,583 | **14,684,116** |

**3.2x more exact traces.** This is why the section 0b table's -2.46 ms for
subdivision 2 no longer reproduces: that was measured before
`--micro-handoff-scale` defaulted to 2.0, and the two dials fight each other.

**Pair them and the trace count is held fixed**, because dividing the microvoxel
by k and multiplying the handoff scale by k leaves the projected size identical:

| config | exactTraces |
| --- | ---: |
| subdivision 5, scale 2.0 | 4,615,690 |
| subdivision 2, scale 5.0 | 4,613,691 |
| subdivision 3, scale 3.4 | 4,540,990 |

With the retirement distance pinned, coarsening then buys what it should - a
shorter march per trace - and it is worth real time. 1920x1080, three repeats:

| config | repeats | median | spread |
| --- | --- | ---: | ---: |
| subdivision 5 + scale 2.0 (3.9 cm) | 51.29 / 51.13 / 50.86 | 51.13 ms | 0.43 |
| **subdivision 2 + scale 5.0 (9.8 cm)** | 49.03 / 49.07 / 49.10 | **49.07 ms** | **0.07** |

**-2.06 ms, about 4% of the frame**, for microvoxels at 9.8 cm instead of 3.9 cm
and **no change in how far exact carving reaches**. It is a quality tier, so the
terrain exactness gates need a deliberate rebaseline before it becomes default.

The dial is a trap in its current form: setting subdivision without the matching
scale is a 3.2x regression that looks like a coarsening "optimization". Either
derive the scale from subdivision or document them as one setting.

### 0d. The descriptor indirection is NOT the cost - denormalizing it is neutral

Section 0b attributes micro cost to "the dependent descriptor load (6,603,134
per frame, exactly 1:1 with eligible parents, so no reuse across the DDA walk)".
That attribution was tested directly and **does not hold**.

Reaching a descriptor was a two-step chain: a packed id word, then a scattered
dependent `microMaterials[descriptorId]`. Because the descriptor is identical
for every voxel sharing a palette entry, it can be denormalized into an array
keyed by palette address - the value the eligibility test has already computed -
collapsing the chain to one cached load. That is pure denormalization: same
values, no quality change, no package format change, ~16 bytes per palette entry.

Built, and **every counter is byte-identical** (`eligible=15718061`,
`exactTraces=4615690`, `fragments=18096470`). Whole-frame effect:

| | median of 3 | spread |
| --- | ---: | ---: |
| before | 51.29 ms | - |
| after denormalization | 51.13 ms | 0.43 |

**Inside the noise.** The descriptor array is small enough to sit in cache
permanently, so the "scattered dependent load" was already an L1 hit and
removing it recovers nothing.

What that leaves as the real cost of the micro path, by elimination: not the
descriptor fetch (neutral here), and not the stepping - `oneFace` measured
**1.54 steps per trace** and the parent walk **1.16 probes per fragment** with
81% first-hit exits. It is the per-entry *ALU*: the handoff weight, the
`smoothstep`, and the selector hash, all of which run on every one of the
15.7 M eligible entries even though only 4.6 M go on to trace.

That points the next attempt at making the *eligibility decision* cheap rather
than the fetch or the walk - which is what `--brick-exact-micro-distance`
already does with a single distance compare, measured in section 0b at -4.11 ms
(50 m) to -7.52 ms (10 m).

### 6a. Heightmap microvoxel: the reconstruction is already hardware bilinear

Measured and read out of the code, 2026-08-15. This is the strongest form of
section 6 and it subsumes the "exact micro in compute" port.

**The workload is entirely a heightfield.** With micro counters on, terrain at
1920x1080 reports:

```text
eligible=15718061   descriptorLoads=15718061        (exactly 1:1)
staircase contexts: interior=11286150  flat=2738813  slope=1693098  wall=0  other=0
trace paths:        oneFace=1832763    volume=0
exact contexts:     generalVolume=0    otherHeightfield=0
```

**Zero volume traces.** Every one of the five micro profiles except STAIRCASE is
unused on terrain, so a heightfield representation strands nothing. 71.8% of
entries are *interior* voxels, which store a surface patch they never use.

**The stored form is already a heightfield.** `SparseVoxelMicroGeometry.hpp`
keeps four XZ corner heights as UNorm6 in the low 24 bits of `microSurfaceData`,
and `sparseVoxelMicroSurfaceColumnDepth` reconstructs a column by **bilinear
interpolation of those four corners** at microcell centres `(x+0.5)/subdivision`:

```text
depth = floor((1 - bilinear(corners, u, v)) * subdivision)
```

So the replacement is a *re-layout*, not a new representation, and **the texture
unit's bilinear filter is exactly the reconstruction** the shader currently does
by hand. The descriptor decode, context byte, eligibility test and handoff
selector all collapse into one filtered fetch.

Corner values are shared between neighbouring parents - that sharing is what
makes the patch C0 - so the raster is one texel per **parent voxel corner**
(0.195 m), *not* per microvoxel (3.9 cm). A 1024^2 ring covers 200 m; five rings
reach 3.2 km for about 10 MB against the current 108 MB.

**The existing atlas cannot be reused.** `SparseVoxelPlanetSurfaceAtlas` already
holds a resident R16Float `height` plus an RG16Float `heightMinMax` pyramid with
CPU-built mips - the right shape, including the min/max data a conservative DDA
would want for empty-space skipping. But it is a planet *overview* asset:

```text
materialAtlas=3072x2048  ->  faceResolution=1024
sourceRadius=6371000m    ->  cube face edge ~10007 km  =>  9.77 km per texel
```

Against a 3.9 cm microvoxel that is ~250,000x too coarse. And
`PlanetVoxelChunk.cpp:584` evaluates every noise octave in planet space
specifically so pages and cube faces agree "without storing a dense global
heightmap", so no fine raster exists to borrow. A camera-local clipmap has to be
produced; that is the real work, and it is the same work however simple the DDA
that reads it turns out to be.

**The raster already exists on the CPU - it is just never uploaded.** Chunk
voxelization builds `haloHeights`, a dense per-column height array with a
neighbour halo, and `sampleHeight(x, z)` is a direct index into it
(`PlanetVoxelChunk.cpp:2203`). It is the *final* surface height - authored
package raster plus `proceduralDetailStrength` relief - at parent-voxel spacing,
with the halo already solving cross-chunk continuity. Uploading it per resident
chunk is the whole data path.

That also removes the corner encoding entirely. The stored UNorm6 patch is not a
height: it is a parent-relative signed *coverage*,
`clamp(((cornerHeight - elevationOrigin)/voxelSize - top) / gradientLength /
kTerrainMicroSlopeParentSpan, 0, 1)`, which depends on each parent's own `top`,
so the same world corner stores different values in different parents. Only the
four cell-centred `sampleHeight` values underneath it are shared. Putting the
height field itself on the GPU lets the shader derive what it needs and deletes
the per-voxel patch, the context byte and the 108 MB with it.

**Source the raster from the built height field, not from GPU-ported noise.**
Same corners and same bilinear means quality-neutral, so the terrain exactness
gates need no rebaseline, and there is no risk of CPU and GPU noise disagreeing
and cracking page seams. `microSurfaceData` can be dropped from memory once the
raster exists. Porting the noise remains the later option that buys genuinely
continuous cross-parent slopes, at the cost of a deliberate rebaseline.

### 6. Heightmap microvoxel

`microSurfaceData` is one word per occupied voxel: **108.1 MB, 80% of the brick
material footprint**, mostly zeros because interior voxels store nothing. The
fragment also does 6.6 M dependent descriptor loads per frame, 1:1 with eligible
parents. Replace the baked four-corner staircase patch with a direct read of the
page height raster, which is O(area) not O(voxels), and get continuous slopes
across parent boundaries instead of a staircase clamped inside one voxel.

Risks: ray-vs-heightfield is a root find, not a point sample; the raster needs
GPU-resident clipmap-consistent addressing; and it is a **quality change**, so
terrain exactness gates need a deliberate rebaseline.

### Why the G-buffer is not on this list

It looked like the best candidate: twelve full-resolution render targets at
**72 bytes per pixel**, 149 MB written per frame at 1080p, on a frame that is
provably per-pixel bound.

```text
instance     R32Uint    4      baseColor        RGBA16F  8      normal      RGBA16F  8
voxel        R32Uint    4      matProperties    RGBA16F  8      motion      RG16F    4
material     R32Uint    4      emissive         RGBA16F  8      depth       D32      4
materialUv   RG32Float  8      geometryNormal   RGBA16F  8      temporalId  R32Uint  4
                                                                            total   72 B/px
```

It costs **1.27 ms at 1080p, 2.3% of the frame** (0.61 ms/Mpx, and 0.74 ms/Mpx
at 720p - consistent). Packing it to ~44 B/px would recover at most ~0.6 ms and
would cost precision on four attachments plus an exactness rebaseline. Not
worth it. 149 MB/frame at ~1.3 ms implies roughly 115 GB/s effective, which is
ordinary for this hardware: **bandwidth is simply not the bottleneck at these
sizes.**

Measured with the shadow-target technique, because a plain duplicate changes
the output hash - see the protocol below.

## Compute raster: the 64-bit atomic path is blocked on MoltenVK

The Nanite-style design - traverse the hierarchy once, route by projected
footprint into a raster queue and a compute queue, and resolve the compute queue
with a single 64-bit `atomicMin` over packed `(depth | payload)` - is the right
shape for this renderer. The evidence supports it: the fragment performs
6,603,134 eligible parent hits and 2,656,470 exact traces per frame for only
~2,890,000 distinct occupied parents, so with 15x overdraw the same parent's
microgeometry is re-derived many times over.

**The single-atomicMin form is not available through this Vulkan path.**

```text
deviceName  = Apple M4
driverName  = MoltenVK 1.2.11
shaderInt64                = true
shaderBufferInt64Atomics   = false     <-- blocker
shaderSharedInt64Atomics   = false
VK_KHR_shader_atomic_int64 : not present
```

Verified end to end: `wgslc-naga --atomic-u64-min-max` compiles
`atomicMin` on `atomic<u64>` and emits `OpCapability Int64Atomics`, so the
shader toolchain is ready. But requesting `DeviceFeatures_Atomic64_MinMax` sets
`features12.shaderBufferInt64Atomics`, `vkCreateDevice` fails, and the sample
aborts before its first frame. Metal's own 64-bit atomic support is irrelevant
to the Vulkan/MoltenVK route because MoltenVK does not expose it to Vulkan.
Phase 17 of `terrain-voxel-triangle-cluster-plan.md` adds the separate escape
hatch: a native Metal compute-raster backend may bypass this Vulkan feature bit
only after it proves compatible native Metal device/queue/resource interop with
the MoltenVK-backed frame.

Three ways forward, in order of cost:

1. **32-bit packed atomicMin over a tiled candidate list.** R9 already specifies
   16x16 tiles with bounded per-tile candidate lists. A bounded list means the
   payload is a *slot index within the tile*, not a global ID, so
   `atomicMin(depth << 12 | slot)` fits in 32 bits with 20 bits of depth and 12
   bits of slot. Depth precision only has to order candidates within a tile -
   the exact value is recomputed in the deferred resolve - so 20 bits is ample.
   **This is the recommended path** and needs no new device features.
2. **Two-pass 32-bit.** `atomicMin` the depth, then re-run the candidates and
   have the one whose depth matches write the payload. Doubles candidate work
   and needs a deterministic tiebreak for exact ties.
3. **A native Metal compute-raster backend.** Phase 17 allows this before the
   full native Metal RHI if the renderer can acquire a compatible Metal adapter
   from the MoltenVK-backed path, synchronize with the active frame, and share
   or explicitly hand off the required buffers/textures. This path may use the
   single 64-bit atomicMin form directly in Metal. The shader home for that
   route is
   `assets/shaders/voxel/svraster/metal/sparse_voxel_terrain_compute_raster_atomic64.metal`;
   the C++ pass contract is
   `src/voxel/svraster/rendering/TerrainMetalComputeRasterPass.hpp`.
4. **The full native Metal RHI** (`docs/metal-rhi-plan.md`) would make the
   ownership model cleaner, but is a larger dependency than the raster test.

Do not start the Vulkan compute raster on the assumption that a 64-bit
atomicMin is available. Option 1 changes the tile kernel's payload packing, not
its structure. The native Metal route has its own capability probe and must not
inherit Vulkan/MoltenVK feature assumptions in either direction. Today the
engine can export the MoltenVK-backed Metal device, queue and textures through
the MetalFX bridge; the native compute-raster pass also needs buffer interop
before it can execute against terrain cluster/tile/visibility buffers. The
production terrain compute-raster route is also gated to the Terrain Virtual
Geometry G-buffer path; the brick tile raster remains a legacy/diagnostic
experiment unless explicitly opted into.

### H0 census: 32-bit packing is comfortably sufficient - LANDED

`--brick-tile-bin-diagnostic` registers a measurement-only census
(`SparseVoxelBrickTileBinPass`, three compute kernels plus a readback) that
projects every visible brick's tight occupied box and counts the candidate list
each 16x16 screen tile would receive. It registers no draw and writes no
attachment.

Default terrain package, 1920x1080, camera-walk:

```text
tile=16x16  grid=8160  binned=14559  tileRefs=141371  nonEmptyTiles=4800
avgCandidates=29.5  maxPerTile=413  maxTilesPerBrick=656  overflowTiles=0
nearPlane=0  offscreen=84
histogram (lowerBound:tiles)
  1:9  2:540  4:1943  8:1115  16:414  32:152  64:270  128:244  256:113
```

**The answer is yes, with room to spare.** The busiest tile in the frame needs
**413 candidates** against a 4096 slot capacity, and **no tile overflows**. Nine
bits of slot would already cover it, which would leave 23 bits of ordering
distance rather than 20. The `atomicMin(distance << slotBits | slot)` design is
viable on this platform with no new device features.

The rest of the census sizes the stages around it:

- **141,371 tile references** is the scatter buffer - about 565 KB at four
  bytes per entry. Small.
- **4,800 non-empty tiles of 8,160** (59%) is the tile-kernel dispatch.
- **29.5 candidates per tile on average**, with a tail to 256-511, is well
  inside what a tile kernel can chunk through threadgroup memory.
- **656 tiles for the largest single brick** is the per-brick footprint the
  count/scan/scatter has to tolerate; near-camera bricks dominate it.

H0 is therefore complete and its verdict is positive.

### H1 in progress: tile candidate lists built and self-checked

The census became a real binner. `SparseVoxelBrickTileBinPass` now runs five
kernels - Reset, Count, Reduce, **Scan**, **Scatter** - producing the per-tile
candidate lists the tile kernel will consume:

- **Scan** is a single-workgroup blocked exclusive prefix over the tile counts.
  One workgroup of 256 lanes walks the grid in chunks and does a serial scan of
  the 256 partials, which beats a multi-pass scan and its dispatch overhead at
  this width.
- **Scatter** walks the visible bricks a second time and claims a slot per
  touched tile with `atomicAdd` on that tile's cursor, so `tileOffsets` ends
  holding each tile's exclusive end and a tile's range is
  `[end - count, end)`. Order inside a tile is arbitrary by design: the tile
  kernel resolves nearest-wins by depth key, not by list order.

Self-checked rather than assumed. `scatterHighWater` records the largest slot
claimed plus one; a correct scan/scatter pair fills exactly
`[0, tileReferences)`, so the two must be equal and the log flags
`SCAN/SCATTER MISMATCH` when they are not:

```text
tileRefs=141371  scatterOverflow=0  candidateCapacity=1044480  highWater=141371
```

Capacity is 128 candidates per tile, about seven times the measured 17, and the
scatter reports overflow rather than dropping candidates - the fallback is
explicit rather than a silent hole.

### H1 tile kernel: coverage parity exact, 99.2% on distance

`SparseVoxelBrickTileRasterPass` runs the tile kernel and diffs it against the
hardware raster's resolved `coverage` and `linearDepth`. It writes only its own
comparison buffers and is never presented.

**The packed 32-bit atomic turned out to be unnecessary.** One 16x16 workgroup
owns a tile and one thread owns a pixel, so each thread keeps its nearest hit in
a register and the inner loop has no atomic at all. The depth|payload key the H0
census sized is only needed by a primitive-parallel variant; pixel ownership
makes it moot. The census is still what proved the candidate lists are bounded,
which the chunked shared-memory fetch depends on.

First result, 1920x1080, terrain micro materials disabled (the kernel does not
yet reproduce exact micro carving):

```text
compared=2073600  bothCovered=1186826
hardwareOnly=0    computeOnly=0
distanceMatches=1177305   maxDelta=152.83 m
```

- **Coverage parity is exact.** Not one pixel of the 2.07 M disagrees about
  whether it is covered. Both paths bin, cull and intersect the same set.
- **Distance agrees on 99.20%** of covered pixels to within one millimetre.

The remaining **9,521 pixels (0.80%)** disagree, and a 152 m maximum delta says
they are picking a *different surface*, not accumulating float error.

**Instrumented rather than guessed further.** The compare kernel now records the
direction of each disagreement and the identity of the worst pixel:

```text
computeNearer=28   hardwareNearer=9492
worstPixel=977578  worstHardware=169.5956 m  worstCompute=322.4230 m
```

**99.7% of mismatches are the compute path missing a nearer surface**, not
mis-ordering a tie. Pixel 977578 is (298, 509); the hardware finds geometry at
169.6 m where the compute kernel's nearest is 322.4 m - a whole brick away.

**Six hypotheses tested, all wrong.** Recorded because each looked convincing,
and because the pattern across them is the real finding:

1. **The `brickOccupiedCellRange` maximum bound.** Not a bug - the helper
   already returns an exclusive maximum (`+ 1u` on each axis), so the half-open
   test in the kernel is correct.
2. **DDA start-cell precision.** At a kilometre f32 carries about 6 cm against a
   0.195 m voxel, so flooring a world-space entry position can land a cell late
   and skip the true first hit. Biasing the entry backwards - always safe,
   because the per-cell slab test re-derives the exact distance - made parity
   *worse* by 551 pixels. Reverted.
3. **Phase one not being binned.** Real and worth fixing on principle: the
   hardware draws both raster phases into one depth buffer, so a compute path
   binning only phase zero is missing whatever the second phase recovered. The
   binner now walks both lists as one logical range (+25 bricks, +194 tile
   references). It moved parity by **two pixels**. Kept, because it is correct,
   but it is not the cause.

4. **`sign(0.0)` in the DDA step.** Real defect: the step direction came from
   the raw ray direction, and `sign(0.0)` is zero, so an axis-aligned ray never
   advances on that axis, stalls, and reports a miss. Fixed by deriving the step
   from the same safe direction the reciprocal uses. Kept - it is a genuine bug
   and axis-aligned view rays are common - but it moved nothing.
5. **Grass writing into the shared buffers.** The experimental grass pass took
   `coverage` and `linearDepth`, so its blades could have been nearer geometry
   the brick-only kernel could not reproduce. Disabling it changed nothing;
   that unused experiment has since been retired from the sample and build.
6. **Comparing against the wrong raster phase.** Real defect: the raster chain
   is registered twice, an occlusion prepass and the final phase, both resolving
   into the same `coverage`/`linearDepth`. The instrumentation was attached to
   the first registration, so it diffed the compute result against the
   *prepass*. Both halves of H1 are now pinned to `svr.sample.Final`. Kept, and
   it moved nothing.

**`hardwareNearer` has stayed at exactly 9,492 across all six**, with the same
worst pixel and the same two distances to four decimal places. Six independent
changes not perturbing it by a single pixel means none of them touches the
mechanism.

### H3 merge: hardware-first, landed behind `--brick-tile-merge`

The compute path now carries its winning brick and cell through to a merge
kernel that rebuilds the same `ResolvedVoxelSurface` the hardware resolver
publishes - material addressed by the popcount of the occupancy bits before the
cell, plus the identical `brickSurfaceIdentity` hash, so a merged pixel keeps
the temporal identity it would otherwise have had. A merged pixel that carried
only a depth would shade as garbage; this is why the winning cell has to be
plumbed rather than just the distance.

**The merge is hardware-first and that is load-bearing, not a preference.** The
hardware result is the starting answer and the compute path may only *improve*
it - it wins a pixel only when it has a strictly nearer hit and can supply the
whole record. The traversal is known to miss nearer surfaces on 9,492 pixels; a
merge seeded by hardware is immune to a miss, because a missing compute hit
simply leaves the hardware answer standing. Seeded the other way round, every
one of those misses would punch a hole.

Verified: on the synthetic fixture the merge is **byte-identical** to the
baseline, meaning it wins zero pixels there and cannot regress what already
works. On terrain it can only touch the 22-40 pixels where compute finds
something strictly nearer.

It defaults **off** and is not itself a performance win - it becomes one only
once routing sends work exclusively to one path instead of running both.

### H5 routing: `--brick-tile-route PIXELS`

A brick whose projected occupied-box footprint is at or below the threshold is
owned by the compute raster; everything else stays on hardware. The hardware
vertex stage declines a routed brick by collapsing its proxy to a degenerate
position, which is how a vertex stage refuses an instance without an indirect
compaction, and the binner bins exactly the complement.

**Both sides call one shared predicate**, `sparseVoxelBrickRoutedToCompute`, on
one shared footprint measure mirrored into both uniform blocks. Disagreement
here is not cosmetic: both declining a brick leaves a hole, both drawing it
doubles the work and reintroduces the overdraw routing exists to remove.

The threshold is not free to choose. The compute walk does not carve
microgeometry, so a routed brick has to be one whose microvoxels the exact
handoff has already retired - which is what makes "small footprint to compute"
principled rather than arbitrary.

Verified end to end on the synthetic fixture:

```text
--brick-tile-route 0          coveredPixels=19600   (bit-exact with baseline)
--brick-tile-route 1000000    coveredPixels=19600   (every brick on compute)
```

Routing *everything* to compute - hardware drawing nothing at all - reproduces
the hardware coverage exactly. That is the whole hybrid path exercised in one
run: decline, bin, tile-raster, merge, publish.

**One bug this caught, worth recording.** The merge originally bound the shared
`coverage` and `linearDepth` read-only, so it published a resolved surface while
coverage stayed zero and every downstream consumer treated the pixel as empty.
Routing everything to compute rendered a completely blank image, which is what
exposed it - a partial routing threshold would have hidden it as a subtle hole.
**Route everything to one path as a test; a partial split hides exactly this
class of defect.**

Terrain behaves the same. Coverage is preserved exactly however much of the
frame is routed:

| terrain, 1280x720 | bricks binned | coveredPixels |
| --- | ---: | ---: |
| `--brick-tile-route 0` | 14,564 | 558,093 |
| `--brick-tile-route 2000` | 15,357 | 558,093 |
| `--brick-tile-route 1000000` | 16,716 | 558,093 |

**No holes at any routing fraction, including every brick on compute.**

(An earlier revision of this note claimed terrain routing collapsed to zero
coverage. That was a measurement error: `--skip-output-signature` disables the
coverage readback, so the zero was the counter being absent, not the geometry.
It reported zero at route 0 as well, which is what gave it away.)

### H4: the routed record is complete

The merge now rebuilds the whole record the hardware fragment would publish:

- **Hit face.** The slab test carries out the axis whose near plane the ray
  crossed last, the traversal returns it with the winning cell, and the merge
  packs it exactly as the fragment does - axis in the low bits, bit 2 set when
  the ray entered through the positive face. This is what the G-buffer turns
  into a geometric normal, so without it every routed pixel shaded flat.
- **Bottom albedo**, including the secondary-palette indirection when
  `SECONDARY_ENABLED` is set, and the per-voxel terrain context in the high
  byte.
- **The transient face/hit nibble** republished into the metadata word, with the
  rest of that word carried through untouched.
- `microPayload` and `materialSampleAddress` stay zero, which is **correct**:
  the hardware leaves both zero for a non-micro hit, and a routed brick is by
  construction one whose microgeometry the handoff has already retired.

Verified by routing the entire frame to compute:

```text
route<=0        depthHash=8e7f...  geometryNormalHash=3721f365f2f0aa83
route<=1000000  depthHash=8e7f...  geometryNormalHash=3721f365f2f0aa83
```

**The geometry normals are bit-identical with every brick rasterized in
compute**, which is the field the face reconstruction feeds and the one that was
visibly wrong before. Terrain coverage stays at 558,093 at full routing.

`resolvedSurfaceHash` still differs, and that is an H1 question rather than an
H4 one: the record now carries every field, but `surfaceT` is computed by a
different-though-equivalent DDA, so it differs in the low bits and any hash over
it changes.

### Why bit-exact `surfaceT` is a rewrite, not a fix

The fragment publishes `max(travelled, nearDistance)`, where `travelled` is the
**accumulated** DDA parameter: it starts at the brick entry and is advanced by
successive additions through a coarse 4-cubed walk and then a fine 8-cubed one.
The compute kernel instead re-derives the distance with a single slab
intersection against the winning cell. The two are algebraically identical and
numerically are not - accumulated addition and one division disagree in the low
bits, and any hash over `surfaceT` sees it.

Matching bit-for-bit therefore means reproducing the hardware's *accumulation
order*: the same two-level walk, the same additions, in the same sequence.
That is a rewrite of the traversal rather than a correction to it, and it trades
away the freedom to use a cheaper walk - which was part of the point of moving
to compute. It is worth doing only if some consumer genuinely needs bit-exact
depth from the routed path; the temporal identity, which is what history
validation actually keys on, already matches.

Two parts of that were cheap and are done. The near-plane clamp the fragment
applies was genuinely missing. And the traversal now reports the **accumulated**
entry parameter of the cell it stops in, carried through the walk, rather than
re-deriving it with a fresh slab intersection - which is both closer to the
published contract and one slab test per cell cheaper.

Current parity, terrain, 1920x1080:

```text
bothCovered=1186826  hardwareOnly=0  computeOnly=0
distanceMatches=1186786  (99.9966%)   maxDelta=0.331 m
```

**40 pixels of 1.19 M.** Coverage exact, geometry normals bit-identical under
full routing. The residual sits at cell and box boundaries: the worst reports
`spanning=0`, meaning the hardware's hit falls fractionally outside every
candidate box, which is an edge case in the box test rather than a traversal
defect. Closing the last 40 requires the full two-level coarse-then-fine
accumulation described above.

### H1 timing: the hybrid path is performance-neutral without micro - MEASURED

The unmeasured premise is now measured. Routing-threshold sweep, 1920x1080,
camera-walk, min-of-3 with a 20 s cooldown, `--disable-terrain-micro-materials`
on **every** arm - without that the routed arm would "win" merely by skipping
micro work the hardware arm still does, which measures nothing:

| arm | min-of-3 | spread | vs baseline | FPS |
| --- | ---: | ---: | ---: | ---: |
| baseline (hardware only, no tile chain) | 43.45 ms | 0.69 | - | 23.0 |
| `--brick-tile-route 0` (both paths full) | 46.47 ms | 1.32 | **+3.02 ms** | 21.5 |
| `--brick-tile-route 2000` (split) | 44.38 ms | 0.33 | +0.93 ms | 22.5 |
| `--brick-tile-route 1000000` (compute owns all) | 43.42 ms | 0.26 | **-0.03 ms** | 23.0 |

**The compute raster costs almost exactly what the hardware raster costs.** At
route 0 both paths do the whole frame and the compute chain adds 3.02 ms; at
full routing the hardware declines every brick and gives back the same 3.02 ms,
landing 0.03 ms from baseline against run spreads of 0.26-0.69 ms. The split arm
falls between them, so the model is linear in how much work each path owns.

Verified not to be a blank-frame artifact - the trap that already caused one
wrong conclusion in this document. With the signature readback **enabled** at
1920x1080, `coveredPixels=1254989` at route 0 and `coveredPixels=1254989` at
route 1000000. The routed arm renders the whole image; the timing is real.

So one visibility record per pixel instead of 15.2x overdraw, which was R9's
entire premise, buys **nothing on its own**. Early-Z already rejects the
overdraw cheaply, and the per-pixel candidate loop (29.5 candidates average)
costs about what the rejected fragments did.

### H1 micro sizing: 8.1x redundancy is the one place routing can win

If the compute raster only matches hardware on plain traversal, the win has to
come from work that overdraw makes *redundant*. Exact microgeometry is that
work, and section 0b already established the shape of its cost: it is **per
entry**, not per column - the descriptor load is 1:1 with eligible parents.

So the discriminating question is how many times each path would have to enter
the micro path. The tile kernel now counts that directly, gated on the same
`debugFlags.z` switch that enables the fragment's counters, so one run produces
both halves. 1920x1080, camera-walk, micro materials **enabled**:

```text
fragment  eligible=15718061   exactTraces=4615690   fallbackCandidates=11102371
compute   occupiedCellHits=1940577   microEligibleHits=1940577
```

| | eligible parent hits | per covered pixel |
| --- | ---: | ---: |
| hardware fragment | 15,718,061 | 13.2 |
| compute tile walk | **1,940,577** | **1.6** |

**8.1x.** The fragment's 13.2 entries per covered pixel tracks the 15.2x
overdraw; the compute walk enters 1.6 times per covered pixel because it visits
each candidate at most once. Carving-induced continuation does not change this
materially - the fragment reports `carvedParents=191822`, 1.2% of eligible.

Because the cost is per entry, that ratio converts to time. Micro is 4.55 ms of
raster+cull at the shipping `--micro-handoff-scale 2.0` (14.57 ms against the
10.02 ms micro-off floor), so an 8.1x cut in entries is worth roughly **4 ms of
a ~48 ms micro-enabled frame** - about what the micro handoff itself recovered.

**This reverses the reading of the neutral sweep.** Routing is not a win for
traversal and never will be, but it is the only mechanism that removes the
micro path's redundancy, and that redundancy is measured at 8.1x. Exact micro in
compute is therefore justified - on this number, not on R9's original premise.

### H1 exact micro in compute: BUILT, correct, and measurably slower

The port landed. `sparse_voxel_brick_tile_raster.wgsl` now carves exact
microgeometry through the same shared, binding-free includes the fragment uses -
the four brick-side helpers moved into `sparse_voxel_brick_micro_common.wgsl` so
both paths evaluate **one** floating-point expression tree, which is what the
existing comment on `brickTraceExactMicroHeightfield` always said the point was.

**Parity, terrain, 1920x1080, micro materials ENABLED** (previously impossible):

| | before the port | after |
| --- | ---: | ---: |
| distanceMatches | 1,011,694 | **1,186,781** of 1,186,820 |
| computeNearer | 175,110 | **23** |
| maxDelta | 292.68 m | **0.42 m** |
| microPayload mismatch | - | **94** |
| materialSampleAddress mismatch | - | **95** |
| surfaceIdentity mismatch | - | **95** |

Coverage stays exact (`hardwareOnly=0 computeOnly=0`), and the residual ~95
pixels are the same cell/box boundary class as before (`spanning=0`).

**Two defects the record comparison caught**, neither visible in distance parity:

1. The payload's carve face is the *descriptor's* carved face,
   `select(firstTrailingBit(faceMask), 3u, terrainStaircaseSurface)`, not the
   face the ray entered through. `brickMicroPayload` permutes the microcell into
   that face's frame, so taking `micro.face` rotated every payload into the
   wrong axis **while leaving the unpermuted cell - and therefore the surface
   identity - looking correct**. 950,109 pixels wrong; identity said 96.
2. `heightfield` must not be `|| solidStaircase`. A solid staircase interior is
   traced through the heightfield entry point but is not published as a
   heightfield payload, and the flag also selects the cell permutation.

Distance parity was 99.997% while 80% of payloads were wrong. **A traversal that
stops in the right place is not the same as a record that publishes correctly**;
the two need separate instruments.

**The payoff measurement is negative.** 1920x1080, min-of-3, 20 s cooldown,
micro materials on every arm:

| arm | min-of-3 | spread | vs baseline | FPS |
| --- | ---: | ---: | ---: | ---: |
| baseline (hardware only, micro ON) | 51.29 ms | 0.51 | - | 19.5 |
| `--brick-tile-route 1000000` | 54.06 ms | 0.14 | **+2.77 ms** | 18.5 |
| `--brick-tile-route 0` (both full) | 60.94 ms | 0.32 | +9.65 ms | 16.4 |

Micro took the compute chain from 3.02 ms to 9.65 ms, and full routing is
**2.77 ms slower** than the hardware raster it replaces.

**Why the 8.1x did not convert - measured, not guessed.** Counting exact traces
rather than entries:

| | fragment | compute | ratio |
| --- | ---: | ---: | ---: |
| eligible entries | 15,718,061 | 2,015,393 | 7.8x |
| **exact traces** | 4,615,690 | **1,425,952** | **3.2x** |
| exact share of entries | **29%** | **71%** | - |

The compute walk only evaluates the **frontmost** occupied cell per candidate -
the near surfaces, where projected microvoxel size is largest and the handoff
selects exact carving. The fragment's extra 13.7 M entries are overwhelmingly
distant or occluded parents taking the *cheap* fallback. So overdraw's
redundancy is concentrated in the cheap work, and the compute path removes
almost none of the expensive work while losing early-Z and taking the whole
divergent micro stack into one kernel.

**The lesson, and it invalidates the earlier sizing above.** Entry count was
used as a proxy for cost on the strength of "the cost is per entry" from section
0b. That claim was about why *coarsening microvoxels* does not help - the fixed
per-entry cost dominates *column stepping* - and it does not license treating
every entry as equally expensive. The right denominator was exact traces, and it
was one counter away the whole time.

**This does not weaken the heightmap case in 6a - it sharpens it.** The cost is
in the ~4.6 M exact traces, and a filtered texture fetch replaces the expensive
part of exactly those, on the **shipping fragment path**, with no compute raster,
no routing and no parity surface at all.

### H5's gate cannot pay: the chain has a 1.5 ms fixed entry price

The extremes were measured first, which left the configuration the gate was
*designed* for untested: small-footprint bricks to compute, large ones to
hardware. Small footprint means distant, which means the handoff has already
retired exact micro, so this split should hand compute the cheap work and keep
the expensive near-field exact traces on hardware. 1920x1080, micro ON,
min-of-3:

| arm | min-of-3 | spread | vs baseline | bricks binned | nonEmptyTiles |
| --- | ---: | ---: | ---: | ---: | ---: |
| baseline | 51.34 ms | 0.11 | - | - | - |
| `--brick-tile-route 64` | 52.84 ms | 0.79 | **+1.50 ms** | **28** | 37 |
| `--brick-tile-route 256` | 52.81 ms | 0.13 | +1.47 ms | 114 | 156 |
| `--brick-tile-route 1024` | 52.97 ms | 0.82 | +1.63 ms | 1,585 | 515 |
| `--brick-tile-route 1000000` | 54.06 ms | 0.14 | +2.77 ms | 16,675 | 4,800 |

**Routing 28 bricks of 16,675 - 0.17% of the frame - costs 1.50 ms.** A 16x
change in threshold moves the frame 0.16 ms, inside the spread. The curve is
flat because the cost is not the routed work; it is the price of admission.

Where the fixed 1.5 ms goes, all of it independent of how much is routed:

- The tile kernel dispatches **one workgroup per tile over the whole 8,160-tile
  grid**, so at route 64 it launches 8,160 workgroups to service 37 non-empty
  tiles - 220x more than needed. Empty tiles exit at their first bounds test,
  which is cheap per workgroup but not free at that multiple.
- The merge dispatches over **all 2,073,600 pixels** (32,400 workgroups) and
  early-outs on `computeCoverage == 0`.
- The binner's Count and Scatter passes walk **all visible bricks** to find the
  28 routed ones.

All three are compactable - an indirect dispatch over non-empty tiles and a
compacted merge would remove most of it. **But doing so only reaches the
starting line**, because the routed work itself is neutral at best: traversal
measured -0.03 ms and micro measured +2.77 ms. There is no threshold, and no
amount of dispatch compaction, that turns those two numbers into a win.

That is the argument for leaving H5 off, and it is now measured at both ends of
the threshold range rather than assumed from the extremes.

### H1 bit-exact `surfaceT`: closed, no consumer needs it

Settled by looking at the consumers rather than by doing the rewrite. Every
reader of `surfaceT` is one of two kinds:

- **World-position reconstruction** - `sparse_voxel_depth.wgsl`,
  `sparse_voxel_material_gbuffer.wgsl`, `sparse_voxel_terrain_normal.wgsl` all
  multiply it back along the camera ray. A low-bit difference is geometrically
  meaningless there.
- **Tolerance-based depth comparison** - the same two shaders compare neighbour
  distances against an explicit `depthLimit` derived from
  `pixelFootprint = |surfaceT| / focal`. These are slack tests by construction.

Nothing hashes or equality-tests it. In particular **`brickSurfaceIdentity`
takes no `surfaceT` input at all** - it hashes the brick's world minimum, voxel
size and cell index - and that hash is what temporal reuse validates history
against. The identity already matches bit-for-bit.

The only thing that changes is `resolvedSurfaceHash`, which is an exact-output
test signature, not a renderer consumer. Reproducing the fragment's two-level
coarse-then-fine accumulation order would trade away the freedom to use a
cheaper walk to move a number nothing reads. **Do not do it.**

## Remaining work, precisely scoped

**H1 is complete, and its verdict is that the hybrid path should stay off.**
Bit-exact `surfaceT` is closed as unnecessary; exact micro in compute is built,
reaches 99.992% record parity, and is **2.77 ms slower** than the hardware
raster. Both halves of the compute path are now measured: traversal is neutral,
micro is a regression. Nothing further should be invested in routing until some
*other* change alters that arithmetic.

What remains open in H1 is only the ~95-pixel boundary residual
(`spanning=0`, maxDelta 0.42 m), which is cosmetic at this point because the
path it affects is not enabled.

The port is smaller than the fragment's inline code suggests, because the exact
micro stack is already binding-free pure code:
`sparse_voxel_micro_geometry.wgsl`, `voxel_geometry_ir.wgsl` and
`voxel_geometry_field_cache.wgsl` declare **no bindings at all**, so compute can
call `traceSparseVoxelExactMicroOneFace` / `...Volume` directly. Needed:
`microMaterials` and `brickPaletteDescriptorIds` bound on the tile pass
(`brickMicroSurfaceData` already is, at binding 17); the four helpers that
currently live in `sparse_voxel_brick_draw.wgsl` -
`brickSafeInverseDirection`, `SparseVoxelBrickParentInterval`,
`brickExactMicroParentInterval`, `brickTraceExactMicroHeightfield` - moved into
the shared include so both paths keep **one floating-point expression tree**
(the existing comment on the third of these says that is exactly why it was
factored out); the eligibility/handoff decision block replicated; and
`BrickTraceResult` extended so the merge can publish `microPayload` and
`materialSampleAddress` instead of leaving them zero.

Note the traversal contract change: a carved parent must **continue** the walk
rather than return a hit, which is what `candidateAccepted = false` does in the
fragment.

**H6, fused visibility and material resolve. Deprioritised by the H1 timing.**
The whole compute chain it would optimise is 3.02 ms, and the merge is one
kernel inside that. R10 already measured the analogous hardware-path fusion at
0.467 ms marginal. There is no version of this worth building before the micro
port, and after the micro port it should be re-sized against whatever the chain
then costs.

**R7, anisotropic bricks.** Independent of the compute raster and should not be
entangled with it. First landable unit per the raster plan: the six 4-bit
occupied-bound fields plus three 2-bit log2 extents in the existing 32-bit word,
the CPU/WGSL helpers for flatten, bounds and coarse dimensions, and CPU/GPU
parity tests - landed together, because a partially migrated encoding is read by
old shaders as the legacy 3-bit layout.

**Premise, now measured.** The hybrid path has been timed: routing is
**neutral** on traversal alone (-0.03 ms at full routing, spreads 0.26-0.69) and
the compute chain costs 3.02 ms to replace 3.02 ms of hardware raster. H6 is
deprioritised on that number. The one remaining prize is the 8.1x micro entry
redundancy. Both results are written up above.

### The instrument found it in one run

Rather than a seventh hypothesis, the capture kernel was made to answer the
discriminating question directly: for the worst pixel, how many of its tile's
candidates have an occupied box the ray enters at or before the hardware's
answer? Zero would indict the binner; nonzero indicts the traversal.

```text
worstNode=74784  tileCandidates=86  spanning=1
```

**One.** The winning brick was in the candidate list all along - the binner and
the culling agreement were never at fault - and `traceBrick` rejected a brick
whose box the ray demonstrably enters at the right distance.

That narrowed it to the loop, where the defect is visible by inspection. The
walk was ordered `test cell -> step -> check box.exit`. After stepping into the
final cell inside the box, `min(sideT) > box.exit` fired and returned a miss
**without ever testing the cell just entered** - silently dropping the last cell
along every ray whose hit was in it. The exit test is now gone entirely; the
cell-range check at the top of the loop and the iteration cap are the
termination conditions, and a few extra empty-cell tests are the price.

| | before | after |
| --- | ---: | ---: |
| distanceMatches | 1,177,305 | **1,186,777** of 1,186,826 |
| hardwareNearer | 9,492 | **16** |
| computeNearer | 29 | 33 |
| maxDelta | 152.83 m | **0.331 m** |

**99.9959% distance parity with coverage still exact.** The remaining 49 pixels
disagree by at most 0.33 m - under two voxels at this 0.195 m scale - and the
worst of them now reports `spanning=0`, meaning the hardware's hit sits just
outside every candidate box. That is the signature of a boundary case at a cell
or box edge, a different and much smaller problem than the one just fixed.

The lesson worth keeping: six hypotheses cost far more than the one instrument
that discriminated between the two possible causes. When a defect is stable and
reproducible, measure which half of the system owns it before theorising about
either half.

That 9,521 is now 40-51 pixels of 1.19 M, and bit-exact `surfaceT` has since
been closed as unnecessary. What is left of H1 is the micro path. Coverage
parity being exact on the first run remains a strong signal that the binning,
culling agreement and candidate lists are right - the disagreement is confined
to which of several candidates wins a pixel.

Note that parity holds **only** with `--disable-terrain-micro-materials`. With
micro materials on, the compute path disagrees exactly as expected for a walk
that does not carve: `distanceMatches=1011694 computeNearer=175110
maxDelta=292.7m`. Those 175 k pixels are the micro path's absence, not a
traversal defect, and they are the parity number the micro port has to close.

## Shipped defaults, 2026-08-16

```text
previous defaults                        38.532 ms
new defaults (no micro, no background)   33.554 ms   -4.978 ms at 1080p
probe flicker                            0.0353 -> 0.0241 mean, 0.0104 -> 0.0033 severe
```

**Microgeometry retired and the planet background removed**, worth ~5 ms and
*better* stability. `--brick-exact-micro-all` and `--enable-planet-background`
restore the old behaviour; the R1/R2 micro equivalence suites now request the
former explicitly, because with micro retired they were comparing two identical
no-micro images.

**The 42 km horizon is deliberately NOT default, despite being 3.4 ms faster.**
Isolated at a rugged camera in the walk fixture, it is the thing that
destabilizes the lighting:

```text
new defaults + 42 km horizon        flicker 0.0647 mean  0.0459 severe
  restore 25 km                             0.0242       0.0033   <- the cause
  restore the background mesh               0.0649       0.0460   (no effect)
  restore microgeometry                     0.1092       0.0834   (worse)
```

More distant voxel terrain in view means more LOD churn, and probes lose history
to it. This is very likely the same effect as the reported light blink when
terrain LOD updates, and it should be fixed before the horizon ships.

Probe tile likewise stays at 14: coarsening to 24 measured 7.26 ms cheaper and
indistinguishable on smooth dunes, and doubled flicker at this camera.

**Method note.** Every performance figure in the sections below was measured at
one smooth-dune camera position. The stability figures above come from a rugged
position in the same walk and contradict some of them. Sample several camera
positions before promoting any of it.

## Terrain to the horizon: 42 km on the voxel raster - MEASURED

**Result, both arms interleaved in one session, 3 repeats, medians:**

```text
shipping default, 25 km            38.300 ms   spread 0.273
optimized, 42 km horizon           29.405 ms   spread 0.381
                                   -8.895 ms   23.2% faster, 68% more distance
```

The optimized arm is `--camera-far 42000 --terrain-render-distance-km 42
--disable-planet-background --brick-exact-micro-max-voxel 0.01 --probe-tile 24`,
with the two riser-artifact fixes in the shipping path. Four independent changes
contribute: exact micro retired by page voxel size (-6.9 ms of raster+cull), the
probe tile coarsened to 24 (-7.26 ms), the planet background removed (free), and
the grazing-aware macro normal (quality, ~neutral).

The voxel raster reaches the horizon, and the cost is not where the fragment
counters point. Everything below is depth-only isolation (cull + brick raster +
depth), camera-walk, 3 repeats, medians, spreads quoted.

```text
                                    depth-only   whole frame   fragments   overdraw
25 km (default)                       14.250        39.418     18,096,470   15.163x
42 km                                 17.539        42.926     14,758,640   11.402x
42 km + exact micro max voxel 0.4 m   15.345        41.248     14,758,640   11.402x
```

**Net cost of the horizon: +1.83 ms** (whole frame, spread 0.83), down from
+3.51 ms, for 42 km of voxel terrain against 25 km. Raster+cull is 41% of the
frame at 42 km and remains where any further work belongs. The gate has not
been measured at 25 km; it should help there too, since the same coarse rings
exist, but that is an expectation and not a measurement.

**The horizon rasterizes 18% fewer fragments than 25 km and costs 3.29 ms
more.** Extending the render distance builds a deeper clipmap whose coarser
rings serve the near field too, so overdraw *falls* from 15.163x to 11.402x and
peak depth complexity from 252 to 89. The added cost is entirely per-fragment.

**What moved: exact micro traces, 4.62 M -> 7.02 M (+52%), while eligible
entries fell 15.72 M -> 13.29 M.** The eligible-to-trace rate went 29% -> 53%,
which is the signature of the handoff accepting more, not of more geometry.

The mechanism is algebraic, not a guess:

```text
projectedMicroPixels = (voxelSize / subdivision) * focal / (fineT * scale)
```

`voxelSize` is the *page's*, so a 400 m-voxel ring at 40 km projects larger
microvoxels than a 0.195 m ring at 40 m and stays above the handoff threshold
out to the horizon. Those rings are subdividing voxels larger than their own
source data - the detail is invented, and the handoff cannot see that.

**Fix: retire exact micro by page voxel size** (`--brick-exact-micro-max-voxel`,
`microTuning[2]`), in the same separately compiled variant the distance cutoff
uses, so the shipping fast path keeps neither comparison.

```text
42 km depth-only baseline              17.539
  distance cutoff 100 m                15.611   -1.928
  max voxel 1.6 m (rings 0-3)              -    traces -26%
  max voxel 0.8 m (rings 0-2)          15.680   -1.859
  max voxel 0.4 m (rings 0-1)          15.345   -2.194   <- best
  max voxel 0.4 + distance 300         15.323   -2.216   (combining adds nothing)
```

Geometry-neutral: fragments and covered pixels are byte-identical across every
max-voxel value, so the gate changes only the shading path. At 0.4 m the trace
count (4.45 M) drops *below* the 25 km baseline's 4.62 M - the horizon's micro
inflation is fully removed - and the two retirements are substitutes rather than
complements, so only one is worth shipping.

### Exact micro is 39% of raster+cull, and the cost is tracing, not the handoff

With the horizon config (42 km, background off), depth-only isolation:

```text
42 km, background off                          17.569
  hoisted max voxel 0.8 (rings 0-2)            15.781   -1.788
  hoisted max voxel 0.4 (rings 0-1)            15.321   -2.248
  micro off everywhere (shader gate)           10.684   -6.885
  micro materials off (voxelization changed)   10.944   -6.625
```

**The shader gate alone reaches the ceiling** - there is no need to change
voxelization, and geometry is untouched (fragments and covered pixels are
byte-identical at every setting).

**The per-entry ALU is not the cost.** The gate was first written at the bottom
of the micro block, skipping only the trace, then hoisted above the descriptor
load, the descriptor validation, the parent key, the handoff weight and the
selector hash. Eligible entries fell **13.29 M -> 6.93 M** and the frame moved
**0.02 ms** (15.345 -> 15.321). This contradicts the earlier decomposition
recorded in section 0d; that conclusion should be treated as retired.

What the cost tracks is **exact traces, near-linearly**: 7.02 M traces cost
6.9 ms (~0.98 ns each), and removing 37% of them returned 36% of the time.
Restricting to ring 0 rather than rings 0-1 buys only a further 0.16 ms, so the
traces are concentrated in the nearest ~47 m, where projected microvoxel size is
largest.

Whole frame, against the 25 km shipping default at 39.418 ms:

```text
42 km, no gate                          42.926   +3.508
42 km + max voxel 0.4 (~94 m of micro)  40.877   +1.459
42 km + max voxel 0.2 (~47 m of micro)  40.718   +1.300
42 km, micro off everywhere             37.412   -2.006   <- 68% more distance, faster
```

**Quality trade, stated plainly:** terrain is 100% staircase-profile, so exact
micro carving is what turns the blocky height field into carved slopes. With it
off, near-field terrain is 0.195 m voxel cubes - roughly 36 px across at 5 m -
and the staircase is plainly visible. This is a deliberate look change, not a
free win, and it is why the flag is not a default.

**Disabling the planet background is free, not a win:** +0.030 ms. It is an
architecture decision (voxels own the horizon), not a performance one.

### The black risers at distance: a grazing-angle silhouette test, not micro

The artifact - dark dashes along terrain contours, black speckle toward the
horizon - is measurable in the geometry-normal buffer. Fraction of terrain
pixels whose normal is *side-facing* (a staircase riser rather than the ground):

```text
band                 before   after
mid-distance y480     30.7%    3.6%
mid-distance y560     21.6%    5.3%
near field  y800       3.6%    4.4%   (unchanged - real geometry)
```

Up to a third of the horizon band was shading as unlit vertical faces.

`sparseVoxelTerrainMacroNormal` already exists to prevent exactly this, and two
things stopped it working:

1. **It was gated on `EXACT_MICRO_MATERIAL`.** Removing that gate was tried and
   **measured no change at all** - side-facing normals in the horizon band
   stayed at 26.2% - because the binding constraint was the discontinuity bound
   below. It has since been **reverted**: leaving it out let the reconstruction
   replace the true face normal on near-field faces that had never used it, and
   at a lit/shadowed boundary a reconstructed normal leans toward the lit
   orientation and draws a bright outline along the edge. Keep the change that
   measured a win; drop the one that did not.
2. **Its discontinuity bound was not grazing-aware.** `pixelFootprint * 24`
   assumes near-normal incidence; on ground seen at a grazing angle - the whole
   horizon band - distance changes by far more than a pixel footprint between
   neighbours, so every neighbour was rejected as a silhouette and the
   reconstruction fell back to the riser normal. Dividing the bound by
   `max(|dot(terrainUp, viewDirection)|, 0.02)` fixes it, and the clamp keeps a
   true silhouette rejectable.

Only the second change moved the number; the first is what lets micro be retired
without reintroducing the artifact. Both are in the shipping path.

```text
whole frame, 42 km, background off
  before this work                   42.926
  + grazing fix, micro on            41.109   vs 25 km default  +1.691
  + grazing fix, micro off           36.468   vs 25 km default  -2.950
```

**42 km of voxel terrain, artifacts fixed, is 2.95 ms faster than the shipping
25 km configuration.** Micro costs 4.64 ms whole-frame in this configuration.
With it off the horizon is clean and the near field shows its 0.195 m steps as
faint dashed lines - genuine geometry, not the black blocks the artifact
produced.

### 10 cm voxels instead of microgeometry - MEASURED, and it works

Replacing sub-voxel microgeometry with *smaller real voxels* was proposed and
measured in the 42 km configuration:

```text
0.195 m voxels, micro on (shipping look)   33.459 ms
0.195 m voxels, micro off                  28.934 ms   -4.53
0.10 m voxels,  micro off                  29.320 ms   -4.14   <- finer geometry
```

**Ten centimetre voxels with no microgeometry cost 0.39 ms more than 0.195 m
with no microgeometry, and are 4.14 ms cheaper than the shipping look** - while
representing the detail as actual geometry rather than an encoded sub-voxel
pattern that needs edge detection, slope classification and 117 MB of per-voxel
surface data.

The reason it is nearly free is worth recording, because the earlier table's
"-terrain-voxel-size 0.1 = +2.42 ms" suggested otherwise: **leaf count barely
moves** (13.69 M -> 13.44 M) and brick count *falls* (275,001 -> 245,517). The
surface shell is a fixed number of voxels deep, so halving the voxel size keeps
a similar leaf count over a proportionally thinner shell rather than multiplying
it by eight.

### Overdraw and HZB culling are closed - four experiments, all neutral

Worth restating together, because 11-15x overdraw keeps attracting proposals:

| experiment | expected if overdraw were the cost | measured |
| --- | --- | ---: |
| compute raster, 1 record/pixel instead of 15x | large win | **-0.03 ms** |
| front-to-back ordering, so early-Z can reject | large win | +0.11 ms |
| `--disable-occlusion`, removing HZB entirely | large loss | +0.22 ms at 25 km, +0.20 ms at 42 km, **+0.12 ms with micro off** |

The last row bounds the prize: the *entire* value of occlusion culling today is
0.2 ms of a 17.5 ms raster, so improving the HZB can only pay if it makes
qualitatively more things cullable. It cannot, structurally: a grazing terrain
brick's screen footprint straddles visible and hidden pixels, so a conservative
depth test at brick granularity can never reject it. And the first row is
decisive - replacing 15.2x overdraw with exactly 1x cost the same, because
early-Z already rejects the overdraw for about what a per-pixel candidate loop
costs.

The 0.12 ms row matters most: it was measured *after* exact micro was removed,
when per-fragment work had fallen by 39% and overdraw should have become
relatively more important. It did not. The conclusion is not an artifact of
micro dominating the fragment.

Today's 42 km data says it a third way: **18% fewer fragments, 3.29 ms slower.**
Fragment count is not the binding constraint. The remaining structural lever on
the raster is R7 anisotropic bricks, whose argument is stronger at 42 km because
more of the screen is grazing.

In the micro-off regime the raster+cull block is 10.70 ms, and the only knob
that still moves it is voxel size: `--terrain-voxel-size 0.4` is **-0.399 ms**
(10.303), consistent with the -1.44 ms it was worth when micro was still being
paid for on twice as many voxels.

## Dead - killed by measurement, do not re-propose

| Idea | Result |
| --- | ---: |
| **Terrain mesh proxy** (CBT or chunk-grid mesh, carving voxels in the fragment) | **+3.25 ms.** Overdraw fell 15.163x -> 1.524x, and 1.197x back-face culled - a 10-12x reduction in fragment invocations - and the frame got *slower*. Same reason the compute raster failed: the overdraw was never the cost. See `sparse-voxel-terrain-mesh-proxy-plan.md` for the full T0-T2 record, which is worth keeping: the height raster, the seam proof and the containment analysis stand on their own |
| Pass merging / encoder reduction | 2.93 ms of fixed cost exists in total |
| Double-buffering SVGF history (4 full-res copies) | **-0.02 ms**. Free |
| Front-to-back brick ordering | +0.11 ms, neutral. Reverted |
| Merging the R2 split's three render passes | +0.07 ms p50, neutral |
| R2 compact-record raster | rejected on cost |
| Halving the scene-ray grid | **0.00 ms** |
| Spatial ReSTIR | **0.00 ms** |
| GTAO / volumetric fog | 0.05 / 0.07 ms |
| BrickResolve fusion (R10) | 0.97 ms at 1080p |
| **G-buffer format packing** (12 targets, 72 B/px, 149 MB/frame) | **1.27 ms at 1080p, 2.3% of frame** |
| Grass rendering | +0.00 ms |
| Planet background | -0.29 ms |
| Planet overview (static vs adaptive) | +0.11 ms |
| **Probe distance-density thresholds** | **0.22 ms across half=100m..16800m.** See below |
| Raster LOD / clipmap / voxel-size / distance tuning | ~1.4 ms of 20.1 ms total, see section 5 |
| **Compute raster to beat overdraw on traversal alone** (R9's premise) | **-0.03 ms at full routing.** Exactly neutral. Early-Z already rejects the 15.2x overdraw for about what the per-pixel candidate loop costs |
| **Exact microgeometry in the compute raster** | **+2.77 ms at full routing.** Built and 99.992% record-correct. Overdraw's redundancy is concentrated in the *cheap* fallback entries; compute keeps 1/3.2 of the expensive exact traces while losing early-Z. Entries fell 7.8x, exact traces only 3.2x |

### Probe distance striding does not transfer

Probe density already strides with distance - half at
`0.35 * traceExtent`, quarter at `0.65` - and on terrain that resolves to
**half at 16,800 m and quarter at 28,800 m**, against a camera that sees terrain
at hundreds of metres. The striding therefore never engages, which looked like
the same defect the micro handoff had: a distance LOD scaled off an extent so
large it never fires.

It is not. Forcing the thresholds down to 800/2400, 400/1200, 200/600 and even
100/300 metres moved the frame by **0.22 ms at most**, inside the run spreads.

Why it does not transfer: probe tiles are a *screen-space* grid, so every tile
holds a probe whatever its receiver distance, and the dominant cost -
`ProbeInterpolate` at 4.79 ms - is per covered pixel, not per probe. The 12.2 ms
that `--probe-tile` recovers comes from reducing density *everywhere*, including
the near field where the probes actually are. There is no distance-shaped
subset to remove.

That makes DDGI the only route to that 12.2 ms: something has to carry the
low-frequency term so the surface tile can coarsen globally without the quality
loss that coarsening alone produces.

## Measurement protocol

1. `--benchmark-lighting` aggregates, GPU timestamps **disabled**, camera-walk,
   LOD 0.75/1.5, >=3 repeats, quote the run spread with every delta.
2. **`--gpu-timings` lies about attribution.** Vulkan timestamps bracket Vulkan
   passes, not Metal encoders, so a MoltenVK stall is billed to the *next* pass.
   It overstated BrickResolve by 4.7x.
3. **Register-twice** (`--profile-duplicate-pass`) for idempotent passes: the
   duplicate rewrites identical values, so the whole-frame delta is that pass's
   marginal cost. **Neutrality-check the exact-output hash first** - the
   G-buffer fails it. Never valid for draws (early-Z rejects the duplicate).
   For a non-idempotent pass, duplicate it into *throwaway targets of identical
   format* instead: same work, same bandwidth, nothing reads the result.
4. **Min-of-N with a cooldown between runs.** A no-cooldown sweep had every
   arm's third repeat inflated by thermal drift: 6 ms within-arm spread against
   the usual 0.06-0.27 ms.
5. `--no-taa` silently forces `internal = output`. Confirm the
   `Sparse voxel resolution: output=... internal=...` line in every upscaled arm.
