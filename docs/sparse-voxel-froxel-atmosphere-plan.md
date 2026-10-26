# Froxel volumetrics, atmospheric shadows, and voxel-lighting plan

Status: core world-froxel path implemented; advanced near-field visibility and
colored optical-depth storage remain follow-up work  
Primary target: `SparseVoxelRendererSample`, ground-to-orbit planet rendering,
25 km local voxel visibility  
Reference model: Sébastien Hillaire, *A Scalable and Production Ready Sky and
Atmosphere Rendering Technique* (EGSR 2020)

## Outcome

Replace the current coarse aerial-perspective approximation with a real,
camera-frustum froxel pipeline that:

- produces visible, depth-correct atmospheric haze and local volumetric fog;
- produces stable shafts when voxel terrain or the planet blocks the sun;
- shades voxel surfaces with the same atmospheric sun and sky used by the
  background;
- remains stable during camera motion and the day/night cycle;
- covers the default 25 km voxel view without making every froxel perform a
  long Tree64 ray every frame;
- runs before TAA/TSR/MetalFX and writes a useful reactive signal; and
- keeps the existing Hillaire transmittance, multiple-scattering, Sky-View,
  and orbit-view paths.

The recommended design is a view-aligned froxel volume with logarithmic depth,
one local-medium evaluation per cell, sparse/reprojected sun visibility, one
front-to-back integration per screen tile, and a separate atmospheric hook in
directional-light evaluation for voxel receivers.

## Why the current volume is not sufficient

The current implementation is a useful aerial-perspective LUT, but it is not a
complete froxel renderer:

- `atmosphere_aerial_perspective.wgsl` uses a fixed 16-32 cubed texture.
- Every z cell independently re-integrates the ray from the camera to that
  cell. Work is repeated and adjacent slices are not a composable sequence of
  local participating-media segments.
- Terrain visibility is sampled from one low-resolution terrain shadow map.
  There is no view-volume visibility history and no near-field Tree64 detail.
- RGB in-scattering is stored with one scalar mean transmittance. Atmospheric
  extinction is wavelength-dependent, so this can wash out or incorrectly
  color distant voxel surfaces.
- There is no temporal reprojection, neighborhood clamp, or sun-shadow update
  cadence for the volume.
- The atmosphere composite attenuates already-shaded surface color, but the
  directional light used to shade the surface is still the authored sun color.
  `atmosphereCoupledLighting()` currently couples sky intensity, not the
  receiver's direct-sun transmittance.
- The legacy analytic distance fog in `SparseVoxelComposePass` can overlap the
  physical atmosphere path unless it is explicitly disabled.

This plan keeps the old volume behind a fallback switch until the new path
passes its image and timing gates.

## Physical split: three different effects

These effects must remain separate in code and in tests:

1. **Sun-to-surface atmosphere transmittance** colors and dims direct sunlight
   at a voxel receiver. It belongs in directional-light evaluation before
   reservoir weighting or probe accumulation.
2. **Sun-to-froxel visibility** creates volumetric shadows and shafts. It is a
   scalar geometric visibility multiplied by atmospheric sun transmittance and
   the local scattering source.
3. **Froxel-to-camera transmittance and in-scattering** attenuate the completed
   surface radiance and add aerial perspective. They belong after material
   composition and the voxel/coarse-planet merge.

The atmosphere itself produces smooth attenuation and planet self-shadowing.
Structured shafts require an occluder such as voxel terrain, the coarse planet,
or a future cloud density field; clear air alone does not create a patterned
volumetric shadow.

## Target frame graph

```text
profile changed
  Transmittance LUT ----+
  Multiple-scattering --+-----------------------------+
                                                       |
camera/sun changed                                     |
  Sky-View LUT ----------------------------------------+
  Atmosphere ambient SH -> WorldGI sky record          |
                                                       |
current camera + medium settings                       |
  Froxel Grid/Media -----------------------------------+--+
                                                          |
voxel/terrain data + sun                                  |
  Near Tree64 visibility rays ----+                       |
  Stabilized terrain cascades ----+-> Froxel Visibility --+
  Analytic planet visibility -----+                       |
                                                          v
                                             Froxel Light Injection
                                                          |
previous local-source/visibility volume ----------------->| temporal filter
                                                          v
                                         Front-to-back Z Integration
                                           L_view.rgb + opticalDepth.rgb

G-buffer receiver positions + transmittance LUT
  Atmospheric direct-sun factor
      -> screen direct lighting / probe direct lighting
      -> normal voxel denoise -> material compose
      -> coarse local planet shading
      -> planet merge
                    |
                    v
  Atmosphere Resolve/Composite
    surface * exp(-opticalDepth) + in-scattering
    empty pixel -> Sky-View LUT + sun/stars
                    |
                    v
             TAA / TSR / MetalFX
```

Froxel work can overlap voxel G-buffer/lighting on async compute once resource
dependencies are correct. The first implementation should use normal render
graph ordering; overlap is a measured optimization, not a prerequisite.

## Coordinate and precision contract

### Units

- Authored CPU atmosphere values remain in metres and inverse metres.
- Existing atmosphere LUT shaders may continue using kilometres internally.
- Every froxel shader converts exactly once at its boundary and names values
  with `Metres` or `Km` where practical.
- Tree64 rays remain in voxel-world metres.
- Optical coefficients and segment length must use matching units.

Add assertions/tests for all uploaded conversion factors. A metres/kilometres
mistake can turn the result black or make the atmosphere visually disappear.

### Camera-relative planet position

Do not reconstruct planet-scale positions by adding two large 32-bit world
coordinates. Upload:

```cpp
struct AtmosphereFroxelFrameGPU
{
  Mat4 cameraToWorld;
  Mat4 previousWorldToCamera;
  Vec4 cameraPlanetPositionKm; // xyz radial position, w radius from center
  Vec4 cameraWorldPositionMetres;
  Vec4 localUpAndMaximumDistanceKm;
  Vec4 focalPrincipal;
  Vec4 previousFocalPrincipal;
  UVec4 gridDimensionsFrameIndex;
  Vec4 depthDistribution;
  UVec4 revisions;
};
```

For a receiver or froxel center:

```text
planetPositionKm = cameraPlanetPositionKm.xyz
                 + (worldPositionMetres - cameraWorldPositionMetres.xyz) * 0.001
```

Compute `cameraPlanetPositionKm` in double precision on the CPU. This is stable
for a 25 km local world and still supports the ground-to-orbit transition.

### Froxel XY mapping

- Allocate from the renderer's internal resolution, not presentation
  resolution.
- A froxel column covers a fixed tile of internal pixels.
- Generate its view direction from the stable projection plus a small temporal
  sub-tile jitter. Do not reuse geometry projection jitter blindly.
- Store current and previous projection parameters for reprojection.
- Handle partial edge tiles explicitly.

### Froxel Z mapping

Use one logarithmic distribution from a small positive near distance to the
configured volumetric maximum. Logarithmic slices act like depth cascades
without seams or separate view volumes.

For normalized boundary `u` in `[0, 1]`:

```text
d(u) = near + scale * (exp2(u * log2(1 + (far - near) / scale)) - 1)

u(d) = log2(1 + (d - near) / scale)
     / log2(1 + (far - near) / scale)
```

Recommended defaults:

- `near = max(cameraNear, 0.10 m)`;
- `far = min(volumetricMaximumDistance, 25,000 m)` in local view;
- `scale = 64-150 m`, exposed only in advanced settings; and
- cell center at `d((z + 0.5) / sliceCount)`, with segment endpoints computed
  from integer boundaries rather than a center-derived approximation.

The mapping and inverse must be shared by CPU reference code, froxel shaders,
the composite, and debug views.

## Quality tiers and memory

| Tier | XY tile | Z slices | Shadow refresh | Intended use |
|---|---:|---:|---:|---|
| Low | 16 x 16 | 32 | 1/8 cells/frame | integrated GPUs, MetalFX |
| Medium (default) | 12 x 12 | 48 | 1/4 cells/frame | normal gameplay |
| High | 8 x 8 | 64 | 1/2 cells/frame | screenshots/high-end |
| Ultra/debug | 8 x 8 | 96 | all cells/frame | validation only |

At 1200 x 720 internal resolution, Medium contains `100 * 60 * 48 =
288,000` cells. At a 600 x 360 MetalFX input it contains 72,000 cells. The
allocator must recompute from the actual internal extent on resize.

Initial resources:

| Resource | Format | Meaning |
|---|---|---|
| `froxelMedium` | RGBA16F 3D | RGB extinction/km, A local fog density/material |
| `froxelVisibility[2]` | R16F 3D | current/history sun visibility and validity policy |
| `froxelSource[2]` | RGBA16F 3D | RGB local source term, A temporal confidence |
| `froxelIntegratedRadiance` | RGBA16F 3D | cumulative RGB radiance, A mean transmittance/debug |
| `froxelIntegratedOpticalDepth` | RGBA16F 3D | cumulative RGB optical depth, A reactivity/confidence |

The default is about 44 bytes per froxel before alignment and transient aliasing:
roughly 12.7 MB at the Medium 1200 x 720 grid and 3.2 MB at a 600 x 360
internal grid. Confirm actual backend allocation sizes in diagnostics. Alias
current-only intermediates when the render graph supports safe texture aliasing.

If storage `R16F` is unreliable on a backend, use RGBA16F for visibility first;
do not silently fall back to an unfilterable integer format.

## Froxel equations

For segment length `ds`, RGB extinction `sigmaT`, and local in-scattering source
`J` in radiance per distance:

```text
segmentT = exp(-sigmaT * ds)
integral = (1 - segmentT) / max(sigmaT, epsilon)
Lnext    = L + Tview * J * integral
Tnext    = Tview * segmentT
```

Use the `sigmaT -> 0` limit (`integral = ds`) to prevent NaNs. Accumulate
optical depth and derive colored view transmittance as:

```text
tau += sigmaT * ds
Tview = exp(-tau)
```

The light injection source is:

```text
Jdirect = Vsun * Tsun * Lsun *
          (sigmaRayleigh * phaseRayleigh +
           sigmaMie      * phaseMie +
           sigmaFog      * phaseFog)

Jmulti  = LmultipleScattering * sigmaScattering
J        = Jdirect + Jmulti + optionalLocalEmission
```

`Vsun` is scalar geometric visibility. `Tsun` is RGB atmosphere
transmittance from the froxel to the top of atmosphere. Keep these values
separate in debug modes.

## Render passes

### 1. `AtmosphereFroxelMediaPass`

Add:

- `src/runtime/atmosphere/rendering/AtmosphereFroxelMediaPass.hpp/.cpp`
- `assets/shaders/atmosphere/wgsl/atmosphere_froxel_media.wgsl`

Responsibilities:

- reconstruct the camera-relative center and endpoints of every froxel;
- evaluate Rayleigh, Mie, ozone, and configured exponential fog density;
- write colored extinction and the local-fog density/material parameter;
- mark cells outside the top atmosphere or beyond the planet intersection as
  vacuum; and
- optionally skip cells strictly behind the nearest opaque depth in all pixels
  covered by the tile.

Do not integrate lighting here. Keeping media and lighting separate allows
shadow/source histories to update without rebuilding static density.

### 2. `SparseVoxelFroxelSunVisibilityPass`

Add voxel-specific code under `src/voxel/svraster/rendering/` and WGSL beside
the other sparse-voxel shaders. It combines three visibility sources:

1. **Analytic planet visibility** is exact and always enabled.
2. **Stabilized terrain shadow cascades** provide cheap mid/far terrain
   visibility. Extend `SparseVoxelAtmosphereTerrainShadowPass` to two or three
   light-space extents with texel-snapped origins, conservative depth bias, and
   a 4- or 9-tap tent filter.
3. **Tree64 sun rays** refine near and important froxels. Reuse the existing
   Tree64 trace representation through a compact ray queue; do not dispatch one
   full ray for every cell every frame.

Default distance policy:

| Range | Primary visibility | Tree64 cadence |
|---|---|---:|
| 0-2 km | Tree64 + near terrain cascade | half of eligible cells/frame |
| 2-8 km | terrain cascade + reprojected Tree64 | quarter rate |
| 8-25 km | far terrain cascade + analytic planet | eighth rate/on disocclusion |
| beyond 25 km | analytic planet/atmosphere only | none |

Eligibility should require non-negligible scattering, non-night sun radiance,
and a segment that can contribute before the opaque surface. Rotate the update
pattern in XY and Z. Sample a deterministic point on the physical solar disk
so history converges toward a soft penumbra.

When Tree64 data is missing or streaming generations change, use the terrain
cascade result rather than zero. Invalid/missing occupancy must never turn a
whole froxel column black.

Bias is based on froxel footprint and selected Tree64 LOD, not a single world
constant:

```text
bias = max(0.5 * selectedVoxelSize, 0.1 * froxelWorldDiameter)
```

Clamp it to a documented range and test contact leakage separately.

### 3. `AtmosphereFroxelLightInjectionPass`

Add:

- `AtmosphereFroxelLightInjectionPass.hpp/.cpp`
- `atmosphere_froxel_light_injection.wgsl`

For every contributing cell:

- sample the existing transmittance LUT using cell radius and sun zenith;
- multiply by analytic planet and voxel/terrain visibility;
- evaluate Rayleigh, Mie, and local-fog phase functions;
- sample the existing multiple-scattering LUT;
- write the local RGB source term, not cumulative aerial perspective; and
- write confidence zero for invalid coordinates or stale visibility.

The direct solar source is zero when the planet occludes the sun. Multiple
scattering may remain nonzero near the terminator and on the night side as
specified by the LUT.

### 4. `AtmosphereFroxelTemporalPass`

Reproject the current cell center into the previous frustum and previous depth
distribution. Trilinearly sample history only when all validity checks pass:

- previous UV and z are in bounds;
- camera cut/discontinuity epoch matches;
- atmosphere profile and medium revisions match;
- Tree64/terrain-shadow generation is compatible;
- relative extinction difference is below threshold;
- sun angular delta is below the continuous-update threshold; and
- the current and previous cells are on the same side of the opaque-depth
  boundary.

Use neighborhood min/max clamping on local source and visibility. Suggested
history weights are 0.85-0.95 for static source, 0.6-0.8 during camera motion,
and zero for a camera cut or major day/night discontinuity. Fast sun movement
must shorten history rather than leave a detached shaft.

Filter the local source/visibility before Z integration. Reprojecting only the
cumulative result tends to drag entire rays across depth boundaries.

### 5. `AtmosphereFroxelIntegratePass`

Dispatch one thread per XY column and loop from near to far over z. This turns
the old repeated prefix integrations into one linear scan per column.

Write cumulative RGB radiance and cumulative RGB optical depth at each slice.
Stop when all RGB transmittance components fall below a small threshold or the
opaque tile depth is reached. The output at any surface distance can then be
sampled without another ray march.

If profiling shows poor occupancy for a long serial z loop, add a subgroup
prefix implementation later. The simple serial scan is easier to validate and
is normally cheap at 32-64 slices.

### 6. `AtmosphereFroxelResolvePass`

Resolve at internal pixel resolution before temporal upscaling:

- covered pixels sample the cumulative volume at `linearDepth`;
- empty pixels sample at atmosphere/volume exit only when local fog or shafts
  are enabled; the normal Sky-View LUT remains authoritative for the infinite
  sky;
- upsample neighboring froxel columns with depth-aware weights to avoid halos
  across terrain silhouettes; and
- output RGB radiance plus RGB optical depth, or composite directly if that is
  measurably cheaper.

The recommended first version keeps composition in
`SparseVoxelAtmosphereCompositePass` and replaces its legacy aerial-volume
binding with the two integrated froxel textures.

## Fixing voxel shading with the atmosphere

### Required lighting contract

Authored directional color/intensity represents solar radiance at the top of
the atmosphere. For every directional-light sample at receiver `P`:

```text
Lsun(P) = authoredSunRadiance
        * atmosphereTransmittance(P, directionTowardSun)
        * voxelVisibility(P, directionTowardSun)
```

Only the direct sun term receives this multiplier. Do not multiply the final
lighting texture, because that texture also contains sky irradiance and finite
lights.

### Integration points

Create a shared WGSL helper, for example
`assets/shaders/atmosphere/wgsl/atmosphere_surface_lighting.wgsl`, and a compact
`AtmosphereSurfaceLightingGPU` uniform. Bind the transmittance LUT and uniform
to every active directional-light consumer:

- `sparse_voxel_scene_ray_create.wgsl` before direct-light target/PDF weights;
- `sparse_voxel_probe_rays.wgsl` before directional radiance enters probe SH;
- the coarse local planet background shader;
- any active WorldGI fallback directional shader; and
- future reflection/specular direct-sun consumers.

The helper reconstructs the receiver's planet-relative position, rejects
planet-occluded sun directions, and samples the LUT. It must return identity
when the atmosphere is disabled so existing pipelines remain valid.

Applying transmittance before reservoir weighting is important: changing only
the final chosen sample would make the stored target and selection weights
inconsistent.

### Sky illumination

Keep `AtmosphereAmbientPass` as the source of atmosphere sky SH, but verify that:

- its values are irradiance/radiance in the convention expected by probes;
- the sun disk is not projected into the sky SH if direct sun is already a
  separate light;
- night ambient is intentional and never a hard-coded beige fallback;
- the same sun/profile revisions wake relevant probe and screen histories; and
- a failed or not-yet-generated Sky-View LUT uses a visible diagnostic fallback
  instead of silently writing black.

### Material composition and view atmosphere

Preserve the existing material boundary:

```text
surfaceRadiance = albedo * materialIndependentLighting + emissive
finalRadiance   = surfaceRadiance * exp(-froxelOpticalDepth)
                + froxelIntegratedRadiance
```

Do not apply aerial transmittance to the material-independent lighting buffer
before denoising. Do not put albedo into froxel injection. Emissive surfaces are
attenuated on the view path like any other surface but are not multiplied by
sun transmittance.

When physical atmosphere is active:

- disable `SparseVoxelComposePass::applyDistanceFog`;
- disable its analytic sky background for the final path;
- let `SparseVoxelAtmosphereCompositePass` own sky, sun disk, stars, view
  transmittance, and in-scattering exactly once; and
- use the merged depth after voxel/coarse-planet selection.

## Settings and command-line controls

Add a small supported surface rather than exposing every constant:

```text
--atmosphere-volumetrics off|legacy|froxel
--atmosphere-froxel-quality low|medium|high
--atmosphere-froxel-distance <metres>          # default 25000
--atmosphere-froxel-shadows off|terrain|hybrid
--atmosphere-froxel-shadow-rate <1|2|4|8>
--atmosphere-froxel-debug <view>
```

Advanced UI can expose tile size, slices, depth scale, temporal weight, fog
extinction/albedo/anisotropy, shadow bias, cascade extents, and cascade filter
radius. Sanitize all values and show the allocated froxel dimensions/memory.

Default sample behavior:

- physical atmosphere on;
- froxel mode on at Medium;
- 25 km maximum distance;
- hybrid shadows on when Tree64 is available, terrain-only otherwise;
- legacy analytic distance fog off; and
- automatic quality reduction when internal resolution or measured GPU time
  exceeds budget.

## Diagnostics

Extend `AtmospherePassKind` and the diagnostics overlay with individual timings
for media, visibility, injection, temporal, integration, resolve, and composite.

Required debug views:

- froxel z slice and world-space distance;
- extinction RGB;
- local fog density;
- atmosphere sun transmittance RGB;
- analytic planet visibility;
- terrain-cascade visibility;
- Tree64 visibility and trace-age heat map;
- final sun visibility;
- local source radiance;
- history weight/rejection reason;
- cumulative radiance;
- cumulative optical depth/transmittance RGB;
- surface direct-sun transmittance;
- voxel direct-only, sky-only, and finite-light-only lighting; and
- legacy/new difference heat map.

Log one concise allocation line on creation/resize and one warning on fallback.
Do not print per-cell or per-frame warnings.

## Implementation phases

### Phase 0 - Contracts, reference math, and observability

Deliverables:

- inventory every directional-light consumer and every existing distance-fog
  application;
- add CPU froxel depth mapping/inverse and homogeneous-medium integration;
- add `AtmosphereFroxelSettings`, quality selection, revisions, and sanitized
  defaults;
- add diagnostics/pass IDs and empty debug-view plumbing;
- capture current noon, sunset, night, valley, horizon, and orbit baselines; and
- capture GPU timings and light-buffer values for the reported broken scene.

Exit criteria:

- CPU reference math passes double-precision tests;
- the exact place where direct sunlight loses atmosphere coupling is verified;
- atmosphere and distance fog cannot both be enabled unnoticed; and
- baselines and budgets are checked into test assets or documented reproducibly.

### Phase 1 - Grid, resources, and identity path

Deliverables:

- allocate/reallocate froxel textures from internal resolution;
- implement XY/Z mapping and debug slice rendering;
- add render-graph passes with vacuum/identity outputs;
- add resize, camera-cut, profile-revision, and device-loss handling; and
- keep `legacy` selectable for A/B comparison.

Exit criteria:

- enabling froxel mode with vacuum media produces bitwise-equivalent surface
  color within format tolerance;
- no out-of-bounds writes at odd resolutions; and
- all resources are released and rebuilt correctly.

### Phase 2 - Local media and front-to-back integration

Deliverables:

- media injection for exponential fog and atmosphere extinction;
- source injection with a constant unshadowed light;
- serial Z integration with RGB optical depth; and
- depth-aware surface sampling.

Exit criteria:

- homogeneous fog matches the analytic Beer-Lambert solution;
- near geometry is less attenuated than far geometry;
- no silhouette halos or NaNs; and
- the old analytic distance fog is disabled in physical mode.

### Phase 3 - Hillaire atmosphere coupling

Deliverables:

- transmittance and multiple-scattering LUT sampling in froxel injection;
- Rayleigh/Mie/fog phase functions;
- top-atmosphere and ground intersection clipping;
- Sky-View/background handoff; and
- colored transmittance in composite.

Exit criteria:

- zenith, horizon, sunset, night, and camera-altitude sweeps remain finite;
- sky and terrain share the same color temperature;
- the horizon has no legacy/froxel seam; and
- orbit behavior still uses its intended direct atmosphere path.

### Phase 4 - Volumetric shadows

Deliverables:

- stabilized terrain shadow cascades;
- sparse Tree64 froxel ray queue and resolve;
- analytic planet occlusion;
- near/mid/far policy and safe streaming fallback; and
- solar-disk jitter plus spatial tent filtering.

Exit criteria:

- a known wall/mountain creates a visible shaft;
- no long rectangular cascade bands appear at distance;
- cascade transitions remain below the image threshold during camera motion;
- missing Tree64 chunks do not create black volumes; and
- shadow cost meets the amortized budget.

### Phase 5 - Temporal stability

Deliverables:

- froxel reprojection, validity masks, clamping, and history confidence;
- rotating sparse shadow updates;
- sun-motion and camera-cut invalidation;
- day/night discontinuity integration; and
- atmosphere contribution to the upscaler reactive mask.

Exit criteria:

- static scenes converge without boiling;
- moving cameras do not leave detached shafts;
- accelerated time of day does not smear the terminator; and
- enabling MetalFX does not erase or excessively ghost the effect.

### Phase 6 - Atmosphere-correct voxel surface lighting

Deliverables:

- shared receiver atmosphere helper;
- transmittance-LUT bindings in screen and probe directional-light paths;
- coarse-planet use of the same direct-sun contract;
- light/probe revision propagation; and
- direct-only/sky-only diagnostics.

Exit criteria:

- a horizontal voxel surface shows a clear `N dot L` response at noon;
- sunlight reddens and weakens toward sunset instead of leaving beige unshaded
  terrain;
- voxel shadows affect direct sun but not sky ambient or finite lights;
- probes and screen-space direct lighting agree within tolerance; and
- night has no direct solar leakage.

### Phase 7 - Performance, quality scaling, and cleanup

Deliverables:

- transient resource aliasing and empty-cell/depth early-outs;
- adaptive shadow update rate;
- optional async compute after dependency validation;
- automatic quality tiers based on internal resolution and timer history;
- legacy/new comparison telemetry; and
- removal of the legacy volume only after the rollback window.

Exit criteria:

- Medium meets its budget on the qualification Apple GPU;
- Low remains useful rather than simply disabling shadows;
- no persistent resource growth on resize or quality changes; and
- release smoke tests pass with and without MetalFX/frame generation.

## Test plan

### CPU tests

Add `tests/atmosphere/AtmosphereFroxelTests.cpp` covering:

- depth boundary monotonicity, endpoints, and inverse round trips;
- odd viewport/tile dimension calculations;
- camera-relative planet reconstruction at large coordinates;
- ray/sphere atmosphere and ground intersections;
- medium density at ground, scale heights, top atmosphere, and below ground;
- Beer-Lambert transmittance for vacuum and homogeneous media;
- analytic versus discrete source integration;
- zero-extinction limit without division by zero;
- Rayleigh and Mie phase normalization/finite ranges;
- temporal reprojection inside/outside frustum and z-slice changes;
- history invalidation for camera, profile, sun, and Tree64 generations;
- quality-tier dimensions and memory accounting;
- cascade selection/blend and texel snapping;
- shadow cadence rotation covering every cell;
- atmosphere surface transmittance at noon, sunset, night, and altitude; and
- disabled-atmosphere identity behavior.

### GPU pass tests

Add `tests/atmosphere/AtmosphereFroxelPassGPUTests.cpp` and a small inspection
shader. Test every pass independently with readback summaries and selected
texels:

| Pass | Required GPU cases |
|---|---|
| Media | vacuum, homogeneous fog, exponential height falloff, outside atmosphere, sentinels |
| Visibility | fully lit, analytic planet shadow, terrain occluder, Tree64 occluder, stale/missing generation fallback |
| Injection | sun off, noon, sunset color, forward Mie lobe, multiple scattering only, finite outputs |
| Temporal | static convergence, camera translation, camera cut, sun jump, density change, neighborhood clamp |
| Integration | vacuum identity, opaque cell, analytic homogeneous solution, RGB extinction, early termination |
| Resolve | near/far depth selection, odd resolution, silhouette edge, empty background, out-of-range depth |
| Surface lighting | atmosphere disabled, noon attenuation, sunset reddening, planet-occluded night, finite-light invariance |
| Composite | `surface*T+L`, emissive attenuation, sky ownership, no double fog, reactive-mask write |

Use guard textures/buffers or untouched sentinel regions to catch dispatch
bounds errors. Assert finite min/max/mean as well as expected direction of
change; a finite but entirely black result should fail.

### End-to-end GPU tests

Build deterministic tiny scenes at 320 x 180 or smaller:

1. flat voxel ground at noon;
2. ground plus a vertical voxel wall producing a shaft;
3. valley with a far mountain crossing a terrain cascade;
4. 25 km plane/terrain depth ramp;
5. sunset with the sun just above and below the horizon;
6. night with stars and zero direct sun;
7. camera moving laterally across a shaft;
8. accelerated day/night cycle;
9. Tree64 chunk streaming/revision change;
10. voxel-to-coarse-planet transition; and
11. atmosphere off/legacy/froxel equivalence cases.

Run representative cases with reverse-Z, odd dimensions, internal render
scales, TAA/TSR, MetalFX upscaling, and frame generation. Frame generation is
not a substitute for testing the real rendered frames.

### Visual regression metrics

Store HDR references where supported and compare:

- exposure-normalized RMSE/SSIM for stable views;
- direct-only luminance ratios for noon/sunset/night;
- shaft centroid and edge-width stability over camera motion;
- temporal residual after a fixed convergence count;
- cascade-boundary luminance jump;
- horizon seam maximum error; and
- percentage of non-finite, clamped, or exactly black covered pixels.

Allow backend-specific tolerances, but never accept non-finite output.

### Performance tests

Record per-pass GPU timestamps at Low/Medium/High for:

- 600 x 360, 1200 x 720, and 1920 x 1080 internal resolutions;
- clear view, dense fog, and many visible Tree64 occluders;
- static camera, moving camera, and fast-moving sun; and
- stable versus actively streaming Tree64 data.

Initial Medium budget at a 1200 x 720 internal resolution:

| Work | Target | Hard gate |
|---|---:|---:|
| Media + injection | 0.35 ms | 0.55 ms |
| Visibility/shadow amortized | 0.45 ms | 0.80 ms |
| Temporal + integration | 0.35 ms | 0.60 ms |
| Resolve/composite increment | 0.20 ms | 0.35 ms |
| Total froxel increment | 1.35 ms | 2.00 ms |

These are acceptance targets to validate on the chosen qualification machine,
not claims about current performance. Report median and 95th percentile over a
warm run. If the hard gate fails, reduce shadow cadence/tier first; do not
silently reduce physical sun or fog intensity.

## Failure handling and fallbacks

- If the transmittance or multiple-scattering LUT is invalid, use legacy mode
  or a visible diagnostic sky; do not produce a black frame.
- If froxel allocation fails, log once and select legacy aerial perspective.
- If Tree64 is unavailable or over streaming headroom, keep terrain/planet
  visibility and mark Tree64 confidence invalid for temporal blending.
- If terrain cascades are unavailable, use analytic planet visibility and
  unshadowed local atmosphere; do not treat absent data as shadow.
- A camera cut clears histories but still renders current-frame unshadowed or
  terrain-shadowed volumetrics.
- Atmosphere disabled must be an identity path with no lingering history.

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| Froxel banding at 25 km | logarithmic slices, jitter, temporal filtering, RGB optical depth |
| Detached/ghosted shafts | reproject local source, aggressive validity, sun-motion response |
| Light leaking near terrain | depth-aware resolve, adaptive ray bias, near Tree64 refinement |
| Far rectangular artifacts | stabilized cascades, tent filtering, cascade blending, no hard invalid=shadow |
| Washed-out terrain | atmosphere-correct direct sun before lighting accumulation; remove double fog |
| Black terrain/night | physical sky SH fallback and explicit sun/sky debug separation |
| Probe/screen mismatch | shared receiver helper and shared revision inputs |
| Excess Tree64 cost | sparse queue, near/mid/far cadence, terrain fallback, early eligibility |
| Planet-scale precision | double CPU origin plus camera-relative float offsets |
| MetalFX loss/ghosting | composite before upscaling and write atmosphere reactivity |
| Binding limits | audit per-stage limits in Phase 0; consolidate atmosphere bindings if needed |

## Acceptance checklist

The work is complete only when all of the following are true:

- Atmosphere is visible against empty sky and over covered voxel terrain.
- Voxel terrain has direct sun cosine shading, colored atmospheric sunlight,
  physical sky ambient, and no accidental constant beige illumination.
- A voxel wall or mountain creates a smooth, stable volumetric shaft.
- The planet blocks direct and volumetric sunlight below the horizon.
- Colored view transmittance and in-scattering agree at the surface/horizon.
- No legacy distance fog or second aerial composite is active in froxel mode.
- The voxel/coarse-planet handoff has no atmosphere or lighting seam.
- 25 km views do not show hard shadow rectangles, black missing-data regions,
  or visible z-slice bands.
- Camera movement, resize, time-of-day changes, and Tree64 streaming recover
  without persistent history artifacts.
- TAA/TSR and MetalFX preserve the effect without excessive ghosting.
- CPU tests, every-pass GPU tests, end-to-end GPU tests, release smoke tests,
  and performance gates pass.

## Recommended first implementation slice

Implement Phases 0-2 as one reviewable slice: grid math, identity resources,
homogeneous media, RGB optical-depth integration, and composite replacement.
It proves the new architecture and fixes repeated z-prefix work without yet
depending on sparse voxel shadow traversal.

Then implement Phase 6's receiver sun transmittance before tuning volumetric
shadows. That change makes voxel shading visibly coherent with the atmosphere
and provides a trustworthy lit scene against which the shadow volume can be
judged. Add terrain cascades and sparse Tree64 refinement only after the
unshadowed froxel result and surface lighting pass their analytic tests.
