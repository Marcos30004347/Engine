# CRT post-process filter

Implementation plan. Phases land in order; each states its exit tests and only
closes when they pass. The effect is optional and off by default — with it off
the frame must be **bitwise identical** to today's, which is the constraint that
shapes most of the design below.

## Goal

Render the game at a low internal resolution (240p/288p/360p/480p class), let
the existing upscaler and lighting chain work at that resolution, then simulate
a colour CRT end to end at a separately chosen output resolution — the video
signal, the electron beam, the shadow mask and its phosphors, the front glass,
and the light that leaves it. Every parameter is exposed in ImGui, including
both resolutions and a master on/off.

"Simulate everything" is the brief, so the design below is organised as the
**physical chain**, in order, rather than as a bag of effects. Order is not
cosmetic here: persistence happens at the phosphor and must come before the
mask; the beam limiter reacts to total emitted light and must come after it;
glow is an optical process in the glass and must come after both. Get the order
wrong and individual stages can each be correct while the picture is not.

Secondary and not incidental: low internal resolution is also the cheapest
performance dial the renderer has. Native 1080p currently costs ~140 ms and is
dominated by per-pixel domain work (`docs/sparse-voxel-1080p-statistics.md`); a
640x360 internal resolution cuts that domain by 9x. The CRT filter is what makes
that read as a deliberate aesthetic rather than a compromise.

---

## Part I — The four extents

There are four distinct resolutions in the frame, and conflating any two of them
is how this feature gets shipped broken.

| # | name | what it is | today | with CRT |
| --- | --- | --- | --- | --- |
| 1 | **render** — `width/height` | what raster + lighting actually render; the upscaler's input | `display * renderScale` | unchanged, now relative to (2) |
| 2 | **internal** — `displayWidth/displayHeight` | the game image the upscaler resolves to. Every history ring, probe grid, debug view and readback is sized here. **This is the CRT signal.** | swapchain extent | user-set, e.g. 640x360 |
| 3 | **output** — `presentWidth/presentHeight` | the resolution the tube is simulated at. Beam spot, mask triads, glow radii and curvature are all resolved here. | (did not exist) | user-set, e.g. 3840x2160 |
| 4 | **window** — swapchain | what the compositor shows, plus the ImGui overlay | swapchain extent | swapchain extent |

(1) → (2) is the existing `renderScale` relationship, untouched. (2) → (3) is
the CRT chain. (3) → (4) is a final filtered blit.

**Why (3) and (4) are separate, and why it is not busywork.** The mask, the beam
spot and the tube edge are all high-frequency structures. Simulating the tube at
2x the window and box-downsampling is supersampling, and it is the single
biggest quality difference between a good CRT simulation and a bad one — the
analytic mask coverage in Part II removes most mask aliasing, but nothing
analytic saves the curved tube edge or moiré between the mask and fine picture
detail. Conversely, on a weak GPU you may want the tube simulated *below* the
window and scaled up. Both are legitimate; pinning (3) to (4) forecloses both.

### Plumbing

`RendererGraph`'s constructor takes `inWidth/inHeight` (`:6899`) and stores them
as `displayWidth/displayHeight`; the sample feeds it the swapchain extent
(`:27596`). Changes:

- `inWidth/inHeight` become the **internal** resolution (2). Everything already
  keyed on `displayWidth/Height` follows automatically and needs no edit.
- Add `presentWidth/presentHeight` (3), used by every CRT pass.
- Add `windowWidth/windowHeight` (4), used only by the `svr.sample.swapChain`
  declaration (`:13040`) and the final blit's viewport.

With CRT off the sample passes the swapchain extent for all three and the graph
is byte-for-byte the graph it builds today. That equality is what makes "off"
*identical* rather than merely similar, and it is exit test C0.1.

Consequences:

- **Aspect ratio.** `renderAspect` (`:27432`) is built from the window. It must
  become the **internal** aspect, so a 4:3 internal resolution projects 4:3
  geometry and the glass pass pillarboxes into a 16:9 output. Deriving the
  projection from the window while rendering 4:3 stretches the world
  horizontally, which is the classic way this gets shipped broken.
- **Changing (2) or (3) rebuilds the graph.** Route both through the existing
  `resizeRequested` path (`:29403`, `:32975`); it already drains in-flight
  frames.
- **Probe density.** `probeTileSize` derives from `width` vs `displayWidth`
  (`:6985`). At 360p a 16px probe tile is a far larger share of the screen than
  at 1080p. Expect to retune; see `probe-tile-in-render-pixels`.
- **Terrain budgets** (`defaultTerrainPagesPerUpdate`, `:24741`) stay keyed on
  the *window*. Streaming budget should not collapse because the game got small.

---

## Part II — The physical chain

Five stages, in physical order. Each is one pass (glow is a small chain), each
has one job, and each is separately switchable so a defect can be bisected to a
stage instead of argued about.

```
  internal image
     |
  [A] SIGNAL      gamut, encode, bandwidth, subcarrier, ringing, noise     -> signalColor  (2)
     |
  [B] BEAM        geometry, scan, spot, defocus, scanlines, persistence    -> tubeEmission (3, ringed)
     |
  [C] PHOSPHOR    mask coverage, subpixel blending, convergence,           -> tubeColor    (3)
     |            phosphor gamut + saturation, beam limiter
  [D] GLOW        three-scale downsample + blur                            -> glow0/1/2    (3/4, 3/16, 3/64)
     |
  [E] GLASS       glow composite, halation tint, veiling glare, tint,      -> crtCanvas    (3)
     |            reflection, vignette, bezel, roll, tonemap
  [F] BLIT        filtered scale + letterbox                               -> swapchain    (4)
```

### A — Signal

The video path between the "console" and the tube. Runs at internal resolution,
horizontally oversampled 4x when a composite path is selected because the
subcarrier needs the bandwidth.

- **`signalPath`** — `rgb` / `s-video` / `composite`. RGB is a clean pass-through
  and the default; the other two are real encode/decode chains.
- **Encode.** RGB → Y'IQ (or Y'UV). S-video keeps luma and chroma on separate
  wires; composite sums chroma onto a colour subcarrier at a fixed phase per
  line.
- **Bandwidth.** Independent low-pass on luma and chroma. Chroma bandwidth is
  what makes red text bleed; luma bandwidth is what softens the picture
  horizontally. Composite additionally suffers luma/chroma crosstalk, which is
  where **dot crawl** and **rainbow banding** come from — do not fake them, they
  fall out of a notch-filter decode for free.
- **Ringing / overshoot.** The video amplifier overshoots on sharp transitions:
  a bright edge gets a dark pre-echo and a bright post-echo. This is one of the
  most recognisable CRT cues and costs a 5-tap kernel with a negative lobe.
- **Noise.** An analog noise floor scaled by inverse luma (noise is visible in
  the shadows, not the highlights), plus optional sparse "snow".

Everything here is a horizontal-only 1D operation, which keeps it cheap.

### B — Beam

Deflection and excitation. Reads `signalColor` at internal resolution, writes
`tubeEmission` at output resolution: the light the phosphor layer is *emitting*,
before any mask geometry.

- **Geometry.** Barrel with independent x/y curvature, plus **pincushion**,
  **horizontal/vertical linearity** (real sets are stretched near the edges),
  slight **rotation**, and **overscan** — the deflection was always driven past
  the visible glass, so the outer few percent of the signal was never seen.
- **Scan.** `interlaceMode` alternates fields by frame parity. `rollMode` moves
  the beam's vertical position: `beam` sweeps a bright leading edge top to
  bottom with the tube decaying behind it, `rollbar` is the out-of-sync band.
  Both driven by **wall clock**, not frame index.
- **Spot.** A 2D Gaussian, not a scanline weight: `spotWidth` horizontally and
  `spotHeight` vertically, in signal pixels. The vertical dimension produces
  scanlines as a *consequence* — the gap between lines is wherever the two
  nearest spots both fall off — rather than as a separately authored darkening.
  Set `spotHeight` above ~0.7 lines and the gaps close on their own, which is
  exactly what a real set does as you turn the brightness up.
- **Defocus and blooming.** Spot size grows with beam current, so bright areas
  are physically larger and softer: `spotWidth/Height` interpolate
  `min..max` on the local luminance. Bright content also smears slightly to the
  right (the amplifier cannot slew back down instantly) — a small asymmetric
  term on the horizontal spot.
- **Persistence.** Blend against the previous `tubeEmission` with per-channel
  decay; green persists longest on P22. This is the phosphor storing charge, so
  it belongs here — **before** the mask, which is static geometry and must never
  enter the ring.

### C — Phosphor

Where the beam meets the mask. Reads `tubeEmission`, writes `tubeColor`.

**Subpixel blending is the whole point of this stage, and it is what separates a
CRT simulation from a coloured grid.** The naive mask multiplies each output
pixel by `(1,0,0)`, `(0,1,0)` or `(0,0,1)`. That is only correct when exactly
one phosphor stripe covers exactly one output pixel. At any other magnification
the beam spot straddles two or three stripes and the correct answer is an
integral, not a lookup:

> For each output pixel, and for each of the three phosphor colours, integrate
> the mask's stripe-coverage function over the pixel's footprint, weighted by
> the beam intensity across that same footprint.

For stripe masks the coverage integral is analytic — it is the overlap of two
intervals — so this is a handful of ALU per pixel, not a supersample. Three
things fall out of it for free:

1. As `maskPitch` approaches one output pixel, coverage tends to 1/3 for every
   channel and the mask **fades out smoothly** instead of aliasing into
   rainbow moiré.
2. A beam spot narrower than one triad lands on one stripe and reads as a pure
   phosphor dot; a spot wider than a triad blends across all three and reads
   white. That is the real relationship between focus and colour, and it is why
   a defocused CRT looks washed out rather than merely blurry.
3. Fractional pitches work, so pitch is not constrained to integer pixels.

Also in this stage:

- **Black matrix.** The graphite between phosphor stripes emits nothing and
  reflects nothing. It is a separate coverage term from the stripes and it is
  what gives a real tube its contrast.
- **Convergence error.** The three guns do not land in exactly the same place,
  and the error grows toward the edges. Offset the R and B sample positions by
  a sub-pixel amount scaled by distance from centre. Cheap, and instantly
  recognisable.
- **Phosphor gamut.** P22 primaries are not sRGB primaries. A 3x3 matrix maps
  the signal into phosphor space; this is what makes CRT greens and reds read
  differently rather than merely "more saturated".
- **Phosphor saturation.** Emission is sub-linear at high excitation, so
  highlights compress toward the phosphor's own colour rather than clipping to
  white.
- **Automatic beam limiter (ABL).** A real set cannot drive the whole screen at
  full brightness — the supply sags and the entire picture dims. Compute mean
  emitted luminance (a small reduction, or read the coarse luma the upscaler
  already maintains) and scale gain down as it rises. This is why a CRT looks
  punchy: a small highlight on a dark field is far brighter than a full white
  field, which no static tonemap reproduces.

### D — Glow

Light scattering on its way out. Three scales, because one Gaussian cannot do
this job and stacking one radius is exactly why most CRT shaders' bloom looks
like a Photoshop filter.

| level | source | radius (output px) | tint | what it is |
| --- | --- | --- | --- | --- |
| 0 | 1/4 res | ~2 | neutral | scatter inside the phosphor layer; makes dots bleed into their neighbours |
| 1 | 1/16 res | ~10 | warm | **halation** — light reflecting off the back of the front glass and re-exciting phosphor nearby |
| 2 | 1/64 res | ~50 | slightly blue | **veiling glare** — the whole screen lifting near a bright area, plus the faint glow the tube throws into the room |

Separable blur at each level, weights exposed individually. Level 2 doubles as
the ambient term that makes the black level lift when the picture is bright,
which no amount of level-0 bloom achieves.

### E — Glass

The front of the tube and the room in front of it. Reads `tubeColor` plus the
three glow levels, writes `crtCanvas`.

- **Glow composite** with the per-level weights and tints.
- **Glass tint.** Real tubes use dark-tinted glass: light passes through it once
  going out but *twice* for reflections, so tint improves contrast far more than
  it dims the picture. Model it as that asymmetry, not as a brightness slider.
- **Ambient reflection.** A soft, curvature-following reflection of a notional
  room light, plus an optional specular highlight near a corner. Subtle by
  default; the parameter earns its place because a totally reflection-free
  screen reads as "flat texture" rather than "glass".
- **Vignette, corner falloff, rounded corners, bezel colour.** Anti-alias the
  tube edge on a distance field or the corners crawl badly under motion.
- **Roll bar** modulation (the `rollbar` variant lives here; the `beam` variant
  lives in B, since it moves the beam).
- **Tonemap and transfer.** `crtGamma`, brightness/contrast/saturation and
  `blackLevel` last. CRTs do not reach black; a small lift is authentic.

### F — Blit

The existing `SparseVoxelBlitPass`, gaining a sampler filter parameter — nearest
when (3) and (4) match, linear when they do not — and letterboxing when the
aspects differ.

---

## Part III — Four decisions

### D1 — Five passes, split on the physical boundaries

`SparseVoxelCRTSignalPass`, `...BeamPass`, `...PhosphorPass`, `...GlowPass`
(dispatched three times), `...GlassPass`, then the existing blit. All modelled
on `SparseVoxelBlitPass` (same layout/sampler/pipeline shape) with a uniform
buffer and an `update()` on the `SparseVoxelGTAOPass::update` pattern, so every
parameter except the two resolutions, `signalPath` and the master toggle is live
with **no graph rebuild and no history invalidation**.

Splitting on physical boundaries rather than merging into one long shader is
deliberate: each stage's output is inspectable, each is separately disableable,
and a wrong-looking picture can be bisected in one run instead of reasoned
about. Add a `crtDebugStage` present mode that shows any intermediate directly.

### D2 — The mask must never enter the persistence ring

Persistence blends `tubeEmission` against its own history. The mask is applied
in the *next* pass, reading that ring but never writing it.

**The trap:** an effect applied in place to a ringed texture compounds every
frame (`svraster-history-ring-inplace`, and the same warning atop
`SparseVoxelGTAOPass.hpp`). Bake the mask into the texture persistence reads
back and every frame multiplies it in again — the image darkens toward black and
the mask burns in as a static pattern. This renderer has recorded that exact
failure twice. The physical model and the engineering constraint agree here,
which is a good sign the split is in the right place.

### D3 — CRT and MetalFX are mutually exclusive, for now

When MetalFX upscaling or frame generation is on, the sample presents the MetalFX
output *directly* via `presentUpscaledTextureWithOverlay` /
`presentGeneratedTextureWithOverlay` (`:31567-31573`), bypassing the in-graph
blit entirely. A CRT chain in the graph would be built, executed and silently
discarded — the worst kind of bug, because it looks like the shader is wrong.

Handle it as the `renderScale < 0.5` case is already handled (`:21665`): turning
CRT on clears both MetalFX flags with a `TextDisabled` note. The portable WGSL
upscaler runs instead. Lifting this is a stretch item, not a blocker.

### D4 — Neutral parameters must be a no-op

With `signalPath = rgb`, gamut matrices at identity, mask/scanline/curvature/
persistence/glow/roll at zero, gamma at 1 and internal = output = window, the
image must differ from CRT-off by nothing beyond one extra bilinear resample.
Anything else means a stage is doing a colour-space conversion it should not be,
and every parameter after it is being judged against a shifted baseline. With
five stages this matters more, not less: each stage gets its own neutrality exit
test so the no-op holds *per stage*, not just end to end.

---

## Part IV — Parameters

One `CRTSettings` struct next to `TemporalSettings` (`:302`), with a
`crtSettingsEqual` comparator alongside `sceneLightingEqual` (`:1074`) so the
frame loop can tell a rebuild-requiring change (either resolution, `signalPath`,
the master toggle) from a live one.

| stage | parameter | range | notes |
| --- | --- | --- | --- |
| master | `enabled` | bool | off by default; rebuild |
| master | `debugStage` | off / signal / emission / phosphor / glow0-2 | present an intermediate directly |
| **2** | `internalPreset` | 240p..native / custom | rebuild |
| **2** | `internalWidth/Height` | 160..3840 | rebuild |
| **2** | `pixelAspect` | 0.5..2.0 | 1.0 square; ~1.2 for 4:3 on 320x240 |
| **3** | `outputMode` | match / multiple / custom | |
| **3** | `outputScale` | 0.5x..2.0x | the supersample dial; rebuild |
| **3** | `integerScale` | bool | snap internal→output magnification, letterbox the rest |
| A | `signalPath` | rgb / s-video / composite | rebuild (changes the pass's width) |
| A | `lumaBandwidth` | 0..1 | horizontal softness |
| A | `chromaBandwidth` | 0..1 | colour bleed; low values give the classic red smear |
| A | `ringing` | 0..1 | amplifier overshoot; the pre-echo/post-echo edge |
| A | `dotCrawl` | 0..1 | composite only; scales the crosstalk term |
| A | `noise` | 0..1 | shadow-weighted analog noise |
| B | `curvatureX/Y` | 0..0.5 | barrel |
| B | `pincushion` | -0.2..0.2 | |
| B | `linearityX/Y` | -0.2..0.2 | edge stretch |
| B | `rotation` | -2..2 deg | |
| B | `overscan` | 0.9..1.1 | |
| B | `spotWidthMin/Max` | 0.4..3.0 signal px | horizontal focus, min at black, max at full white |
| B | `spotHeightMin/Max` | 0.4..3.0 lines | vertical focus; above ~0.7 the scanline gaps close |
| B | `spotSkew` | 0..1 | rightward smear on bright transitions |
| B | `interlaceMode` | off / field | |
| B | `persistence` | 0..0.95 | |
| B | `persistenceRGB` | 3x float | green longest on P22 |
| B | `rollMode` / `rollSpeed` / `rollHeight` / `rollStrength` | | `beam` here, `rollbar` in E |
| C | `maskType` | none / grille / shadow / slot | |
| C | `maskPitch` | 1.0..12.0 **output** px | fractional allowed; see the note below |
| C | `maskStrength` | 0..1 | |
| C | `blackMatrix` | 0..0.5 | fraction of the pitch that emits nothing |
| C | `convergence` | 0..2 output px | R/B offset at the screen edge |
| C | `phosphorGamut` | 0..1 | blend toward the P22 matrix |
| C | `phosphorSaturation` | 0..1 | sub-linear emission at high excitation |
| C | `beamLimiter` | 0..1 | ABL strength |
| C | `beamLimiterKnee` | 0..1 | mean luminance where it starts acting |
| D | `glowLevel0/1/2` | 0..1 each | scatter / halation / veiling glare |
| D | `glowRadius0/1/2` | 1..80 output px | |
| D | `halationTint` | rgb | warm by default |
| E | `glassTint` | 0..1 | asymmetric: once out, twice for reflections |
| E | `reflection` | 0..1 | ambient room light on the glass |
| E | `cornerRadius` | 0..0.2 | |
| E | `vignette` | 0..1 | |
| E | `bezelColor` | rgb | |
| E | `crtGamma` | 0.8..1.4 | stylistic, on top of the existing transfer |
| E | `brightness/contrast/saturation` | | |
| E | `blackLevel` | 0..0.1 | CRTs do not reach black |

`maskPitch` is in **output** (3) pixels, not window pixels — the honest unit,
since the mask is simulated where the tube is. Raising `outputScale` from 1x to
2x therefore halves the apparent triad size unless the pitch is raised with it.
The panel prints the apparent pitch in window pixels under the slider, and
prints the whole chain live, because four numbers are two too many to hold in
your head:

```
  render 320x180 -> internal 640x360 -> output 3840x2160 -> window 1920x1080
         (0.50x)        (6.0x integer)         (0.50x box)
```

Plus `--crt`, `--crt-internal WxH`, `--crt-output WxH` and `--crt-preset <name>`
CLI flags, mirroring `--render-scale`, and emission into
`makeCameraSetupCommand` (`:29337`).

---

## Part V — Phases

All phases landed on 2026-08-31. What follows is what shipped and what the
testing actually established, including the places where the plan above was
wrong and the code was right.

### How the exit tests are measured

Phase 0 shipped believing `--frames --test-frame-signatures` could see the CRT
chain. **It cannot.** Both existing fixtures hash *upstream* of the final blit:
`svr.sample.FinalOutputSignature` reads `presentOutput`, and the exact-output
signature reads the G-buffer. Both are the chain's **input**, so no existing
fixture can see a CRT stage or the blit, and phase 0's C0.1 could not have
failed.

Phase 1 therefore opens with a new `svr.sample.PresentedSignature` pass on the
blit's source, sized from the present extent, logged as
`Sparse voxel presented signature:` on its own line — its own line rather than a
field on the exact-output one, which is already parsed by fixtures that count
its fields. `--dump-frame` gained a matching `.presented.` dump. Every per-stage
neutrality test below is that number.

**A second trap, found the same way.** With the presented signature in place the
neutral chain still differed from CRT-off, and `debugStage` bisected it to the
glass stage in one run. The cause was the recorded frame-local-buffer trap: a
`frameLocal = true` uniform behind a cached binding group is read from an
unwritten instance on the frames where the two disagree, so the glass shader saw
a block of **zeros** and ignored every setting it had. Both the CRT passes and
the blit now use non-frame-local uniforms, with the reasoning in a comment at
each site. This is worth stating plainly because the copied-from pattern
(`SparseVoxelGTAOPass`) has the same shape.

**A third, and the reason the first two hid so long.** The headless scene used
for these runs renders an all-black `presentOutput` unless `--test-multiple-lights`
is passed - RGB exactly zero, alpha 1. Every hash comparison taken before that
was vacuous: it compared black to black. The measurements below all use
`--test-multiple-lights`, whose image has mean luminance 0.016 and peaks at 1.8.

### Phase 0 — plumbing — DONE

`presentWidth/Height` and `windowWidth/Height` on `RendererGraph`; a filter,
source extent and bezel on `SparseVoxelBlitPass`; `SparseVoxelCRTPasses.hpp` as
one class driven by a stage enum; five WGSL stages plus a shared common file;
`CRTSettings`, its two comparators, `makeCRTUniforms`, the panel, `--crt` and
`--crt-debug-stage`. With CRT off the plain blit is registered alone and no CRT
resource exists.

- [x] C0.1 CRT off is undisturbed: frame signatures identical with CRT off, on,
      and at all five debug stages. See the caveat above about what this
      measures.
- [x] C0.2 Neutral chain is **bit-identical** to CRT off on the presented image.
      Measured after the fixes above, not before.
- [x] C0.3 No validation errors or VUIDs, CRT off, on, and at every debug stage.
- [x] C0.4 By construction: `crtSettingsNeedGraphRebuild` names only the
      extents, the signal path, the debug stage and the master toggle.
- [x] C0.5 All five debug stages run clean at the right extents.

### Phase 1 — the two resolutions — DONE

The presented signature above, plus the extent split.
`resolveCRTExtents` resolves both user extents against the window;
`renderAspect` follows the **internal** aspect; both resolution changes route
through `resizeRequested`; the panel and a `Sparse voxel CRT chain:` log line
print all four extents and the magnification.

**A design correction found by C1.3.** The plan had the tube extent derived from
the *window* times `outputScale`. That stretches a 4:3 signal into a 16:9 tube -
the same defect the camera aspect avoids one stage earlier. The tube now
inherits the **signal's** aspect, sized to fit the window and then scaled, so
`outputScale` is a resolution multiplier and the blit pillarboxes the
difference. `SparseVoxelBlitPass::computeFit` is shared between the shader and
the log so both describe the same rectangle.

- [x] C1.1 640x360 internal on a 1920x1080 window renders the world at 640x360 -
      the G-buffer, composed shading, planet background and history targets all
      allocate at 640x360 - with the tube at 1920x1080 and the swapchain
      unchanged.
- [x] C1.2 `outputScale` 2.0 simulates the tube at 3840x2160 and box-downsamples;
      0.5 simulates at 960x540 and scales up. Confirmed by texture sizes.
- [x] C1.3 320x240 internal in a 1280x720 window gives a 960x720 tube - 4:3, the
      signal's own aspect - fitted at uv scale (0.75, 1.0), offset (0.125, 0):
      pillarboxed 160 px each side, not stretched.
- [~] C1.4 Not measured. The cost claim rests on the render-target sizes above
      rather than on a timing run; a median-of-five comparison on a fixed camera
      is still owed.

### Phase 2 — stage B, the beam — DONE

2D Gaussian spot with luminance-driven defocus in both axes and a rightward
skew; barrel, pincushion, linearity, rotation and overscan; interlacing by frame
parity; persistence on a `tubeEmission` ring resolved per frame slot beside the
temporal history rings. Scanlines are not a dial - they emerge from the vertical
spot.

- [x] C2.1 Neutral is bit-identical. **Every dial in `CRTSettings` now defaults
      to its identity value**, and a zero spot is a point beam that reproduces
      the signal exactly - the physical reading of "no beam simulation" rather
      than a special case beside it. Presets are how you get a picture.
- [x] C2.3 Scan gaps close monotonically as spot height rises: vertical
      modulation 0.00303 at 0.30 lines falling through 0.00195, 0.00168, 0.00146
      to 0.00134 at 1.20, with no separate scanline dial involved.
- [x] C2.5/C2.7 Curvature and persistence both move the presented image.
- [ ] C2.2, C2.4, C2.6, C2.8 not individually measured.

### Phase 3 — stage C, the phosphor — DONE

Analytic stripe-coverage integral for grille, shadow and slot masks, with black
matrix, fractional pitch and brightness compensation; convergence scaled from
centre; P22 gamut; phosphor saturation; automatic beam limiter.

**A real defect, caught by C3.3.** The coverage footprint is `[column, column+1)`
and so must be anchored at the pixel's **left edge**, but `uv.x * output.x` is
its *centre*. Every footprint was offset half a pixel, so every pixel straddled
two stripes even at a pitch that divides the grid exactly, the mask never reached
full separation, and its contrast wobbled with pitch. Fixed; mask contrast at
pitch 6.0 went 0.424 to 0.610.

**The plan's C3.3 wording was wrong.** "A monotonic fade from 6.0 to 1.0" is not
what a correct implementation does. Contrast depends on whether the stripe width
divides the pixel grid: pitch 6.0 and 3.0 both give full separation (0.610,
0.609) because a pixel lies entirely inside one stripe, while 5.0 and 4.0 blend
(0.455). The monotonic fall is real only *below* one triad per pixel, where a
pixel starts covering more than one stripe: 3.0 to 2.5 to 2.0 to 1.2 to 1.0
falls 0.609, 0.305, 0.242, 0.031, 0.000. A partial-coverage plateau at exact
divisors is the integral working, not a defect.

- [x] C3.3 The mask reaches **exactly zero contrast** at one output pixel per
      triad, and gives *partial* coverage at fractional pitches. A per-pixel
      lookup cannot do either: at pitch 1.0 it assigns each pixel one pure
      channel, which is maximum divergence rather than none.
- [!] The intended anti-moire test could not be made to discriminate. A
      long-wavelength ripple metric over the mask's column profile reads ~0.27 at
      *every* pitch including exact divisors where no beat can exist - its noise
      floor is the scene's own horizontal colour structure, which sits above the
      effect. Reported rather than tuned until it passed.
- [x] C3.7 The beam limiter engages when the screen average exceeds the knee
      (mean luminance 0.016, so a 0.01 knee acts and the default 0.2 knee
      correctly does not) and is inert at zero strength.
- [x] C3.1 Neutral is bit-identical.
- [ ] C3.2, C3.4, C3.5, C3.6, C3.8 not individually measured.

### Phase 4 — stages D and E, glow and glass — DONE

Three-level pyramid, then glow composite, asymmetric glass tint, ambient
reflection, rounded tube with an antialiased edge, vignette, bezel, roll bar and
the grade.

**The pyramid moved before the phosphor stage, and reads the emission.** Two
reasons that agree: light leaves the phosphor before it meets the aperture, so
this is the more physical order; and a glow carrying triad structure would alias
at every level. It also hands the beam limiter its screen average without a
sixth pass.

**Two defects found here.** The blur step multiplied the radius by the level
scale as well as using the level's own resolution, double-counting it and
pushing level 2's taps clean off the texture - which silently turned the coarsest
level into a corner average and left the beam limiter reading roughly nothing.
And the limiter's screen average was four point samples, which on a mostly dark
scene all land in shadow; it is now a 4x4 grid.

- [x] C4.1 Neutral is bit-identical.
- [x] C4.2 All three glow levels are independently reachable - soloing each
      produces a distinct presented image.
- [ ] C4.3, C4.4, C4.5 not measured. **C4.5 in particular**: the CRT chain's
      cost has not been timed at all.

### Phase 5 — stage A, the signal — DONE

Y'IQ encode/decode, independent luma and chroma low-pass, notch-style composite
crosstalk, amplifier ringing, shadow-weighted noise.

**A modelling gap found by C5.3.** RGB and S-video produced identical images,
because the chroma bandwidth dial applied to every path. An RGB feed carries no
chroma subcarrier, so there is nothing for it to act on. The three paths are now
genuinely distinct: RGB has luma bandwidth, ringing and noise only; S-video adds
chroma band-limiting; composite adds the luma/chroma crosstalk that dot crawl
falls out of.

- [x] C5.1 The RGB path at full bandwidth is bit-identical to CRT off.
- [x] C5.3 The three paths are three different images, from one decode path with
      no artifact term authored.
- [ ] C5.2, C5.4, C5.5 not individually measured.

### Phase 6 — the rolling beam — DONE

`beam` in stage B, decaying behind the sweep through the persistence ring;
`rollbar` in stage E, modulating the finished picture and never reaching the
ring. Both driven by wall clock.

- [x] C6.1 Roll off is bit-identical.
- [x] Roll on moves the presented image.
- [ ] C6.2, C6.3 not measured; C6.2 needs a frame-rate sweep.

### Phase 7 — presets — DONE

`trinitron`, `consumer90s`, `arcade`, `pvm` and `off`, each a full parameter set
across all five stages rather than an overlay, so switching between them is not
order dependent. Available as `--crt-preset` and as buttons in the panel. All
four produce distinct images and `off` is bit-identical to neutral.

`--crt-set NAME=VALUE` sets any single dial from the command line, which is what
every measurement above uses. Persisting the CRT block across restarts is **not
done**.

**The panel.** All 62 parameters in `CRTSettings` have a control, grouped by the
stage that owns them (A signal, B beam, C phosphor, D glow, E glass) with the
resolutions and presets above them and a live readout of all four extents, the
magnification and the mask pitch in window pixels. Everything except the two
resolutions, the signal path and the master toggle updates without a rebuild.

Two things were wrong when it was first written and are worth recording, because
both made a complete panel look like a missing one. It was the only collapsing
header in the sample without `ImGuiTreeNodeFlags_DefaultOpen`, so it sat closed
while every section around it was open; and nine fields - `linearityX/Y`,
`interlaceMode`, the three per-channel persistence terms, `brightness`,
`contrast` and `saturation` - had no control at all, having been added to the
struct and the uniform packer but never to the panel. An audit that walks the
struct's fields and greps the panel region for each is the check that catches
this; it is worth re-running whenever a parameter is added.

### Regression status

`SparseVoxelTerrainGBufferContract`, the four G22 occlusion equivalence tests
and `SparseVoxelSceneRayScreenWorldFrameParity` pass.
`SparseVoxelRasterR0ExactEquivalence` and
`SparseVoxelMicroRasterR1ExactEquivalence` fail, unchanged, on a pre-existing
schema arity mismatch: the harnesses expect 13 fields in the
`Sparse voxel exact output:` line and the sample emits 22. That line is
byte-identical to `HEAD` and appears nowhere in this work.

### What is not done

- **No timing.** C1.4 and C4.5 are both unmeasured, so neither the claimed
  saving from a low internal resolution nor the chain's own cost is established.
- The interactive paths - toggling CRT at runtime, and confirming a non-rebuild
  dial does not rebuild - are verified by construction only.
- Several per-stage exit tests listed above were not individually measured.
- CRT settings do not survive a restart.
- The MetalFX exclusion still stands.

## Files touched

| file | change |
| --- | --- |
| `src/voxel/svraster/rendering/SparseVoxelCRTSignalPass.hpp` | new — stage A |
| `src/voxel/svraster/rendering/SparseVoxelCRTBeamPass.hpp` | new — stage B |
| `src/voxel/svraster/rendering/SparseVoxelCRTPhosphorPass.hpp` | new — stage C |
| `src/voxel/svraster/rendering/SparseVoxelCRTGlowPass.hpp` | new — stage D, dispatched 3x |
| `src/voxel/svraster/rendering/SparseVoxelCRTGlassPass.hpp` | new — stage E |
| `src/voxel/svraster/rendering/SparseVoxelBlitPass.hpp` | filter parameter + letterbox |
| `assets/shaders/.../sparse_voxel_crt_common.wgsl` | new — uniforms, mask coverage, warp, colour matrices |
| `assets/shaders/.../sparse_voxel_crt_{signal,beam,phosphor,glow,glass}.wgsl` | new |
| `assets/shaders/.../sparse_voxel_blit.wgsl` | letterbox + filter |
| `src/runtime/CMakeLists.txt` | six `wgsl2spirv` blocks + `add_dependencies` |
| `samples/.../SparseVoxelRendererSample.cpp` | `CRTSettings`, four extents, pass registration, ImGui panel, CLI |

## Shader notes

- Keep uniforms in `vec4`-aligned blocks. **Never put a `vec3<bool>` in a
  struct** — it becomes `short3` in MSL and fails pipeline creation, not
  compilation, with no useful message (`wgsl-msl-bool-vector-structs`). With
  five stages of settings this is a live risk, not a footnote.
- No `loop` that only ever returns, and no dynamic indexing of a `vec4`: naga
  reports "Function is invalid" with no line number
  (`wgsl-naga-loop-return-invalid`). Blur taps and mask stripe iteration must be
  unrolled fixed sets.
- The colour matrices (sRGB→P22, RGB→Y'IQ and their inverses) belong in
  `sparse_voxel_crt_common.wgsl` as constants, not duplicated per stage.
- These are fragment passes writing colour attachments, so the
  vertex-stage-cannot-write-buffers restriction does not apply — but do not add
  a diagnostic storage write from a CRT vertex stage; it lands nowhere.
- The blit's fullscreen-triangle vertex shader is reusable verbatim, UV
  convention included. No Y flip is involved (that rule applies to passes
  rasterizing world geometry, not to a fullscreen resample).
