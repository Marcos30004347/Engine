# FSR 3.1-class temporal upscaling

Status document. Updated as phases land; each phase states its exit tests and
only closes when they pass.

## Context

The renderer already drives an **FSR2-class** chain in WGSL from
`src/runtime/rendering/gpgpu/TemporalSuperResolutionPass.hpp`:

| shader | role |
| --- | --- |
| `fsr2-reconstruct-dilate-cs.wgsl` | depth dilation, motion reconstruction |
| `fsr2-depth-reactive-cs.wgsl` | reactive mask |
| `fsr2-lock-cs.wgsl` | pixel locks |
| `fsr2-accumulate-cs.wgsl` | the accumulator |
| `fsr2-rcas-cs.wgsl` | sharpening |
| `fsr2-history-copy-cs.wgsl` | history ring |

Structurally that is the same pipeline FSR 3.1 upscaling uses, so this is a
targeted upgrade of the accumulator and lock stages rather than a new pipeline.

**The reported symptom is heavy aliasing, and two input defects were found that
no accumulator can compensate for.** They are Phase 0 and they block everything
else: an FSR 3.1 port on top of them would look identical to what is there now.

## Phases

### Phase 0 — fix the upscaler's inputs — DONE (2026-08-02)

Temporal anti-aliasing needs sub-pixel sample diversity. Two defects remove it.

1. **Jitter is disabled entirely on the default resolver.** `currentJitter` is
   forced to zero when `gbufferResolver == BrickHierarchy`, which is the
   default. The renderer samples the same sub-pixel position every frame, so
   there is no sub-pixel information to average and geometric edges cannot be
   anti-aliased at all.
2. **Jitter amplitude is 4x too small elsewhere.** `jitterScale = 0.25f`
   against a Halton sequence centred on zero gives +/-0.125 px. Covering the
   pixel footprint needs +/-0.5 px, i.e. `jitterScale = 1.0`. At the current
   value 15/16 of each pixel is never sampled.

Both fixed. `haltonSequence`/`temporalJitterOffset` moved out of the sample into
`src/runtime/rendering/TemporalJitter.hpp` so the properties are testable
directly; `jitterScale` defaults to 1.0; every resolver now jitters.

On the brick resolver specifically: the alternation between adjacent cells that
motivated disabling jitter is not a defect to suppress, it *is* the sub-pixel
coverage anti-aliasing is made of. What it genuinely threatens is history
validation keyed on voxel identity - and the probe path already avoids that,
reprojecting on a surface test rather than a voxel id.

Exit tests (`ctest -L temporal.t0`, 3/3 passing):
- [x] T0.1 offsets lie inside the pixel and span >0.8 of it; scale dial is linear
- [x] T0.2 deterministic, periodic on exactly the stated period, no phase at the
      pixel centre, no duplicate phases; phase count follows the upscale area ratio
- [x] T0.3 **no regression, a clear improvement**: with jitter on the brick
      resolver, probe spatial roughness went 0.0256 -> **0.0169** (-34%), flicker
      0.0494 -> 0.0534 and irradiance 0.47 -> 0.54 both inside their usual run to
      run band, valid probes 634 -> 615 (-3%, expected as probes cross surfaces
      more often). The predicted "geometry shake" does not appear in the metrics.
- [x] T0.4 no validation errors, both DI modes
- [!] **T0.3 was judged on the wrong evidence.** It used probe stability, which
      measures the lighting stage and cannot see the upscaler's output at all.
      Visual testing found the screen shaking badly at `jitterScale = 1.0`, and
      the Phase 1 instability metric had in fact recorded it - 0.000039 -> 0.000730,
      a nineteen fold rise in output frame-to-frame difference - which was
      wrongly explained away as "jitter is frame-to-frame variation". A
      converging accumulator absorbs jitter; the output should hold still while
      the input moves. It does not, so `jitterScale` ships at 0.5 as an interim
      and returns to 1.0 once Phase 3 lands. **The amplitude is not the defect -
      the accumulator's inability to resolve it is.**

Tests live in `tests/rendering/TemporalJitterTests.cpp`. They report failure by
return code rather than `assert`, because the test binaries build in the same
Release configuration as everything else and NDEBUG would erase both the
assertion and the call inside it.

### Phase 1 — a metric — DONE (2026-08-02)

"Less aliasing" has to be a number or later phases are tuned blind. Needs a
readback of the final upscaled image plus two measures:

- **temporal stability** — mean absolute difference between consecutive final
  frames on a static camera; this is shimmer
- **edge aliasing** — high-frequency energy sampled along depth discontinuities

Implemented as `temporal-metrics-cs.wgsl` + `TemporalMetricsPass`, reducing the
final upscaled image against the previous frame's accumulated colour into three
atomics, read back and logged as `Sparse voxel temporal metrics:`. Built only
under `--test-frame-signatures`. `--jitter-scale N` added so the input can be
varied without a rebuild.

Exit tests:
- [x] T1.1 metrics computed and logged under `--test-frame-signatures`
- [x] T1.2 metrics respond in the expected direction to jitter, 60 settled
      frames on the static camera:

      | jitter | instability | edge energy | edge fraction |
      | --- | --- | --- | --- |
      | 0.0 (off) | 0.000039 | 0.062202 | 0.015142 |
      | 1.0 (on)  | 0.000730 | 0.059277 | 0.015646 |

      Instability rises with jitter, as it must - jitter *is* frame-to-frame
      variation. Edge energy per edge pixel falls 4.7% and the edge fraction
      rises 3.3%, which together are the signature of anti-aliasing: the same
      edges spread across more pixels at lower per-pixel magnitude. The effect is
      modest but consistent across both sub-measures.
- [x] T1.3 run to run on a fixed path: instability 0.000730 / 0.000724 /
      0.000730, edge energy 0.059280 / 0.058820 / 0.059287. Spread under 1%,
      well inside the 4.7% signal T1.2 needs to resolve.

**The first version of this metric failed T1.2** - the edge figure did not move
at all between a jittered and an unjittered render, because it averaged the
Laplacian over every pixel and Sponza is overwhelmingly smooth surface, so the
aliased minority was diluted to nothing. It now thresholds and averages over
edge pixels only. Recorded because the same mistake is easy to repeat: an image
metric is not an edge metric.

### Phase 2 — mode scaffolding — DONE (2026-08-03)

`TemporalSuperResolutionPass::Mode` selects the accumulator. Both pipelines are
created up front over one shared binding layout and the choice is made at
dispatch, so switching costs nothing and needs no graph rebuild. Exposed as
`--fsr2` / `--fsr31` and as an **Accumulator** combo in ImGui under
*Temporal AA / TSR > Upscaler*.

Exit tests:
- [x] T2.1 both modes selectable at runtime, no graph rebuild required
- [~] T2.2 FSR2 mode unchanged. The FSR2 shader is untouched and the only change
      to its inputs is a uniform struct gaining fields it does not read, so it
      should be bit-identical. It cannot be *demonstrated* bit-identical here:
      instability reads 0.000619 against 0.000613 before the change, a 1%
      difference consistent with the probe-lighting churn documented below
      rather than with a code change. A stricter check needs a deterministic
      lighting input.
- [x] T2.3 superseded - both modes measured directly against each other below
      rather than being checked equal before an algorithm change.

It also carries a concrete acceptance target beyond the mechanical work: with a
correct one-pixel jitter, the accumulator must hold the output still. Today it
does not, which is the shaking. Instability at `--jitter-scale 1.0` is the
number to drive down; **0.000077** (the unjittered floor) is the ideal and
**0.000613** is where the current FSR2-class chain sits.

`TemporalSuperResolutionPass` gains a mode enum. FSR2 and FSR 3.1 accumulate and
lock stages coexist as separate entry points over a shared
`fsr2-temporal-common.wgsl`. Nothing algorithmic changes; this makes the
comparison possible and keeps a working fallback.

Exit tests:
- [ ] T2.1 both modes selectable at runtime, no graph rebuild required
- [ ] T2.2 FSR2 mode is bit-identical to the pre-change output
- [ ] T2.3 Phase 1 metrics equal in both modes before any algorithm change

### Phase 3 — the accumulator — IN PROGRESS, NOT SOLVED (2026-08-02)

> **Read the instrumentation results further down before the older analysis
> here.** The term chain has since been measured, and the conclusion this
> section builds toward - that something between `baseAccumulation` and the
> final `mix` discards the history - is wrong. Retention is fine. The sweeps and
> the identity-rejection finding below remain valid; the diagnosis of where the
> residual lives does not. All instability and edge figures in this section are
> also at half scale; see the metric fix.

**The primary defect is identity-based history rejection, and it is measured.**

`identity_disocclusion` in `fsr2-accumulate-cs.wgsl` compares the pixel's voxel
identity against the four history taps and returns full disocclusion on a miss.
It is then combined as
`disocclusion = max(depthDisocclusion, identityMismatch, 1 - normalConfidence)`,
and a `max` means any one signal can veto the other two. Under a correct
one-pixel jitter the brick resolver flips identity on every voxel-boundary pixel
every frame, so identity vetoes agreeing depth and normal and history is thrown
away - which is why instability scaled *linearly* with jitter amplitude. The
accumulator was not accumulating.

Measured at `--jitter-scale 1.0`, 60 settled frames, via the new
`--no-identity-rejection` diagnostic flag:

| identity rejection | instability | edge energy |
| --- | --- | --- |
| on (current) | 0.000730 | 0.059287 |
| off | **0.000266** (-64%) | **0.042609** (-28%) |

Both improve together, which is the signature of history actually being used:
stiller *and* better anti-aliased at the same time. For reference the unjittered
floor is 0.000039 instability / 0.062202 edge energy, so simply disabling the
test recovers most of the accumulation and delivers a 31% reduction in edge
energy against the unjittered image - real anti-aliasing.

Disabling it outright is not the fix: identity rejection is what prevents
ghosting across genuine disocclusions. The fix is the FSR 3.1 philosophy applied
to exactly this line - **an identity mismatch should lower confidence, not veto
agreeing evidence**. Reject on identity only where depth or normal also
disagree, rather than combining all three with `max`.

**Landed:** identity now lowers confidence instead of vetoing. Depth and normal
corroborate each other and still veto; identity on its own contributes only
`identityAlone = 0.25` of a rejection, rising to a full one where depth or normal
agree that something changed. Measured at `--jitter-scale 1.0`, 60 settled
frames:

| static camera | instability | edge energy |
| --- | --- | --- |
| hard reject (before) | 0.000730 | 0.059287 |
| **weighted (now)** | **0.000386** (-47%) | **0.041855** (-29%) |
| rejection disabled | 0.000266 | 0.042270 |

Edge energy is now slightly *better* than disabling the test altogether, at
meaningfully lower risk. Against the unjittered image (0.062202) that is a 33%
reduction in edge energy - real anti-aliasing, with instability roughly halved.

| interior camera (moving) | instability |
| --- | --- |
| hard reject (before) | 0.000058 |
| weighted (now) | 0.000024 |
| rejection disabled | 0.000023 |

**This moving-camera row is not yet evidence of anything.** Under motion the
weighted version behaves almost exactly like disabling the test, which could be
correct history retention or could be ghosting - and the pre-change 0.000058 is
not a gold standard either, because that configuration was failing to accumulate
at all. The metric cannot separate "correctly rejecting" from "failing to
accumulate", which is precisely the gap T3.2 exists to close.

Next actions:
1. A disocclusion-heavy camera path plus a ghosting measure, so the moving-camera
   row means something. Until then `identityAlone` is tuned on static evidence
   only.
2. **Swept.** Exposed as `identityRejectionStrength` /
   `--identity-rejection-strength`, static camera at full jitter:

   | strength | instability | edge energy |
   | --- | --- | --- |
   | 0.00 | 0.000267 | 0.041601 |
   | **0.15** | 0.000336 | **0.039683** |
   | 0.25 | 0.000385 | 0.041088 |
   | 0.40 | 0.000462 | 0.044140 |
   | 0.60 | 0.000583 | 0.050200 |
   | 1.00 (old behaviour) | 0.000721 | 0.057778 |

   Instability rises monotonically with strength, as it must - more rejection is
   less accumulation. Edge energy does **not**: it has a genuine minimum at 0.15,
   below which it worsens again. A little identity rejection beats none, because
   it discards history that is genuinely wrong and would otherwise smear. That
   minimum is why 0.15 is the default rather than zero, and it is evidence
   rather than taste.

   At 0.15 against the pre-change accumulator: instability -54%, edge energy
   -31%. Against the unjittered image (0.062202): edge energy -36%.
3. Then the remaining FSR 3.1 items - lock rework and luminance instability -
   which are still untouched.

#### The metric was halving every number it reported (fixed 2026-08-02)

Before any of the numbers below can be compared to the ones above them: **every
instability and edge-energy figure recorded in this document prior to this
section is exactly half of the true value.**

`TemporalMetricsPass` allocated its uniform buffer as `frameLocal`, but the pass
builds its binding group once in `ensure()` and keeps it. The group therefore
addressed whichever per-frame instance existed at creation, while `upload()`
wrote the instance belonging to the frame being recorded. With two frames in
flight the shader read an instance nothing had ever written on every other
frame, saw a zero viewport, and returned from every invocation - so the reset
pass zeroed the buffer, the measure pass added nothing, and the readback came
back all zeros. Averaging a frame range therefore averaged in a zero for every
real sample.

The metrics storage and readback buffers are now non-frame-local too: both are
written and consumed inside one frame, and `graph->bufferRead` resolves the base
instance outside the run context, so a frame-local readback hands back another
frame's numbers on the frames where the two disagree.

Ratios are unaffected - numerator and denominator were halved together - so
every conclusion drawn from the earlier sweeps still stands. Absolute figures
from here on are the true ones. The unjittered floor is **0.000077**, not
0.000039, and jitter 1.0 at the shipping defaults is **0.000654**, not 0.000336.
The gap between them is unchanged at 8.5x.

This is the third time in this work that the measurement, not the code, was
wrong in a way that inspection would never have caught. Distrust a metric that
has never been checked against a case whose answer is known independently.

#### The term chain was instrumented, and the hypothesis was wrong

The accumulator now writes its whole history-weight term chain to two debug
targets, which `TemporalMetricsPass` reduces to screen means alongside the
existing figures. They are logged as `Sparse voxel temporal terms:` under
`--test-frame-signatures`.

Static camera, 55+ settled frames:

| term | jitter 1.0 | jitter 0 |
| --- | --- | --- |
| baseAccumulation (constant of the uniforms) | 0.9727 | 0.9727 |
| disocclusionFactor | 0.9911 | **1.0000** |
| reactiveFactor | 0.9996 | 0.9999 |
| motionFactor | 0.9983 | 1.0000 |
| lockContribution | 0.0127 | 0.0033 |
| **historyWeight** | **0.9661** | 0.9726 |

**Nothing collapses `historyWeight`.** The accumulator retains 96.6% of its
history every frame at full jitter, against 97.3% unjittered. The suspicion
recorded above - that the lock status or the reactive term was discarding the
0.97 that `baseAccumulation` produces - is **refuted**: `lockContribution` is
0.013 and `reactiveFactor` is 0.9996, both inert. Retention is not the problem,
and the FSR 3.1 lock rework is not the fix for this symptom.

Confirmed by driving the weight directly. `--accumulation-frames` was added and
swept; `historyWeight` moves a long way and instability does not:

| accumulationFrames | historyWeight | instability |
| --- | --- | --- |
| 12 | 0.9393 | 0.000671 |
| 24 (default) | 0.9661 | 0.000651 |
| 48 | 0.9814 | 0.000643 |
| 64 | 0.9815 | 0.000648 |

Tripling the retained fraction `(1 - w)` changes instability by 4%. Whatever
moves the output does not come through the blend weight.

#### What it actually is: a 0.19% minority that resets completely

A screen mean cannot tell "every pixel keeps a little less" from "a few pixels
keep nothing", and those want opposite fixes. Counting the second directly:

| | jitter 1.0 | jitter 0 |
| --- | --- | --- |
| fraction of pixels with historyWeight < 0.5 | **0.00186** | **0.00000** |
| disocclusionFactor | 0.9911 | 1.0000 |
| instability | 0.000654 | 0.000077 |

0.19% of pixels discard their history **entirely**, every frame, at full jitter -
and exactly zero do so unjittered. The size is consistent: the residual above
the floor is 0.000577, which over 0.19% of the screen is ~0.31 compressed-luma
of change per resetting pixel, about what a pixel restarting from one jittered
sample of a Sponza edge should show. That per-pixel figure is inferred from the
two measurements rather than measured on its own, so treat it as a plausibility
check, not a proof - what is measured is the fraction and that it is zero
without jitter.

Attribution of the per-frame change confirms it comes through the blend and not
the neighbourhood clamp. Splitting `final - history` into
`(1-w)(current - history) + w(clipped - history)` gives 0.000740 for the blend
term against 0.000149 for the rectification term - 5:1. Rectification is not the
residual.

So the shake is **not** an accumulation failure. It is a per-pixel *validation*
failure: a one-pixel jitter flips the surface a pixel resolves on a discrete
voxel resolver, ~0.2% of pixels per frame fail their history test outright, and
those pixels restart from a single jittered sample. `disocclusionFactor` is the
term that carries it, and it is exactly 1.0000 when jitter is off - the
rejection is entirely jitter-induced.

Identity accounts for roughly 60% of it, and buying that back is expensive:

| | disocclusionFactor | instability | edge energy |
| --- | --- | --- | --- |
| identity rejection on (0.15) | 0.9911 | 0.000651 | 0.0772 |
| `--no-identity-rejection` | 0.9967 | 0.000547 | 0.0979 |

Turning identity off recovers 16% of the instability and gives back 27% of the
edge quality. That trade confirms 0.15 as the right default and says the
remaining rejection is the normal-confidence term (depth is already ruled out).

**The next target is therefore surface validation under sub-pixel jitter, not
the lock rework.** A pixel whose resolved voxel or face normal flips because the
sample moved by half a pixel has not been disoccluded, and treating it as a hard
disocclusion is what is left of the shake.

#### Attributing the minority, and a partial fix (2026-08-03)

Testing suspects one at a time by turning each off and watching the screen mean
failed twice, and the reason is structural: the rejecting pixels are 0.19% of
the frame, so any whole-frame average is dominated by the 99.8% that do not
reject and is nearly blind to the ones that do. Counting causes *among rejecting
pixels*, in a single run, settled it:

| among pixels with historyWeight < 0.5 | share |
| --- | --- |
| normal disagrees | 88% |
| identity disagrees | 82% |
| `depthClip` | **0%** (screen mean 0.0000) |

`depthClip` is completely inert on this content and can be dismissed. The other
two fire *together* on the same pixels, which is what made them look immovable
one at a time: a `normalRejectionStrength` sweep from 1.0 to 0.0 changed nothing
on its own, because that dial only demotes the normal when identity confirms the
surface, and on these pixels identity did not.

**Both had to be addressed at once.** Two changes:

1. `identity_disocclusion` searches 3x3 instead of the 2x2 bilinear footprint.
   The history stores one identity per pixel, but a pixel straddling a voxel
   boundary does not have one - jitter lands it on voxel A then B, and the
   accumulated colour is legitimately a mixture. That mixture *is* the
   anti-aliasing. Against 2x2 the alternate voxel routinely appeared in none of
   the four taps, so the test reported a disocclusion on a static camera where
   nothing can have been disoccluded.
2. `normalRejectionStrength` added: a normal mismatch is demoted to a confidence
   term when identity confirms the surface is still under the pixel. Voxel faces
   are axis aligned, so the two faces meeting at a silhouette are perpendicular
   and a half-pixel jitter takes `dot(normal, historyNormal)` to zero.

Static camera at full jitter, 55+ settled frames:

| | rejected fraction | instability | edge energy |
| --- | --- | --- | --- |
| before | 0.00186 | 0.000651 | 0.0771 |
| 3x3 identity only | 0.00182 | 0.000643 | 0.0781 |
| 3x3 + normalStrength 0.25 (**now**) | **0.00149** | **0.000613** | **0.0745** |
| 3x3 + normalStrength 0.0 | 0.00142 | 0.000614 | 0.0739 |

Rejections down 20%, instability down 6%, edge energy down 3%. `0.0` measures
marginally better again and `0.25` is taken instead, on the same reasoning that
put identity at 0.15: ghosting is still unmeasurable, and keeping a quarter of
the veto costs about 1% of edge energy.

**This is a real improvement and it is not the fix.** Instability goes from 8.5x
the floor to 8.0x. The remaining 0.14% are pixels where the jittered sample
lands on a voxel that is absent from the whole 3x3 history neighbourhood *and*
carries a different normal - silhouettes against background or against distant
geometry. Going further needs one of:

- a history that can hold more than one identity per pixel, so a boundary pixel
  can say "my history is a mixture of A and B" instead of naming one;
- dropping geometric rejection where motion is ~0 and leaning on colour
  rectification instead, which is what stock TAA does;
- accepting the residual.

The first two both trade measured shimmer for **unmeasured ghosting**, which is
exactly the trade T3.2 exists to arbitrate. **Do not pick between them before
T3.2 lands.**

#### A second defect found on the way: injected motion

The dilate stage removes the frame-to-frame projection jitter from the motion
vectors, on the assumption that a jittered projection put it there. This
renderer's G-buffer velocity does not contain it, so the subtraction was
manufacturing motion on a static camera:

| dilate stage | motion px/axis | historyWeight |
| --- | --- | --- |
| `rawMotion - jitterMotion` (was) | 0.62 / 0.54 | 0.9236 |
| `rawMotion + jitterMotion` | 0.65 / 0.58 | 0.9213 |
| `rawMotion` (now) | **0.024 / 0.021** | **0.9661** |

Both signs give the same magnitude, which is only possible if the raw velocity
is ~0 and the correction is the entire signal. It cost a further 4% of the
history every frame through the motion term.

Fixed as an explicit `motionIncludesJitter` setting rather than a change to the
shared shader: `testTsrReprojectsStochasticLightingWithoutCurrentFrameClipping`
asserts the subtraction, so the contract stays the default and the sparse voxel
renderer declares that its velocity is jitter-free.

**Open question.** *Why* this G-buffer's velocity carries no jitter is not
established. `sparse_voxel_gbuffer.wgsl` computes
`clipToUv(currentClip) - clipToUv(previousClip)` from two matrices that are each
jittered with their own frame's offset, which should leave the delta behind. It
measurably does not. This is worth chasing, because whatever makes the jitter
cancel may also be affecting the velocity under real camera motion - which is
**unverified**: `--test-camera-yaw` teleports between eight yaw angles every
frame and cannot measure a reprojection, and no smooth-motion path was
available. Do not assume motion vectors are correct under movement.

#### Current state, stated plainly

**The shaking is not fixed**, but it is now localised and quantitatively
explained. At `--jitter-scale 1.0` the output sits **8.5x** above the stable
floor, and that gap is 0.19% of pixels resetting completely every frame rather
than any weakness in the accumulation.

The single number that matters. Static camera, 55+ settled frames, true values
(everything before the metric fix above was reported at half these):

| configuration | instability | x floor |
| --- | --- | --- |
| `--jitter-scale 0` (stable reference) | 0.000077 | 1.0x |
| jitter 1.0, before any fix | 0.001460* | 19.0x |
| jitter 1.0, before the validation fix | 0.000651 | 8.5x |
| jitter 1.0, current defaults | 0.000613 | 8.0x |

\* the previously recorded 0.000730 doubled to the corrected scale, not
re-measured on the current tree.

Edge energy is 0.0745 against 0.1221 unjittered - a **39% reduction**, so the
anti-aliasing half is working well. What is left is a validation minority:
0.15% of pixels still restart from a single jittered sample every frame.

#### Dials swept, and why they are the wrong lever

| dial | range | instability effect |
| --- | --- | --- |
| identity rejection strength | 0.0 - 1.0 | large |
| rectification width (`--temporal-stability`) | 0.0 - 1.0 | moderate |
| accumulation frames | 12 - 64 | **none** (4%) |
| depth threshold | 0.0025 - 0.15 (60x) | **none** |

Depth rejection and accumulation length are both ruled out by sweep. The two
dials that do move it - identity strength and rectification width - move it by
changing *how often a pixel is rejected outright*, which is consistent with the
minority finding and not with a retention problem.

#### Next action

Surface validation under sub-pixel jitter. A pixel whose resolved voxel id or
face normal flips because the sample moved half a pixel has not been
disoccluded; it is the same surface seen from a slightly different sub-pixel
position, and the accumulator currently treats it as a hard disocclusion. The
identity path was already softened to a confidence weight (0.15); the
normal-confidence path still vetoes at full strength and now carries the larger
share of the remaining rejection.

The FSR 3.1 lock rework is **de-prioritised**: `lockContribution` measures 0.013
and cannot be responsible for this symptom.

#### Diagnostic flags added

`--jitter-scale`, `--accumulation-frames`, `--identity-rejection-strength`,
`--no-identity-rejection`, `--temporal-stability`, `--temporal-depth-threshold`.
All read out through `--test-frame-signatures` as
`Sparse voxel temporal metrics:` and `Sparse voxel temporal terms:`.

#### Stationary edge stabilization landed (2026-08-07)

The remaining jitter minority now uses edge-aware temporal coverage rather than
hard per-sample ownership. A 3x3 geometry transition mask identifies voxel,
face, and silhouette boundaries. At those pixels, identity/normal/depth
rejection is softened only while scene motion stays inside the known jitter
envelope; moving geometry restores the full disocclusion path. The same mask
mixes a small positive Gaussian reconstruction into Lanczos, avoiding negative-
lobe ringing without blurring the rest of the image.

The confidence path removes jitter magnitude per axis rather than assuming a
projection-dependent sign. It does not modify the history reprojection UV.

Static fixture, shipping jitter, frames 55-63:

| configuration | instability | edge energy | rejected fraction |
| --- | ---: | ---: | ---: |
| feature disabled | 0.000492 | 0.051945 | 0.00243 |
| edge blend 1.0, softness 0.5 | 0.000494 | **0.050154 (-3.4%)** | **0.00000** |

The moving-camera path still rejects 15-35% of history while crossing the
scene, confirming that the stationary relaxation fades out under real motion.
Full one-pixel jitter remains a stress case; the shipping amplitude is still
0.25 until a disocclusion/ghosting metric justifies raising it.

### Phase 3 — remaining scope

Re-ordered by the term-chain measurement above, which moved the lock rework from
"largest expected win" to "measurably inert here".

- **Surface validation under sub-pixel jitter.** The measured residual, and
  partly addressed on 2026-08-03 (3x3 identity search plus
  `normalRejectionStrength`), which took the rejecting minority from 0.19% to
  0.15% of the frame. The rest is pixels whose jittered sample lands on a voxel
  absent from the whole 3x3 history neighbourhood with a different normal, and
  closing it needs a history that can name more than one identity per pixel, or
  dropping geometric rejection at low motion. **Blocked on T3.2** - both options
  buy shimmer with ghosting, and ghosting is not yet measurable.
- **Luminance instability.** Per-pixel luma stability detects shading changes
  motion vectors cannot explain, so history is dropped locally rather than
  globally. Directly relevant here: probe lighting changes without geometry
  changing, which FSR2 handles poorly.
- **Rectification bounds.** Measured at a 5:1 smaller contribution than the
  blend term, so this is a refinement rather than a fix.
- **Lock rework.** De-prioritised. `lockContribution` measures 0.013 on a static
  camera at full jitter and cannot explain the symptom this phase is chasing.
  Still worth doing for thin-feature behaviour in motion, which is unmeasured.

Exit tests:
- [ ] T3.1 Phase 1 metrics improve against FSR2 on a fixed camera path
- [ ] T3.2 no ghosting regression on a disocclusion-heavy path
- [ ] T3.3 no energy drift on a static camera over 300 frames

### Phase 3 — FSR 3.1 implemented, and measurably worse (2026-08-03)

`fsr31-accumulate-cs.wgsl` implements all three remaining FSR 3.1 items over the
shared common header. It works, it is selectable, and **on this content it loses
to FSR2 and is not the default.** Static camera, jitter 1.0, 55+ settled frames:

| accumulator | instability | edge energy | edge fraction | rejected |
| --- | --- | --- | --- | --- |
| **FSR2 (default)** | **0.000619** | **0.0744** | 0.0173 | 0.00149 |
| FSR 3.1 | 0.000689 | 0.1112 | 0.0122 | 0.00250 |
| FSR 3.1, instability off | 0.000688 | 0.1128 | 0.0122 | 0.00250 |

Instability 11% worse and edge energy **50%** worse - fewer edge pixels carrying
higher magnitude, which per the Phase 1 reading is anti-aliasing being undone.

**Resolved by removing the lock rework (2026-08-03).** FSR 3.1 now ships
luminance instability and instability-scaled rectification bounds over *FSR2's
lock, kept verbatim*, and measures level with FSR2: instability 0.000624 against
0.000619, edge energy 0.074574 against 0.074435 - both inside the sub-1%
run-to-run band. **It is now the default**, with `--fsr2` switching back at any
time without a rebuild. Level rather than better is the honest summary: the
static-camera metrics can barely see what luminance instability targets, which
is shading change without geometry change.

The lock rework is reverted and stays reverted until its mechanism is
understood. What was measured before reverting:

**Bisected (2026-08-03), and it splits two ways.** `--luma-instability-strength
0` zeroes the instability term but *not* `lockContribution`, so that flag never
isolated the lock; forcing the contribution to zero separately does. Edge
energy, static camera at full jitter:

| configuration | edge energy | instability |
| --- | --- | --- |
| FSR2 | 0.0744 | 0.000619 |
| FSR 3.1, lock forced to zero | 0.0864 | 0.000853 |
| FSR 3.1, lock active | 0.1128 | 0.000688 |

So the reworked lock is about two thirds of the regression (0.0864 -> 0.1128)
and something else is the other third (0.0744 -> 0.0864). **Neither is
explained.** One hypothesis was tested and refuted: FSR2's lock block also
raises `thisFrameReactive` when a pixel's luminance drifts from the value it
carried when locked, which the rework had dropped. That line is restored - it is
load-bearing and belongs there regardless - but it did not close the gap.

The mechanism remains open. The puzzle is that the reworked lock can only ever
*raise* the history weight (`max(preLock, floor)`), and more retained history
should smooth edges rather than sharpen them; the measured direction is the
opposite. Suspect the reactive feedback path, where a larger history-to-current
divergence raises `temporalReactive`, which persists in the history alpha and
is read back the next frame.

Two earlier statements in this document were too strong and are corrected here:
"the regression is the lock rework" (it is two thirds of it), and "FSR2's lock
is inert" - `lockContribution` is inert *in FSR2*, but the block computing it is
not, and a lock that does engage actively hurts on this content.

That outcome was predicted by measurement before it was implemented, which is
worth stating: the term chain had already shown `lockContribution` at 0.013 -
FSR2's lock is inert here - and a rework of an inert term has nothing to gain
and the whole existing behaviour to lose.

**The first FSR 3.1 result was a 34% improvement, and it was a bug.** The new
lock raised the history floor without consulting the disocclusion term, so
pixels whose history had just been invalidated were handed it back. The
give-away was in the instrumentation: the fraction of pixels retaining less than
half their history fell to *exactly* zero while the disocclusion term was still
firing on 0.5% of them. Both metrics improve when history is wrongly retained,
because that is what smearing looks like to a shimmer measure - precisely the
failure mode Phase 1 was written to catch, caught. FSR2 has the same structural
hole and gets away with it only because its lock never engages.

Two design notes worth keeping:

- **Luminance instability must not be measured on the pre-accumulation luma.**
  FSR2 fills its four-frame ring with the upsampled current-frame luma, so every
  entry is a differently-jittered sample of the same surface and an edge is
  unstable *by construction* - jitter making edges move is the entire point.
  Measured that way it reports every edge as unstable and drops history exactly
  where anti-aliasing needs it. The FSR 3.1 accumulator writes accumulated luma
  into the ring instead and excludes the current frame from the comparison,
  which asks whether the *output* has settled. That fixed the term but did not
  rescue the mode.
- The four-frame luma ring was already being maintained and shifted every frame
  by FSR2 and **never read for any decision**. That is what made luminance
  instability free to implement.

Exit tests:
- [x] T3.1 Phase 1 metrics compared against FSR2 on a fixed camera path - and
      FSR 3.1 loses, so this closes against the mode rather than for it.
- [ ] T3.2 no ghosting regression on a disocclusion-heavy path - **still the
      blocking gap.** It is what would have caught the lock bug above directly
      instead of by inference from a debug counter.
- [ ] T3.3 no energy drift on a static camera over 300 frames

### Phase 4 — reactive and sharpening — NOT STARTED

Improved auto-reactive generation and RCAS tuning. Probe mode currently writes
reactive = 0 deliberately; that reasoning was specific to FSR2's behaviour and
is revisited here.

Exit tests:
- [ ] T4.1 reactive mask responds to probe lighting change without geometry change
- [ ] T4.2 sharpening does not reintroduce edge aliasing per Phase 1 metrics

### Phase 5 — tune and choose the default — NOT STARTED

Exit tests:
- [x] T5.1 both modes documented with measured numbers (Phase 3 above)
- [x] T5.2 default selected on evidence - **FSR2**, on a 50% edge-energy and 11%
      instability advantage. Revisit if the lock regression is explained, and
      re-run once the probe input is stable.

## LOD transitions blinked the lighting (fixed 2026-08-03)

`sparse_voxel_light_field.wgsl` rejected a reprojected history tap on exact
equality of the **full** identity:

    if (any(textureLoad(historyIdTexture, tap, 0).xy != identity) || ...) continue;

`identity.y` is the temporal id, and on the brick resolver - the default - that
is the *exact brick cell index*. A LOD change replaces it while the surface, its
position and its normal are unchanged, so every tap fails, the pixel loses its
entire lighting history in one frame, and the light blinks. Two sites, both in
the reprojection gather.

Fixed by making the exact id corroboration rather than a requirement, which is
the rule the rest of the renderer already follows - `probe_matches_surface`
comments that reprojection "must be a surface test, never a voxel-identity
test", and `can_reuse` matches receiver position and hit distance with an
explicit tolerance. Instance and face class (`identity.x`) must still match
exactly, so history never crosses to another object or another face direction;
a cell-index mismatch now costs the tap half its weight instead of discarding
it, so an exact match still wins wherever one exists.

Static-camera metrics are unchanged by it, as they should be - there are no LOD
transitions on a static camera, so this is a change that only acts on the case
it was written for. **That also means it is unverified**: confirming it needs a
camera path that crosses LOD boundaries, which does not exist yet and is the
same gap T3.2 leaves.

## Blocking Phase 2: the probe lighting input is unstable (2026-08-03)

Reported visually as regions of the screen dancing between dark and light,
mostly on flat walls. It is real, it is measurable, and **it blocks the FSR2 vs
FSR 3.1 comparison Phase 2 exists to make** - the instability metric cannot tell
an accumulator apart from a light source that is changing underneath it. This is
the Phase 0 lesson again: fix the input before tuning the accumulator.

`--test-frame-signatures` now splits `validProbes` into `uniformProbes` and
`refinedProbes`, because flat walls are served by the uniform tile grid alone (a
flat tile stops the refinement descent at level 1 having placed nothing) and the
combined count cannot separate "the grid is unstable" from "refinement is
churning". Static camera, 55+ settled frames:

| jitter | uniform grid swing | refined swing |
| --- | --- | --- |
| 0 | 2.9% | 5.3% |
| **0.5 (shipping)** | 4.8% | **36.2%** (246-353) |
| 1.0 | 6.4% | **50.1%** (252-402) |

The uniform grid is steady. **Adaptive refinement churns by a third of its set
every frame, and it is jitter-driven.** A region holds a refined probe one frame
and falls back to its tile probe the next - two different brightness levels over
a coherent area - and a probe that exists for one frame has no accumulated cell
history, so it is a raw 64-ray estimate: noisy as well as different.

### The mechanism, confirmed by report and measurement (2026-08-03)

Reported: with TAA/TSR **enabled** the image dances, regions going dark then
light then dark, and **disabling TAA fixes it**. That inverts the earlier
reading in this document, which assumed the black regions were a lighting bug
TAA was *masking*. TAA is not masking it - the jitter is causing it, and the
churn table above already said so (3.5% swing at jitter 0 against 17-26% with
jitter). The wrong conclusion was drawn from the right number.

The chain, in `sparse_voxel_probe_place.wgsl`:

1. `probe_representation_error` hard-rejects on normal:
   `if (orientation < settings.thresholds.x) { return 1.0e6; }`
2. A probe's normal comes from its **seed pixel**, which jitters within its tile
   every frame by design.
3. Voxel faces are axis-aligned, so adjacent faces are perpendicular. A seed
   that lands on a different face flips the probe's normal by 90 degrees.
4. Every pixel in the tile is tested against that normal, so one flip inverts
   representability for **the whole tile at once** - not for a pixel, for a
   region.
5. Refinement demand spikes for that tile, probes are placed, and next frame the
   seed lands back and they vanish.

A whole tile changing brightness in one frame, driven by jitter. That is the
reported symptom exactly, and it explains why the two attempts below were inert:
both worked on the budget arbitration, which is downstream of this.

**Grading the normal test does not fix it** and is the obvious dead end: the
threshold for counting a pixel unrepresented is `error > 1.0`, so softening
1.0e6 to any finite penalty leaves it unrepresented. The fix has to keep the
probe on one surface across frames - when no jittered candidate matches the
previous frame's surface, fall back to the previous probe's own seed pixel
rather than accepting a candidate on a different face.

### Fixed: re-offer the previous seed pixel (2026-08-03)

The anchor only worked when some jittered attempt happened to land back on last
frame's surface. When none did, the tile was *forced* onto whatever the attempts
hit. `probe_place` now offers the previous probe's own seed pixel as one more
election candidate, ranked below every continuing jittered attempt so the probe
still slides across its surface when it can. A tile can still move to a new
surface - only when the old one is genuinely gone, rather than merely unsampled
this frame.

`probeFlicker`, the figure that tracks "tiles bouncing from dark to light":

| jitter | before | after | |
| --- | --- | --- | --- |
| 0 | 0.0458 | **0.0008** | 57x |
| 0.5 (shipping) | 0.0477 | **0.0058** | 8.2x |
| 1.0 | 0.0543 | 0.0203 | 2.7x |

Spatial roughness swing also fell (27.8% -> 8.5% at the shipping jitter) and the
image metrics are unchanged. Note what the *shape* of that table says: before,
flicker sat near 0.05 whatever the camera jitter was, which is the signature of
a term that had nothing to do with camera jitter dominating the measurement. It
now scales with jitter as it should. Most of the probe flicker in this renderer
was the placement seed alone.

The refined probe count still swings ~37%, so the demand churn documented above
is *not* fixed - but the probes themselves now hold their surfaces, which is
what the lighting sees.

### What that churn is not

Two fixes were tried against the budget arbitration and **both measured
completely inert**, leaving the counts byte-identical:

1. An incumbency bonus promoting candidates whose region already held a probe,
   on the argument that an incumbent carries history while a newcomer is a raw
   estimate. No effect. (First written to promote the value *before* binning it
   into the demand histogram, which is inert twice over: incumbents dominate the
   accepted set, so the histogram shifts up one bin and the cutoff derived from
   it shifts up with it.)
2. Forcing **every** candidate to maximum priority, which disables the cutoff
   entirely. Also no effect.

(2) is the decisive one: if removing the budget constraint does not change how
many probes are placed, the budget is not what limits placement, and no priority
or hysteresis scheme applied there can steady the set.

### Black regions with TAA/TSR off: the code path found, the fix unverified

`sparse_voxel_probe_interpolate.wgsl` emits **black** when a pixel finds neither
a weighted probe nor a fallback candidate. The fallback only searched the
pixel's own 2x2 tile footprint, so a pixel whose four neighbouring tiles all
hold invalid probes got nothing - and which tiles are valid churns every frame,
so the region alternates. The temporal upscaler averages that away, which is
exactly why it appears as solid black the moment TAA/TSR is switched off.

The fallback search now widens to 3x3 when the footprint yields nothing. It runs
only in that case, so the common path is unaffected, and it can only replace
black with a poor estimate - it can never darken a pixel that already had one.

**It is not verified.** Probe flicker, roughness and mean irradiance all moved
by less than run-to-run noise, and so did the image metrics - because none of
them can see this failure. They measure *probes*; this is a per-pixel hole in
the *gather*. Judging the change on them would repeat the T0.3 mistake exactly:
a metric that cannot observe the thing it is being used to judge. Verifying it
needs a counter in the interpolate pass for pixels taking the black path, which
is the next step and is the same instrumentation the churn below needs.

### Where it must be

Upstream of arbitration, in the per-pixel representability test that decides how
many regions are unrepresented - which runs against a jittered G-buffer, so the
demand itself moves every frame. That is the same defect class as everything
else this session: a validation test written before Phase 0 turned on sub-pixel
camera jitter, which now flips on a fraction of its inputs every frame.

**Next action:** instrument the descent's demand (unrepresented pixels and
regions per level, per frame) the way the accumulator's term chain was
instrumented, rather than trying a third fix. The pattern this session is
emphatic: every fix aimed by reasoning missed, and every one aimed by
measurement landed.

## The light blinking has a reproducer, and it is not the camera we measured (2026-08-03)

Reported: the light blinks, worst looking down at the floor from above and from
inside Sponza, and it happens with TAA and TSR **off**. Both views reproduce
headlessly. Settled frames, 30+:

| camera | jitter | probeFlicker | meanProbeIrradiance min-max |
| --- | --- | --- | --- |
| default preview (static) | 0.25 | ~0.004 | steady |
| `--test-camera-roof-down` | 0 | **0.099** | 0.123 - 0.400 |
| `--test-camera-roof-down` | 0.25 | **0.149** | 0.074 - 0.392 |
| `--test-camera-interior` | 0 | **0.144** | 0.0019 - 0.0065 |
| `--test-camera-interior` | 0.25 | **0.165** | 0.0007 - 0.0066 |

The probe documentation records roof-down's reference flicker as ~0.002 and a
static camera as ~0.017. These are 10x to 70x above that, and the scene's *mean*
irradiance swings by 3x to 9x from one frame to the next - not a subtle defect.

**Two things this settles.**

It is largely **not** jitter-driven: roof-down at jitter 0 still flickers 0.099,
and interior 0.144. The reported symptom persisting with TAA/TSR off is
therefore expected, and the earlier framing in this document - that the jitter
caused it - was wrong for this defect. (The placement-seed fix above was still
real and still helps; it addressed a different, jitter-driven component and took
the *default preview camera* from 0.048 to 0.006.)

And it explains why every measurement in this document looked healthy while the
renderer visibly blinked: **they were all taken on the default preview camera,
which does not exhibit this.** That camera reads ~0.004; the cameras the user
actually flies read 0.10-0.17. Nothing was being measured where the bug lives.
Use `--test-camera-interior` for probe-stability work from here.

## The light blinking: found, measured and fixed (2026-08-03)

**Cause: `sparse_voxel_tree64_trace.wgsl` fabricated a HIT when the dynamic
layer's data was not publishable.** The `TREE64_ANY_HIT_GLOBAL_DYNAMIC` path
turned `TREE64_STATUS_INVALID` into `TREE64_STATUS_HIT` at the volume entry -
"visibility is fail-closed". A fabricated hit is indistinguishable downstream
from a real occluder, and a screen probe rebuilds its whole hemisphere from one
frame of rays, so whenever a cohort of rays took that branch the probe field
darkened scene-wide and stayed dark until the data published again.

It now propagates `INVALID`, which is what the sibling static/chunk path already
does. It merges to `UNKNOWN_RESIDENCY` in `sparse_voxel_scene_hit_merge.wgsl`,
which every consumer treats as "keep history" - the correct answer for a ray
whose occlusion genuinely is not known this frame.

This is the same branch recorded in the `tree64-dynamic-fail-closed` note, whose
last line was that the dynamic path fabricating a hit "remains a latent trap".
It was.

### It is a collapse, not a flicker, and that is why nothing found it

Binned by frame over 400 settled frames on `--test-camera-interior`, mean probe
irradiance does not sit anywhere. It holds ~0.0065 for tens of frames, drops to
0.0029, and climbs back:

    frames 120-159  irr=0.006764
    frames 160-199  irr=0.006096
    frames 200-239  irr=0.002901   <- the blink
    frames 240-279  irr=0.005200
    frames 280-319  irr=0.006756

Measured as worst-20-frame-window against best, over 400 frames:

| camera | before | after |
| --- | --- | --- |
| `--test-camera-interior` | **2.57x - 2.93x** | **1.14x - 1.38x** |
| `--test-camera-roof-down` | 1.08x | 1.03x |
| default preview | 1.01x | 1.00x |

Interior mean irradiance 0.0058 -> 0.0065, and the worst window 0.0026 -> 0.0057.
Every pre-fix run measured >= 2.57x and every post-fix run <= 1.38x, so the
separation is clean rather than inside the noise.

### The measurement that mattered: this metric has a 3x run-to-run spread

Before any of the above, five runs of **identical flags** on the interior camera:

| run | flickerMean | flickerMedian | irradiance |
| --- | --- | --- | --- |
| 1 | 0.0853 | 0.0626 | 0.006564 |
| 2 | 0.1128 | 0.0919 | 0.005860 |
| 3 | 0.0918 | 0.0693 | 0.006485 |
| 4 | **0.2145** | **0.2000** | **0.003180** |
| 5 | 0.1087 | 0.0874 | 0.005902 |

2.5x on the mean, 3.2x on the median. **Every interior figure recorded in this
document before today is a single sample of a quantity that varies threefold**,
including the 0.144 and 0.165 that the "it is not jitter-driven" conclusion was
built on. The spread was not noise - it was whether a run's measurement window
happened to overlap a collapse episode, which is the defect itself aliasing into
the sampling.

Four dials were swept before that was known, and all four read "inert":
probe trace interval, rays per update, camera jitter (0 to 1.0), and lighting
inputs. **Those verdicts are void**, because the spread is larger than any effect
they could have had. They are not re-run here; the collapse is fixed and the dials
should be re-swept against the 400-frame binned figure if they are wanted.

The lesson this document keeps recording, one level up: it is not enough to ask
whether a metric can see the defect. A metric whose noise floor has never been
established cannot support any verdict at all, and "I ran it once and it read
0.144" is not a measurement.

### What is not fixed

Per-probe flicker on the interior camera is **unchanged** by this: mean 0.111 ->
0.111, median 0.091 -> 0.085. The collapse and the residual flicker are two
different things, and only the first is now closed. The residual is broad rather
than a minority - median 0.085 means the *typical* probe moves 8.5% per frame -
which is a different shape from the accumulator's 0.19% minority and wants a
different investigation. Roof-down (median 0.0077) and the preview camera
(0.0005) are both fine, so it is specific to dim indirect interiors.

## Metrics: two structural blind spots closed (2026-08-03)

**1. Every reduction is now split over a 4x4 screen grid.** A whole-frame mean
divides a localised defect by the fraction of the frame that is fine, which puts
a band covering a fifth of the screen inside the run-to-run noise of every figure
here. Logged as `Sparse voxel temporal regions:` with a `peakRatio` (worst region
over frame mean) as the one number to watch. It immediately showed the interior
camera's bottom row running 3.4x the frame mean, which no existing figure showed.

**Region instability and region edge energy cannot see a defect that is
stable**, because both are differences. A persistently wrong region reads zero on
each. So the reduction also carries **mean luminance and lit-pixel fraction** per
region, logged as `Sparse voxel temporal shading:`. That is the pair that can see
a dark band.

**2. The metrics pass measures what is presented, not the accumulator's output.**
It used to be registered inside the `temporalEnabled` block, so it existed only
when the accumulator did - and the configuration the right-hand-side band is
reported in is TAA and TSR *off*, which is exactly the one nothing could observe.
It now sits next to the blit and reduces `presentOutput` whatever that is.

This was caught by its own first use: with the metric still reading the upscaler,
selecting the overdraw debug view and the coverage view produced *identical*
numbers for two completely different pictures.

`--test-toggle-taa` added, which disables TAA and TSR together at frame 6. That
is a different code path from `--test-toggle-tsr`, which leaves TAA on and only
drops the upscale; the whole temporal chain is absent in the former.

### Probe gather: per-pixel counters, and the black-region fix is verified

Every probe figure was a per-*probe* mean and could not see a per-*pixel* hole in
the gather - probes can all be healthy while pixels between them find none of
them and emit black. `sparse_voxel_probe_interpolate.wgsl` now counts covered,
black, fallback and widened-search pixels, globally and per screen region, logged
as `Sparse voxel probe gather:`.

**The 3x3 fallback widening works, and this is the verification the plan said was
impossible.** Interior camera, TSR off, 921600 covered pixels:

    blackFraction=0.000000  widenedFraction=0.000235  widenedRescueRate=1.0000

Every pixel whose 2x2 footprint found nothing was rescued by the wider sweep, and
**no pixel emits black in any configuration tested** - TSR on, TSR off, or TAA and
TSR both off, on all three cameras.

### The right-hand-side band: probe gather ruled out, not reproduced

The black-pixel counter reads exactly zero in every configuration, so the band is
**not** the probe interpolation black path. It also does not reproduce headlessly
at 1280x720 on any of the three cameras: per-region luminance with TAA and TSR
off is left-right symmetric (roof-down reads 0.1071 / 0.0720 / 0.0768 / 0.1086
across the top row), and no region is dark.

What the region metric *did* find is that **in any debug-view mode the presented
image occupies only the top-left quarter of the screen at renderScale 0.5**:
regions 2, 3, 6, 7 and both bottom rows read exactly 0.0000. `debugOutput` is
allocated at render resolution while the blit targets display resolution. That is
a real localised defect of the same family, found in one run by the region split,
but it is not the reported band and is not fixed here.

## HZB texel addressing was wrong on every coarse mip (2026-08-03)

Three cull shaders mapped a screen UV onto a HZB mip with `uv * storedMipDimensions`.
A mip texel covers exactly 2^level base pixels, so the correct mapping is
`uv * baseDimensions / 2^level`. The two differ whenever the base dimensions are
not a multiple of 2^level, because the stored mip rounds down and the remainder
rows and columns are never reduced into it:

| | mip 4 | mip 5 | mip 6 |
| --- | --- | --- | --- |
| 1280x720 | covers all | loses 16 rows | loses 16 rows |
| 640x360 (renderScale 0.5) | loses 8 rows | loses 8 rows | **loses 40 of 360** |

Scaling UV by the stored size stretches the mip across the whole screen, so every
texel is read at an offset that grows toward the far edge - a brick tested against
an occluder belonging to a different part of the screen, which is how geometry
that is really visible gets culled, near the right and bottom edges, intermittently
as the camera moves bricks across the misalignment. That matches the reported
"bricks disappear near screen edges".

Fixed in `sparse_voxel_brick_cull.wgsl`, `sparse_voxel_lod.wgsl` and
`sparse_voxel_tlas_culling.wgsl` via shared helpers in the new
`sparse_voxel_hzb_common.wgsl`, plus a guard that keeps anything reaching into a
mip's uncovered strip visible - the same answer the test already gives for
geometry that leaves the screen.

**The arithmetic is proven; the fix is not demonstrated against the symptom.**
`hzbCulledNodes` reads 0 on both test cameras - it counts the raster path, not the
brick path - so no existing counter can show bricks being recovered. Confirming it
needs a per-region cull counter on the brick path, which does not exist.

## Overdraw debug view (2026-08-03)

`SparseVoxelDebugMode::BrickOverdraw` (19), selectable in the G-buffer combo or as
`--debug-mode 19`. The brick draw fragment shader counts invocations per pixel -
before any `discard` and before the depth test, because overdraw is how much work
the rasterizer was asked to do - and the resolve pass drains the counter with an
`atomicExchange` into the buffer the present shader reads as a heatmap.

Draining it in resolve rather than in a dedicated reset pass is deliberate: resolve
already runs exactly once per pixel after every draw phase, so one exchange both
publishes the frame's total and arms the counter for the next frame. A separate
reset pass would have to be ordered against two draw phases and two frames in
flight to be correct.

**It costs nothing when not selected**, as asked: the two per-pixel buffers are
allocated as single words and the fragment counter is behind a uniform branch.
Changing the debug mode already forces a graph rebuild, which is what makes that
affordable.

One trap worth recording: the per-frame uniform update rebuilds
`SparseVoxelBrickDrawUniformsGPU` from scratch, so the enable flag set at graph
build was zeroed on frame one and the counter read zero everywhere. The debug
view's first check passed anyway because the metric was still reading the
upscaler rather than the presented image - two blind spots hiding each other.

## A null dereference in `--depth-only` (fixed 2026-08-03)

`gbufferPass->updateMaterialOverride(...)` sat outside its `if (gbufferPass !=
nullptr)` for want of braces, so every configuration that does not build a
G-buffer pass dereferenced null. `--depth-only` segfaults on it, taking out
`SparseVoxelVSVOFileDepthRendererG19` and `SparseVoxelOcclusionDepthEquivalenceG22`.
Both pass again; the suite is 64/65 with only the documented pre-existing
`SparseVoxelSceneRayGPUValidationTests` failure left.


## Brick occlusion culling: 90% of bricks never reached the depth test (2026-08-03)

Reported: culling does not appear to be culling much, and overdraw is high. Both
true, and the cause was that the occlusion test had five ways to answer "visible"
and only one of them was a depth comparison. A screen-wide "occluded vs visible"
pair cannot tell which bail-out is firing, so the counters now attribute it -
logged as `Sparse voxel brick occlusion:` under `--brick-measure-area`.

First reading, interior camera:

    tested=48851 occludedByDepth=3728 bailNearPlane=22721 bailOffscreen=21158
    bailSpan=0 bailDegenerate=0 bailHysteresis=1

**Only 7.6% of bricks reached the depth comparison.** Roof-down bailed zero times
on everything, which is why it culls well and the interior does not: the interior
camera is inside the geometry, so bricks straddle the near plane and hang off the
screen edge constantly.

Four changes, each measured:

**1. Do not run the test on out-of-frustum bricks.** Out of frustum already loses,
so the test cannot change the answer - and every brick behind the eye has corners
with `w <= 0` and took the near-plane bail. This was 42206 wasted tests per frame
and it was burying the real attribution. `bailNearPlane` went to **zero**: the
entire near-plane count was out-of-frustum bricks. There is no near-plane problem.

**2. Clip the screen rectangle to the viewport instead of bailing on it.** The
old comment argued that clamping "compares the brick against a texel it does not
actually cover". That is the wrong way round: a brick is only ever visible where
it is on screen, so its on-screen footprint is exactly the region the test must
cover, and restricting the region can only make the reduced occluder harder to
beat - it culls more, never less. This bail was **57% of everything the interior
camera drew** (1673 of 2918 bricks drawn purely because the test declined to judge
them).

**3. Step to a finer mip when a coarse one does not cover the rectangle.** The
remainder strip a coarse mip drops (see the HZB addressing section) is a region
the pyramid genuinely has no data for, so that bail is legitimate - but each finer
level loses a smaller strip and mip 0 loses none. Stepping down converted every
one of the 625 coverage bails into a real test.

**4. Texel span cap 4 -> 8.** Stepping to finer levels widens the span, so the cap
started firing. The bricks that trip it are the large near ones, which are worth
more than their count suggests because they dominate overdraw:

| cap | drawn | occluded | bailSpan |
| --- | --- | --- | --- |
| 4 | 2022 | 4630 | 322 |
| 6 | 1955 | 4690 | 248 |
| **8** | **1829** | **4816** | 109 |
| 12 | 1798 | 4847 | 12 |

12 buys a further 1.7% of the drawn set for four times the worst-case taps, so 8.

### Result

Interior camera, one settled frame:

| | before | after |
| --- | --- | --- |
| bricks drawn | 2918 | **1829** (-37%) |
| occluded by depth | 3728 | **4823** (+29%) |
| visible area (pixels) | 3775346 | **3032732** (-20%) |
| overdraw | 16.4x | **13.2x** |
| bails that are not a real answer | 2917 | 116 |

Roof-down is unchanged in every figure, which is correct - it never bailed, so
none of these changes can reach it.

For scale, with occlusion disabled entirely the same frame draws 6645 bricks for
5463271 pixels of fill - **23.7x overdraw**. So culling now removes 72% of the
bricks and 44% of the fill.

**Frame time moves 2%** (12.75ms -> 12.44ms median over 200 frames). The fill
reduction is real but this camera is not fill-bound - the CPU summary shows ~10ms
of GPU wait dominated by the lighting chain - so the win is mostly headroom rather
than frame rate today.

### It is lossless, and that is tested rather than argued

Occlusion enabled against occlusion disabled, brick resolver, both cameras:
**identical `outputHash`** while the enabled run draws 1829 bricks against 6645.
The check discriminates by a factor of 3.6 and still matches bit for bit.

Made permanent as `SparseVoxelBrickOcclusionEquivalenceG22`. The brick path had
**no equivalence coverage at all** - the existing occlusion equivalence tests are
pinned to the SVRaster resolver - which is how a test that refused to judge 57% of
what it drew survived unnoticed.

Two things make it a real test rather than a green tick:
- it runs on `--test-camera-interior`, the only camera with enough depth
  complexity to engage culling; on the open cameras it would pass whether or not
  occlusion worked;
- it **fails if culling removed nothing**, so it cannot pass vacuously.

Verified by sabotage: biasing the depth comparison by 0.02 so genuinely visible
bricks get culled makes it fail. A test that has never been seen to fail is not
evidence, and this one has now been seen to fail for the right reason.


## Probe questions answered from the code (2026-08-03)

**Do probes use screen traces? No.** `screenTraceActive = screenTraceEnabled &&
!probeMode` - the screen tracer belongs to the per-pixel path and is deliberately
not registered in probe mode, because compaction and hit merge both branch on it
and telling them it ran made them consume an untouched screen-hit buffer, which
came back INVALID for every probe ray.

**Is jitter accounted for? Yes, twice.** Probes jitter their own seed pixel
within the tile every frame by design, and the camera's TAA jitter rides in
`focalPrincipal`'s principal point, which is what probes reconstruct world
positions with - so probe placement and the camera agree on where a pixel is.

## Probe shadow rays were capped at less than half the traced volume (2026-08-03)

`sceneProbeTraceDistance` was the diagonal of the **occupancy volume**, which
shares the directory's resolution but keeps the fine voxel cell. The rays are
traced against the **Tree64**, which uses the coarsened trace cell and therefore
spans four times as much world:

    probe trace distance = 4434   occupancy extent = 2560
    trace cell = 20               traced extent   = 10240   (scene extent 5120)

So no occluder past ~4434 units could cast a shadow, on a scene whose diagonal is
~8868. Now derived from the structure the rays actually traverse: 17736.

This is the same trap the trace *bias* a few lines above already sidesteps, with
the same comment about the two resolutions no longer being the same. The
reasoning was never carried over to the distance.

**It is not the reported missing-shadow symptom.** Extending the cap fourfold
moves mean probe irradiance by -4% on the interior camera and +2% on roof-down,
so very few rays were reaching it. The fix is right and the cause of "no shadows
past a certain distance" is still open. Ruled out on the way:

- **Shadow traversal LOD** cannot do it. Its error is one-sided - an occupied
  region is reported coarser, never missed - and `shadowTraversalLODMaximumWorldSize`
  is 2.0 against a 20-unit trace cell, so it never engages at all here.
- **Query-limit exhaustion** is the surviving hypothesis: a ray that runs out of
  its traversal budget returns `QUERY_LIMIT`, which merges to a status every
  consumer treats as "not a hit", i.e. unshadowed - and longer rays exhaust it
  sooner. **It is currently unmeasurable**: probe rays do not report through
  `SparseVoxelSceneRayStatistics` (the interior camera logs 26-44 rays, which is
  the per-pixel path). Counting query-limit terminations per probe ray is the
  next step and the prerequisite for judging any fix.

## Normal-coherence SH damping: implemented, measured inert, shipped off

Voxel faces are axis aligned, so where geometry is finer than a pixel the
resolved normal flips between perpendicular axes under jitter, and bands one and
two of the SH irradiance - exactly the part that depends on the normal's
direction - swing with it. That is a plausible mechanism for the reported "tiny
spots that keep switching brightness".

`sparse_voxel_probe_interpolate.wgsl` compares the pixel's geometry normal with
its right and lower neighbours and damps the directional bands toward the DC term
where they disagree, band two twice as fast as band one so the result is a
widening lobe rather than a mismatched blend. Two texture loads, no filter kernel,
and no cost on the flat majority where coherence is 1.

**It does not measure as an improvement**, static cameras at the shipping jitter,
140 settled frames:

| | instability | edge energy |
| --- | --- | --- |
| interior, off | 0.000070 | 0.0274 |
| interior, on | 0.000070 | 0.0285 |
| roof-down, off | 0.003862 | 0.1391 |
| roof-down, on | 0.004046 | 0.1421 |

Identical on one camera and 5% worse on the other. So it ships as
`--probe-normal-damping`, **default off**, rather than as a default that no
measurement supports.

The honest reading is that none of the three test cameras shows the content this
targets - distant sub-pixel voxel detail - so this is a case where the metric
probably cannot see the defect rather than one where the defect is absent. That
is a reason to withhold the default, not a reason to trust the number: validating
it needs a camera path with distant fine geometry, which does not exist yet and is
the same gap as T3.2 and the LOD-crossing path.


## The right-hand-side band: reproduced, localised, attributed (2026-08-03)

It reproduces headlessly. `--test-camera-interior --test-toggle-taa` (TAA and TSR
both off, 1280x720), fallback fraction per 4x4 screen region, row major:

    [0.037, 0.037, 0.031, 0.796,
     0.012, 0.025, 0.017, 0.795,
     0.010, 0.040, 0.023, 0.801,
     0.008, 0.036, 0.016, 0.779]

**The rightmost quarter of the screen, full height, is ~80% fallback** against
1-4% everywhere else. It took logging the fallback counter *per region* to see
it: the whole-frame figure is 21.7%, which reads as a mild elevation.

**The fallback path is wrong lighting, not a hole.** It evaluates a probe that
failed every acceptance test and scales it by `fallbackScale` (0.75), so the
region is shaded from probes that do not describe it, slightly too dark. That is
the reported symptom exactly, and it is invisible to every other figure: probe
flicker, roughness, irradiance and the black-pixel counter are all normal.

### What it is not

- **Not missing probes.** Valid-probe fraction by grid-column quarter reads
  0.823 / 0.874 / 0.820 / 0.829 - the right column's probes exist and are valid
  at the same rate as everywhere else. The gather is *rejecting* them.
- **Not a wrong viewport or a stale resolution.** The interpolate pass logs
  `dispatch=1280x720 principal=(640.0,360.0) focal=(623.5,623.5)
  rasterViewport=1280x720 probes=160x90` - every value correct for the
  configuration.
- **Not scene geometry.** The same camera at 640x360 is uniform: fallback
  0.025-0.086 across all sixteen regions, total 4.7%. Doubling the render
  resolution takes the right quarter from 4% to 80%.
- **Not the plane test.** Flooring the acceptance radius at one voxel - the
  radius is a pure screen footprint and ignores voxel size, so it was a good
  candidate - moved the total from 0.2168 to 0.2145. Reverted.

### What it is

Rejection reasons per covered pixel, leftmost against rightmost screen column:

| reason | left | right |
| --- | --- | --- |
| **orientation** (`dot(probeNormal, pixelNormal) <= 0`) | 0.87 | **3.60** |
| plane distance | 0.12 | 2.00 |
| depth | 0.02 | 0.19 |
| visibility | 0.09 | 0.03 |
| bilinear, normal cone | 0.00 | 0.00 |

Orientation dominates, and the normal-cone count being exactly zero is the
informative part: no candidate ever survives `orientation > 0` only to fail the
0.85 cone. Probes are either on the pixel's own face and accepted, or on a
*perpendicular* face and rejected outright. The right quarter is a region where
the probes systematically land on different voxel faces from the pixels they are
asked to light.

That is the failure the fallback's own comment already describes - "a face
pointing +X surrounded by probes on +Y and -Z faces has a zero or negative dot
with every one of them" - and it is resolution dependent because halving the tile
size in world terms makes each probe's seed pixel land on minor faces instead of
the dominant one.

**Not fixed.** The obvious move is the philosophy that fixed the accumulator's
identity veto: let a perpendicular probe lower confidence rather than veto, so
the strict gather can still produce a weighted result instead of dropping to the
fallback. That changes lighting everywhere, not just here, and there is no
ghosting or light-leak measure to arbitrate it - the same gap as T3.2. Do not
apply it blind.

### Diagnostics added

- `Sparse voxel probe fallback:` - fallback fraction per screen region. This is
  the line that found it.
- `Sparse voxel probe rejects:` - the six gather rejection reasons, summed over
  the leftmost and rightmost screen columns. Column against column, because a
  whole-frame total mixes the broken quarter into three healthy ones.
- `Sparse voxel probe columns:` - valid-probe fraction per quarter of the grid
  width, which separates "no probes here" from "probes rejected here".
- `Sparse voxel probe uniforms:` - what the interpolate pass believes the
  viewport, principal point, focal length and probe grid are.


## The band, continued: a fix that was wrong, and one that helps (2026-08-03)

### The proposed fix would have invented lighting

Last session's conclusion was to let a perpendicular probe lower confidence
rather than veto, by analogy with the accumulator's identity test. **That would
have been wrong.** A probe traces only the *hemisphere above its own normal* -
`sparse_voxel_probe_common.wgsl` uses a hemispherical octahedral map precisely
because "a full-sphere octahedral map wastes half its cells". Evaluating a
perpendicular probe's SH along this pixel's normal therefore reads directions it
never sampled. The orientation gate is **correct**, and relaxing it is not a
confidence question but a fabrication.

The analogy failed because the two tests are not the same kind. Identity was a
*proxy* for "same surface", so softening it lost nothing real. Orientation is a
statement about what data exists.

### What does help: widen the strict gather

When the bilinear footprint yields nothing, the gather now searches one tile ring
further, applying **every** test unchanged - orientation, plane, depth,
visibility - with a distance falloff outside the footprint. Nothing is lit by a
probe that cannot see it, and it is strictly safer than what it replaced: the
fallback it displaces is a *single* probe that failed every test, scaled by 0.75.

Fallback fraction, interior camera:

| | before | after |
| --- | --- | --- |
| 640x360, whole frame | 0.047 | **0.018** |
| 1280x720, left three columns | 0.037 / 0.037 / 0.031 | **0.008 / 0.008 / 0.009** |
| 1280x720, right column | 0.796 | 0.766 |

Four times less wrong lighting over three quarters of the screen. The band itself
barely moves.

### The band, narrowed to one statement

**At render width 1280 the rightmost quarter's pixels sit on voxel faces that no
probe within a 4x4 tile neighbourhood shares.** Per candidate, that column
rejects ~100% on orientation against ~27% on the left.

Ruled out this session, each by measurement rather than argument:

| hypothesis | evidence against |
| --- | --- |
| degenerate G-buffer normals | `degenerateNormal = 0.000` in both columns |
| wrong viewport or stale resolution | pass logs `dispatch=1280x720 principal=(640,360) focal=623.5 probes=160x90` |
| bad position or depth reconstruction | mean depth 471.8 right against 500.7 left, and 480.9 / 501.6 at 640x360 |
| probe tile world size | swept 8/12/16/24: right column 0.766 / 0.732 / 0.734 / 0.705. Tile 16 at 1280 has the *same* grid and world footprint as tile 8 at 640, and still fails |
| camera jitter | at 640x360, jitter 0 and 0.5 are both uniform (0.011-0.031) |
| plane acceptance radius | flooring it at one voxel moved the total 0.2168 -> 0.2145 |
| global refinement budget | at `--probe-adaptive-percent 200` the left three columns reach **exactly 0.000** while the right column does not move at all |

That last row is the sharpest. Refinement demonstrably solves this exact class of
failure - it drives three quarters of the screen to zero - and generates no
demand whatsoever for the fourth. The remaining question is not "how should the
gather treat perpendicular probes" but **why the representability descent asks
for nothing in that column**, and the answer is upstream in
`sparse_voxel_probe_place.wgsl`, not in the gather.

Not chased further this session: the per-tile refinement cap is 8 for a 64-pixel
tile, so it is unlikely to bind, but that is reasoning rather than measurement and
should be counted per tile before it is believed.

### Diagnostics added

`Sparse voxel probe rejects:` now also carries `degenerateNormal` and `meanDepth`
per screen column, which is what eliminated the two reconstruction hypotheses in
one run each.


## The band, sharpened: it is a missing probe, not a rejected one (2026-08-03)

### Not the graph rebuild

Reported as appearing after toggling TAA/TSR off at runtime, which rebuilds the
render graph. Both paths were measured:

- `--test-toggle-taa` (disable at frame 6): the band appears **exactly at frame
  6** and never recovers. Frames 0-5 read 0.007-0.026; frame 6 onward 0.72-0.77.
- `--no-taa` (added; disabled from startup, no rebuild ever): the band appears
  identically, 0.715-0.769.

So the rebuild is not required and the step at frame 6 is simply the
configuration changing. It is the configuration.

### Located to 1/16 of the screen

Splitting the diagnostic into sixteen vertical strips instead of a 4x4 grid:

    [0.002 0.001 0.007 0.011 0.018 0.010 0.012 0.013
     0.007 0.012 0.030 0.001 0.067 0.996 0.962 0.985]

The edge is at strip 12-13, i.e. **x ~= 1040 of 1280**, and the last three strips
are 96-99% fallback. Near total, not statistical - almost no pixel in the right
19% of the screen is served by the strict gather.

### The accounting is now closed, and it points elsewhere

Two `continue`s ran before any acceptance test and were uncounted: the probe
index falling outside the buffer, and the probe's VALID bit being clear. Both
instrumented, both **exactly zero** in every strip. So every candidate reaches
the tests, and the rejection counts account for all of them.

That reframes the earlier reading. The tests run in sequence, so the plane count
is over candidates that **already passed orientation**. Rightmost strip against
leftmost, per covered pixel:

| | left | right |
| --- | --- | --- |
| rejected on orientation | 1.92 | 5.00 |
| **rejected on plane distance** (orientation-compatible) | 0.58 | **3.74** |
| rejected on depth | 0.12 | 0.50 |
| probe index out of range | 0.00 | 0.00 |
| probe not valid | 0.00 | 0.00 |

**There are 3.74 orientation-compatible probes per pixel there, and every one of
them is too far off the pixel's surface plane.** That is not a gather that is too
strict - a probe on a parallel surface a hundred units behind the pixel *should*
be rejected. It means no probe is placed on the surface those pixels are on at
all.

So the earlier framing - "no probe shares the pixel's voxel face" - was half
right and the wrong half was load bearing. Probes with the right facing exist
nearby; they are on the wrong *surface*. The question is why the tile grid puts
no probe on this surface and why refinement, which drives the other three
quarters of the screen to exactly 0.000 at `--probe-adaptive-percent 200`, asks
for nothing here.

Flooring the plane radius at a trace cell (20 world units) was tried against this
and moved the total 0.2168 -> 0.2145, which is consistent: the offending probes
are not marginally off-plane, they are on a different surface entirely.

### Next step

Instrument `sparse_voxel_probe_place.wgsl` the way the gather and the brick cull
now are: per screen region, how many pixels the descent finds unrepresented, how
many regions it wants to refine, and how many refined probes it actually places.
Every hypothesis that can be tested from the consumer side has now been
eliminated, and the demand side has never been measured.


## Placement instrumented: three quarters of the screen get no refinement (2026-08-03)

`sparse_voxel_probe_place.wgsl` now reports, per screen region on the same 4x4
grid: tiles, covered pixels, pixels the first refinement level found
unrepresented, refined probes placed, and tiles that wanted a refined probe and
could not have one. Logged as `Sparse voxel probe placement:`. Everything before
this was consumer-side and could only ever say "no probe served this pixel",
never whether one was ever asked for.

Interior camera, 1280x720, shipping defaults (`adaptivePercent = 25`):

    unrepresented   placed        starved
    1.56 1.31 1.31 6.49    493 472 435 2248    393 331 333 535
    0.51 0.87 0.46 6.50      0   0   0    0    185 283 173 787
    0.69 1.00 0.55 6.55      0   0   0    0    249 363 193 851
    0.72 0.89 0.39 6.52      0   0   0    0    258 319 145 832

Two findings, and the first is not about the band at all.

**Refinement places nothing outside the top row of the screen.** Rows one to
three receive *exactly zero* refined probes, anywhere, while 6229 tiles report
starvation. The whole adaptive budget - 3648 probes, matching the reported
`refinedProbes` - is consumed by the tiles that happen to run first, and tiles
are dispatched in row-major order. The demand histogram and cutoff that exist to
prioritise this are computed from the previous frame and do not reorder anything
within it, so the allocation is first-come-first-served by workgroup id.

That explains an old puzzle recorded above: forcing every candidate to maximum
priority left the counts byte identical, which was read as "the budget is not the
binding constraint". The cutoff is not binding; the *capacity* is, completely,
and it is handed out by position rather than by need.

Raising it to `--probe-adaptive-percent 200` distributes placement across all
rows and takes three of the four screen columns to **exactly 0.000** fallback.
It costs **21%** of frame time (12.77ms -> 15.52ms median over 150 frames), and
capacity saturates at 200% - 800% places an identical 28800. The default is not
changed here: a fifth of the frame for a partial quality win is a call to make
deliberately, and the better fix is to spread the existing budget by demand
rather than by tile order.

**The band is not explained by any of it.** In the right column: demand is
generated (6.5 unrepresented pixels per covered pixel, four to thirteen times the
rest of the screen), probes *are* placed there once the budget allows (6100 in
the top region at 200%), and the fallback stays at 0.763. Raising the per-tile
cap from 8 to 32 changes nothing either.

### Everything now eliminated for the band

Added this session to the list above: camera height (0.12 / 0.25 / 0.40 / 0.60 -
band present at all four, so the repro camera is not simply buried in geometry),
per-tile refinement cap, refinement starvation, and probe index mapping
(`sparse_voxel_probe_index` is `y * width + x`, matching what placement uses).

The band survives every change to placement, budget, tile size, resolution-scaled
probe radius, jitter, camera height and gather search radius, while remaining
fixed to the rightmost ~19% of the screen at any camera position. A screen-space
constant that no content parameter moves is the signature of an addressing or
bounds error, and the gather's own accounting is now complete and clean - so it
is not in the gather.


## The band: FOUND AND FIXED - a ten-bit pixel field (2026-08-03)

It was an addressing error, and the previous session's closing sentence named
its location correctly. It is in `sparse_voxel_probe_place.wgsl`, in the seed
pixel election.

### The defect

The election ranks candidate seed pixels with a single `atomicMin` over a packed
score, and the score carried the **screen** pixel at ten bits per axis:

    let packed = select(1u, 0u, continues) << 23u |
        (attempt << 20u) | (pixel.y << 10u) | pixel.x;

    let seedPixel = min(
        vec2<u32>(winner & 0x3ffu, (winner >> 10u) & 0x3ffu),
        dimensions - vec2<u32>(1u));

Ten bits hold 0-1023. At render width 1280 every candidate from **x = 1024**
rightwards spills x's bit 10 into the y field, so the winner decodes to
`(x - 1024, y + 1)` - a pixel 1024 columns to the left and one row down. Every
tile in the right 256 columns built its probe from a surface on the far side of
the screen.

The same packing appears in the refinement elect (`regionScore`), so refined
probes in that column were misplaced identically. That is why raising the
adaptive budget never touched the band.

### Why it presented exactly as it did

Every previously puzzling observation falls out of this:

| observation | explanation |
| --- | --- |
| fixed to the rightmost ~19% at any camera | 1024/1280 = 80%; a pixel constant, not a content one |
| clean at 640x360 | x never reaches 1024, so the field never overflows |
| tile 16 at 1280 fails where tile 8 at 640 does not, at identical world footprint | the boundary is in screen pixels |
| valid-probe fraction normal in the right column (0.82-0.87) | the probes are perfectly valid - they describe a real surface, just the wrong one |
| `outOfRange` and `notValid` exactly 0.00 | the probe *index* was always fine; only the seed pixel was corrupt |
| 3.74 orientation-compatible probes per pixel rejected on **plane distance** | a probe seeded 1024 columns away often shares a face class with the wall on the right and is nowhere near its plane |
| `degenerateNormal` 0.000, depth reconstruction sane | the probe reads a real, covered pixel - just the wrong one |
| refinement demand 4-13x the rest of the screen in that column | the tile probe genuinely represented nothing there, so the descent asked for probes every frame |

### The fix

The score now carries the candidate's **tile-local** pixel, eight bits per axis.
A tile-local coordinate is bounded by the tile size, which is clamped to 32 where
it is authored, so it cannot overflow at any render resolution. Ordering is
unchanged: within a tile `tileOrigin` is constant, so ranking by local
coordinates is identical to ranking by screen coordinates.

### Measured, interior camera, 1280x720, `--no-taa`

Fallback fraction per 4x4 screen region, row major:

    before  [0.010,0.014,0.010,0.766,  0.006,0.010,0.014,0.769,
             0.003,0.018,0.016,0.765,  0.003,0.013,0.011,0.711]
    after   [0.006,0.006,0.008,0.008,  0.006,0.010,0.014,0.017,
             0.003,0.018,0.016,0.013,  0.003,0.013,0.011,0.004]

**The right column goes from 0.71-0.77 to 0.004-0.017.** The band is gone.

Gather rejections per covered pixel, right screen column: orientation
**17.01 -> 1.83**, plane **9.06 -> 0.22**, depth 0.84 -> 0.01. The right column's
rejection profile is now indistinguishable from the left's (1.44 / 0.24 / 0.02).

Placement agrees from the producer side, which is the independent confirmation:
unrepresented pixels per covered pixel in the right column **6.49/6.50/6.55/6.52
-> 1.40/1.01/1.03/1.08**, in line with the rest of the screen. Starved tiles fall
**6229 -> 4144 (-33%)**, and the refinement budget stops being eaten by phantom
demand: the top-right region took **2248 of 3648** refined probes before and
takes 871 now, while the other three top regions roughly double (493/472/435 ->
1033/905/839).

### Causality, established in both directions

- Reverting the packing reproduces the band **exactly** (0.766/0.769/0.765/0.711,
  identical to baseline). Re-applying removes it. This is the sabotage check
  lesson 4 asks for, and it was run.
- At 640x360 the fix is **bit-identical**: the same four `outputHash` values over
  30 frames, before and after. It is a provable no-op wherever the field did not
  overflow.
- At 1280x720 only the **fourth column of regions changes**. Per-region shading
  luminance in the bottom row reads 0.0078/0.0035/0.0055 before and
  0.0079/0.0035/0.0054 after - unchanged to three decimals - while region 15,
  the only region spanning x = 1024, moves. The change is localised to the pixel
  boundary the mechanism predicts.
- Verified clean at 1690x1022 (the display cap this machine applies to
  `--benchmark-resolution`, so 1920 and 2560 both render there): fallback
  0.000-0.015 across all sixteen regions, where the old code would have broken
  the rightmost 39% of the screen.

`ctest -R SparseVoxel`: 65/66, only the pre-existing unrelated
`SparseVoxelSceneRayGPUValidationTests`.

### The cost: probe flicker on the interior camera rises

This is a real, reproducible regression on one metric and it is not swept under
the result. Median per-probe flicker, 400 frames binned by 20, min/max per the
noise-floor discipline:

| | runs | bins over 0.30 | worst bin |
| --- | --- | --- | --- |
| before | 4 | 2 / 80 | 0.478 |
| after | 5 | 20 / 100 | 0.998 |

Attributed, not guessed at. It is not the attempt-field change (see below):
width-fix-only shows it too. It is not more refined probes (3648 before, 3648
after) nor fewer valid ones (18048 both). What does change is that the right
column's probes now track the real right-hand content instead of duplicating the
calm left-hand surfaces, and that column's valid-probe fraction drops
**0.83 -> 0.73** because the real content there includes uncovered background -
probes toggling valid register as flicker.

So the before-number was flattered: 20% of the probes were measuring the wrong,
quieter thing. That is lesson 1 in reverse - the metric could not observe the
defect, so it scored the defect well. The right reading is that this enlarges the
already-open "residual per-probe flicker on the interior camera" item to cover
the fifth of the screen that was previously excluded from it, and that item still
needs a per-region flicker breakdown before it can be attributed further.

### A latent second overflow in the same expression, deliberately not changed

The previous-frame seed is re-offered as one more candidate at
`PROBE_PLACE_ATTEMPTS << 20u`. `PROBE_PLACE_ATTEMPTS` is 8 and the attempt field
was three bits, so that index overflowed into the continuity bit: the candidate
has always ranked as "non-continuing, attempt zero", not as the comment's
"continuing, attempt eight".

Ranking it as the comment intended was implemented and **measured, and it is
worse** - probe flicker climbs steadily across a 400 frame static run, ending at
median 0.88-0.99 in the last bin of three runs. Pinning the tile to one pixel
stops the seed sliding across its surface, which is what the jitter exists for.
The shipped fix therefore reproduces the existing rank exactly
(`1u << PROBE_PLACE_CONTINUES_SHIFT`), and the comment now records what the code
actually does. Changing it is a separate question with its own measurement.

### Still open, unchanged by this

Refinement starvation. The distribution is unchanged - rows one to three still
receive exactly zero refined probes while 4144 tiles starve - because that is an
allocator-ordering defect, not an addressing one. The demand-ordered allocator
remains the right fix, and it is now the largest known probe-placement problem.


## Out of scope

Frame generation. Separate feature with its own pacing and latency machinery.

## Log

- **2026-08-02** Document created. Phase 0 opened after inspection found the two
  jitter defects.
- **2026-08-02** Phase 0 landed. Jitter extracted and tested, amplitude
  corrected to a full pixel, enabled on every resolver. T0.1-T0.4 pass. The
  brick resolver's stability concern did not materialise; spatial roughness
  improved 34%. Phase 1 (metrics) is next and is a prerequisite for judging any
  accumulator change.
- **2026-08-02** Phase 1 partially landed. T1.2 failed for the edge metric: it
  averaged over the whole image and did not respond to jitter at all.
- **2026-08-02** Visual test: screen shakes badly at `jitterScale = 1.0`.
  Re-reading Phase 1's own numbers, the instability metric had already recorded
  it and the reading was explained away. T0.3 is annotated: it was judged on
  probe stability, which cannot see the upscaler output. Jitter ships at 0.5 as
  an interim; the defect is the accumulator, and driving instability at
  `--jitter-scale 1.0` down toward the unjittered floor is now Phase 3's
  acceptance target.
- **2026-08-02** Depth rejection **eliminated** as a cause: flat across a 60x
  threshold sweep. Rectification width swept and exposed. Residual instability
  is 8.6x the stable floor and localised to the `historyWeight` term chain -
  lock status or reactive most likely. Dials are exhausted; next step is
  instrumenting the individual terms.
- **2026-08-02** `identityRejectionStrength` exposed and swept. Edge energy has
  a real minimum at 0.15 - a little identity rejection beats none - so that is
  now the default. Instability -54% and edge energy -31% against the pre-change
  accumulator. Ghosting is still unmeasured, so the dial is tuned on static
  evidence only.
- **2026-08-02** Phase 3 first change landed: identity mismatch now lowers
  confidence rather than vetoing depth and normal. Static camera at full jitter
  improves 47% in instability and 29% in edge energy, beating even a full
  disable on edge energy. Moving-camera behaviour is unresolved pending T3.2.
- **2026-08-02** Phase 3 root cause found and measured. Identity-based history
  rejection vetoes agreeing depth and normal via a `max`, so a correct jitter
  makes the brick resolver flip identity on boundary pixels and history is
  discarded every frame. `--no-identity-rejection` added as a diagnostic:
  instability -64%, edge energy -28%. The accumulator rewrite now has a specific
  first target rather than a general one.
- **2026-08-03** Phase 2 landed and FSR 3.1 implemented in full
  (`fsr31-accumulate-cs.wgsl`: luminance instability, lock rework, instability-
  scaled rectification). Mode is a dispatch-time choice with no graph rebuild,
  exposed on the CLI and in ImGui. **FSR2 stays the default**: FSR 3.1 measures
  11% worse on instability and 50% worse on edge energy, and disabling the
  instability term changes nothing, so the regression is the lock rework and is
  unexplained. The first run showed a 34% *improvement* that turned out to be a
  ghosting bug - the new lock raised the history floor on pixels the disocclusion
  term had just rejected - caught because rejected-pixel fraction went to exactly
  zero while disocclusion was still firing. T5.1/T5.2 close on this evidence.
- **2026-08-03** Probe lighting instability reported visually and confirmed
  measurable: adaptive refinement churns **36%** of its probe set per frame on a
  static camera at the shipping jitter, against 4.8% for the uniform grid, and
  it scales with jitter. This **blocks Phase 2** - an FSR2 vs FSR 3.1 comparison
  run over a light source that changes underneath it measures the lighting.
  Two fixes aimed at the budget arbitration both measured inert, and forcing
  every candidate to maximum priority proved the budget is not the binding
  constraint at all. The churn is upstream in the representability test. Not
  fixed; instrument the descent's demand before attempting a third fix.
- **2026-08-03** Causes attributed *within* the rejecting minority rather than
  by toggling suspects and watching the screen mean - which had been wrong twice,
  because 0.19% of the frame cannot move a whole-frame average. Normal and
  identity fire together (88% / 82% of rejecting pixels), `depthClip` is
  completely inert (0%). Fixed both at once: the identity search widened to 3x3,
  and a `normalRejectionStrength` that demotes the normal veto where identity
  confirms the surface. Rejections -20%, instability -6%, edge energy -3%; 8.5x
  the floor becomes 8.0x. Real, and not the whole fix - the rest needs either a
  multi-identity history or dropping geometric rejection at low motion, and both
  trade measured shimmer for unmeasured ghosting, so **T3.2 first**.
- **2026-08-03** History-validation dials exposed in ImGui under "Temporal AA /
  TSR": identity rejection and its strength, normal rejection strength, temporal
  stability, depth threshold. There is still **no upscaler mode picker** - Phase
  2 was never started and only one accumulator exists.
- **2026-08-02** Term chain instrumented, and the hypothesis it was built to test
  is **refuted**. `historyWeight` measures 0.966 on a static camera at full
  jitter - nothing collapses it, `lockContribution` is 0.013 and the reactive
  term 0.9996, both inert, and sweeping `accumulationFrames` 12-64 moves
  instability by 4%. The residual is instead **0.19% of pixels discarding
  history outright every frame** (0.00% unjittered), which is a surface
  *validation* failure under sub-pixel jitter, not an accumulation failure. The
  FSR 3.1 lock rework is de-prioritised on that evidence.
- **2026-08-02** The metric itself was defective: a frame-local uniform buffer
  behind a cached binding group made every other frame read a zero viewport and
  report all zeros, so every average over a frame range was **exactly halved**.
  Ratios are unaffected, absolute figures before this point are not. Third time
  the measurement rather than the code was the thing that was wrong.
- **2026-08-02** Second defect found while instrumenting: the dilate stage was
  subtracting a jitter delta this G-buffer's velocity never contained,
  manufacturing 0.62 px/axis of motion on a static camera and costing 4% of the
  history per frame. Landed as an explicit `motionIncludesJitter` setting so the
  shared pass keeps its tested contract. Why the velocity is jitter-free is an
  open question, and velocity under smooth camera motion remains unverified.
- **2026-08-02** Phase 1 closed. Edge metric reworked to threshold and average
  over edge pixels only, plus an edge-fraction figure. T1.1-T1.3 all pass; the
  gate now discriminates at 4.7% against under 1% run to run noise. Phase 2
  (mode scaffolding) is next and is unblocked.
- **2026-08-03** The light blinking is **found and fixed**: the Tree64 dynamic
  trace fabricated a HIT when instance data was unpublishable, and a fabricated
  hit is a real occluder to everything downstream, so the probe field collapsed
  scene-wide for tens of frames at a time. It now propagates INVALID, which the
  sibling static path already does and which consumers read as "keep history".
  Interior collapse ratio 2.57-2.93x -> 1.14-1.38x over 400 frames. Found only
  after establishing that the interior metric has a **3x run-to-run spread**,
  which voids every single-run interior number previously recorded here and the
  four dial sweeps taken against them. Per-probe flicker is unchanged and is a
  separate, still-open problem. Metrics gained a 4x4 region split, per-region
  luminance (the only figure that can see a *stable* defect), per-pixel probe
  gather counters - which verify the 3x3 fallback fix: zero black pixels in every
  configuration - and now measure the presented image rather than the
  accumulator's output. HZB texel addressing corrected in three cull shaders.
  Overdraw debug view added. A null dereference in `--depth-only` fixed.
- **2026-08-03** Brick occlusion culling: attribution counters showed **only 7.6%
  of bricks ever reached the depth comparison**; the rest took one of four
  conservative bail-outs. The near-plane bail turned out to be entirely
  out-of-frustum bricks being tested pointlessly (42206 per frame), and the
  offscreen bail - 57% of everything drawn - was a rectangle that should have been
  clipped to the viewport rather than given up on. With those fixed, plus a
  finer-mip step for the coverage strip and the span cap raised to 8: bricks drawn
  -37%, occluded +29%, overdraw 16.4x -> 13.2x, output bit-identical. Covered by a
  new `SparseVoxelBrickOcclusionEquivalenceG22` that asserts both halves - the
  image is unchanged *and* culling removed something - and that has been seen to
  fail under deliberate sabotage.
- **2026-08-03** Probe questions: screen traces are **not** run in probe mode (by
  design), and jitter **is** accounted for in probe placement (own seed jitter
  plus the camera's, via focalPrincipal). Probe shadow rays were capped at 4434
  units by the occupancy volume while the Tree64 they are traced against spans
  10240 - fixed, but worth only -4% irradiance, so the missing distant shadows
  are still unexplained; shadow traversal LOD is ruled out and query-limit
  exhaustion is the live hypothesis, currently unmeasurable because probe rays do
  not report through the scene-ray statistics. Normal-coherence SH damping
  implemented against the reported "tiny switching spots", measured inert on every
  available camera, and therefore shipped as `--probe-normal-damping` default off.
- **2026-08-03** The right-hand-side band is **reproduced and attributed**, after
  being unreproducible all project. `--test-camera-interior --test-toggle-taa`:
  the rightmost quarter of the screen is 80% *fallback* - wrong lighting from
  probes that failed every acceptance test - against 1-4% elsewhere, and the
  whole-frame figure of 21.7% hides it completely. Ruled out: missing probes
  (valid 0.82-0.87 in every column), wrong viewport (all uniforms correct), scene
  geometry (uniform at 640x360 on the same camera), and the plane test (flooring
  the radius at a voxel moved nothing). The cause is the orientation gate: probes
  land on perpendicular voxel faces from the pixels they light, 3.6 rejections per
  pixel against 0.87 on the left. Not fixed - the fix is to weight rather than
  veto, which changes lighting globally and needs a light-leak measure first.
- **2026-08-03** The band's proposed fix - weight rather than veto on orientation
  - is **withdrawn**: probes trace only the hemisphere above their own normal, so
  a perpendicular probe has no data for the pixel and relaxing the gate would
  fabricate lighting rather than soften a proxy. What landed instead is a widened
  *strict* gather, searching one tile ring further with every test unchanged when
  the footprint yields nothing: fallback 0.047 -> 0.018 at 640x360 and
  0.037 -> 0.008 over the left three columns at 1280x720. The band itself is
  unmoved and now narrowed to a single statement - at width 1280 the right
  quarter's pixels sit on voxel faces no probe within a 4x4 tile neighbourhood
  shares - with degenerate normals, viewport, depth reconstruction, tile world
  size, jitter, plane radius and global refinement budget all eliminated by
  measurement. Refinement drives the other three columns to exactly zero and
  generates no demand for this one, so the cause is upstream in the
  representability descent.
- **2026-08-03** The band is **not** the graph rebuild: `--no-taa` (added) shows
  it identically with no rebuild, and under `--test-toggle-taa` it appears exactly
  at the toggle frame. Located to x ~= 1040 of 1280, where the last three
  sixteenths of the screen run 96-99% fallback. The rejection accounting is now
  complete - the two pre-test skips are instrumented and both read exactly zero -
  and it reframes the cause: the tests are sequential, so the plane count is over
  candidates that already passed orientation, and **3.74 orientation-compatible
  probes per pixel are rejected as off-plane**. Compatible probes exist; they are
  on a different surface. No probe is placed on the surface those pixels are on,
  so the defect is in placement demand, not in the gather. Instrumenting the
  refinement descent is the next step.
- **2026-08-03** Placement instrumented at last. Independently of the band it
  found that **refinement places nothing below the top quarter of the screen**:
  rows one to three get exactly zero refined probes while 6229 tiles starve,
  because the adaptive budget is claimed in workgroup order and tiles dispatch
  row major. `--probe-adaptive-percent 200` fixes the distribution and takes three
  screen columns to exactly 0.000 fallback, for 21% of frame time; capacity
  saturates there. Default left alone pending a demand-ordered allocator. The band
  itself is untouched by placement, budget, per-tile cap or camera height, and is
  now eliminated from the gather as well - its accounting is complete and clean.
- **2026-08-03** The right-hand-side band is **FOUND AND FIXED**. The seed pixel
  election in `sparse_voxel_probe_place.wgsl` packed the **screen** pixel into ten
  bits per axis, so from x = 1024 rightwards x's high bit spilled into the y field
  and the elected seed decoded 1024 columns to the left and one row down: every
  tile in the right 256 columns built its probe from the far side of the screen.
  The score now carries the tile-local pixel, bounded by the tile size. Right
  column fallback **0.71-0.77 -> 0.004-0.017**; the right column's rejection
  profile now matches the left's (orientation 17.01 -> 1.83, plane 9.06 -> 0.22);
  placement demand there **6.5 -> 1.1-1.4** per covered pixel and starved tiles
  -33%. Causality established both ways - reverting reproduces the band exactly,
  640x360 output is bit-identical, and only the region column spanning x = 1024
  changes at 1280x720. Every prior elimination was correct; the constant they all
  pointed at was 1024. Cost: interior-camera probe flicker rises (bins over 0.30,
  2/80 -> 20/100 across matched runs), because that column's probes now track real
  content instead of duplicating the calm left-hand surfaces - the old number was
  scoring the defect well. `ctest -R SparseVoxel` 65/66, pre-existing failure only.
- **2026-08-03** Two FSR deviations from the reference algorithm found by reading
  the chain against it, implemented behind independent flags, and measured
  separately on static-camera Sponza (`--test-obj-preview sponza.obj
  --voxel-size-meters 5 --lod-threshold 1.5 --test-camera-interior
  --test-frame-signatures --frames 80`, 55+ settled frames, three or more runs
  per config because the noise floor here is wide enough to invent a result).

  **① The Lanczos kernel weight was computed and discarded.** `compute_upsampled`
  has always produced `upsampled.weight` - the summed kernel weight this output
  pixel's centre received from the source grid - and nothing read it. The blend
  was driven entirely by `baseAccumulation`, a constant of the uniforms, so one
  screen-wide rate stood in for the quantity that actually varies at a 0.5 render
  scale. The consequence that matters is not the steady state but the **absence
  of a ramp**: a pixel disoccluded last frame holds exactly one frame of evidence
  and was then handed 0.966 retention, locking a single sample in as though it
  were thirty frames of history. `accumulationWeighting` blends against a
  reprojected per-pixel accumulated weight instead (new R16Float history ring,
  bindings 13/26), giving 1/n until the pixel reaches the cap - which is set in
  the units `accumulationFrames` already meant, so at the cap the ratio reduces
  to `baseAccumulation` exactly.

  | config | runs | instability | edge energy | edge fraction | historyWeight |
  | --- | --- | --- | --- | --- | --- |
  | baseline | 3 | 0.00007 x3 | 0.0276 / 0.0279 / 0.0271 | 0.00117 / 0.00124 / 0.00088 | 0.848 |
  | ② only | 2 | 0.00007 / 0.00008 | 0.0280 / 0.0273 | 0.00126 / 0.00095 | 0.847 / 0.849 |
  | ① only | 2 | 0.00005 x2 | 0.0274 / 0.0270 | 0.00073 / 0.00074 | 0.877 |
  | ①+② | 4 | 0.00005 x3, 0.00008 | 0.0272 / 0.0267 / 0.0303 / 0.0273 | 0.00066 / 0.00051 / 0.00129 / 0.00069 | 0.877 |
  | ① shipping | 2 | 0.00005 x2 | 0.0269 / 0.0270 | 0.00058 / 0.00073 | 0.877 |

  Instability **-29%** and edge fraction **-39%**, reproducible across seven runs
  with ① against five without. **Edge energy does not move** (0.0276 mean without,
  0.0271 with, excluding one outlier run) - so this steadies edges rather than
  sharpening them, and the "edges look bad at 0.5 scale" complaint is only
  partly addressed. Default on.

  Two diagnostics change meaning and must not be compared naively against
  earlier entries: `historyWeight` 0.848 -> 0.877 is now a *distribution* mean
  over ramping and settled pixels, and `rejectedFraction` 0.051 -> 0.082 counts
  ramping pixels (weight < 0.5 by construction while accumulating) as well as
  rejecting ones. They move in opposite directions because the shape changed, not
  because retention got worse - and both improved image figures rule out harm.

  **② Accumulation ran on raw linear HDR.** Nothing in this renderer tonemaps or
  applies exposure and its finite lights ship at intensity 12, so the Lanczos
  lobes, `box.boxVec` and the AABB clip were all operating where a single bright
  tap sets the box and the negative lobes overshoot in proportion to it.
  `tonemapAccumulation` compresses in the dilate stage and expands at the
  accumulator's final store, keeping the history texture linear for RCAS.
  **Measured inert** - two runs indistinguishable from baseline on every figure.
  Default **off**: it also silently redefines `reactiveScale` and the 0.015
  rectification margin, both fitted in linear space, and shipping it on the
  strength of the argument alone is what the reverted lock rework already cost.
  `--tonemap-accumulation` enables it. Judging it needs a ringing measurement the
  harness does not have; edge energy is a magnitude and a ring adds energy on
  both sides of an edge, so it can cancel.

  **Not addressed, in priority order.** `ReconstructPrevDepth` is absent
  entirely: `fsr2-depth-reactive-cs.wgsl` compares source-resolution dilated
  device depth against the output-resolution history depth the accumulator wrote,
  with a raw smoothstep on reverse-Z, and the comments in that same file explain
  why that quantity is unreliable under translation before the non-stochastic
  path (the one this renderer runs) uses it anyway. `fsr2-rcas-cs.wgsl` is not
  RCAS but an unsharp mask with a contrast gate, so it cannot bound its own
  overshoot and amplifies resampling residual. `find_surface` picks the
  nearest-depth pixel of a 3x3 but the depth-clip and reactive-mask lookups still
  use the centre's `sourceUv`. `compute_upsampled` centres its 4x4 window on the
  *jittered* source position while measuring sample offsets from the unjittered
  one. `kernelBias` reads only `downscaleFactor.x`.
- **2026-08-03** RCAS, and **a defect in the edge metric that invalidates part of
  the entry above**. `fsr2-rcas-cs.wgsl` was not RCAS but an unsharp mask, which
  differs in the two ways that decide whether sharpening a temporally upscaled
  image helps: it cannot bound its own overshoot (so it haloes hard edges and
  multiplies up whatever sits in a flat neighbourhood, which after a 0.5-scale
  reconstruction is per-frame resampling residual), and it mixed the image toward
  a box blur by `0.22 * stabilityFactor` - 15% at the shipping stability - and
  then sharpened *that*. Real RCAS solves per pixel for the strongest filter that
  still cannot exceed the local ring min/max. Its limiter assumes the signal is
  bounded by one, so the cross is range-compressed inside the pass regardless of
  `tonemapAccumulation`; that is a precondition of the algorithm here, not the
  same choice the accumulator makes.

  **The metric could not see any of it, and the reason generalises.**
  `temporalEdgeEnergy` is `sum / edgePixels` where both are gated by the *same*
  `TEMPORAL_METRICS_EDGE_THRESHOLD`. Sharpening admits more pixels, the newly
  admitted ones are the weakest, and the mean falls as fast as the sum rises - the
  two cancel by construction. The conditional set is also only ~0.08% of the
  screen here (~740 pixels of 921600), so it has no statistical power either.
  Sweeping `--sharpen-strength` (newly exposed; it was hardcoded to 0.20 at the
  call site, so the stage had never been swept) from 0 to 1.0 moved it not at all.
  Fixed by reporting `edgeDensity` = the same sum over *total* pixels: no moving
  denominator, and flat surface still contributes nothing because it never enters
  the sum. `temporalRegionEdgeEnergy` has the same defect and is not yet fixed.

  Against the fixed figure, two runs per point, static-camera Sponza:

  | sharpen | instability | edgeDensity |
  | --- | --- | --- |
  | unsharp 0.0 | 5.02e-5 / 5.22e-5 | 1.52e-5 / 1.67e-5 |
  | unsharp 0.5 | 5.39e-5 / 5.41e-5 | 1.90e-5 / 1.58e-5 |
  | unsharp 1.0 | 5.22e-5 / 5.65e-5 | 1.58e-5 / 2.40e-5 |
  | RCAS 0.0 | 5.49e-5 / 5.30e-5 | 2.45e-5 / 2.00e-5 |
  | RCAS 0.5 | 5.98e-5 / 6.17e-5 | 2.21e-5 / 5.24e-5 |
  | RCAS 1.0 | 1.008e-4 / 1.042e-4 | 3.55e-5 / 4.15e-5 |

  - **The unsharp mask's dial is inert across its whole range.** 0 to 1.0 moves
    neither figure outside noise, so the old sharpener was only ever contributing
    its unconditional blur. That is the defect, more than the overshoot.
  - **RCAS's dial is live and monotonic on instability**, 5.4e-5 -> 6.1e-5 ->
    1.02e-4, with tight repeats. Full strength doubles shimmer, so it is a real
    tradeoff dial rather than a free win.
  - **RCAS at 0 against unsharp at 0** is the blur removal in isolation:
    edgeDensity +38% for instability +6%. This is the part that addresses "edges
    look soft at 0.5 scale", and it costs almost nothing.

  Default `robustSharpen` on, `sharpenStrength` left at 0.20 - deliberately the
  minimal change, so what ships is "the blur is gone and the sharpener is now
  bounded". `edgeDensity` still shows a 2.4x spread between repeats at one point,
  so **do not tune the strength default on it**; 0.5 and 1.0 are worth judging by
  eye. `--no-robust-sharpen` and `--sharpen-strength N` are the dials.

  **Correction to the previous entry:** the "② range compression measures inert"
  conclusion was drawn on `temporalEdgeEnergy` and is therefore *unsupported*
  rather than established - the instrument could not have seen a ringing change.
  Its `historyWeight` being unmoved is still evidence it does not disturb the
  accumulator, and instability (full-screen, no threshold, tight repeats) remains
  the one trustworthy figure in the harness - so the ① result stands. ② should be
  re-judged against `edgeDensity`.
- **2026-08-03** `ReconstructPrevDepth` implemented, and **left off by default on
  the evidence**. FSR2 has a whole pass for this and the pipeline never had it;
  `fsr2-depth-reactive-cs.wgsl` instead differenced source-resolution reverse-Z
  device depth against the output-resolution history depth the accumulator wrote,
  which its own comments call unusable under camera translation before the
  non-stochastic path used it anyway.

  Built: `fsr2-reconstruct-clear-cs.wgsl` plus a scatter in the dilate stage into
  a frame-local `u32` storage buffer (a buffer, not a texture - **WGSL has no
  texture atomics**), keeping per previous-frame pixel the nearest current-frame
  depth that reprojects there. The consumer asks the question a disocclusion test
  actually has to ask - "does a nearer surface also claim the history I am about
  to read" - rather than "did depth change here", which camera motion alone
  answers yes to. Depths are compared as **view distances**,
  `B / (deviceDepth + A)` with A and B taken straight from the projection's (2,2)
  and (2,3), so reverse-Z, normal Z and an infinite far plane all work without the
  pass knowing which is in use.

  Also added `--test-camera-walk`, exposing the smooth sinusoidal translation
  that previously only existed inside the `camera-walk` benchmark sequence. It is
  **the only camera here that can exercise reprojection at all** -
  `--test-camera-yaw` teleports between eight angles per frame and the interior
  camera does not move - and it is essentially deterministic: repeat runs agree to
  four significant digits, a far tighter noise floor than the interior camera.

  | config | instability | edgeDensity | historyWeight | depthClip |
  | --- | --- | --- | --- | --- |
  | old signal, static | 5.49e-5 | 2.34e-5 | 0.8768 | **0.0000000** |
  | old signal, walk | 5.48e-5 | 1.63e-5 | 0.8767 | **0.0000040** |
  | new, walk, sep 0.02 | 5.74e-5 | 1.82e-5 | 0.8555 | 0.0228 |
  | new, static, sep 0.02 | 5.58e-5 | 2.85e-5 | 0.8578 | 0.0212 |
  | new, walk, sep 0.005 | 9.55e-5 | 4.71e-5 | 0.7210 | 0.1538 |
  | new, walk, sep 0.08 | 5.52e-5 | 1.64e-5 | 0.8727 | 0.0051 |

  **The old signal is inert under motion** - 4e-6 mean clip while walking through
  Sponza, where surfaces genuinely disocclude at every silhouette. So the pipeline
  has effectively had *no* depth-based disocclusion rejection at all, which is
  worth knowing independently of the replacement.

  **But the replacement fires as hard standing still (0.0212) as moving (0.0228),
  and that is disqualifying.** A disocclusion signal that reads the same whether
  or not the camera moves is measuring something else. It is measuring footprint
  straddling: jitter makes every pixel's reprojected bilinear footprint overlap
  its neighbours, so a silhouette pixel always finds a nearer surface claiming part
  of its history - but on a static camera that surface was there last frame too and
  the history already holds the correct anti-aliased mixture. Rejecting it is
  wrong, and it costs 1.9% of history every frame for no measured gain. This is the
  same trap the accumulator's identity test documents: a pixel straddling a
  boundary does not *have* one surface, and that mixture is the anti-aliasing.

  Two intermediate versions were measured and discarded on the way, both showing
  up as static-camera false positives: deriving the previous-frame position from
  the **jittered** pixel grid on the scatter side while the reader removed jitter
  first (plus a half-texel disagreement) - now a shared
  `reconstructed_depth_position` helper so the two sides cannot drift - and taking
  the **nearest of the four taps** instead of weighting them bilinearly, which
  reaches into whichever neighbour the footprint touches and raised instability
  (6.98e-5) instead of lowering it. Neither was the root cause.

  **What it needs:** the previous frame's reconstructed depth to compare against,
  so "the nearer surface just arrived on this footprint" separates from "it was
  always there" - state the pass does not keep. Failing that, the identity
  corroboration the accumulator already uses for exactly this problem. The
  separation dial is live and monotonic across 0.005/0.02/0.08, so once the
  false-positive floor is gone it is tunable. `--reconstructed-depth` enables it,
  `--depth-separation-scale N` sets the threshold.
- **2026-08-03** Auto-exposure implemented - the last wholly absent FSR stage -
  and the range-compression question it was expected to answer is now **settled in
  the negative**.

  `fsr2-exposure-cs.wgsl`, two entry points in one module because a reduction
  cannot read its own total: a workgroup tree reduction of log-luminance (one
  atomic per 8x8 group rather than per pixel, a 64x reduction in contention on a
  single address) into a fixed-point accumulator, then FSR's photographic
  derivation from the geometric mean - ISO 100, K = 12.5, q = 0.65 - clamped so one
  degenerate frame cannot blow out the chain. Computed on the GPU inside the pass
  and read from a buffer, because routing it via the CPU would cost a readback and
  a frame of latency, and an exposure that lags its own image is worse than none.
  Applied before compression in the dilate stage, undone at the accumulator's
  store, and applied again in RCAS - whose limiter has the same [0,1) precondition.
  History stays stored *un-exposed*, so a frame whose exposure differs from the one
  that wrote the history still reads it correctly.

  The old `fsr2-reconstruct-clear-cs.wgsl` became `fsr2-frame-reset-cs.wgsl` and
  clears the exposure accumulators too - same "clear before anything accumulates"
  barrier, one fewer dispatch. `createPipeline` now takes an entry name.

  All figures on `--test-camera-walk`, which repeats to four significant digits:

  | config | instability | edgeDensity |
  | --- | --- | --- |
  | compression off (shipping) | 5.524e-5 / 5.520e-5 | 1.764e-5 / 1.761e-5 |
  | compression only | 5.484e-5 / 5.480e-5 | 1.605e-5 / 1.615e-5 |
  | compression + exposure | 5.564e-5 / 5.568e-5 | 1.139e-5 / 1.141e-5 |
  | + box scale 2 | 5.428e-5 | 1.240e-5 |
  | + box scale 4 | 5.376e-5 | 1.377e-5 |
  | + box scale 8 | 5.396e-5 | 1.389e-5 |

  **Range compression buys no stability and costs edge energy**: -8.6% edge density
  on its own for a 0.7% instability change, and exposure quadruples the cost to
  -35%. The mechanism is now understood rather than guessed. `box.boxVec` is a
  standard deviation, and how much it constrains anything depends entirely on the
  space it is measured in: on unexposed linear HDR it is set almost wholly by the
  brightest tap and is effectively **inert**, so compression does not "fix" the box
  - it *switches it on*, and a tight box clips exactly the accumulated sub-pixel
  detail anti-aliasing is made of. The FSR 3.1 accumulator's own comment about a
  looser box preserving sub-pixel detail says the same thing from the other side.

  So `rectificationBoxScale` was added to re-fit the box for the space it is now
  measured in, and swept. It recovers part of the loss and improves instability
  while doing so, but **saturates by 4.0 and never reaches the uncompressed
  baseline**: the best point is -2.7% instability for -22% edge density. On content
  whose complaint is soft edges that is still the wrong trade, so this is not a
  tuning failure - compression is a net negative on this content and
  `tonemapAccumulation` stays off. Exposure is gated on it and therefore inert by
  default; both are correct and available for FSR parity behind
  `--tonemap-accumulation`, `--no-auto-exposure` and
  `--rectification-box-scale N`. This **supersedes the "unsupported rather than
  established" note** two entries up: it is now measured, on a metric verified to
  respond and a camera that repeats, and the answer is no.

  **Still not implemented: shading-change detection**, FSR 3.1's actual
  anti-ghosting mechanism and the piece this renderer has the strongest case for
  (probe lighting changes while geometry does not). It compares luminance across
  *pyramid mips* between frames, and what was built here is a scalar reduction, not
  a mip chain - so it needs a real pyramid with per-mip storage views, plus the
  previous frame's mips retained. Also absent: reactive/transparency mask autogen
  (the pass consumes a caller-supplied disocclusion mask instead), and the debug
  view. `temporalRegionEdgeEnergy` still carries the conditional-mean defect.
- **2026-08-03** Shading-change detection implemented - the last named FSR 3.1
  algorithmic piece - and **off by default because no benefit could be
  demonstrated**, which is a different verdict from the two before it.

  `fsr2-coarse-luma-cs.wgsl` reduces the current colour to one log-luminance cell
  per 16x16 source pixels; the accumulator samples that grid at the pixel's source
  UV and the previous frame's grid at the *reprojected* UV, and charges the
  difference - in stops, so it is brightness-independent - as a multiplicative
  history term next to instability. The previous frame's grid is a new history ring
  (bindings 28/29). Coarse on purpose: at 16x16 the jitter and aliasing average out,
  so what survives is a change in shading rather than in where the samples landed.
  Reprojecting the previous grid is equally load-bearing - without it, camera motion
  slides the grid under every pixel and the whole screen reports a change.

  It is the only term in the accumulator that can react to lighting moving under
  stationary geometry, which every geometric validation term correctly calls
  "nothing happened". That gap is real and is what ghosts when probe lighting
  changes.

  **Two defects of mine, found by measurement, worth recording as traps.**

  1. The reduction excluded pixels below a luminance floor, so a cell's geometric
     mean was taken over a **varying population**: a cell straddling a lit/unlit
     boundary changes which pixels qualify from frame to frame and its mean swings
     by many stops for no change in the lighting. Symptom that gave it away - the
     threshold was *irrelevant*: raising it fourfold changed the measured history
     cost not at all, because the firing cells were saturating it. The floor is now
     applied by clamping, so the population is fixed. After the fix the term is
     inert on the walk camera at every strength (historyWeight 0.87666 / 0.87664 /
     0.87665 at 0 / 0.5 / 1.0), where before it cost 3.3%. **The entire apparent
     cost was this bug.**
  2. The static interior camera's run-to-run spread is wide enough that two runs of
     the *same* config gave instability 5.62e-5 and 6.58e-5, edgeDensity 2.86e-5 and
     4.29e-5. An earlier reading of a strength trend on that camera was inside its
     own noise and was withdrawn. Only `--test-camera-walk` is tight enough to
     support a verdict here.

  **What it measures, after the fix.** The dial is live and monotonic - stops 0.0 /
  0.05 / 0.25 gives historyWeight 0.87576 / 0.87629 / 0.87662 - so the chain is
  wired end to end. But at any threshold that avoids false positives it is inert to
  five or six significant figures on everything the harness produces, *including*
  the per-region breakdown, on a scenario built for this: `moving-occluder` steps an
  occluder once and genuinely moves a shadow (screen instability 5.4e-4 -> 2.2e-3 at
  the step, edge density doubling). On against off across that transient is
  identical - region 6 at the step frame is 0.005948 against 0.005914.

  The reason is **scale**, and it is the same tension the whole audit keeps
  producing. Per pixel, the question reads jitter as change - the trap identity, the
  normal veto and the luma ring each fell into first. At 16x16 it is jitter-proof
  but this content's lighting changes are roughly half a stop to a stop over a small
  part of the screen, so a *proportionate* response is correspondingly tiny. 4x4 was
  tried and is no better. The window between the two may not exist at a single fixed
  scale, which is a concrete argument for the real SPD pyramid FSR 3.1 uses rather
  than the tile reduction built here: several scales let it ask the question at the
  one where the answer is legible. Turning the threshold below the lighting
  estimate's own noise floor is the alternative, and that is measured to cost history
  for nothing.

  Dials `--shading-change-strength N` and `--shading-change-stops N`. Validating it
  needs either the multi-scale pyramid or a ghosting metric (T3.2), and preferably
  both - a screen mean cannot see a localised ghost, which is why the region
  breakdown exists and why even that was not enough here.

- **2026-08-03** The reported "lit regions switch between dark and lit with TAA and
  TSR on" is **found and fixed, and it is not a temporal defect at all**. The probe
  tile size is authored in pixels and every probe pass runs at the *render*
  resolution, so TSR's 0.5 render scale silently doubled each tile's footprint per
  axis - one probe describing four times the screen area, and a tile spanning two
  surfaces alternates between them. `computeProbeTileSize` now reads the setting as
  **output** pixels and converts, so probe density no longer depends on what the
  upscaler renders at.

  Established by a matched 2x2 over resolution and tile size on
  `--test-camera-interior`, and severe probe flicker is a pure function of the
  tile's footprint in output pixels, matching across both resolutions:

  | render res | tile | uniform probes | severe flicker | frame ms |
  | --- | --- | --- | --- | --- |
  | 640x360 | 8 | 3600 | 0.0081 | 30.5 |
  | 640x360 | 6 | 6420 | 0.0062 | 31.4 |
  | 640x360 | 4 | 14400 | **0.0042** | 41.8 |
  | 1280x720 (`--no-taa`) | 8 | 14400 | **0.0042** | - |
  | 1280x720 (`--no-taa`) | 16 | 3600 | 0.0073 | - |

  **Jitter is not involved**: `--jitter-scale 0` moves severe flicker 0.0081 ->
  0.0079, inside the last digit, so the obvious reading of "TAA off fixes it" -
  that the camera jitter churns placement - is wrong. This harness is *deterministic*
  on these figures (two runs of the shipping config agree to every digit printed),
  so the small differences here are real; contrast the wide noise floor on the
  final-image metrics.

  The refinement budget cannot substitute for density: `--probe-adaptive-percent`
  25 / 100 / 200 gives 0.0082 / 0.0081 / 0.0081 for +2% and +7% frame time. Caveat -
  flicker is reported over the uniform grid alone, so it is structurally blind to
  refined probes; what the measurement supports is that refinement does not rescue
  the *uniform* grid, which is what serves broad wall and ceiling areas.

  Cost is real and was taken deliberately: **+37% frame time** at the shipping
  0.5 scale, because TSR no longer saves anything on GI. `--probe-tile 12` is the
  middle point (half the improvement for +3%), and `--probe-tile-in-render-pixels`
  restores the old behaviour exactly. The final-image `instability` metric does not
  move between any of these configs - it never saw this defect, which is consistent
  with everything else this document records about probe-stage defects being
  invisible downstream.

  Found on the way and fixed separately: the previous-frame seed candidate in
  `sparse_voxel_probe_place.wgsl` - the anchoring written specifically to stop tiles
  flipping onto a perpendicular voxel face - was gated on `flags.x`, which is the
  **diagnostics** flag. It therefore ran in every headless measurement and in no
  interactive frame. It has its own `flags.y` now (`--no-probe-seed-continuity` to
  disable). Measured inert on flicker either way (0.0837 against 0.0840), so it is
  a correctness fix, not the cause of the reported symptom.
