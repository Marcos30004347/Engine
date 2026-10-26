# Sparse voxel sky, atmosphere, and day/night plan

> Follow-up: the implementation plan for replacing the current aerial volume
> with a temporally stable froxel pipeline, including voxel surface-lighting
> coupling and sparse volumetric shadows, is in
> [`sparse-voxel-froxel-atmosphere-plan.md`](sparse-voxel-froxel-atmosphere-plan.md).

## Outcome

Replace the renderer's hand-authored sky gradient and distance fog with one
coherent, physically shaped atmosphere that works from the local voxel surface
through the coarse horizon planet and into whole-planet orbit view.

The target experience is:

- One sun controls direct lighting, the visible solar disk, atmospheric colour,
  the planet terminator, and the day/night clock.
- Sky illumination, the visible background, and aerial perspective agree.
- The voxel/coarse-planet handoff does not reveal a colour or fog seam.
- Atmosphere changes and accelerated time of day remain interactive.
- The normal ground-view path costs at most 0.5 ms at the renderer's default
  internal resolution on the qualification GPU. Lower tiers should stay below
  0.25 ms. These are acceptance budgets, not assumed timings.
- Terrain-cast volumetric shadows are optional. The default always includes the
  planet's analytically computed atmospheric shadow, which provides the
  terminator and night side without a shadow texture or voxel ray traversal.

The implementation should follow Sébastien Hillaire's 2020 paper, *A Scalable
and Production Ready Sky and Atmosphere Rendering Technique*, using a
transmittance LUT, a non-iterative multiple-scattering LUT, a camera-relative
Sky-View LUT, and an aerial-perspective volume. The engine should adapt the
paper to its existing sparse-voxel lighting and planet-representation boundary
rather than reproduce an Unreal-specific integration.

## Recommendation in one sentence

Ship the LUT atmosphere with analytic planet self-shadowing first; apply aerial
perspective after the voxel/coarse-planet merge; use a direct atmosphere march
for orbit view; and do not put the distant background planet through voxel
volumetric shadows unless profiling later proves that an optional low-resolution
terrain-shadow mode is worth its cost.

## Current renderer baseline

The repository already has most of the integration points:

- `SceneRayLightingSettings` owns a directional light and an analytic sky with
  horizon, zenith, ground, exponent, and intensity controls.
- `WorldGILightBVHBuildPass` publishes singleton directional and sky lights to
  direct-light reservoirs, probes, and the light field.
- `sparse_voxel_sky.wgsl` is the shared sky-radiance evaluator for those paths.
- `SparseVoxelComposePass` shades voxel materials and currently draws the
  analytic background/distance fog.
- `SparseVoxelPlanetBackgroundPass` cheaply rasterizes and independently shades
  the coarse local planet cap.
- `SparseVoxelPlanetMergePass` cross-fades voxel and coarse-planet shaded colour
  and publishes one coherent depth/normal/temporal identity.
- `SparseVoxelPlanetOverviewPass` renders the normalized whole globe during
  navigation.
- TAA/TSR, motion vectors, temporal histories, reactive masks, and internal
  render scaling already exist.
- Sparse occupancy and Tree64 can trace surface shadow rays, but there is no
  cheap arbitrary-position volumetric shadow query suitable for dozens of
  atmosphere samples per pixel.

The important pipeline constraint is that the coarse local planet is deliberately
excluded from voxel lighting, AO, tracing, and denoising. The atmosphere must
preserve that boundary.

## Target frame graph

```text
Atmosphere profile changed (usually not per frame)
  TransmittanceLUTPass ------+
  MultiScatteringLUTPass ---+--------------------+
                                                  |
camera/sun changed                                |
  SkyViewLUTPass <--------------------------------+
  AerialPerspectiveLUTPass <----------------------+
  AtmosphereAmbientPass <-------------------------+

voxel G-buffer -> voxel lighting -> SVGF -> compose surface colour
coarse local planet -> cheap surface shading ----------------------+
                                                                  v
                                                        PlanetMergePass
                                                                  |
                                               AtmosphereCompositePass
                                             sky / sun / aerial perspective
                                                                  |
                                                         TAA / TSR / present

orbit/overview globe -> OverviewAtmospherePass -> NavigationMergePass
```

`AtmosphereCompositePass` belongs after `SparseVoxelPlanetMergePass` and before
TAA/TSR. At that point the pass sees the chosen surface colour and the chosen
linear depth, so exactly the same aerial perspective is applied to near voxels,
the cross-fade band, and the coarse planet. Empty pixels receive the Sky-View
LUT and the analytic sun disk. This placement also lets temporal upscaling
stabilize the final atmosphere.

When no planet background is active, the same pass follows composition
directly. Debug views can bypass it or explicitly request an atmosphere overlay.

## Engine data model

Add a reusable atmosphere module under `src/runtime/atmosphere/` rather than
embedding the physical model in the sample. Voxel-specific bindings remain in
`src/voxel/svraster/rendering/`.

### `AtmosphereProfile`

Use metres in authored/runtime CPU data and convert to a camera-relative planet
frame before uploading floats.

```cpp
struct AtmosphereProfile
{
  double groundRadiusMetres;
  double topRadiusMetres;
  math::Vec3f rayleighScatteringPerMetre;
  float rayleighScaleHeightMetres;
  math::Vec3f mieScatteringPerMetre;
  math::Vec3f mieAbsorptionPerMetre;
  float mieScaleHeightMetres;
  float mieAnisotropy;
  math::Vec3f ozoneAbsorptionPerMetre;
  float ozoneCentreMetres;
  float ozoneHalfWidthMetres;
  math::Vec3f groundAlbedo;
  float atmosphereRadianceScale;
};
```

Provide Earth, Mars-like, thin, and disabled presets. Use the paper's Earth
defaults as the validation preset: 6,360 km ground radius, 100 km atmosphere,
8 km Rayleigh scale height, 1.2 km Mie scale height, Mie `g = 0.8`, and the
paper's RGB scattering/absorption coefficients. Do not bake Earth constants
into shaders.

`atmosphereRadianceScale` is intentionally an engine exposure bridge. The
current renderer has no final display tone mapper and uses small relative light
values, so the first version must retain those units instead of introducing
literal solar lux. Physical photometric units can follow a dedicated exposure
and tone-mapping project.

### `SunSettings`

```cpp
struct SunSettings
{
  bool enabled;
  math::Vec3f directionPlanetSpace; // direction light travels
  math::Vec3f color;
  float intensity;
  float angularRadiusRadians;       // default 0.004675, approximately 0.2678 deg
  bool drawDisk;
  float diskLuminanceScale;
};
```

Keep the engine's convention that the stored directional vector is the
direction light travels. Centralize conversions to the direction *toward* the
sun in the atmosphere shader library; do not let individual passes choose a
sign.

### `DayNightCycleSettings`

```cpp
struct DayNightCycleSettings
{
  bool enabled;
  bool paused;
  DayNightCycleMode mode;       // celestial clock or manual art direction
  double timeOfDayHours;       // 0..24
  double dayLengthSeconds;     // real seconds per simulated day
  double dayOfYear;            // 0..orbitalPeriodDays
  double orbitalPeriodDays;
  double axialTiltDegrees;
  double latitudeDegrees;
  double longitudeDegrees;
  double timeScale;
  double manualAzimuthDegrees;
  double manualElevationDegrees;
};
```

Store cycle state in double precision. Compute one planet-space sun vector per
frame, then transform it into the active local tangent frame for voxel and
local-cap rendering. The overview renderer receives the same planet-space
vector directly. The initial implementation needs solar declination and hour
angle, not a full astronomical ephemeris:

```text
declination = asin(sin(axialTilt) * sin(orbitalPhase))
hourAngle   = 2*pi*(timeOfDayHours/24 - 0.5) + longitude
sun vector  = local basis(latitude) * (declination, hourAngle)
```

Expose a simpler art mode that directly animates azimuth/elevation. Both modes
must produce the same final `SunSettings`; rendering passes should not know
which controller generated it.

### `AtmosphereFrameState`

Upload one alignment-safe uniform shared by all atmosphere passes:

- Camera position relative to planet centre in kilometres or normalized planet
  units, selected once and documented consistently.
- Ground/top radii in the same unit.
- Local planet up and planet-space/tangent-space transforms.
- Sun direction, colour, intensity, and angular radius.
- Current/previous unjittered camera matrices.
- LUT dimensions and reciprocal dimensions.
- Frame index, quality tier, and feature flags.
- Profile revision and discontinuity epoch.

Keep large planet positions in double precision on the CPU. Upload
camera-relative floats, as the existing local cap and normalized overview paths
already do.

## Shared shader library

Add `assets/shaders/atmosphere/wgsl/atmosphere_common.wgsl` with:

- Stable ray/sphere intersection for inside and outside views.
- Altitude and Rayleigh/Mie/ozone density functions.
- Rayleigh and Cornette-Shanks phase functions.
- RGB extinction and analytic segment transmittance helpers.
- Transmittance and multiple-scattering LUT mappings and inverse mappings.
- Nonlinear Sky-View latitude mapping concentrated around the horizon.
- Aerial-perspective depth-slice mapping.
- Planet-solid visibility test for atmosphere samples.
- NaN/Inf guards and lower bounds for `1 - f_ms`.

All passes must call this file rather than carry private copies of sphere,
density, or phase math. Add C++ tests for mapping round trips and CPU reference
tests for intersections and density profiles.

## LUT passes

### 1. Transmittance LUT

Add `AtmosphereTransmittancePass` and
`atmosphere_transmittance.wgsl`.

- Default: `256 x 64`, `RGBA16Float`, 40 integration steps.
- Inputs: atmosphere profile only.
- Axes: altitude and view zenith using the Bruneton/Hillaire mapping.
- RGB: transmittance to the atmosphere boundary; alpha reserved.
- Update only when the profile or planet radii change.
- Rebuild immediately on edit; its paper reference cost is 0.01 ms on a GTX
  1080, but the engine must measure its own backend.

Do not lower this LUT first on low quality. The paper found its accuracy more
important to maintaining a matching look than reductions in the other LUTs.

### 2. Multiple-scattering LUT

Add `AtmosphereMultipleScatteringPass` and
`atmosphere_multiple_scattering.wgsl`.

- Default: `32 x 32`, `RGBA16Float`.
- Axes: sun zenith cosine and normalized altitude.
- Integrate 64 uniformly distributed directions, with 20 steps per direction
  for the desktop reference tier.
- Evaluate the paper's second-order contribution and transfer factor, then use
  `Psi_ms = L_2nd / max(1 - f_ms, epsilon)`.
- Treat order two and higher as isotropic and ignore terrain visibility, as in
  the paper.
- Update only when profile, radiance scale, or ground albedo changes. Sun
  movement does not dirty it because sun zenith is one LUT axis.

Use compensated accumulation or at least finite clamps in FP32 registers; only
the stored LUT should be FP16. Add a debug counter/flag for denominator clamps
so dense artistic atmospheres cannot silently explode.

### 3. Sky-View LUT

Add `AtmosphereSkyViewPass` and `atmosphere_sky_view.wgsl`.

- Desktop default: `200 x 100`, `RGBA16Float`, 24-30 samples.
- Low default: `96 x 50`, 8 samples.
- Parameterize longitude around local up and nonlinear latitude about the
  horizon. The sun is not baked as a sharp disk.
- Inputs: transmittance, multiple scattering, camera altitude/up, and sun.
- Regenerate every frame while the sun or camera moves; otherwise reuse.
- Include exact planet-solid visibility per integration sample so sunset and
  the Earth's shadow through the atmosphere work with no shadow map.

The composite pass draws the solar disk afterward at full resolution, applies
atmospheric transmittance to it, and clips it against the ground sphere. This
avoids blurring the disk in the low-resolution/nonlinear LUT.

### 4. Aerial-perspective volume

Add `AtmosphereAerialPerspectivePass` and
`atmosphere_aerial_perspective.wgsl`.

- Default: `32 x 32 x 32`, `RGBA16Float`.
- RGB stores in-scattered radiance; alpha stores scalar mean transmittance for
  the first version, matching the paper.
- Use XY camera-frustum tiles. Use a configurable exponential/logarithmic depth
  distribution rather than a fixed 32 km linear range: this renderer needs
  precision across its first hundreds of metres while still reaching the
  coarse horizon cap.
- Choose the far distance as the maximum of the active voxel far range, the
  local planet transition end, and a minimum atmosphere range. Clamp it to the
  atmosphere exit distance.
- Recompute for camera/sun changes. Empty slices outside the atmosphere write
  identity transmittance and zero scattering.

The composite equation for a surface is:

```text
output = surfaceRadiance * transmittance + inScatteredRadiance
```

This supersedes `SparseVoxelComposePass` distance fog when physical atmosphere
is enabled. Retain the old fog and analytic sky as a disabled-by-default legacy
fallback until the new path passes the visual gates.

### 5. Ambient sky representation for voxel GI

The visible Sky-View LUT alone is not enough: voxel probes, DI reservoirs, and
the light field all call `sparse_voxel_sky_radiance` for arbitrary directions.
Leaving the old gradient there would make the sky background and surface
illumination disagree.

Add `AtmosphereAmbientPass`, producing low-frequency sky radiance for the local
camera region. The preferred first implementation is first-order RGB spherical
harmonics (four RGB coefficients) because the sun remains a separate
directional light and diffuse sky is low frequency. It is cheap to evaluate in
all existing sky call sites and can fit into the four float vectors already
available in a sky `WorldGILightGPU` record if that record is explicitly
repacked for atmosphere mode.

Implementation sequence:

1. Sample the physical sky over 32-64 fixed spherical directions in one small
   compute dispatch.
2. Project to four RGB SH coefficients.
3. Publish them into the sky light record or a small atmosphere environment
   buffer.
4. Update `sparse_voxel_sky.wgsl` to select analytic-gradient or atmosphere-SH
   evaluation by a flag.
5. Update average-luminance/importance evaluation to use the SH DC term.

Audit every includer: DI reservoir, spatial reservoir, DI target, scene-ray
creation, probe rays/resolve, light field, sky guide, and debug present. The
same function must remain the single radiance definition. The visible
background continues to sample the higher-resolution Sky-View LUT; first-order
SH is only for diffuse/importance transport.

If first-order SH produces objectionable dawn/dusk terrain colour, expand to
second order in a separate buffer. Do not add texture sampling to every ray path
until profiling shows SH is insufficient.

## Atmosphere composition

Add `SparseVoxelAtmosphereCompositePass` under
`src/voxel/svraster/rendering/` and a matching WGSL shader.

Inputs:

- Merged or voxel-only shaded colour.
- Linear depth and coverage.
- Transmittance, Sky-View, and aerial-perspective LUTs.
- Current unjittered raster state and atmosphere frame constants.

Outputs:

- `RGBA16Float` atmosphere-composited colour.
- Optional `R16Float` reactive mask contribution around the sun disk and
  rapidly changing high-contrast sunset regions.

Behavior:

- No surface: sample Sky-View, add the attenuated sun disk, and optionally add
  stars when the sun is below the horizon.
- Surface: sample the aerial volume by pixel/depth and apply transmittance plus
  in-scattering.
- Depth beyond the aerial volume: use its last slice and analytically extend
  transmittance, or fall back to a short 4-8 step march. Never silently leave
  the coarse planet un-fogged.
- Above the atmosphere: do not use the ground-view Sky-View LUT; route to the
  overview/direct-march path.

Use the stable unjittered principal point when constructing atmosphere rays,
matching the current analytic sky. TAA/TSR should stabilize undersampled
effects; it should not be asked to average a deliberately jittering static sky.

## Local coarse planet integration

Keep `SparseVoxelPlanetBackgroundPass` as a cheap raster surface pass.

Change it as follows:

- Continue computing direct sun from its normal and the shared sun direction.
- Replace the analytic gradient's diffuse term with atmosphere SH.
- Add analytic planet night-side visibility to direct sun.
- Do not apply aerial perspective in this shader. The central atmosphere
  composite applies it after `SparseVoxelPlanetMergePass`, guaranteeing a
  seamless voxel/background transition.
- Preserve the current slope/voxel-coherence approximation; it addresses the
  representation handoff and is orthogonal to atmosphere.

The coarse cap should not participate in Tree64, occupancy shadows, or voxel
probes. That would defeat the reason it has a separate raster path.

## Whole-planet orbit/overview integration

The paper notes that a ground-oriented Sky-View LUT wastes precision on empty
space when the camera is in space. Follow its recommendation and use direct
on-screen ray marching for orbit view.

Add `SparseVoxelPlanetOverviewAtmospherePass`:

- Dispatch/rasterize only the projected atmosphere-shell rectangle, not the
  full screen when the globe is small.
- Intersect each ray with ground and atmosphere spheres in normalized overview
  coordinates.
- March 8-16 samples for the default tier, sampling transmittance and multiple
  scattering LUTs.
- Composite overview surface colour through atmosphere, render the limb, and
  include exact planet-solid shadowing/terminator.
- Render the sun disk in space only when it is not occluded by the planet.
- Keep stars as the empty-space background.

Use altitude hysteresis to choose ground or orbit mode so ascent does not flicker:

```text
ground -> orbit when altitude > 0.20 * atmosphere height
orbit  -> ground when altitude < 0.15 * atmosphere height
```

During the navigation cross-fade, render both atmosphere results and use the
existing overview blend. Optimize this overlap only if the transition exceeds
budget.

For a distant planet, terrain-cast volumetric shafts are normally sub-pixel and
not worth tracing. Surface relief normals, the analytic sphere terminator, and
the atmospheric limb provide the important cues at essentially constant cost.

## Day/night behavior

### Sun and direct light

The cycle controller updates one shared sun direction. Feed it to:

- `WorldGILightBVHBuildPass` directional record.
- All atmosphere LUTs/compositors.
- Local planet background shading.
- Whole-planet overview shading and atmosphere.
- Any surface shadow system enabled for the voxel path.

Fade direct sun to zero only after the disk is geometrically below the local
horizon; do not hand-author a separate colour/intensity curve that fights
atmospheric transmittance. An optional art multiplier may adjust sunrise colour
or brightness after the physical result.

### Temporal identity and cache invalidation

The current sample hashes exact directional and sky floats into light revisions.
If the sun moves every frame, that policy can wake or discard reservoirs and
probe histories every frame. Change the contract before enabling the cycle:

- The sun keeps a stable light handle across continuous motion.
- A `lightingDiscontinuityEpoch` changes only on a teleport, profile switch,
  manual time scrub, enable/disable, or a large instantaneous sun jump.
- Reservoirs that re-evaluate their selected light under current parameters may
  retain identity during smooth motion.
- Cached probe/light-field values refresh progressively. Quantize their sun
  invalidation threshold (initially 0.25 degrees or a 2% radiance change) rather
  than comparing exact floats.
- Reset the sky guide only for profile/mode discontinuities, not ordinary clock
  ticks.
- TAA/TSR history remains valid for smooth time progression; flag a reactive
  region around a rapidly moving sun disk or after a scrub.

Add tests demonstrating that 60 seconds of accelerated time does not produce a
full-screen history reset every frame.

### Night sky

For the first release:

- Let physical sky scattering approach darkness when the sun is below the
  planet horizon.
- Add a procedurally generated or authored star cubemap sampled only for empty
  sky and attenuated by atmosphere transmittance.
- Provide a small `nightAmbientScale` art control for playability.
- Keep moon lighting out of the minimum scope because the current light list
  treats the directional light as a singleton.

A second directional celestial light can later add the moon. It must have its
own disk, angular radius, phase/brightness approximation, and shadow policy;
do not fake it by turning the sun white at night.

## Volumetric shadow strategy

Use explicit quality modes:

### `PlanetOnly` — required fallback and cheapest tier

At each atmosphere sample, analytically test whether the ray toward the sun
hits the ground sphere. This produces:

- Night-side atmospheric darkness.
- The planet's shadow visible from high altitude/space.
- A correct broad terminator for the local and overview planets.

Cost is one ray/sphere test per integration sample and no extra resources.

### `TerrainApproximation` — default medium tier

Only add this after the base path is stable. Build a single low-resolution
sun-space depth map from the coarse planet heightfield/local cap:

- `256 x 256` or `512 x 512`, `R16Float`/depth format.
- Update when the anchor changes or the sun moves more than 0.25 degrees.
- Cover only the visible/local atmosphere region.
- Sample once per atmosphere ray step with 2x2 PCF or a small analytic
  penumbra derived from sun angular radius.
- Allow a fade with distance and low sun elevation to hide limited coverage.

This captures broad mountain shadows without tracing sparse voxels for every
volume sample. It is appropriate only because the planet terrain is a
heightfield; arbitrary overhangs and buildings are not promised.

### `RaymarchedLocal` — optional high tier

If visible god rays are a requirement, ray march the atmosphere at quarter or
half resolution with:

- 8-16 default samples, 32 maximum.
- Blue-noise sample offsets.
- The terrain shadow map above, not per-sample Tree64/occupancy rays.
- Temporal reprojection using camera motion and a conservative history clamp.
- Bilateral upsampling against depth.

The paper's 32-sample example raised atmosphere cost to roughly 1.0 ms on a GTX
1080, versus 0.31 ms for its unshadowed 720p path. Treat this mode as an
explicit budget choice. Clouds would require this ray-marched mode and a cloud
shadow/transmittance input; they are not part of the first implementation.

Do not use the existing half-resolution occupancy surface-shadow ray pass for
volume samples. It generates one ray from a surface receiver and does not scale
to `pixels x atmosphereSteps` arbitrary origins.

## Quality tiers and expected memory

| Setting | Low | Default | High |
| --- | ---: | ---: | ---: |
| Transmittance | 256x64 / 40 | 256x64 / 40 | 256x64 / 40 |
| Multiple scattering | 32x32 / 16 dirs | 32x32 / 64 dirs | 32x32 / 64 dirs |
| Sky-View | 96x50 / 8 | 200x100 / 24 | 256x128 / 32 |
| Aerial perspective | 24x24x16 / 8 | 32x32x32 / 16 | 48x32x32 / 24 |
| Orbit march | 8 steps | 12 steps | 24 steps |
| Volumetric terrain shadow | Off | Off | Quarter-res / 16 |

At default `RGBA16Float`, the four main LUTs require approximately:

- Transmittance: 128 KiB.
- Multiple scattering: 8 KiB.
- Sky-View: 156 KiB.
- Aerial perspective: 256 KiB.

The core LUT set is therefore about 548 KiB before alignment and optional
history. Even double-buffering the camera-dependent products remains modest.

Measure GPU timestamps for every atmosphere pass. Display rolling median and
95th percentile in an ImGui `Atmosphere` diagnostics section. Auto quality may
reduce Sky-View/aerial step counts, but it must never switch profile or shadow
mode without an explicit user setting.

## Settings and tools

Replace the current flat `Sky light` controls with grouped controls while
retaining a legacy mode:

### Atmosphere

- Enabled / legacy analytic fallback.
- Preset and editable ground/top radii.
- Rayleigh, Mie, ozone, ground albedo, Mie anisotropy.
- Radiance scale.
- Quality tier.
- Ground/orbit mode diagnostic and freeze-LUT toggle.

### Sun

- Enabled, colour, intensity, angular radius, draw disk.
- Manual azimuth/elevation.
- Day/night cycle enabled, pause, time, date/season, latitude, axial tilt,
  day length, and time scale.

### Night

- Stars enabled/intensity/rotation.
- Night ambient scale.

### Volumetrics

- Shadow mode: Planet only / Terrain approximation / Raymarched local.
- Step count, resolution scale, temporal weight, maximum distance.

### Debug

- Show each LUT.
- Single scattering only / multiple scattering only.
- Transmittance, aerial RGB, aerial alpha.
- Atmosphere sample count and shadow factor.
- Force ground/orbit renderer.
- Per-pass timings and LUT revision counters.

Add command-line equivalents for deterministic test captures: atmosphere
preset, time of day, paused cycle, quality, and volumetric-shadow mode.

## File-level implementation map

New reusable files:

- `src/runtime/atmosphere/AtmosphereTypes.hpp`
- `src/runtime/atmosphere/AtmosphereController.hpp/.cpp`
- `src/runtime/atmosphere/rendering/AtmosphereTransmittancePass.hpp`
- `src/runtime/atmosphere/rendering/AtmosphereMultipleScatteringPass.hpp`
- `src/runtime/atmosphere/rendering/AtmosphereSkyViewPass.hpp`
- `src/runtime/atmosphere/rendering/AtmosphereAerialPerspectivePass.hpp`
- `src/runtime/atmosphere/rendering/AtmosphereAmbientPass.hpp`
- `assets/shaders/atmosphere/wgsl/atmosphere_common.wgsl`
- One WGSL entry file per pass above.

New sparse-voxel integration files:

- `src/voxel/svraster/rendering/SparseVoxelAtmosphereCompositePass.hpp`
- `src/voxel/svraster/rendering/SparseVoxelPlanetOverviewAtmospherePass.hpp`
- Matching WGSL entry files under `assets/shaders/voxel/svraster/wgsl/`.

Modify:

- `samples/sparseVoxelRenderer/SparseVoxelRendererSample.cpp` for settings,
  frame-graph registration, updates, diagnostics, CLI, and day/night control.
- `WorldGILightBVHBuildPass.hpp` and `WorldGITypes.hpp` for atmosphere sky SH
  and stable continuous-light revision semantics.
- `sparse_voxel_sky.wgsl` and all binding layouts only as required by the
  chosen SH storage.
- `SparseVoxelComposePass` to make legacy sky/fog conditional and leave empty
  pixels ready for atmosphere composition.
- `SparseVoxelPlanetBackgroundPass` to consume atmosphere ambient and shared
  sun, without adding aerial perspective internally.
- `SparseVoxelPlanetOverviewPass`/navigation merge to route through the orbit
  atmosphere path.
- `src/runtime/CMakeLists.txt` to compile and copy all new shaders.

Do not fold atmosphere settings into `SparseVoxelScene`; they are world/render
environment state, not sparse geometry asset content.

## Delivery phases

### Phase 0 — contracts and reference harness

1. Add CPU atmosphere types, Earth preset, coordinate conventions, and dirty
   revision rules.
2. Add CPU reference functions for ray/sphere intersection, density, and LUT
   mappings.
3. Add a headless reference image/csv harness for transmittance at zenith,
   horizon, and several altitudes.
4. Add pass timing plumbing and LUT debug texture presentation.

Exit gate: CPU mappings round-trip within tolerance, all constants are finite,
and Earth reference transmittance is monotonic with optical path length.

### Phase 1 — physical sky background and sun

1. Implement transmittance and multiple-scattering LUTs.
2. Implement Sky-View LUT and full-resolution sun disk.
3. Add atmosphere composite for empty pixels only.
4. Keep old analytic sky for GI temporarily, but couple its intensity to the
   same sun setting and label the mismatch in the debug UI.

Exit gate: stable noon, sunset, twilight, high-altitude, and below-horizon
captures; no horizon seam; no NaN/Inf under all presets.

### Phase 2 — aerial perspective and planet handoff

Implementation status (2026-08-12): implemented. The renderer now builds a
camera-frustum `RGBA16F` aerial-perspective volume, with cumulative in-scattered
radiance in RGB and mean transmittance in alpha. The desktop tier uses
`32 x 32 x 32` froxels with 12 integration steps; low uses `16 x 16 x 16` with
8 steps. Depth uses a tested logarithmic mapping, defaults to a 100 m near
distribution scale, and spans the maximum of the camera far plane, active
planet transition end, and a configurable 32 km minimum. The unified composite
runs after `SparseVoxelPlanetMergePass` and applies
`surface * transmittance + inScattering` to both representations. Legacy fog
remains forced off. Shader compilation, CPU mapping contracts, descriptor
creation, and a live Vulkan frame are validated; qualification captures with a
planet package remain content acceptance work. An eight-frame instrumented
static-fixture run at 160 x 90 output / 80 x 45 internal resolution reported
0.064 ms median and 0.215 ms p95 total atmosphere time (0.007 ms aerial volume
and 0.104 ms composite on the latest frame). This is a
useful smoke measurement, not the final qualification-resolution budget proof.

1. Implement the aerial-perspective volume.
2. Move atmosphere composition after `SparseVoxelPlanetMergePass`.
3. Disable legacy distance fog while atmosphere is active.
4. Verify voxel/coarse-planet cross-fade at all view distances and render
   scales.

Exit gate: colour and transmittance remain continuous through the representation
transition, and the core atmosphere stays within the default 0.5 ms budget.

### Phase 3 — lighting unification

Implementation status (2026-08-12): implemented. A 32-direction Low / 64-
direction default compute reduction projects the physical Sky-View LUT into
four RGB first-order spherical-harmonic coefficients stored in the existing
singleton sky light record. The shared sparse-voxel sky evaluator detects this
payload, so direct-light reservoirs, spatial reuse, secondary scene rays,
probes, light fields, and debug shading all consume the same physical ambient
without acquiring another texture binding. The coarse planet uses the same SH
and retains a constant-cost analytic sphere terminator; terrain volumetric
shadows remain deferred because they add little value to a distant body.
Continuous sun motion retains stable light handles, separates exact CPU light
updates from a discontinuity epoch, and quantizes progressive GI refresh to
0.25-degree direction or 2% radiance bins. A 3,600-frame/60-second regression
checks that smooth motion crosses only progressive bins rather than resetting
history every frame. Phase 4 will turn on the continuous-motion contract from
the actual day/night controller. A live one-frame Vulkan smoke test completed
with 1,190 covered pixels and a stable output signature. In the small 160 x 90
debug timing smoke run, the single-invocation ambient projection reported
0.039 ms; that run's broader benchmark command separately exits on a missing
actual-scene stage timer qualification error, so this is a pass-cost observation
rather than a full performance qualification.

1. Implement atmosphere ambient SH projection.
2. Route voxel probes, reservoirs, light field, coarse planet, and debug
   shading through it.
3. Fix continuous sun revision/invalidation semantics.
4. Add progressive probe refresh for moving sun.

Exit gate: a grey diffuse sphere and coarse/voxel terrain receive matching sky
colour, and a running cycle does not hard-reset all temporal lighting each
frame.

### Phase 4 — day/night and night sky

Implementation status (2026-08-12): implemented. The renderer now owns a
double-precision, finite-value-sanitized clock with celestial and manual
azimuth/elevation modes, pause/scrub, negative or accelerated time scaling,
and coupled day/year wrapping. The generated direction drives the one existing
directional-light record, preserving its stable handle. Voxel and local-cap
passes consume the local tangent-space direction; the overview receives that
same direction rotated through the active planet tangent basis, preserving the
light-travel sign and terminator at the representation handoff. Smooth clock
motion uses the Phase 3 quantized progressive-refresh contract. Manual edits,
mode switches, enable transitions, and large angular steps instead mark the
atmosphere reactive region, with full-frame rejection reserved for genuine
discontinuities.

The empty-sky compositor adds a deterministic, texture-free two-layer star
field. Stars fade in only at night, are rejected behind the ground sphere, and
are attenuated through the transmittance LUT. A small isotropic blue night
floor enters only the ambient SH DC coefficient, preventing fully black diffuse
terrain without inventing a second light. Both features are optional and have
intensity controls. Interactive controls cover the clock, location, tilt,
manual sun, stars, and night floor; equivalent command-line controls support
captures and regression runs. There is no established atmosphere settings
persistence location in this sample, so Phase 4 deliberately adds no private
save format.

The CPU regression advances an accelerated 24-hour cycle across 3,600 frames
and checks per-frame continuity, day/year wrapping, direction sign, manual
angles, and bounded progressive revision churn. A live Vulkan midnight/stars
smoke and an accelerated-sunset smoke both completed with 1,190 covered pixels
and without graph rebuilds or history resets. At 160 x 90 output / 80 x 45
internal resolution, the debug timing sample reported 0.274 ms median and
0.323 ms latest total atmosphere time (0.007 ms aerial, 0.217 ms ambient, and
0.007 ms composite on the latest frame). This is a small-resolution pass-cost
observation; the broader qualification command still exits because the sample
lacks an unrelated required actual-scene stage timer.

1. Add the double-precision cycle controller and manual/celestial modes.
2. Add clock UI, pause/scrub, CLI controls, and save/load plumbing if settings
   persistence already has an established location.
3. Add stars and night ambient.
4. Add reactive handling for scrubs and fast sun motion.

Exit gate: a 24-hour accelerated capture has no direction discontinuity,
history flash, sudden ambient jump, or light-sign disagreement between voxel,
local planet, and overview.

### Phase 5 — orbit atmosphere

Implementation status (2026-08-12): implemented. Orbit view now uses a
dedicated `SparseVoxelPlanetOverviewAtmospherePass` whose vertex stage covers
only the conservative projected atmosphere-shell rectangle. Its fragment stage
performs exact ground/top-sphere intersections and a bounded direct march (12
samples by default, configurable from 4 to 32), reusing the Phase 1
transmittance and multiple-scattering LUTs. It attenuates the overview surface
in RGB, produces the atmospheric limb, applies an analytic solid-planet shadow
to the atmosphere and sun, and shades the terminator without tracing overview
terrain. Deterministic stars remain visible in empty space, while the solar
disk is split between the bounded atmosphere pass and the space-background
merge so it remains visible outside the shell rectangle without being drawn
through the planet.

The navigation merge retains both the raw adaptive-CBT overview and the
atmosphere-composited result, allowing the existing ascent/descent blend to run
without a representation pop. A stateful altitude selector enters orbit above
20% of the atmosphere height and returns to ground below 15%, leaving a stable
dead band between the thresholds. The pass exposes shaded, transmittance,
scattering, and shell-bounds diagnostics, plus UI and command-line controls for
enablement and integration sample count. CPU regression coverage verifies the
hysteresis state machine, conservative screen bounds, and ray/sphere ordering;
all Phase 5 tests pass. Live Vulkan overview smoke validation also completed
with a 148 x 148 atmosphere/planet screen span and stable output signature
`e14fec4a050e8383`. At 320 x 180 output resolution, the final debug capture
reported 0.339 ms latest total atmosphere GPU time; the adaptive overview mesh,
not atmosphere, dominated the orbit frame at 6.500 ms. These are debug
pass-cost observations rather than a release-GPU qualification. Terrain-cast
volumetric shadows remain deliberately deferred to Phase 6: at overview
distance, the constant-cost sphere shadow provides the required planet shadow
and terminator at substantially lower cost.

1. Add bounded-screen direct atmosphere march for overview mode.
2. Integrate with ascent/descent blending and the adaptive CBT overview.
3. Add ground/orbit hysteresis and space-view diagnostics.

Exit gate: limb, terminator, planet shadow, surface attenuation, and empty
space remain correct from surface through maximum orbit distance.

### Phase 6 — optional terrain volumetric shadows

Implementation status (2026-08-12, default changed 2026-08-13): implemented.
The renderer now uses `TerrainApproximation` by default while retaining
`PlanetOnly` as the unconditional fallback and explicit cheapest tier. It
derives an approximately 65,000-triangle shadow
caster from the existing local planet-background mesh and rasterizes it into a
persistent, reverse-depth shadow map. The default map is `256 x 256` (`256 KiB`
at `Depth32`); a `512 x 512` (`1 MiB`) option is available for high-quality
captures. The projection is camera-relative and restricted to the configured
local range, so it does not introduce large-planet precision loss or ask the
sparse voxel hierarchy to cast a global shadow.

The map is rebuilt only when it first becomes valid, the terrain anchor
changes, the camera crosses a shadow-texel-sized quantization cell, or the sun
crosses a 0.25-degree direction bin. Otherwise its depth attachment is retained
and the terrain draw count is zero. Sky-View and aerial-perspective integration
sample it with a four-tap PCF filter and apply the result only to direct solar
scattering; multiple scattering and the analytic solid-planet shadow remain
unchanged. Shadow strength fades near the map boundary and as the sun approaches
the horizon. Missing coverage, a zero/empty map, night, or disabled mode all
resolve to fully lit terrain visibility and therefore reproduce the
`PlanetOnly` path without stale shadowing.

UI and command-line controls cover the mode, 256/512 resolution, local range,
depth bias, and strength. The terrain pass is classified separately in GPU
diagnostics, and selecting the planet-only tier does not create an empty shadow
render pass.
CPU regression covers coverage fallback, low-sun and edge fading, update
quantization, and anchor invalidation; the complete Phase 6 test target passes.
A live eight-frame Vulkan terrain-mode smoke at 320 x 180 produced 7,658 covered
pixels and stable output signature `2efc1876523c24de`. Its latest terrain-map
cost was 0.053 ms; even the conservative sum of that pass and the complete
shadow-consuming Sky-View and aerial passes was 0.175 ms, below the 0.5 ms
optional-mode gate. Total atmosphere time was 0.373 ms median and 0.651 ms on
the latest frame in this debug run. Startup pipeline/LUT compilation and the
sample's unrelated missing actual-scene timer are excluded from the optional
feature assessment.

The quarter-resolution temporally reprojected volume was not added. The shadow
map already injects terrain occlusion at the two integrations that generate
local atmospheric scattering, stays comfortably within budget, and requires no
new temporal history. A separate volume would add bandwidth, reprojection
failure modes, and invalidation work without evidence of a visible acceptance
gap. It remains a future quality-tier experiment rather than part of the Phase
6 runtime path.

1. Implement and profile the low-resolution terrain shadow map.
2. First use it only in the aerial/sky integration without a separate temporal
   volume pass.
3. Add the quarter-resolution temporally reprojected march only if visual
   captures justify it.

Exit gate: no more than 0.5 ms additional default-GPU cost for the optional
mode, stable history during camera/sun motion, and a clean fallback to
`PlanetOnly` when coverage is unavailable.

## Validation

Automated closure status (2026-08-12): the atmosphere now has separate CPU and
headless Vulkan CTest targets. `AtmospherePassGPUValidationTests` builds and
executes every production atmosphere pass in one dependency-ordered render
graph: terrain shadow, transmittance, multiple scattering, Sky-View, aerial
perspective, ambient SH projection, LUT debug presentation, local composite,
and orbit composite. A small GPU inspection kernel reduces every image to
readback mean/minimum/maximum values, while the ambient light record is copied
directly. Assertions cover finite/nonnegative radiance, `[0,1]`
transmittance, optical-path variation, opaque/identity alpha contracts,
reverse-depth terrain coverage, exact disabled-composite passthrough, cleared
disabled orbit output, and valid nonzero atmosphere-SH publication.

The same GPU test runs multiple frames to verify persistent transmittance,
multiple-scattering, and terrain maps do not change when rebuild is disabled.
It also compares shadowed and zero-strength terrain modes, requiring terrain
occlusion to reduce direct Sky-View/aerial radiance and the zero-strength mode
to restore the `PlanetOnly` result. Finally it verifies disabled Sky-View and
aerial passes produce zero scattering with identity transmittance. The CPU and
GPU atmosphere targets both pass, as does the `SparseVoxelRendererSample`
build. Run them with:

```sh
ctest --test-dir build \
  -R '^(AtmospherePhase0Tests|AtmospherePassGPUValidationTests)$' \
  --output-on-failure
```

Runtime visibility correction (2026-08-13): GPU inspection showed that the
unscaled Earth preset was numerically valid but too dark for this sample's
non-tonemapped display range. The sample now defaults its exposure bridge to
`atmosphereRadianceScale = 8`, while the reusable Earth profile remains at the
neutral engine value of `1`. `--atmosphere-radiance 0..100` exposes the bridge
for captures and `--atmosphere`/`--disable-atmosphere` make activation
explicit. Orbit atmosphere is also evaluated throughout the existing
surface/overview cross-fade instead of being suppressed until the altitude
hysteresis changes the space-background model. The GPU suite now requires the
calibrated value to materially brighten Sky-View, aerial perspective, local
composition, and orbit composition. A bounded live planet-overview smoke with
MetalFX requested and `--volumetric-shadow-mode terrain` confirmed that both
`Atmosphere.TerrainShadow` and `Atmosphere.Overview` are registered and that
the post-navigation presentation path completes successfully.

The startup log prints the effective atmosphere state, radiance scale, orbit
sample count, and volumetric-shadow mode. Terrain refinement is now the sample
default; a run that reports `volumetricShadows=planet-only` has the
constant-cost spherical planet shadow but no terrain-cast atmospheric
occlusion.

Release-path initialization correction (2026-08-13): static LUT rebuild
requests are now latched for one more update than the maximum frames in flight.
Previously the CPU cleared the one-frame rebuild bit quickly enough that the
GPU could observe only the cleared value, leaving transmittance and
multiple-scattering textures at their zero-initialized contents. That produced
the characteristic black surface sky and also projected zero atmosphere SH
into terrain lighting. A CPU regression test covers the latch lifetime. Normal
speed GPU captures now retain nonzero Sky-View output both without temporal
presentation and with MetalFX upscaling plus frame generation; the exact
1200x720 sample configuration also completes with a nonblack sky.

Volumetric extension (2026-08-13): the aerial-perspective froxel pass now
integrates a local exponential height medium alongside Rayleigh/Mie air. Its
existing logarithmic depth distribution acts as inexpensive distance cascades,
so this adds no volume texture and no full-resolution march. Ground extinction,
height falloff/cutoff, albedo, and anisotropy are runtime controls. Direct fog
scattering consumes the same analytic planet and optional 256-square terrain
visibility used by clear air, making terrain-cast shafts visible at low sun.
The sample defaults to 0.5/km, a 180 m falloff, terrain shadow refinement, and
a 25 km reverse-Z far plane. GPU A/B coverage requires fog to reduce aerial
transmittance and materially change integrated radiance.

Long-range voxel correction (2026-08-13): the original terrain "LODs" changed
only the sampling resolution inside a fixed 31.25 m page. The 7x7 toroidal cut
therefore remained 218.75 m wide and necessarily handed off near 110 m,
regardless of its requested distance. Terrain now uses real concentric spatial
clipmap rings. The default 25 km request derives eleven levels: a complete 3x3
fine cut plus eight border pages for each doubled parent level, or 89 pages in
total. Per-page resolution falls from 160 to 80 to 40 cells, canonical-grid
scales include both spatial and resolution ratios, and explicit overlap masks
give finer rings sole ownership. The checked-in planet produces a 96 km square
conservative resident extent, remains voxel-only through 19.5 km, and consumes
about 115 MiB of packed terrain GPU data. CPU regression covers automatic
distance-to-level selection, 25 km coverage, adaptive canonical scaling, page
count, and the prohibition on treating spatial pages as progressive rewrite
requests. An exact Release GPU smoke at 1200x720 with MetalFX upscaling and
frame generation rendered the 89-page cut successfully.

Streaming follow-up (2026-08-13): the dynamic Tree64 arena still assumed the
old 64-page terrain window. A streamed cut could therefore populate more
Tree64 page models than the initial view and fail both incremental publication
and its fixed-arena remap fallback. Tree64 model, instance, reference, and node
capacities now derive from the physical clipmap slot count. The 25 km fixture
reserves all 89 model slots and 370,329 fixed node slots. The Release GPU
terrain-streaming round trip—an offset of 4,096 fine pages and restoration of
the original cut—publishes both generations without exceeding headroom.

Distant-shadow smoothing follow-up (2026-08-13): the spatial clipmap exposed
the fixed 64-side resolution of each terrain Tree64 model. Keeping full binary
visibility to the 25 km handoff projected those coarse cells as long dark
bands, while the atmosphere's one-texel bilinear terrain-shadow lookup exposed
the 512-square map as rectangular volumetric regions. The terrain preset now
fades Tree64 shadow contrast over 3.75-10 km into an energy-complementary
analytic sun/sky term, samples the physical solar disk through temporal probe
history, and uses a continuous four-texel tent for atmosphere terrain shadows.
The wider atmospheric filter remains in LUT space rather than full-resolution
screen space.

### CPU/unit tests

- Ray/sphere intersections from ground, atmosphere, space, tangent, and miss
  rays.
- Density at sea level, scale height, ozone centre/edges, and atmosphere top.
- LUT mapping/inverse round trips, including exact horizon boundaries.
- Day/night sun vector continuity at midnight, year wrap, equator, poles, and
  zero/nonzero axial tilt.
- Dirty flags: profile, sun, camera, cycle tick, time scrub, and mode switch.
- FP16 packing ranges and finite-value sanitization.

### GPU tests

- LUT dimensions/formats and dispatch bounds.
- Transmittance remains in `[0,1]` and decreases with longer optical path.
- Multiple scattering is finite/nonnegative for Earth, Mars-like, and dense
  artistic profiles.
- Ground shadow test changes correctly when the sun crosses the horizon.
- Aerial composition satisfies the identity case in vacuum and approaches sky
  radiance at high optical depth.
- Sun disk is hidden by the planet and is not baked/blurred into Sky-View.
- Atmosphere SH DC energy roughly matches a numerical sphere average.

### Visual matrix

Capture at fixed seeds and exposure bridge values:

| View | Times | Profiles | Required checks |
| --- | --- | --- | --- |
| Ground | noon, sunset, civil twilight, midnight | Earth, Mars | horizon, sun halo, terrain fog |
| High altitude | noon, sunset | Earth, dense | planet shadow, LUT precision |
| Orbit | day, terminator, night | Earth, tiny dense | limb, surface attenuation |
| Handoff | noon, sunset | Earth | voxel/coarse colour and fog seam |
| Motion | 24 h accelerated | Earth | temporal stability and cache churn |

Compare a small set against a slow CPU/GPU brute-force single-scattering
reference. Exact path-traced matching is not an initial gate; plausible,
energy-stable multiple scattering and continuity are.

### Performance gates

Measure at the renderer's default 50% internal scale and at native 1280x720:

- Per-pass median and 95th percentile GPU time.
- LUT rebuild frame and steady-state frame separately.
- Ground, sunset, orbit, and handoff cases.
- Low/default/high quality.
- Planet-only and optional terrain volumetric shadows.
- Memory for LUTs, histories, and shadow maps.

Fail the default tier if steady-state ground atmosphere exceeds 0.5 ms on the
qualification GPU, allocates more than 2 MiB for the core LUT set without a
measured reason, or causes a measurable full-screen probe/reservoir reset every
cycle frame.

## Risks and mitigations

- **No final tone mapper:** keep current relative light units and expose one
  atmosphere radiance scale. Do not claim literal lux until display exposure
  exists.
- **Sun motion invalidates lighting histories:** introduce stable handles and a
  discontinuity epoch before enabling continuous time.
- **Large planet precision:** retain CPU doubles and camera-relative/normalized
  GPU frames; never upload an absolute 6,000 km camera translation as the basis
  of small local atmosphere differences.
- **Voxel/coarse seam:** centralize aerial composition after the merge and use
  the same ambient SH/direct sun in both representations.
- **Dense atmosphere numerical instability:** clamp transfer denominators,
  accumulate in FP32, expose debug counters, and validate hostile presets.
- **Sunset banding:** preserve transmittance resolution, nonlinear horizon
  mapping, blue-noise only where ray marching is temporally accumulated, and
  keep the sun disk separate.
- **Sky guiding adapts slowly:** initialize its CDF from atmosphere SH or a
  coarse Sky-View reduction after profile discontinuities; retain learned
  feedback during smooth sun motion.
- **Optional volume shadows become the main cost:** default to analytic planet
  shadow, cap sample counts, dispatch below full resolution, and require an
  explicit quality mode.

## Non-goals for the first release

- Spectral rendering.
- Spatially varying weather or atmosphere composition.
- Volumetric clouds and cloud multiple scattering.
- A fully astronomical sun/moon/planet ephemeris.
- Literal photometric units without final exposure/tone mapping.
- Making the coarse planet participate in sparse-voxel GI or occupancy tracing.
- Per-voxel atmospheric media.

## Definition of done

The system is complete when one authored atmosphere and one sun state drive all
visible background, direct light, diffuse sky light, aerial perspective, local
coarse planet, and overview planet paths; the day/night clock can run without
per-frame history resets; the voxel/background transition is visually
continuous; orbit view has a correct limb and terminator; the required
`PlanetOnly` fallback meets the minimum performance budget; and the default
terrain volumetric shadows can be disabled without breaking the base look or
resource graph correctness.
