# Sparse voxel renderer performance roadmap

Status: active profiling-backed implementation, 2026-08-14. Timings marked
**Metal** come from recorded Xcode/Metal captures; timings marked **benchmark**
come from the normal benchmark path. Results from different thermal populations
or optimization stacks are kept separate: they are evidence for a lever, not
additive promises.

The concrete next-stage raster work and the optional world-anchored hybrid DDGI
pilot are specified in
[`sparse-voxel-raster-ddgi-implementation-plan.md`](sparse-voxel-raster-ddgi-implementation-plan.md).
That document records landed R0 plus the opt-in, correctness-qualified but
performance-rejected R1/R2 MicroPrepare/direct-parent experiment, and turns the
remaining column cache, cost-aware LOD, resolve fusion, DDGI
scrolling, Tree64 ray reuse, visibility, and hybrid-composition work into
ordered implementation and acceptance stages.

## Executive summary

The renderer started GPU-bound at 99.702% GPU activity and a 47.95 ms median
native source-frame cadence (20.86 FPS). A benchmark-default bug was then found:
benchmark mode makes the run bounded, and the bounded-test fallback silently
replaced the terrain's intended **0.75 px** raster LOD threshold with **64 px**
unless `--lod-threshold` was present. The earlier **35.696 ms p50 / 28.0 FPS**
"clean checkpoint" therefore measured an extremely coarse 64 px raster cut,
not default-quality terrain.

With raster LOD explicitly pinned to **0.75 px** and Tree64 independently pinned
to **1.5 px**, the authoritative native 1200 x 720 Metal capture has a
**42.729 ms p50 frame span** and **42.719 ms p50 source cadence / 23.41 source
FPS**. Matched normal benchmarks are
**41.686 / 45.857 ms p50/p95**, with a repeat at **41.655 / 46.032 ms**.
TAA, MetalFX, frame generation, and vsync are disabled.

**Superseded, 2026-08-14.** Those captures were taken while the initial R1/R2
brick micro-raster split was the default path, so they measure that experiment,
not the incumbent renderer. That broad-extraction revision cost **40.92 / 44.33
ms p50/p95** against roughly **24.3 / 25.9 ms** for the incumbent. The split is
now off by default. Camera-facing extraction and a tight parent proxy later
reduced the built-in timestamp captures to **26.12--27.03 / 28.14--29.49 ms**,
but the matched incumbent was still faster at **23.63--24.30 /
24.82--26.69 ms**, so performance acceptance remains rejected. The
intended-quality default therefore sits around **24 ms p50**, inside the 25 ms
p50 target and between slightly inside and about 1.7 ms outside it at
p95 depending on the run, rather than 17.719 ms over. Every attribution below
that predates this date and does not name its micro-raster arm was measured on
the initial split path and must be re-derived before it is used to rank a lever.
See stage R2 in
[`sparse-voxel-raster-ddgi-implementation-plan.md`](sparse-voxel-raster-ddgi-implementation-plan.md).

The earlier balanced-profile claim of **24.728 ms p50 / 40.44 median FPS** was
affected by the same silent 64 px cut in addition to its explicit SVGF, probe,
planet, and exact-micro quality controls. It remains useful only as a historical
coarse-LOD performance result; it is not evidence that the intended 0.75 px
terrain path reaches 40 FPS. No native, no-upscaler, no-frame-generation profile
has yet demonstrated 40 source FPS at the intended raster quality.

The remaining recommended order is:

1. Require explicit `--lod-threshold 0.75` and
   `--tree64-lod-threshold 1.5` in every terrain benchmark/capture, and archive
   both resolved values with the result.
2. Preserve the landed zero-work and bandwidth reductions: hidden
   `PlanetOverview`, default-off editor emitters, probe-mode resource gating,
   direct SVGF surface histories, and exact auxiliary MRT `DontCare` actions.
3. Qualify the measured `PlanetBackground` stride/horizon/voxel-ownership path
   and decide whether stride 4 is acceptable as an explicit performance mode.
   It is triangle raster; voxel DDA does not apply.
4. Reduce the **22.517 ms** phase-zero brick raster first. Measure fragment
   proxies, early-Z, DDA skips/probes, and exact-micro work; the pinned LOD sweep
   proves that fewer visible bricks alone can make the frame slower.
5. Reduce probe density and update work with explicit quality tiers. Awake 16,
   probe tile 20, and temporal-only SVGF have real wins in one performance
   stack, but those measurements also came from the coarse-LOD population and
   require remeasurement at 0.75 px.
6. Continue scale-insensitive Tree64/lighting work: validate AABB-first model
   loading in an isolated trace and pursue spatial instance rejection. Keep
   compaction/order changes only when the whole trace chain improves.
7. Attack exact microgeometry with a representation/LOD design rather than a
   per-fragment runtime threshold. The old no-micro number is a coarse-LOD
   diagnostic ceiling, while the attempted runtime threshold regressed and was
   reverted.
8. Re-capture a settled Xcode Metal System Trace after every retained batch;
   then address BrickResolve/G-buffer fusion and remaining encoder boundaries.

Retained implementation work now includes hidden-overview indirect
gating, brick format correction and sibling skip, dynamic Tree64 AABB rejection
before its 112-byte model fetch, local `ProbeInterpolate` load reuse, fused SVGF
statistics, direct SVGF history publication on compatible frame configurations,
probe-mode allocation gating, default-off emitter visualization, and
`PlanetBackground` subdivision/visibility controls. Full workgroup probe caches,
a front-to-back brick sorter, a runtime exact-micro threshold, and an adaptive
100% probe refinement experiment were measured and rejected/reverted.

The new authoritative 0.75 px Metal capture reports **42.729 ms p50 frame
span**, **42.719 ms p50 source cadence**, and **13,921** visible bricks in its
final readback. The raster chain from brick setup through G-buffer spans
**27.124 ms**: setup is
**1.025 ms**, phase-zero raster is **22.517 ms**, the intervening HZB is an
inferred **0.036 ms**, phase-one cull/draw are **0.303 / 0.102 ms**, resolve plus
material is **2.344 ms**, and G-buffer own time is **0.309 ms**. Phase-zero
raster alone consumes 52.7% of the frame median and is the primary target.

The older `/tmp/engine-current-native.trace` parse at **38.947 ms p50** and the
35.696 ms benchmark were collected under the silent 64 px cut. Their encoder
attribution and within-population optimization evidence remain historical data,
but neither may be labeled default quality or compared directly with the
quality-correct 0.75 px population.

## Measurement contract

### Recorded configurations

| | Native Metal capture | Half-scale Metal capture |
| --- | ---: | ---: |
| Output size | 1200 x 720 | 1200 x 720 |
| Internal size | 1200 x 720 | 600 x 360 |
| Render scale | 1.0 | 0.5 per dimension (25% pixels) |
| TAA / upscaler | TAA off; MetalFX upscaler off | MetalFX temporal upscaler left on |
| Frame generation | off | off |
| Vsync | off | off |

These captures are not a pure resolution A/B: half scale includes MetalFX while native does not. Use them to classify scale sensitivity, not assign the whole delta to resolution.

“Raw” is observed encoder duration; “exclusive” or “recoverable” is its critical-path portion after overlap. Do not sum raw times. Group numbers are Xcode timeline groups and may contain several RenderGraph passes.

### Historical initial Metal numbers

The following native/half-scale capture established the original bottleneck
shape, but it predates explicit raster/Tree64 LOD metadata. It is retained for
historical scale sensitivity and encoder attribution, not as the current
intended-quality baseline.

| Metric / encoder group | Native 1.0 | 0.5 scale | Interpretation |
| --- | ---: | ---: | --- |
| Source cadence p50 | **47.950 ms / 20.86 FPS** | **28.866 ms / 34.64 FPS** | User-visible real-frame production rate |
| Source cadence p95 | about **50.8 ms** | **31.338 ms** | Preserve p95, not only p50 |
| Scene span p50 | **47.478 ms** | **27.684 ms** | GPU scene occupancy; 21.06 vs 36.12 scene FPS if reported separately |
| GPU active | **99.702%** | **99.683%** | Both configurations are GPU-bound |
| #5 `PlanetOverview` | **6.951 ms exclusive**, 8.954 raw | **7.096 ms recoverable**, 8.897 raw | Fixed/display-resolution work; hidden in the captured surface view |
| #7 first brick raster phase | **15.565 ms exclusive**, 15.595 raw | **5.695 ms recoverable**, 5.724 raw | Dominant native pixel/fragment cost; 2.73x faster at quarter pixels |
| #9 `PlanetBackground` | **0.453 ms exclusive**, 2.439 raw | not isolated | Large raw draw overlaps; optimize submitted geometry but budget critical-path recovery honestly |
| #16 brick resolve + material | **2.271 ms** | not isolated | Full-screen visibility/material conversion |
| #17 G-buffer raster | **0.498 ms** | not isolated | Secondary full-screen raster stage |
| #18 final HZB + probe/trace chain | **7.611 ms** | **8.205 ms** | Does not improve with scale; top trace/probe target |
| #20 `ProbeInterpolate` | **3.797 ms** | **1.200 ms** | Pixel-sensitive; 3.16x faster |
| #22 SVGF | **3.800 ms** | **0.960 ms** | Almost ideal 4x pixel scaling |
| #24 GTAO + compose + gizmo | **0.482 ms** | not isolated | Low priority until separated |
| MetalFX critical tail | disabled | **0.887 ms** | Required cost of the captured half-scale presentation path |
| Final blit | **0.049 ms** | not isolated | Not a current bottleneck |
| Current allocated memory | about **2.095 GiB** in the older capture | not separately recorded | `currentAllocatedSize` was unavailable in that trace; treat allocation removal as a residency/bandwidth goal, not an assumed frame-time gain |

The 19.084 ms half-scale cadence reduction combines cheaper pixel work, a changed temporal/presentation path, and fixed costs. `PlanetOverview` and #18 explain why quarter pixels do not approach a 4x frame-time gain.

### Authoritative intended-LOD Metal attribution

The current acceptance capture explicitly sets raster LOD to **0.75 px** and
Tree64 LOD to **1.5 px**. At native 1200 x 720 with TAA, MetalFX, frame
generation, and vsync disabled, its frame span/source cadence is
**42.729 / 42.719 ms p50**, respectively, or **23.41 source FPS**. The final
readback contains **13,921 visible bricks**.

| Intended-LOD raster stage | Duration | Interpretation |
| --- | ---: | --- |
| Brick setup | **1.025 ms** | Fixed cull/list setup before raster |
| Phase-zero brick raster | **22.517 ms** | Dominant pass; 52.7% of the complete frame median |
| Current-frame HZB | **0.036 ms inferred** | Inferred intervening encoder interval; not an independently counter-resolved body |
| Phase-one brick cull | **0.303 ms** | Rejects/re-admits against current HZB |
| Phase-one brick draw | **0.102 ms** | Recovery draw is already small |
| Brick resolve + material | **2.344 ms** | Full-screen visibility/material conversion |
| G-buffer | **0.309 ms own** | Final geometry attachment emission |
| Setup-to-G-buffer chain span | **27.124 ms** | Dependency span; do not add it to the component rows |

The setup-to-G-buffer chain consumes 63.5% of the frame median. Phase-zero
raster is 83.0% of that chain and is the first optimization target; changing
phase-one recovery cannot recover enough time by itself.

### Historical coarse-LOD post-batch attribution

The final parse of `/tmp/engine-current-native.trace` contains **291 complete
frames**, but the run inherited the bounded-test **64 px raster LOD**. Three
related timing populations describe that historical coarse cut:

| Historical trace metric | p50 | p95 | Mean / interpretation |
| --- | ---: | ---: | --- |
| App GPU span | **38.947 ms** | **42.908 ms** | **39.197 ms**; first app encoder start to last app encoder end |
| Source cadence | **39.033 ms** | **43.189 ms** | **39.207 ms / 25.62 FPS**; consecutive real-frame production |
| App-active union | **38.611 ms** | **42.655 ms** | In-frame utilization is **99.30% median / 99.06% aggregate** |

The consistently parsed original app GPU span was **48.058 ms p50**. The coarse
**38.947 ms** is **9.111 ms / 19.0% shorter**. This is the authoritative
trace-to-trace comparison within that historical LOD population. It must not be
compared as one population with either the 0.75 px acceptance capture or the
35.696 ms normal benchmark: LOD, `xctrace` overhead, and thermal state differ.

| Group | Final pass mapping | Parsed duration |
| --- | --- | ---: |
| #6 | Brick setup | **1.011 ms** |
| #7 | Brick raster / exact micro | **15.748 ms own; 16.112 ms encoder span** |
| #14 | Pre-probe compute | **2.511 ms** |
| #17 | `ProbeRays` | **8.132 ms own** |
| #19 | `ProbeInterpolate` | **4.132 ms** |
| #20 | History blit | **0.312 ms** |
| #21 + #22 + #23 | Post/SVGF chain | **3.622 ms total** |
| Render tail | Final render work | **0.707 ms** |

Brick raster was already the dominant individual encoder even at 64 px, followed
by probe ray generation/tracing and interpolation. At the intended 0.75 px cut,
phase-zero raster grows to 22.517 ms and strengthens that priority. Group numbers
are build-specific; use each pass mapping only for its exact trace, and do not
sum spans or raw durations into a synthetic frame time. `currentAllocatedSize`
was not available in the historical trace, so the older approximately 2.095 GiB
allocation figure cannot be updated from it.

Benchmark JSON is now **schema version 14**. Its configuration block records the
resolved raster and Tree64 LOD thresholds independently, the SVGF iteration
count, probe tile/adaptive percentage/awake rays, planet stride, terrain micro
materials, R2 micro-raster mode and record capacity, the exact-micro distance
cutoff, and the resolved probe distance thresholds. Its memory block records
the micro record, residual, dispatch, draw-argument, and total buffer bytes.
Each timestamped frame and the aggregate block also report
`brick_phase0_total_ms`, computed by summing the common setup and HZB with
either the incumbent draw or the complete R2 replacement chain before
calculating percentiles. Recomputing that grouping over the two R2 attribution
captures gives 8.29--8.37 ms p50 / 8.66--9.13 ms p95 for R2 versus
6.16--6.20 ms p50 / 6.61--7.03 ms p95 for the incumbent.
Benchmark mode preserves the scene raster preset unless `--lod-threshold` is
explicit; that raster override does not also change Tree64 unless
`--tree64-lod-threshold` is explicit. Preserve the metadata with every result
so stacked profiles cannot be mistaken for default-quality measurements.

### Source FPS versus display FPS

Always report `source FPS = 1000 / source cadence p50`. Scene-span FPS and
display FPS are different metrics:

- With frame generation disabled, the recorded source and displayed real-frame
  rates are effectively the same: 20.86 FPS native and 34.64 FPS at half scale.
- MetalFX upscaling changes resolution, not the number of source frames.
- Frame generation may present an interpolated image between two real images
  and therefore approximately double presentation events. It does not halve
  source GPU time, does not make 30 source FPS into 60 source FPS, and can add a
  tail and latency. Report it as, for example, “30 source FPS / approximately
  60 displayed FPS with interpolation,” never just “60 FPS.”
- CPU `Present` includes drawable/vsync waits. It is not another render pass.

### Current checkpoints and controlled A/Bs

| Test | Before | After | Meaning |
| --- | ---: | ---: | --- |
| Authoritative Metal, raster LOD 0.75 / Tree64 1.5 | no matched pre-change quality trace | **42.729 ms frame span / 42.719 ms cadence p50** | **23.41 source FPS**, 13,921 final visible bricks; current intended-quality baseline |
| Authoritative Metal raster chain | no matched pre-change quality trace | **22.517 ms phase-zero raster; 27.124 ms setup-to-G-buffer span** | Raster is 52.7% of frame p50 and the complete chain is 63.5% |
| Historical coarse-LOD Metal app GPU span | **48.058 ms p50 original** | **38.947 / 42.908 ms p50/p95; 39.197 mean** | **-9.111 ms / -19.0% p50** within the old 64 px population only |
| Historical coarse-LOD Metal source cadence | prior population not reparsed as cadence | **39.033 / 43.189 ms p50/p95; 39.207 mean** | 25.62 median source FPS across 291 complete frames; not the quality baseline |
| Historical coarse-LOD Metal app-active union | prior population not reparsed with this query | **38.611 / 42.655 ms p50/p95** | 99.30% median / 99.06% aggregate in-frame utilization |
| Earlier post-batch Metal source cadence | 48.020 / 50.989 ms p50/p95 | **39.023 / 41.902 ms** | Historical LOD-64 parse: 8.998 ms / 18.7% lower p50 within that population |
| Earlier Metal GPU-active union | 47.597 / 50.216 ms p50/p95 | **38.917 / 41.881 ms** | Historical parse retained for provenance |
| Metal #5 `PlanetOverview` raw / exclusive | 8.955 / 6.948 ms | **0.0185 / 0.00038 ms** | p95 0.0189 / 0.00063 ms; remaining encoder is clear-only |
| Metal #7 first brick raster exclusive | 15.565 ms | **10.618 ms** | Real composite reduction; sibling skip still needs isolated A/B |
| Metal #18 probe/Tree64 | 7.611 ms | **7.712 ms** | No measured win; AABB-first ordering remains unproven in isolation |
| Metal #20 / #22 / #16 | 3.797 / 3.800 / 2.271 ms | **4.121 / 3.745 / 2.285 ms** | ProbeInterpolate / SVGF / BrickResolve are essentially retained work |
| Historical coarse-LOD optimization batch, benchmark p50 | 48.120 ms (20.78 FPS) | **38.575 ms (25.92 FPS)** | Raster LOD 64; 9.545 ms / 19.8% lower within that population; p95 after is 41.423 ms |
| Historical coarse-LOD native 1:1 temporal-resolver benchmark | no pre-change baseline | **45.898 ms (21.79 FPS)** | Raster LOD 64; p95 49.877 ms; scale 1.0 with temporal AA, MetalFX upscaler disabled |
| Historical coarse-LOD disable-exact-micro diagnostic p50 | 48.120 ms | **31.805 ms** | Raster LOD 64; 16.315 ms / 33.9% upper bound that changes geometry quality |

Do not compare benchmark and Metal populations directly, and do not compare the
historical 64 px trace to the current 0.75 px trace as though LOD were unchanged.
The quality-correct Metal capture is the encoder-attributed baseline; matched
normal benchmarks are the pacing check.

The isolated native camera-walk sweep below pins Tree64 at 1.5 px and changes
only the raster LOD threshold. The first 0.75 px run and its repeat are the
current normal-benchmark baseline. P95 was not retained for the other diagnostic
thresholds.

Additional **0.1875 / 0.375 / 0.75 px** fine-cut runs all selected the same leaf
cut, ended at the same 13,921-brick count, and had indistinguishable time. The
current hierarchy cannot increase visible brick count by lowering the threshold
below 0.75 px; optimization must reduce work per leaf/proxy instead.

| Raster LOD threshold | p50 | p95 | Decision |
| ---: | ---: | ---: | --- |
| **0.75 px** | **41.686 ms** | **45.857 ms** | Intended terrain raster quality; 23.99 median FPS |
| **0.75 px repeat** | **41.655 ms** | **46.032 ms** | Reproduces the baseline; 24.01 median FPS |
| 3 px | 41.530 ms | not retained | No material speedup; coarser quality without benefit |
| 6 px | 53.629 ms | not retained | Strong regression despite selecting fewer bricks |
| 12 px | 45.294 ms | not retained | Recovers from the 6 px peak but remains slower than 0.75 px |
| 24 px | 39.322 ms | not retained | Faster only after a large, visible quality reduction |
| 64 px | 35.779 ms | not retained | Extreme diagnostic cut; closely reproduces the old 35.696 ms checkpoint |

The sweep is a U-curve, not a monotonic brick-count curve. Coarser parent bricks
submit fewer 18-vertex instances, but their OR-reduced occupied boxes can cover
more fragments, bridge more empty space, reduce binary empty-octant opportunities,
and change exact-micro work. At 6 px those costs dominate. At 64 px the cut is
finally coarse enough for instance reduction to win, but a retained voxel may be
73.6 pixels wide under the 15% hysteresis band, so that result is not a shipping
quality candidate. Keep 0.75 px as the baseline; 1.5–3 px require visual review
and currently offer no demonstrated performance return.

The next table preserves historical native benchmark A/Bs collected before the
LOD bug was found. Unless a row says otherwise, both sides inherited raster LOD
64. Within-row deltas can still identify a lever, but absolute times cannot be
used as intended-quality checkpoints or added to the 0.75 px budget.

| Controlled comparison | Before | After | Delta / decision |
| --- | ---: | ---: | --- |
| Historical coarse-LOD checkpoint | historical initial 47.950 ms | **35.696 ms p50 / 38.439 ms p95** | Raster LOD 64; performance-quality evidence only, not a default-quality checkpoint |
| Hot coarse-LOD population | separate earlier population | **about 39.85 ms p50** | Thermal/workload drift; use for local A/Bs only, not as a regression against 35.696 ms |
| Planet full source topology and culling disabled → default stride 2 + conservative horizon/voxel ownership culling | 37.727 ms | **35.665 ms** | **-2.062 ms** in historical LOD-64 population; retained implementation still needs intended-LOD repetition and visual qualification |
| Planet stride 2 at probe tile 20 → stride 4 in the same performance stack | 32.813 ms | **31.000 ms** | **-1.813 ms** in historical LOD-64 population; stride 4 remains an explicit quality/performance mode |
| SVGF two A-trous iterations → one | 39.848 ms | **38.529 ms** | **-1.319 ms** average; one iteration is an explicit quality knob |
| SVGF two A-trous iterations → temporal-only, with exact auxiliary MRT `DontCare` | 39.193 ms | **36.016 ms** | **-3.177 ms**; large quality-changing performance mode, not the default |
| Probe awake rays 32 → 16 under temporal-only SVGF | 36.016 ms | **34.880 ms** | **-1.136 ms**; explicit estimator-quality knob pending noise/disocclusion review |
| Terrain-preset probe tile 14 → 20 after awake 16 | 34.880 ms | **32.813 ms** | **-2.067 ms**; explicit spatial-quality knob; tile 28 had no p50 win |
| Exact microgeometry on → off in the stride-4 performance stack | 31.000 ms | **16.443 ms / 60.8 FPS** | **-14.557 ms** historical LOD-64 diagnostic ceiling; disabling exact detail is not a shipping default |
| Brick front-to-back sorter off → on | 39.457 ms | **40.010 ms** | **+0.553 ms**, rejected/reverted; widening the fixed-cost raster kernel also lost |
| Runtime exact-micro threshold control | control | **+4.245 ms** | Rejected/reverted; extra divergent per-fragment work cost more than it skipped |
| Exact auxiliary MRT load actions → `DontCare` | 39.785 ms | **39.193 ms** | Provisional **-0.592 ms** p50; implemented, but p95 remains inconclusive |
| Performance stack before exact-micro distance cutoff: SVGF 0, awake 16, tile 20, planet stride 4 | no directly comparable intended-quality baseline | **30.999 / 33.868 ms p50/p95** | Coarse raster LOD 64; control population for the historical distance sweep |
| Same performance stack, exact-micro distance 500 m | 30.999 / 33.868 ms | **24.349 / 26.258 ms** | **-6.650 ms p50**; 41.07 median FPS, but 38.08 p95 FPS |
| Same performance stack, exact-micro distance 750 m | 30.999 / 33.868 ms | **24.877 / 26.859 ms** | **-6.122 ms p50**; 40.20 median FPS, but 37.23 p95 FPS |
| Same performance stack, exact-micro distance 1000 m | 30.999 / 33.868 ms | **25.109 / 26.855 ms** | **-5.890 ms p50**; 39.83 median FPS and 37.24 p95 FPS |
| Historical balanced profile: SVGF 1, awake 8, tile 20, planet stride 4, exact micro 400 m | separate quality/performance profile | **24.728 / 26.372 ms** | **40.44 median FPS at raster LOD 64**; not evidence for intended terrain quality |
| Historical balanced profile, exact micro 500 m | LOD-64 balanced 400 m: 24.728 ms | **25.012 ms p50** | Coarse-cut comparison only; not an intended-quality 40 FPS result |

The adaptive-refinement 100% experiment was also rejected: it did not provide a
repeatable total-frame benefit. These historical A/Bs identify possible product
tiers, but every retained lever must be remeasured with raster LOD 0.75 and
Tree64 LOD 1.5 before it contributes to the intended-quality budget. The old
exact-micro ceiling identifies representation/raster work as promising in the
coarse population; it does not quantify the available 0.75 px saving.

The exact-micro sweep crossed 40 source FPS only because it stacked explicit
quality controls on top of the accidental 64 px raster cut. Every reported p95
was above 25 ms even there. None of those values demonstrates native 40 FPS at
the intended raster quality; transition, silhouette, porous coverage, and
camera-motion review are still required before the distance cutoff is promoted.

### Quality defaults versus performance controls

The production/default-quality terrain path uses raster LOD **0.75 px** and an
independent Tree64 LOD of **1.5 px**, and keeps exact microgeometry, probe tile
14, 32 awake rays, 200% adaptive face capacity, and two SVGF A-trous iterations.
The generic non-terrain default is tile 12; the canonical terrain package
overrides an unset CLI tile to 14.
The planet background defaults to subdivision stride 2 with conservative
horizon and voxel-ownership culling; stride 1 is the full-source-topology
qualification control. These defaults are separate from the following opt-in
performance controls:

| Control | Performance direction | Expected quality effect |
| --- | --- | --- |
| Raster `--lod-threshold` above 0.75 | Usually selects fewer, coarser parent bricks, but the measured curve is non-monotonic and 6 px is substantially slower | Larger OR-reduced cells/proxies, terrace and silhouette error, LOD transitions; 24/64 px are diagnostic quality cuts |
| `--planet-background-stride 4` | Fewer planet vertices/triangles | Coarser distant curvature/silhouette; qualify orbit and grazing views |
| `--svgf-atrous-iterations 1` | Removes the second spatial pass | More residual lighting noise; temporal response remains |
| `--svgf-atrous-iterations 0` | Temporal-only SVGF | Most residual noise and edge instability; strongest measured SVGF saving |
| `--probe-awake-rays 16` or `8` | Halves or quarters awake-probe direction updates | Slower/noisier cold-start and disocclusion convergence; balanced profile uses 8 |
| `--probe-tile 20` | Fewer uniform probes and interpolation neighborhoods | Lower spatial lighting resolution and more cross-surface pressure |
| Positive `--brick-exact-micro-distance` | Stops exact micro carving beyond a world-space distance; 400–500 m crossed 40 median FPS only in the historical LOD-64 stack | Distance-dependent detail transition; disabled by default (`0`), not remeasured at LOD 0.75, and not p95-qualified |
| `--disable-terrain-micro-materials` | Removes exact micro traversal | Diagnostic only: deletes authored close detail, holes, and silhouettes |

Far analytical lighting is parameterized with
`--probe-local-light-cull-start` and `--probe-analytical-distance`; it is a
representation/quality handoff rather than a demonstrated frame-time control in
the current camera walk. The default remains the qualified 3.75 km / 10 km
transition.

Render-mode names must include scale and temporal path. Raw native is scale 1.0 with `--no-taa --disable-metalfx-upscaler`. `--disable-metalfx-upscaler` alone retains scale 0.5 and internal temporal upscale. Native temporal AA explicitly sets `--render-scale 1.0`, disables MetalFX, omits `--no-taa`, and runs a 1:1 temporal resolver.

## Priority matrix

Metal values from the 0.75 px acceptance capture are labeled intended-LOD.
Other benchmark/Metal values in this matrix are historical LOD-64 evidence and
must be requalified before they are used for an intended-quality budget.

| Priority | Area | Measured evidence | First action | Current status / landing gate |
| --- | --- | --- | --- | --- |
| P0 | Hidden `PlanetOverview` | 6.948 ms exclusive baseline → 0.00038 ms post | Zero the indirect count whenever overview contribution is zero | Implemented; Metal shows only the 0.0185 ms clear encoder; orbit/adaptive quality gates remain |
| P0 | Brick target format order | Shader/texture order differed from pipeline declarations | Match locations 0..3 as RGBA32UI, R32UI, R32UI, RGBA32UI | Corrected; correctness fix, performance unmeasured |
| P0 | Brick raster and exact micro | Intended-LOD phase-zero raster is **22.517 ms**; setup-to-G-buffer spans **27.124 ms**. Historical LOD-64 500 m cutoff changed 30.999 → 24.349 ms p50 | Measure proxy fragments/early-Z/DDA/micro work at 0.75 px, then move the representation decision earlier than the hot fragment loop | Distance variant adds no cutoff comparison when disabled; exact traversal now hoists immutable program flags once per parent; sibling skip retained; sorter and projected-pixel runtime threshold were reverted |
| P0 | Probe / dynamic Tree64 | Historical LOD-64 Metal #17 `ProbeRays` is 8.132 ms own; pre-probe #14 is 2.511 ms | Re-capture at raster 0.75 / Tree64 1.5, split ray generation/traversal attribution, then add spatial instance rejection | AABB-first load ordering implemented; provisional isolated A/B was about -1.57 ms and still needs a matched counter capture |
| P0 | Far analytical probe lighting | Controlled native 1e9/10km/5km cutoff p50 = 38.486/38.598/38.689 ms | Fade finite lights/traced visibility, then emit zero rays past a metre cutoff | Implemented and parameterized; no frame-time win in camera-walk, retain 10 km for quality handoff and add ray counters before more tuning |
| P1 | `ProbeInterpolate` | Historical LOD-64 Metal #19 is 4.132 ms; historical half-scale was 1.200 ms | Re-capture at intended LOD, then reduce redundant neighborhood loads without changing coverage | Raw normal and SH coefficient 0 are reused locally; full threadgroup caches regressed by about 0.55–0.60 ms and were removed |
| P1 | SVGF/post | Historical LOD-64 #21+#22+#23 total 3.622 ms; 2 → 1 iteration saved 1.319 ms and 2 → 0 saved 3.177 ms in local stacks | Retain fused local statistics/direct histories; remeasure iteration tiers at intended LOD | Fused duplicate 3x3 walks and compatible direct history publication implemented; 0/1 iterations remain opt-in quality controls |
| P1 | Brick resolve + G-buffer | Intended-LOD **2.344 + 0.309 ms own** | Fuse compatible full-screen conversion/material work after phase-zero raster | Not started; preserve exact IDs and depth |
| P1 | `PlanetBackground` patches | Historical LOD-64 full/culling-off 37.727 → stride-2/culling-on 35.665 ms; stride 4 saved another 1.813 ms in one stack | Repeat at 0.75 px, qualify stride 2 default and stride 4 tier, then partition sectors if needed | Stride 1/2/4, conservative horizon ring prefix, and exact voxel-ownership fragment rejection implemented; DDA is inapplicable |
| P1 | Histories/readbacks | Probe-mode unused allocation was roughly 480 MiB; SVGF copied four surface histories | Rotate/alias live resources; make diagnostics opt-in | Probe mode now allocates one-element stubs for unreachable ReSTIR/screen/history resources; compatible SVGF paths publish G-buffer surfaces directly into the history ring |
| P2 | Ray compaction/order | 64-thread bitonic sort has 21 barriered rounds; brick sorter 39.457 → 40.010 ms | A/B the complete producer + trace/raster chain, not ordering alone | Experimental brick front-to-back sorter rejected/reverted; retain existing ray ordering until a full-chain win is measured |
| P2 | GTAO/compose | 0.482 ms grouped native | Isolate first, then lower samples or fuse compose | Defer behind P0/P1 |
| P2 | MetalFX/presentation | 0.887 ms half-scale tail; 0.049 ms native blit | Remove avoidable conversions/events around MetalFX | Do not trade source time for advertised display FPS |
| P2 | Barriers/encoder gaps | Included in scene span, not pass sum; auxiliary MRT `DontCare` provisionally saved 0.592 ms p50 | Inspect dependency graph; fuse only proven producer/consumer pairs | Exact unused auxiliary load actions are `DontCare`; p95 needs repetition; never globally weaken hazards |

## Workstreams

### 1. PlanetOverview: eliminate hidden whole-planet work

The profiled raster fallback is six 512 x 512 cube faces: 3,145,728 triangles.
It renders at display resolution and previously remained submitted while the
surface view hid its result in navigation merge. This matches the scale-neutral
6.95–7.10 ms recovery opportunity.

The implementation keeps a cached indirect command in the graph and writes
`indexCount = 0` for a hidden static overview. Its frame-local indirect buffer
cannot race an earlier slot. Post-batch Metal shows raw 0.0185 ms and exclusive
0.00038 ms: the encoder remains only to clear targets. Still validate static and
adaptive-CBT modes; hidden means no active triangles in either path.

When the overview is visible, prefer the existing adaptive CBT after its
topology/temporal gates pass. The fallback should also expose 128/256/512 face
resolutions; 256 reduces static triangles to 786,432 and 128 to 196,608. Select
by projected error, not a global “low quality” switch. Treat visible overview
LOD as separate from the now-demonstrated hidden zero-work gain.

Anchors: `SparseVoxelPlanetOverviewPass::upload`,
`SparseVoxelPlanetOverviewPass::recordCommandBuffer`, and
`buildRasterFallback` in `src/voxel/svraster/PlanetOverviewMesh.cpp`.

### 2. PlanetBackground: patch LOD and camera visibility

`PlanetBackground` is not a voxel traversal. It is a polar surface cap whose
vertex shader projects a direction/elevation mesh. The source 512 rings x 1024
segments contain 524,289 vertices and 1,047,552 triangles, so binary voxel DDA
cannot skip this work.

The implemented pass keeps one immutable source grid and supports power-of-two
topology strides. Stride 2 is the production default and reduces the source cap
to at most 261,632 triangles; stride 1 remains the exact-topology qualification
path, and stride 4 is an explicit performance tier. An indirect command limits
the draw to a conservative radial-ring prefix hidden neither by the minimum
planet sphere nor the configured camera-region horizon. In the fragment stage,
voxel coverage and depth reject planet pixels that are still wholly owned by
the near voxel renderer; every transition/blend pixel is retained. The command
and uniforms are frame-local, so cached graph commands do not race another frame
slot. Changing stride rebuilds topology through the CLI/UI.

The controlled full-source/culling-off versus default stride-2/culling-on test
was **37.727 → 35.665 ms p50 (-2.062 ms)**. In the later stride-2 + probe-tile
20 stack, stride 4 reached **31.000 ms** from **32.813 ms (-1.813 ms)**. That
second result is context-dependent and quality-changing. Both benchmark A/Bs
inherited raster LOD 64, so retain only their within-row direction until they
are repeated at 0.75 px; they cannot be added to one another or promoted without
surface, grazing, ascent, orbit, seam, and camera-cut review.

The next structural step, only if stride 2 is still material in the updated
Metal trace, is immutable sector patching with conservative frustum bounds.
Keep adjacent LODs within one level and use stitch variants or skirts at radial
and sector seams. Because cube-face tangent frames can change handedness, do not
simply enable winding culling; preserve the wrapped azimuth, central fan, and
existing exact projection.

Anchors: `buildPlanetBackgroundMesh` around `ringCount`/`segmentCount`,
`drawPlanetVertex` in `sparse_voxel_planet_background.wgsl`, and
`SparseVoxelPlanetBackgroundPass::recordCommandBuffer`.

### 3. Brick raster, overdraw, and binary hierarchical DDA

The brick path already does substantial useful work:

- culling emits an indirect list in two phases, with a current-frame HZB between
  them;
- the vertex shader draws only the occupied sub-box and three camera-facing
  faces per visible brick;
- the fragment shader first walks a register-held 4 x 4 x 4 mask of two-voxel
  cells, then reads exact occupancy in the selected 2 x 2 x 2 region;
- hardware reverse-Z selects the nearest brick before the full-screen resolve.

At the intended 0.75 px raster LOD, phase zero costs **22.517 ms** and dominates
the **27.124 ms** setup-to-G-buffer dependency span. Setup is 1.025 ms; the
inferred current-HZB interval is 0.036 ms; phase-one cull/draw are only
0.303/0.102 ms; resolve/material and G-buffer own time are 2.344/0.309 ms.
This rules out phase-one recovery or indirect-instance setup as the primary
bottleneck.

This is geometry evaluation, not conventional material shading. Phase zero
loads the packed material only to recover visibility geometry, descriptor,
context, identity, and output payload. BRDF/material-program evaluation is in
the later 2.344 ms resolve/material group. A diagnostic frame submitted
8,033,684 brick-proxy fragments for 490,387 covered pixels (16.38x pre-depth
proxy demand). Of 6,537,618 valid micro candidates, 2,564,323 ran an exact
trace: 963,002 used the one-face path and 1,601,321 used the general volume
path.

The context split enabled a cheaper exact specialization before adding new
geometry. Interior and flat staircase parents accounted for **2,179,011 of the
2,564,323 exact traces (85.0%)**, although both describe a completely solid
parent. The retained shortcut covers Interior only: it reproduces the first
volume-cell bounds and incumbent slab intersection, while skipping five
zero-depth face evaluations and the general DDA loop. A full GPU signature is
identical to the incumbent path (`803e4da30931564f`, 522,430 covered pixels).
Two incumbent control runs measured 41.442/41.475 ms p50; shortcut runs measured
38.522/36.211 ms, with ordinary run-to-run GPU-state variance. The authoritative
Metal System Trace removes that ambiguity: Phase0 falls **22.517 -> 16.884 ms**
(-5.633 ms, -25.0%), and the brick-setup-through-G-buffer span falls
**27.124 -> 21.455 ms** (-5.669 ms, -20.9%). Native source cadence is now
36.432 ms p50 / 40.881 ms p95 (**27.45 FPS** median) and app GPU activity remains
99.34% of the traced span. Diagnostic partitioning confirms the shortcut
eliminates the volume path for this terrain frame: 2,060,857 Interior shortcuts
plus 1,093,480 one-face traces equal all 3,154,337 exact traces, with zero volume
invocations.

The analogous FlatSurface shortcut was rejected: coverage remained identical,
but the full G-buffer signature changed. Flat remains on the exact +Y column
routine until a shader-level f32 boundary/identity proof exists. Actual slopes
also remain on that routine; no wall contexts occurred in the diagnostic
captures.

The Tree64-pinned raster-LOD sweep is deliberately non-monotonic:
0.75/3/6/12/24/64 px produce p50 values of
**41.686/41.530/53.629/45.294/39.322/35.779 ms**. Fewer bricks can lose because
coarser OR-reduced occupied boxes submit wider proxy coverage and alter DDA and
exact-micro behavior. Do not tune visible brick count in isolation. First record
fragments invoked, early-depth rejects, projected proxy area, binary groups
skipped, exact occupancy probes, and micro steps at 0.75, 3, and 6 px.

Increasing the cut through the pixel threshold is not available either:
0.1875, 0.375, and 0.75 px all reach the same leaf floor of **13,921 visible
bricks** and measure about 41.8 ms. A future adaptive selector should compare a
parent's projected occupied-face and expected traversal cost with the sum of its
children, while retaining the 0.75 px error bound; it may intentionally prefer
more small bricks where they reduce proxy fill and micro-DDA work.

The retained implementation adds a binary level above that coarse grid. If the eight
coarse bits in an aligned 2 x 2 x 2 sibling group are all zero, the ray advances
to the exit of the corresponding 4 x 4 x 4 voxel octant. It reuses the existing
64-bit coarse mask and therefore adds no node field, storage read, varying, or
ABI change. The mask shape is the same proven `0x00330033` sibling grouping used
by Tree64. Dense/hit-first regions keep the shorter old path.

A fine-mask prefetch experiment was implemented and rejected. It decoded each
selected 2x2x2 region into an 8-bit register mask from two eager occupancy-word
reads, then loaded a prefix only on an occupied parent. Intended-LOD native
A/B/A p50 was **35.964 / 36.927 / 35.870 ms** (control / prefetch / control), a
roughly 1.01 ms regression versus the control average. The scene is sufficiently
hit-first/cache-friendly that two eager loads cost more than the incumbent lazy
per-step fetch. The variant and CLI were removed; do not retry without counters
showing a materially more miss/carve-heavy population.

It remains subject to an isolated A/B. A stored scale-4 mask is a later option
only if counters show the derived test wins but mask construction is material.
Do not replace the brick with Tree64: Tree64's natural sides are 4/16/64 rather
than 8, and brick hits still need prefix/material addressing.

A fixed-bin front-to-back brick sorter was implemented as an experiment and
reverted: its matched total-frame p50 changed **39.457 → 40.010 ms**, and the
wider fixed-cost cull/scatter kernel could not repay itself through early depth.
Do not reintroduce ordering without first proving high overdraw and measuring
the complete cull + sort + raster chain.

Measure overdraw separately with debug mode 19 (`BrickOverdraw`) and
`--brick-measure-area`. The debug view adds one atomic per fragment and a
readback, so its timing is not a performance result. Record visible bricks,
projected area, fragments, covered pixels, average/peak overdraw, phase-one
reuse, and phase-two HZB rejection. Optimize excessive proxy coverage or LOD
selection before adding more DDA arithmetic.

Add diagnostic-only DDA counters for scale-4 groups tested/skipped, scale-2
cells visited, exact occupancy probes, hit-first exits, misses, and micro
continuations. The existing micro counters already expose eligible parents,
descriptor loads, accepted hits, carved parents, continuations, final discards,
heightfield steps, and volume steps.

#### Visible micro preparation / expansion experiment

Do not expand microvoxels inside `Cull`. That kernel is one cheap lane per node,
runs for both occlusion phases, lacks material/occupancy bindings, and costs only
about 1.025 ms. Variable 0..512 parent work would add divergence and atomics to
the wrong stage. If specialization leaves enough cost, use a separate GPU-only
chain after each visible-brick list is complete:

```text
visible bricks
  -> classify/count unique occupied parents
  -> prefix/compact exact-near carved heightfields
  -> prepare indirect arguments
  -> specialized heightfield raster
```

The visible-micro chain is an opt-in R1/R2 experiment selected with
`--brick-micro-raster`. Reset and
Transform use one 64-lane workgroup per visible brick;
`--brick-micro-prepare-diagnostic` adds only the sizing Scan and readbacks.
Transform resolves material slot and palette address once per unique parent,
compacts guaranteed-exact heightfields into 16-byte records, writes the
remaining 512 occupancy bits, rebuilds the logical 64-bit 4x4x4 mask, and
prepares indirect draw arguments. A compare/exchange reservation accepts or
rejects the entire brick, so overflow never clears an unemitted parent. The
residual output feeds `ResidualDraw`, compact records feed
`PreparedMicroDraw`, and `PreparedTieReplayDraw` handles the sparse ambiguous
ownership samples. The incumbent full-brick draw remains the default;
`--disable-brick-micro-raster` names it explicitly and omits all preparation
resources and passes. At native 1200x720
and the
intended 0.75 px cut, its Phase0 scan reports 13,893 visible bricks and
3,011,051 occupied micro parents: 2,407,445 Interior, 415,359 Flat, 188,247
Slope, and zero Wall/Other. Maximum projected microcell size places 1,676,196
below 0.5 px, 1,261,083 in 0.5--1 px, 56,304 in 1--2 px, and 17,468 at 2 px or
larger. The farthest-AABB test proves only **71,621** parents are exact for every
possible entry ray; just **14,479** of those are heightfields. A compact 16-byte
parent-reference stream is therefore about **231.7 KiB**. It represents 361,975
N=5 columns, while explicit unmerged geometry has a conservative upper bound of
1,230,715 quads. These are Phase0 `(brick, parent)` counts, not globally
deduplicated canonical parents: Phase1 newcomers are excluded, while clipmap
overlap ownership is still applied per final hit in the incumbent fragment
path. They reliably size a conservative Phase0 pilot, not a complete replacement
capacity.

Never materialize every microcell. The 13,921-brick cut has a 7.13 million
parent-voxel upper bound; subdivision five would expand that to as many as 891
million cells. Start with a 16-byte compact parent reference and either generate
the 5x5 depths in the specialized workgroup or cache their 4-bit values (about
32 bytes total with key/metadata). Rasterize a tight parent/column proxy and
reuse the exact 2D intersection first. Explicit exposed top/riser strips with
greedy merging are a second A/B: the unmerged 1.23-million-quad bound can lose
on Apple's tiler even though it removes fragment DDA. Porous/general volumes
keep their 3D DDA. A capacity overflow must leave the parent unmarked so the
current brick shader remains the exact fallback.

The acceptance comparison is the complete `prepare + raster` chain, not the new
draw in isolation. Record unique parents, records/bytes emitted, overflow,
primitives, fragment invocations, early-depth rejects, phase-zero time, and
full-frame p50/p95. Require identical depth, material address, micro payload,
surface identity, and full G-buffer hash.

For exact microgeometry, keep the existing authored projected-detail handoff.
A runtime global pixel-threshold branch regressed total p50 by **4.245 ms** and
was reverted: it added divergent math to the hot fragment path before enough
micro work could be rejected. Disabling exact microgeometry in the historical
LOD-64 stride-4 performance stack changes **31.000 → 16.443 ms (60.8 FPS)**.
That identifies a large ceiling in the coarse population, but neither its
magnitude nor 60.8 FPS transfers to the intended 0.75 px cut, and the saving is
not quality-free.
At the intended 0.75 px cut, disabling terrain micro materials changes the clean
p50 from about **41.69 to 29.10 ms**, a **12.59 ms** diagnostic ceiling that
still misses the 25 ms target and changes geometry. Micro diagnostics count
about 4.10 million heightfield-plus-volume steps in the sampled final frame.
The retained exact-quality metadata hoist loads immutable geometry-program
flags once per parent rather than in those inner loops; two post-change native
runs measured **41.073/45.323** and **40.963/45.561 ms p50/p95**, versus the
41.686/45.857 and 41.655/46.032 ms controls. Treat the roughly 0.65 ms p50
recovery as a pacing A/B until a matched Metal encoder capture confirms it.
Eight-frame full-graph checks of the default shader and the separately compiled
cutoff shader at an effectively infinite cutoff both produced 522,430 covered
pixels and output hash `803e4da30931564f`.
The next design should choose representation/LOD before rasterization, or
preclassify material/brick batches outside the fragment loop. Terrain
heightfields and porous volumes need different fallback rules. A valid change
must preserve close-up silhouette, holes, stable material addressing, and the
intended projected detail.

A separate distance handoff has landed as
`--brick-exact-micro-distance <metres>`: `0` selects the legacy/default exact
shader, while a positive value selects a separately compiled variant and permits
exact carving only within that camera distance. The preprocessor removes the
comparison entirely from the zero/default variant. An independent exact-quality
change now hoists immutable geometry-program flags out of the microcell loops,
so the current default SPIR-V is intentionally no longer byte-identical to the
pre-feature binary; output equivalence is the acceptance gate. Beyond a positive
cutoff, terrain uses its stable solid fallback and porous materials retain
expected-coverage fallback rather than disappearing. Changing zero/nonzero state
rebuilds the brick pipeline; changing one positive distance to another updates
uniform bits.

Do not confuse this with the rejected projected-pixel threshold. In the
historical LOD-64 SVGF-0, awake-16, tile-20, planet-stride-4 stack, 500 m changes
**30.999 / 33.868 ms** to **24.349 / 26.258 ms p50/p95**; 750 m reaches
**24.877 / 26.859 ms**, and 1000 m reaches **25.109 / 26.855 ms**. The old
balanced combination of SVGF 1, awake 8, tile 20, planet stride 4, and 400 m
reached **24.728 / 26.372 ms** only with the same coarse raster cut. The control
is quality-changing, disabled by default, not p95-qualified, and not yet timed
at LOD 0.75; transition-band and authored-detail review remains mandatory.

### 4. Brick format mismatch

The brick fragment contract is:

| Location | WGSL output / texture | Required pipeline format |
| --- | --- | --- |
| 0 | `surface` | `RGBA32Uint` |
| 1 | `bottomAlbedo` | `R32Uint` |
| 2 | `microPayload` | `R32Uint` |
| 3 | `materialSampleAddress` | `RGBA32Uint` |

The textures and WGSL followed that order, but the graphics pipeline previously
declared locations 2 and 3 as `RGBA32Uint`, `R32Uint`. The current tree corrects
the declaration. Treat this as a correctness and API-validation fix; no speedup
has been measured. Gate it with zero Metal validation errors and byte-exact
round trips through `BrickResolve`, especially a valid micro payload next to a
nonzero signed canonical material address.

### 5. Brick resolve and G-buffer

Native capture assigns 2.271 ms to brick resolve/material and 0.498 ms to the
following G-buffer raster. Today visibility is written to four integer render
targets plus depth, copied into resolved/coverage/depth buffers, shaded into
four storage textures, and then expanded by a full-screen raster into seven
geometry attachments plus depth.

First split #16 in a capture. Then test a fused resolve/material compute kernel
that loads each brick target once, writes the required resolver buffers, and
writes material textures in the same dispatch. Keep the final raster only for
attachments/depth that genuinely need fixed-function output. A more aggressive
compute G-buffer is allowed only if the RHI can represent depth correctly.

Do not shade in the brick fragment merely to remove a pass: overdraw would
repeat expensive material work. Do not narrow IDs, depth, UVs, or canonical
addresses without range and temporal-parity tests. Track attachment bytes,
tile-memory load/store traffic, and transition/encoder count as well as time.

Unused auxiliary MRTs now use exact `DontCare` load actions instead of loading
contents that the pass fully overwrites or never consumes. The provisional
historical LOD-64 matched p50 changed **39.785 → 39.193 ms (-0.592 ms)**, so the
change is retained; p95 was inconclusive and the delta must be repeated at
0.75 px in the final Metal trace. This is a per-attachment proof, not permission
to weaken load actions globally.

### 6. Probe and Tree64 lighting

In the historical initial captures, group #18 grows from 7.611 ms native to
8.205 ms at half scale. That suggests a fixed/ray-count/instance-traversal
problem rather than a conventional full-screen shader, but it needs an
intended-LOD recapture. Separate `FinalHZB`, probe placement/ray generation,
compaction, `SceneTree64DynamicTrace`, hit merge, resolve, and filter there.

The far-field path has two explicit world-space thresholds. Starting at
`--probe-local-light-cull-start`, finite/local direct light fades out while
traced sky and directional-sun visibility fades into an unoccluded analytical
evaluation. At `--probe-analytical-distance` (legacy alias
`--probe-sky-only-distance`) ray generation keeps scheduling/history state but
reserves zero traversal slots. Compaction and Tree64 therefore see only nearer
probe rays; resolve deterministically projects sky and directional sunlight into
SH. Far probes also bypass neighbourhood sharing, whose reprojected cells would
otherwise be overwritten by the analytical resolve, and zero-ray probes return
before light selection and active-list atomics. The default 25 km terrain preset
uses 3.75 km and 10 km.

Traced sky visibility and emitter estimates remain unweighted in temporal
history. Resolve applies the current camera-distance weight only while projecting
SH, adds an analytical complement for sky and the first eligible directional
sun, and gives finite lights no complement. This prevents the long probe history
from lagging behind camera motion and popping at the cutoff. Far analytical
values are stored as `NO_SAMPLE`, so re-entering the traced band wakes the probe
with its full ray budget instead of inheriting an unshadowed prior.

A controlled historical LOD-64 native camera-walk (120 warmup + 600 measured
frames) found p50 / p95 of 38.486 / 41.106 ms with a functionally disabled 1e9 m cutoff,
38.598 / 41.110 ms at the 10 km default, and 38.689 / 41.118 ms with a 2–5 km
transition. Those differences are noise, not a performance win. Keep the 10 km
default for its representation/quality handoff; before tightening it, add
near/far-probe and emitted-ray counters and isolate Share/Rays/Tree64 in Metal.
The pacing sweep predates the temporal-history correctness refinement above;
that refinement does not change the emitted-ray cutoff, but repeat the sweep
alongside the counter capture before promoting a new default. Gate on horizon
energy, sunset colour, local-light disappearance, and camera-motion transition
bands; do not count fixed resolve/interpolate work as ray-trace savings.
The cutoff is deliberately decoupled from distant G-buffer normal/albedo
smoothing, so a cutoff A/B changes only the lighting estimator and its work.

The current probe budget is fixed in **output** space: the cap is 13,432 probes
and 859,648 ray slots at both internal resolutions. Probe graph construction now
avoids allocating the unreachable full-resolution ReSTIR path, temporal ray
histories, receiver maps, and screen-hit resources; required inactive bindings
use one-element stubs. The removed allocation is roughly **480 MiB** at native.
Treat this as a residency win until an isolated trace proves a timing delta.

`ProbeInterpolate` now reuses the raw geometry normal and SH coefficient 0
within each invocation, preserving the exact output signature. Two broader
threadgroup-cache variants (about 5.7 KiB and 1.9 KiB) regressed roughly
0.55–0.60 ms and were removed; barriers/shared-memory pressure outweighed their
load reduction. Do not repeat that cache shape without counters. Continue to
profile `Share`/`Resolve` contention and report allocated, scheduled, live,
awake, compacted, and traced counts.

Quality-changing probe controls show useful but bounded wins in the historical
LOD-64 temporal-only performance stack: awake directions 32 → 16 changed **36.016 →
34.880 ms (-1.136 ms)**, then terrain-preset probe tile 14 → 20 changed **34.880 →
32.813 ms (-2.067 ms)**. Tile 28 did not improve p50, and adaptive refinement
at 100% did not produce a repeatable total-frame win, so both were rejected.
Keep 32 awake rays, the terrain preset's tile 14 (generic non-terrain default
12), and 200% adaptive face capacity as quality defaults until noise, thin-
feature, camera-motion, disocclusion, and cross-surface tests justify a named
lower tier.

Tree64 already has hierarchical empty-space traversal: 4-way levels, stackless
region lookup, and an empty 2 x 2 x 2 child-group skip. Keep the region marcher
as default. `--tree64-traversal mirrored-ancestor` implements the alternative
negative-octant/ancestor path but is slower on the current shallow 4/16/64
trees; do not promote it without new measurements.

The current low-risk change intersects an instance's compact world-space AABB
before fetching/validating its larger scattered model record, in both region and
fast dynamic traversal. A provisional historical LOD-64 isolated A/B improved
total p50 by about **1.57 ms**, but it was order-sensitive and needs a final
matched intended-LOD Metal capture.
Validate identical HIT/MISS/INVALID/QUERY_LIMIT results and measure model-buffer
reads/cache misses before claiming it. The captured dynamic set has 89 terrain
pages: A/B a small TLAS or page bins against the global any-hit loop so a ray
does not test all 89. Preserve fail-unknown behavior for invalid residency;
never turn an unavailable node into a fabricated occluder.

Also A/B fusing `SceneHitMerge` into the trace/final resolve so hit data is not
round-tripped solely for a later selection pass. Sweep awake rays from 32 to 16
only as a quality-changing fallback after compaction, atomic, cache, TLAS, and
fusion work; it must pass variance, leak, disocclusion, and thin-occluder gates.

Record active rays, instances tested/AABB rejected, model loads, node visits,
queries, base-cell steps, highest level, hit/miss/unknown/query-limit counts,
and bytes per ray. Compare Tree64 and packed DDA with the same rays, but remember
that the backends have different data and accuracy contracts.

### 7. Ray compaction

The active-ray list is already dense. `SceneRayCompactReset` copies its count to
Tree64 dispatch arguments, while `SceneRayCompact` performs a 64-lane bitonic
coherence sort. Its nested stages for sizes 2..64 total **21** comparison rounds,
each with a workgroup barrier. A 64-thread group can span SIMD-groups on Apple
GPUs, so “wave-local” must be demonstrated with counters rather than assumed.

A/B three paths: direct active-list order, current bitonic order, and a
subgroup/radix variant. Compare **compaction plus Tree64**, not compaction alone.
If sorting wins overall, consider emitting indirect arguments in worklist
finalize and fusing reset; if it loses, delete the sort. Preserve canonical ↔
compact mapping and probe minimum-distance behavior exactly.

### 8. Histories and readbacks

Production must not wait for diagnostics. Timer resolve is already opt-in, and
the former roughly 2 MB stable-visibility plus roughly 50 MB hit-history
readbacks are gated by `--test-frame-signatures`. Apply the same rule to the
temporary brick cull/draw-argument readbacks, which are still registered on the
normal brick path. Page-request readbacks are functional streaming inputs;
double-buffer and consume them asynchronously instead of removing them.

Probe graph construction now sizes unreachable per-pixel ReSTIR candidates,
reservoir histories, receiver maps, screen hits, and related histories to
one-element binding stubs. At native this removes roughly **480 MiB** of
allocation from the probe mode. The live probe histories still need a future
frame-slot/ring audit; do not alias them merely because another DI mode does not
consume them.

SVGF's depth, normal, instance, and voxel histories are now published directly
from the current G-buffer/merged targets into the current history ring when
there are at least two frame slots and MetalFX does not need an incompatible
override. The copy fallback remains for MetalFX and single-slot configurations.
The direct rings retain transfer-destination usage so camera cuts and explicit
temporal clears remain legal. Validate two frames in flight, resize, graph
rebuild, mode toggle, camera cut, MetalFX, and residency revision—copy removal
is invalid if it exposes a texture being overwritten by an in-flight frame.

Apply the same zero-work rule to graph construction. The full-screen light-
emitter gizmo is now off by default, and the pass is excluded entirely unless
the user enables `--light-emitters` or the UI checkbox. Add static atmosphere-
LUT work only when its inputs are dirty, and remove the redundant graph blit
when MetalFX can consume the producer texture directly. Confirm identical color
space, alpha, navigation composition, and synchronization. Keep all counter/
signature/brick-argument copies behind explicit diagnostic flags.

### 9. SVGF and GTAO

SVGF's original 3.800 → 0.960 ms scale change was nearly ideal quarter-pixel
scaling. Its A-trous shader now computes variance and neighbourhood bounds in
one exact local-statistics walk rather than two duplicate 3 x 3 walks. Broader
threadgroup caching was deliberately avoided, and compatible configurations
publish the four surface histories directly as described above.

The historical LOD-64 iteration-count A/B found that two → one A-trous passes
changed the hot population average **39.848 → 38.529 ms (-1.319 ms)**. In the stack with
exact auxiliary MRT `DontCare`, two → zero (temporal-only) changed **39.193 →
36.016 ms (-3.177 ms)**. Remeasure both at LOD 0.75. Keep two as the quality
default; expose one and zero as named performance tiers and gate settled noise,
ghosting, edge energy, thin features, and disocclusion recovery. `--disable-svgf`
remains a diagnostic ceiling, not an optimization.

GTAO is grouped with compose/gizmo at 0.482 ms native, so first isolate it with
`--no-gtao` and an Xcode capture. Only then sweep slices/steps, half-resolution
AO, or temporal reuse. AO must remain after the denoised light multiplier and
before composition unless a new equivalence proof supports reordering.

### 10. MetalFX and presentation

The half-scale MetalFX tail is 0.887 ms; the native final blit is 0.049 ms.
Inspect texture format/color-space conversions, external-command-buffer waits,
drawable acquisition, and the post-navigation composition boundary. Reuse the
RGBA16F source and motion/depth resources directly where MetalFX accepts them.
Specifically A/B bypassing the redundant RenderGraph blit in the MetalFX path;
the upscaler must retain the same source extent, usage, lifetime, and wait edge.
Do not move navigation/overview composition across temporal upscaling without
an explicit motion/depth/quality design.

Frame generation is a presentation feature, not a renderer optimization.
Performance acceptance runs keep it and vsync off. Separately test interpolated
frame pacing, UI composition, artifacts, and latency with frame generation on.

### 11. Barriers and encoder boundaries

The GPU timeline span—not the sum of timed bodies—is the frame occupancy metric
because it includes barriers and submission gaps. MoltenVK can charge a stall to
the following Vulkan pass, so use the Metal encoder timeline to distinguish a
long kernel from an event wait. The default universal queue is intentional on
Apple; `--separate-gpu-queues` is diagnostic only.

Never “remove barriers” globally. For every proposed removal, identify the
producer, resource/subrange, consumer, required visibility, and why one encoder
or a fused kernel satisfies it. Favor pass fusion and accurate access
declarations. Track render/compute/blit encoder count, store/load actions,
layout transitions, `MTLEvent`/fence waits, inter-encoder gaps, and GPU idle.

## Staged implementation

1. **Freeze the incumbent intended-LOD baseline.** Repeat the current roughly
   24 ms raster-0.75/Tree64-1.5 benchmark after cooldown; archive commands,
   build/device IDs, schema-14 benchmark JSON, temperatures, and a new Metal
   capture with the R2 mode recorded as disabled. Keep the old 42.729 ms
   split-path and 35.696 ms raster-64 captures only as historical data.
2. **Qualify retained no-/low-quality-risk work.** Validate hidden overview,
   brick formats/sibling skip, planet stride 2 + visibility, local probe reuse,
   fused SVGF statistics, direct history publication, allocation stubs, default-
   off emitters, and auxiliary MRT load actions in one correctness matrix.
3. **Use the intended-LOD trace mapping.** Target the 22.517 ms phase-zero brick
   raster first, then the 2.344 ms resolve/material and 0.309 ms G-buffer work.
   Re-capture probes/post at raster 0.75 and Tree64 1.5 before ranking them
   against historical #17/#19/#21–#23 values. Capture tile stores, memory
   traffic, encoder gaps, and an allocation metric.
4. **Exact micro qualification.** Remeasure the historical 16.443 ms no-micro
   diagnostic and 400/500/750/1000 m transitions at raster LOD 0.75; lower p95
   below 25 ms without using LOD 64. Then preclassify LOD/material work before
   the fragment path. Do not revive the measured-slower projected-pixel
   threshold or brick sorter unchanged.
5. **Tree64/probes.** Re-run the AABB-first matched capture, then A/B instance
   bins/TLAS and producer/trace fusion. Independently qualify awake 16 and tile
   20 as named performance tiers; keep default-quality values unchanged.
6. **Data movement/full-screen work.** Gate remaining brick diagnostics, rotate
   live probe histories where legal, and fuse BrickResolve/material/G-buffer
   stages only when counters justify it.
7. **Planet/pixel tiers.** Qualify planet stride 4 and SVGF 1/0 with image/
   temporal tests. Add sector patching or adaptive SVGF only if the updated
   trace still assigns enough critical time to them.
8. **Presentation/barriers.** Remove only proven conversions or encoder splits,
   validate MetalFX separately, and measure frame generation as presentation,
   never as source-renderer throughput.

Re-baseline after every landed stage with both LOD thresholds explicit. Replace
historical coarse-LOD attribution with isolated intended-LOD encoder deltas as
they are captured.

## Validation and measurement

### Build and focused tests

```sh
cmake -S . -B build -DENGINE_BUILD_SAMPLES=ON
cmake --build build --target \
  SparseVoxelRendererSample \
  SparseVoxelOccupancyDDATests SparseVoxelOccupancyDDAGPUTests \
  SparseVoxelTree64Tests SparseVoxelTree64GPUTests \
  SparseVoxelTree64DynamicGPUTests SparseVoxelPlanetOverviewTests \
  SparseVoxelPlanetBackgroundTests SparseVoxelSceneRayTests -j

ctest --test-dir build --output-on-failure -R \
  'SparseVoxelOccupancyDDACPUReferenceTests|SparseVoxelOccupancyDDAGPUReferenceTests|SparseVoxelTree64T1CPU|SparseVoxelTree64GPUReferenceTests|SparseVoxelTree64T7DynamicGPU|SparseVoxelPlanetOverviewTests|SparseVoxelPlanetBackgroundTests|SparseVoxelSceneRayABIContractTests|SparseVoxelProbeBenchmarkMetadataT0|SparseVoxelPlanetCbtPhase4GPU|SparseVoxelBrickOcclusionEquivalenceG22'
```

### Reproduce benchmark configurations

Run from `build/tests`. Benchmark mode omits the expensive output-signature
readback; do not add `--gpu-timings` to the normal pacing run. Always spell out
both LOD thresholds even though schema 14 now records them.

```sh
./SparseVoxelRendererSample \
  --terrain-package "$PWD/polygon-terrain-default/planet.planetpkg" \
  --terrain-lod-levels 5 \
  --lod-threshold 0.75 --tree64-lod-threshold 1.5 \
  --benchmark-lighting perf-native-lod075.json \
  --benchmark-sequence camera-walk --benchmark-warmup 120 \
  --benchmark-frames 600 --benchmark-resolution 1200x720 \
  --render-scale 1.0 --no-taa --disable-metalfx-upscaler \
  --disable-frame-generation --disable-vsync

./SparseVoxelRendererSample \
  --terrain-package "$PWD/polygon-terrain-default/planet.planetpkg" \
  --terrain-lod-levels 5 \
  --lod-threshold 0.75 --tree64-lod-threshold 1.5 \
  --benchmark-lighting perf-half-metalfx-lod075.json \
  --benchmark-sequence camera-walk --benchmark-warmup 120 \
  --benchmark-frames 600 --benchmark-resolution 1200x720 \
  --render-scale 0.5 --metalfx-upscaler \
  --disable-frame-generation --disable-vsync

./SparseVoxelRendererSample \
  --terrain-package "$PWD/polygon-terrain-default/planet.planetpkg" \
  --terrain-lod-levels 5 \
  --lod-threshold 0.75 --tree64-lod-threshold 1.5 \
  --benchmark-lighting perf-native-temporal-lod075.json \
  --benchmark-sequence camera-walk --benchmark-warmup 120 \
  --benchmark-frames 600 --benchmark-resolution 1200x720 \
  --render-scale 1.0 --disable-metalfx-upscaler \
  --disable-frame-generation --disable-vsync
```

### Remeasure native performance profiles at intended LOD

These commands are native 1200 x 720 source-renderer profiles: TAA, MetalFX,
frame generation, and vsync are all disabled. Run them from the Release test
directory so the package path resolves exactly as shown. The generated reports
use metadata schema 14. The historical versions of these commands omitted the
LOD flags and measured raster LOD 64; their 24.728/26.372 ms and
24.349/26.258 ms results must not be attached to the corrected commands below.

Balanced quality-control stack, pending intended-LOD remeasurement:

```sh
cd build-release/tests
./SparseVoxelRendererSample \
  --terrain-package "$PWD/polygon-terrain-default/planet.planetpkg" \
  --terrain-lod-levels 5 \
  --lod-threshold 0.75 --tree64-lod-threshold 1.5 \
  --benchmark-lighting perf-balanced-native-lod075.json \
  --benchmark-sequence camera-walk --benchmark-warmup 120 \
  --benchmark-frames 360 --benchmark-resolution 1200x720 \
  --render-scale 1.0 --no-taa --disable-metalfx-upscaler \
  --disable-frame-generation --disable-vsync \
  --svgf-atrous-iterations 1 \
  --probe-awake-rays 8 --probe-tile 20 \
  --planet-background-stride 4 \
  --brick-exact-micro-distance 400
```

More aggressive quality-control stack, pending intended-LOD remeasurement:

```sh
cd build-release/tests
./SparseVoxelRendererSample \
  --terrain-package "$PWD/polygon-terrain-default/planet.planetpkg" \
  --terrain-lod-levels 5 \
  --lod-threshold 0.75 --tree64-lod-threshold 1.5 \
  --benchmark-lighting perf-faster-native-lod075.json \
  --benchmark-sequence camera-walk --benchmark-warmup 120 \
  --benchmark-frames 360 --benchmark-resolution 1200x720 \
  --render-scale 1.0 --no-taa --disable-metalfx-upscaler \
  --disable-frame-generation --disable-vsync \
  --svgf-atrous-iterations 0 \
  --probe-awake-rays 16 --probe-tile 20 \
  --planet-background-stride 4 \
  --brick-exact-micro-distance 500
```

Neither corrected profile has an accepted timing yet. The intended-LOD pacing
baseline is **24.2-24.4 ms p50 / 25.9 ms p95** on the shipped default path as of
2026-08-14; the earlier 41.7 / 46.0 ms figure measured the R1/R2 split that is
now off by default. No 40 FPS claim applies to either.

Also run `interior-static`, `interior-yaw`, `roof-down`, `camera-walk`, and
`transform`. Use `--disable-terrain-micro-materials`, `--disable-svgf`,
`--no-gtao`, and `--disable-screen-traces` only as named diagnostic ceilings,
one at a time. A pure scale A/B must keep the same temporal/upscaler backend in
both runs; it is separate from reproducing the two recorded captures above.

### Xcode/Metal capture

```sh
make xcode-release
make xcode-open
```

Select the `SparseVoxelRendererSample` Release scheme, set GPU Frame Capture to
Metal, leave API validation off for measurement, add the same launch arguments,
run to a settled scene, and capture comparable frames. Use Metal API validation
in a separate correctness run. For structural headless captures:

```sh
make gputrace TRACE_ARGS="--terrain-package $PWD/polygon-terrain-default/planet.planetpkg --terrain-lod-levels 5 --lod-threshold 0.75 --tree64-lod-threshold 1.5 --resolution 1200x720 --render-scale 1.0 --no-taa --disable-metalfx-upscaler --disable-frame-generation --disable-vsync --frames 3 --skip-output-signature"
open build/tests/frame.gputrace
```

Capture overhead invalidates application frame timings in that run; use it for
resources, dependencies, shaders, and counters. Use the settled benchmark and
Metal System Trace/counter timeline for performance attribution.

### Counters to archive

Counter labels vary by Apple GPU/Xcode version; record the displayed name and
export together with the trace.

- encoder start/end, exclusive duration, command-buffer dependency path, GPU
  active/idle, event/fence/drawable waits, and gaps between encoders;
- vertices invoked, primitives submitted/clipped/rasterized, fragments invoked,
  early/late depth rejects, tiles touched, and attachment load/store bytes;
- compute threads/threadgroups, SIMD occupancy/active lanes, execution width,
  ALU/texture utilization, and barrier/stall reasons;
- device/L2/system-memory read and write bytes, cache hit rate, texture reads,
  storage-buffer reads, and bandwidth utilization;
- renderer counters: visible/occluded/overflow bricks, projected area, overdraw,
  DDA visits/skips/probes, micro steps, active rays, instances/AABB rejects,
  model loads, Tree64 queries/node visits/statuses, probe coverage/fallbacks;
- resource/pipeline validation warnings, especially render-target pixel formats,
  load/store actions, undefined contents, and frame-slot hazards.

## Quality and correctness gates

Every optimization must pass all applicable gates before its timing is accepted:

- **Exact visibility:** unchanged coverage/output hashes for no-quality-change
  paths; identical surface, bottom-albedo, micro-payload, canonical material
  address, hit face, material, and temporal ID. Depth must be bit-identical where
  the algorithm is unchanged, otherwise within a predeclared analytic tolerance.
- **DDA boundaries:** camera inside/outside, positive/negative/axis-parallel rays,
  exact face/edge/corner starts, thin slabs, checkerboard, empty, dense, clipmap
  overlap, and a carved parent whose first candidate misses and must continue.
- **Planet geometry:** no holes, cracks, wrapped-seam discontinuity, horizon pop,
  transition double coverage, missing silhouette, or LOD oscillation across
  surface, grazing, ascent, orbit, and camera-cut paths.
- **Tree64:** identical HIT/MISS/INVALID/QUERY_LIMIT status and hit distance/cell;
  no fabricated occlusion on missing residency; no new query-limit, overflow, or
  unknown-residency events.
- **Temporal:** stable IDs across LOD/residency, correct invalidation on real
  changes, no stale frame-slot reads, no ghosting after motion/cuts, and no
  material/history inheritance after page reuse.
- **Post quality:** no regression in settled noise, instability, edge energy,
  disocclusion recovery, AO halos, thin-feature retention, or MetalFX reactive
  behavior. Review native and half-scale images, not only hashes.
- **Performance:** compare at least three settled runs; report median p50 and p95,
  source FPS, scene span, target encoder exclusive time, and counters. Reject a
  change that merely shifts time to another encoder, increases p95 materially,
  or wins only with diagnostic readbacks/timestamps enabled.
- **Operational:** no Metal validation errors, NaNs, capacity overflow, per-frame
  allocation, synchronous diagnostic readback, or memory-budget regression.

The final success criterion is not the sum of optimistic per-pass savings. It is
a new settled Metal trace with a shorter critical path, matching output/quality,
and improved source-frame p50 and p95 in both native and half-scale profiles.
At the intended raster LOD, the authoritative Metal p50 frame span/cadence is
42.729/42.719 ms and the repeated normal benchmark is approximately 41.7/46.0 ms
p50/p95. The historical 24.728 ms balanced result used raster LOD 64 and does
not satisfy the acceptance criterion. The project-wide native 40 FPS goal
remains open at both p50 and p95.
