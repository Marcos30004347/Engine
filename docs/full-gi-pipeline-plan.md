# Standalone World GI Pipeline Plan

## Goal

Build a standalone global illumination system based on the SIGGRAPH 2025 idTech 8 presentation flow:

```text
Update world light structures
    -> sample world visibility
    -> update world radiance cache
    -> update irradiance volumes
    -> final gather using caches
    -> denoise
    -> upscale
    -> composite separate GI outputs
```

This plan assumes the current DDGI and surfel backends are removed or ignored. The new system should stand on its own and use only the generic trace capabilities that already exist:

- screen-space traces
- CWBVH world traces
- SDF world traces
- hit-position shading/material resolve
- existing BRDF shader helpers in `assets/shaders/brdf`

The main design rule is: trace often, shade sparsely, reuse aggressively. Visibility rays and final gather rays should mostly produce hit packets. Full material and light evaluation should happen in persistent world caches and controlled update passes, not everywhere a ray lands.

## Non-Goals

- Do not depend on the current DDGI backend.
- Do not depend on the current surfel backend.
- Do not require baked lightmaps.
- Do not make the final compositor aware of specific GI algorithms.
- Do not denoise final color. Denoise radiance, irradiance, SH, or glossy radiance before composition.

## Primary Outputs

Keep outputs separate so each lighting type can be replaced later.

```text
GIDiffuseIrradianceSH
  2-band SH diffuse irradiance before albedo.
  Suggested storage: 3 x RGBA16F, one texture per color channel.
  Optional storage later: RGB9E5/packed FP16 after quality validation.

GIDiffuseLighting
  Full-resolution diffuse indirect lighting.
  rgb = evaluate_sh(GIDiffuseIrradianceSH, shadingNormal) * diffuseAlbedo
  a = confidence

GIGlossyRadiance
  Half or full-resolution indirect glossy/specular radiance.
  rgb = resolved glossy GI
  a = confidence or encoded hit distance

GIIndirectLighting
  Final replaceable indirect output.
  rgb = GIDiffuseLighting.rgb + GIGlossyRadiance.rgb
  a = combined confidence

GIAmbientOcclusion
  Indirect visibility scalar, independent from color.

GIBentNormal
  Average unoccluded direction for directional occlusion and specular occlusion.
```

Final composition:

```text
directLighting = existing direct lighting path
diffuseGI = GIDiffuseLighting.rgb
glossyGI = GIGlossyRadiance.rgb
indirectGI = diffuseGI + glossyGI
final = directLighting + indirectGI + emissive
```

The compositor only consumes `GIDiffuseLighting`, `GIGlossyRadiance`, and `GIIndirectLighting`. It should not care whether those came from world caches, irradiance volumes, screen probes, ReGIR, ReSTIR, or a future replacement.

## Core Data Model

All trace sources should resolve into one canonical surface record.

```text
GITraceSurface
  worldPosition
  hitT
  geometryNormal
  shadingNormal
  viewDirection
  rayDirection
  uv
  barycentrics
  baseColor
  roughness
  metallic
  emissiveRadiance
  materialId
  instanceId
  primitiveId
  sourceKind
  confidence
  flags
```

Source kinds:

```text
0 invalid
1 primary GBuffer
2 screen trace hit
3 CWBVH hit
4 SDF hit
5 emissive source sample
6 light/reservoir sample
7 sky/miss
```

Resolve rules:

- Screen hits fetch material data from the GBuffer.
- CWBVH hits fetch instance, primitive, barycentrics, UVs, normals, and material data.
- SDF hits fetch approximate material data from the SDF surface shading cache.
- Every hit exposes emissive radiance separately from base color.
- Every fallback path writes a confidence value and debug flags.

## High-Level Frame

```text
1. Prepare frame inputs
   GBuffer, depth, normals, material ids, motion vectors, direct lighting, HiZ.

2. Update trace scene
   CWBVH/TLAS, SDF bricks, material tables, emissive metadata.

3. Update world lighting acceleration
   Cascaded light grid, optional light BVH, emissive alias table.

4. Generate visibility samples
   Irradiance volume rays, screen probe rays, diffuse final gather rays,
   glossy rays, optional reservoir candidate rays.

5. Execute trace stack
   Screen trace first, then CWBVH, then SDF, then sky/miss.

6. Resolve hit shading
   Convert trace hits into GITraceSurface records.

7. Update world radiance cache
   Hash hit positions, build active list, shade active cache entries.

8. Update irradiance volumes
   Consume world radiance cache results and sky results.

9. Resolve screen probes
   Optional high-frequency screen-space cache.

10. Final gather
    Query screen cache, world radiance cache, irradiance volumes, then sky.

11. Denoise and upscale
    Separate diffuse SH, glossy radiance, AO, bent normal.

12. Composite outputs
    Produce GIDiffuseLighting, GIGlossyRadiance, GIIndirectLighting.
```

## World Light Structures

The ray-traced world cannot rely only on screen tile/cluster light lists. It needs world-space light lookup.

### Cascaded Light Grid

This should be the first standalone light culling structure because it maps directly to the SIGGRAPH presentation.

```text
CascadedLightGrid
  16 x 16 x 16 cells per cascade
  6 to 8 cascades
  exponential cascade extents
  first cascade roughly 32 meters wide
  fixed max ids per cell, for example 64
  id types: lights, reflection probes, decals, emissive ranges
```

Frame update:

```text
for each cascade:
    gather items whose bounds overlap the cascade
    for each cell:
        test item OBB/AABB against cell AABB
        write compact ids into cell list
        increment overflow if the list is full
```

Usage:

- World radiance cache shading.
- Glossy fallback probe selection.
- Transparency/froxel lighting later.
- Reservoir candidate generation.

### Light BVH

Add a light BVH after the grid is stable.

Use it for:

- far lights outside useful grid density
- many emissive mesh sources
- large area lights
- ReGIR/ReSTIR candidate sampling
- long glossy rays

The grid is best for predictable local lookup. The BVH is best for sparse long-range sampling.

## Emissive Source System

Emissive surfaces need an explicit sampling path. Accidental ray hits are useful but not enough.

Build a compact emissive source buffer:

```text
GIEmissiveSource
  sourceId
  instanceId
  primitiveId or primitiveRange
  bounds
  area
  materialId
  averageEmissiveRadiance
  powerWeight
```

Build an alias table:

```text
powerWeight = luminance(averageEmissiveRadiance) * area
```

Sampling:

```text
sample source from alias table
sample point on source
evaluate emissive material at UV
compute area-to-solid-angle PDF
trace visibility from receiver
combine with BRDF sampling using MIS
```

Ray-discovered emissive hits should append temporary source candidates for future frames. This captures textured emissive details before texture importance maps exist.

## Visibility Sampling

The system has four ray producers.

### Irradiance Volume Rays

Irradiance volume probes trace visibility from world-space cell centers.

```text
ray origin = probe position
ray direction = stable spherical sequence with per-frame rotation
ray length = cascade/local volume max distance
rays per probe = 16 low, 32 normal, 64 high
```

These rays do not directly write final lighting. They touch the world radiance cache and feed volume integration.

### Screen Probe Rays

Screen probes are optional, but useful as the first cache layer for final gather.

```text
one probe per 16x16 pixels or 32x32.
probe position = visible surface position
probe directions = octahedral or spherical layout
first trace = screen trace
fallback = CWBVH/SDF/world cache
```

They are high-frequency and short-lived. They should not be the only GI source.

### Diffuse Final Gather Rays

Diffuse final gather runs at half or quarter resolution.

```text
1 cosine-weighted ray per pixel per frame
blue-noise direction rotation
origin = visible surface + normal bias
trace order = screen -> CWBVH -> SDF -> sky
lighting = cache queries only
output = 2-band SH
```

### Glossy Rays

Glossy rays run separately from diffuse.

```text
enabled when roughness <= glossyTraceMaxRoughness
half or quarter resolution
direction = reflection vector or GGX VNDF sample
trace order = screen -> CWBVH -> SDF for rough fallback -> probes/sky
output = GIGlossyRadiance
```

For mirror-like surfaces, SDF fallback should be disabled or heavily confidence-weighted because SDF material and shape detail is too approximate.

## Trace Stack

All ray producers use the same trace stack:

```text
screen trace
    if valid and depth/normal confidence passes:
        sourceKind = screen
    else:
        CWBVH trace
            if valid:
                sourceKind = CWBVH
            else:
                optional SDF trace
                    if SDF backend is enabled and valid:
                        sourceKind = SDF
                    else:
                        sourceKind = sky/miss
```

Screen trace is a speed path, not an authority. CWBVH/SWRT is the main world-hit path for the next implementation milestone. SDF remains an optional low-frequency fallback path for broad occlusion and approximate distant surface data, but should be disabled by default until the sparse visibility pipeline is stable.

Each trace result should include:

```text
rayId
producerId
origin
direction
hitT
hit position
instance/primitive ids where available
UV/barycentrics where available
sourceKind
confidence
```

## Direct Shadow Visibility System

Direct ray-traced shadows should not be treated as "less than 1 ray per pixel means missing pixels." The target model is:

```text
sparse visibility samples
    -> reproject previous sparse samples/probes
    -> fill disoccluded holes with new samples
    -> geometry-aware reconstruction to full resolution
    -> temporal accumulation with confidence
    -> edge-aware spatial denoise
```

The sparse trace result is a cache field, not the final image. Full-resolution pixels query that field using depth, normals, plane distance, object/material ids when available, and motion-vector validated history.

### Reference Architecture

Use the local FidelityFX Brixelizer GI SDK as a concrete reference, not as a direct dependency:

- `ignore/FidelityFX-SDK-v1.1.4/docs/techniques/brixelizer-gi.md`
- `ignore/FidelityFX-SDK-v1.1.4/sdk/include/FidelityFX/gpu/brixelizergi/ffx_brixelizergi_main.h`
- `ignore/FidelityFX-SDK-v1.1.4/sdk/src/components/brixelizergi/ffx_brixelizergi.cpp`

Important patterns to copy:

- persistent ping-pong screen probe buffers
- spawn mask and per-tile random seed textures
- screen probe spawn, reproject, fill, project/interpolate passes
- disocclusion mask
- downsampled history depth, normals, roughness, motion vectors
- world-space cache fallback for camera stability

Brixelizer GI stores screen probes in an 8x8 octahedral layout and projects them into SH for diffuse GI. For shadows, Phase 5.2 can start with scalar visibility to prove tracing and reconstruction, but Phase 5.4 should move the probe payload to colored 8x8 directional/light-domain samples for many-light direct lighting.

### Shadow Probe Payload

Start with one screen shadow probe per 8x8 tile. Allow 4x4 in high-variance regions and 16x16 in low-quality modes.

```text
ShadowProbe
  spawnPixel
  worldPosition
  receiverNormal
  geometricNormal
  depth
  planeD
  lightId
  visibilityMean
  visibilityVariance
  historyLength
  confidence
  frameIndex
  flags
```

For area lights, extend the payload:

```text
ShadowProbeArea
  lightSampleUv
  selectedLightPdf
  visibleSampleCount
  totalSampleCount
  optional 4x4 or 8x8 light-domain visibility map
```

Do not start with ReSTIR. First make the probe cache truthful, stable, and inspectable.

### MegaLights-Style Many-Light Shadow Path

The many-light path should be MegaLights-inspired: fixed-budget stochastic direct lighting over many shadow-casting lights, with strong light selection, ray guiding, temporal validation, and denoising. It should not begin as full spatiotemporal ReSTIR DI. Public MegaLights material explicitly frames the system as stochastic direct lighting, and calls out practical scaling/correlation issues with reservoir-resampling approaches. For this renderer, the first useful version is therefore:

```text
visible receiver probe
    -> find candidate lights from cascaded light grid / light BVH
    -> rank/sample candidates by approximate unoccluded contribution
    -> trace a fixed number of visibility rays
    -> store colored direct-light samples in an 8x8 probe atlas
    -> reproject/validate probe samples over time
    -> reconstruct full-resolution lighting with geometry-aware weights
    -> temporal accumulate and denoise
```

This path estimates direct lighting, not only binary visibility. Probe samples should carry light color, attenuation, distance, cone/area-light shape, visibility, and PDF/selection weight. A point light sample should be able to add colored illumination even when the directional light is also present.

Initial candidate weighting:

```text
candidateWeight =
    luminance(lightColor * intensity)
  * attenuation(receiverPosition, light)
  * saturate(dot(receiverNormal, lightDirection))
  * approximateSolidAngle(light, receiverPosition)
  * lightTypePriority
```

The directional light must not starve finite local lights. Use reserved sample lanes until the light sampler is proven:

```text
lane 0: directional/sun candidate when present
lane 1: local point/spot/area candidate when present
lane 2+: weighted many-light candidates
```

Probe storage should preserve directionality. The raw probe debug view must show the actual 8x8 directional/light-domain atlas, not a scalar tile average:

```text
ShadowProbeSample
  lightId
  lightType
  sampleDirection
  lightSamplePosition
  lightDistance
  radiance
  visibility
  targetPdf
  selectedPdf
  confidence
  age
  flags
```

Use this payload as the future ReSTIR bridge. A later reservoir pass can reuse the same selected-light/sample fields, but it must not bypass receiver validation, per-sample age/confidence, or geometry-aware reconstruction.

### Shadow Trace Backend

For now:

```text
screen-space contact trace first
    if valid: use screen hit for high-frequency contact visibility
SWRT/CWBVH fallback
    if valid hit before light distance: visibility = 0
    if miss before light distance: visibility = ambient/sky visibility or 1 depending light type
SDF fallback
    disabled by default
```

Directional hard sun shadows should continue to use VSM/shadow-map style visibility as the base signal. Sparse RT shadows should be a soft-shadow/contact/refinement layer first, because binary hard visibility is too high-frequency for very sparse probes.

### Shadow Reconstruction

Every full-resolution pixel gathers nearby shadow probes and accepts only compatible receivers:

```text
normalWeight = pow(saturate(dot(pixelNormal, probeNormal)), normalPower)
planeDistance = abs(dot(pixelWorldPosition - probeWorldPosition, pixelNormal))
planeWeight = exp(-planeDistance / planeSigma)
screenWeight = exp(-screenDistanceSq / screenSigmaSq)
depthWeight = step(abs(pixelDepth - probeDepth), depthThreshold)
lightWeight = probe.lightId == selectedLightId ? 1 : 0

weight = normalWeight * planeWeight * screenWeight * depthWeight * lightWeight
visibility = weightedAverage(probe.visibilityMean)
confidence = saturate(weightSum / targetWeight)
```

Never blur visibility blindly across depth edges or receiver planes. Plane distance is mandatory to prevent wall-to-floor leaking.

### Temporal Reuse

Reproject the probe cache, not only the final full-resolution shadow mask.

Validation:

```text
valid =
  history pixel is inside viewport
  depth agrees after reprojection
  dot(previousNormal, currentNormal) > 0.95
  abs(dot(previousWorldPosition - currentWorldPosition, currentNormal)) < planeThreshold
  lightId matches
  object/material id matches when available
```

Each probe and reconstructed pixel carries `historyLength` and `confidence`.

```text
alpha = lerp(1.0, 0.05, confidence)
resolved = lerp(currentReconstruction, history, confidence)
```

Disoccluded pixels force `confidence = 0` and must not inherit stale visibility.

### Ray Budgeting

Keep the total ray budget stable, but move rays toward bad history:

```text
for each tile:
    if no valid reprojected probe:
        enqueue disoccluded tile
    else if local variance/high gradient:
        enqueue high-priority tile
    else:
        keep history and skip tracing

trace budget:
    disoccluded tiles first
    high variance / edge tiles second
    low confidence tiles third
    stable tiles only when budget remains
```

This mirrors the GI-1.0/Brixelizer idea: sparse samples are temporally upscaled and redistributed toward failed reprojection instead of randomly filling the whole screen every frame.

### Debug Requirements

Required debug outputs before tuning quality:

- raw traced probe visibility
- reprojected probe validity
- disocclusion mask
- probe confidence/history length
- reconstructed full-resolution visibility before temporal accumulation
- final temporal visibility before denoise
- denoised visibility
- tile/probe placement overlay
- per-tile reason for tracing: disoccluded, high variance, low confidence, forced refresh

## World Radiance Cache

The world radiance cache is the heart of the standalone system. It stores shaded world-space radiance at ray-discovered hit locations.

Use a spatial hash:

```text
baseCellSize = 0.25 m or 0.5 m
lod = power of two based on distance to camera
cellSize = baseCellSize * lod
positionKey = floor(worldPosition / cellSize)
normalKey = optional normal cone bucket
hash = hash(positionKey, lod, normalKey)
collision policy = bounded linear probing
```

Entry:

```text
GIWorldRadianceCacheEntry
  key
  worldPosition
  normal
  cellSize
  baseColor
  roughness
  metallic
  emissiveRadiance
  directDiffuseRadiance
  directSpecularRadiance
  indirectDiffuseIrradianceSH
  glossyRadiance
  confidence
  age
  lastFrameTouched
  materialId
  instanceId
  flags
```

Touch pass:

```text
for each GITraceSurface from probe/final-gather/screen-probe/glossy rays:
    reject invalid or low-confidence hits
    compute cache key
    find or allocate entry
    append entry index to active update list
    store rayId and producerId for shading
```

Shade pass:

```text
for each active cache entry:
    fetch GITraceSurface
    evaluate material if needed
    gather local lights from CascadedLightGrid
    optionally sample LightBVH/emissive alias table/reservoirs
    trace visibility for selected lights if required
    evaluate diffuse BRDF
    evaluate specular BRDF for cache metadata if useful
    add emissive radiance
    query previous-frame irradiance volumes for bounce lighting
    write radiance fields with temporal hysteresis
```

The first version can shade only:

```text
direct diffuse + emissive + previous irradiance volume bounce
```

Then add direct specular and glossy cache fields after diffuse is stable.

## Irradiance Volumes

Irradiance volumes are standalone world-space probe volumes. They are not the old backend.

Use two types:

```text
CascadedIrradianceVolumes
  camera-centered world clipmaps
  broad coverage around the camera

LocalIrradianceVolumes
  placed volumes for interiors, hero spaces, or leak-prone areas
  same probe payload, explicit transform and priority
```

Suggested layout:

```text
cascades: 4 to 6
probe grid per cascade: 12^3 low, 16^3 normal/high
local volume grid: 10^3 default
update cadence: 1 cascade + a small local-volume budget per frame
rays per active probe: 16/32/64 by quality tier
probe radiance atlas: octahedral 8x8 tile plus border
visibility atlas: mean distance and variance
```

Update:

```text
generate probe rays
trace screen/CWBVH/SDF/sky
resolve GITraceSurface
touch/update world radiance cache
for each probe ray:
    if hit:
        query world radiance cache at hit point
    else:
        sample sky
    integrate radiance into probe octahedral atlas
blend with history
```

Sampling:

```text
select best cascade/local volume
sample 8 nearest probes
weight by distance, normal agreement, and visibility
return diffuse irradiance SH or irradiance direction data
```

Volumes provide the last diffuse fallback in final gather. They also feed previous-frame bounce lighting into world cache shading.

## Screen Radiance Cache

Screen cache is the first lookup for final gather because visible information is cheap and detailed.

```text
ScreenRadianceCacheEntry
  screenProbeId
  worldPosition
  normal
  radiance directions or SH
  confidence
  historyAge
```

Update:

```text
place probes on visible surfaces
reproject previous probes
validate by depth, normal, and material id
trace a small ray set
use screen hits when valid
fallback to world cache/volumes
write screen radiance cache
```

Final gather query order:

```text
1. screen radiance cache if projected hit is valid and unoccluded
2. world radiance cache if the traced hit has a valid entry
3. irradiance volumes
4. sky
```

## Diffuse GI Resolve

Diffuse GI uses final gather rays but cache-only lighting.

```text
for each low-res gather pixel:
    reconstruct receiver surface from GBuffer
    generate cosine-weighted ray
    trace stack
    if screen-cache hit valid:
        radiance = screen cache
    else if world-cache hit valid:
        radiance = world cache
    else:
        radiance = irradiance volume at receiver or hit
    project radiance into receiver 2-band SH
    write GIDiffuseIrradianceSH
```

After denoise/upscale:

```text
irradiance = evaluate_sh(GIDiffuseIrradianceSH, shadingNormal)
GIDiffuseLighting.rgb = irradiance * diffuseAlbedo
GIDiffuseLighting.a = confidence
```

Use Lambertian diffuse first. Disney diffuse can be enabled later through a wrapper:

```text
evaluate_diffuse_brdf(model, albedo, roughness, n, wi, wo)
```

## Glossy GI Resolve

Glossy is independent from diffuse.

First version:

```text
for each selected low-res pixel:
    skip if roughness > threshold
    generate reflection/GGX ray
    screen trace
    if miss, CWBVH trace
    if miss and rough enough, SDF fallback
    if hit:
        resolve radiance from screen cache/world cache/direct+emissive estimate
    else:
        sample reflection probe or sky
    write GIGlossyRadiance
```

Reflection probe fallback:

```text
probeRadiance = filtered cubemap lookup
frameIrradiance = current irradiance volume/sample near receiver
probeIrradiance = stored diffuse irradiance for probe
refitRadiance = probeRadiance * safe_ratio(frameIrradiance, probeIrradiance)
```

Later:

- ReSTIR glossy reservoirs.
- Light BVH samples for reflected lights/emissives.
- roughness-bucketed world cache glossy lobes.
- MIS between BRDF rays and explicit emissive/light samples.

## ReGIR and ReSTIR Reservoirs

Reservoirs are optional sampling aids, not required storage.

### ReGIR World Reservoirs

Create a standalone world grid for light/source reservoirs.

```text
GIReservoirGrid
  camera-centered center region
  optional outer cascaded regions
  fixed reservoirs per cell: 1 to 4
  source types: lights, emissive surfaces, bright world-cache entries, reflection probes
```

Update:

```text
for each reservoir cell:
    sample candidates from light grid
    sample candidates from light BVH
    sample candidates from emissive alias table
    sample bright/stable world cache entries
    update reservoir with target PDF
    age stale samples
```

Use ReGIR in:

- world radiance cache shading
- explicit emissive/direct-light sampling for diffuse cache updates
- glossy source selection
- screen probe ray/source selection

### ReSTIR Screen Reservoirs

Use ReSTIR where the domain is screen pixels or screen probes.

```text
GIDiffuseFinalGatherReservoir
GIGlossyReservoir
GIScreenProbeReservoir
```

These can temporally and spatially reuse selected samples, but they should still write into the same diffuse/glossy outputs.

## Denoise and Upscale

Diffuse:

```text
input = low-res GIDiffuseIrradianceSH
spatial = separable bilateral or poisson bilateral
temporal = history with motion/depth/normal/material validation
upscale = bilateral 4-tap or 8-tap
output = full-res GIDiffuseIrradianceSH and GIDiffuseLighting
```

Glossy:

```text
input = low-res GIGlossyRadiance
temporal = roughness-aware accumulation and neighborhood clamp
spatial = radius grows with roughness
upscale = depth/normal/roughness-aware
output = full-res GIGlossyRadiance
```

AO/bent normal:

```text
filter separately
history reject on depth/normal changes
use for directional occlusion and specular occlusion
```

Useful filter weights:

- depth delta
- geometric normal delta
- shading normal delta
- roughness delta
- material id match
- hit distance delta
- cache confidence
- history length

## Resource Formats

Initial safe formats:

```text
World radiance cache radiance: RGBA16F fields or structured FP16 buffers
Irradiance probe atlas: RGBA16F
Visibility atlas: RG16F
Diffuse SH gather: 3 x RGBA16F
Glossy radiance: RGBA16F
Confidence/history: R16F or R8 + frame counter
```

Optimization candidates:

```text
RGB9E5 for irradiance/radiance after validation
packed oct normal for bent normals and probe directions
FP16 structured cache payloads
lower precision confidence/age fields
```

Avoid relying on R11G11B10F for cache/history until color shifts, banding, and pure-white behavior are tested.

## Standalone Pass List

Suggested new pass/module names:

```text
WorldGILightGridUpdatePass
WorldGILightBVHBuildPass
WorldGIEmissiveSourceBuildPass
WorldGIReservoirUpdatePass

WorldGIIrradianceVolumeRayGenPass
WorldGIScreenProbeRayGenPass
WorldGIDiffuseGatherRayGenPass
WorldGIGlossyRayGenPass

WorldGITracePass
WorldGIHitResolvePass

WorldGIShadowProbeSpawnPass
WorldGIShadowProbeTracePass
WorldGIShadowProbeReprojectPass
WorldGIShadowProbeFillPass
WorldGIShadowReconstructPass
WorldGIShadowTemporalPass
WorldGIShadowDenoisePass
WorldGIShadowDebugPass

WorldGIRadianceCacheTouchPass
WorldGIRadianceCacheShadePass
WorldGIRadianceCacheDebugPass

WorldGIIrradianceVolumeUpdatePass
WorldGIIrradianceVolumeResolvePass
WorldGIScreenProbeUpdatePass

WorldGIDiffuseResolvePass
WorldGIGlossyResolvePass
WorldGIDiffuseDenoisePass
WorldGIGlossyDenoisePass
WorldGIUpscalePass
WorldGIComposePass
```

Suggested directories:

```text
src/runtime/globalIllumination/worldgi
assets/shaders/globalIllumination/worldgi/wgsl
```

## Quality Tiers

Low:

```text
diffuse gather: quarter resolution, 1 ray/pixel
irradiance volume rays: 16 per active probe
irradiance volume grid: 12^3
screen probes: off or sparse
shadow probes: 16x16 or 8x8, stable-history reuse first
glossy: SSR/probes only
reservoirs: off
```

Normal:

```text
diffuse gather: half resolution, 1 ray/pixel
irradiance volume rays: 32 per active probe
irradiance volume grid: 16^3
screen probes: on
shadow probes: 8x8, disocclusion/high-variance queue
glossy: SSR + CWBVH fallback for smooth surfaces
reservoirs: optional for world cache shading
```

High:

```text
diffuse gather: half resolution, 1 to 2 rays/pixel
irradiance volume rays: 64 per active probe
more local volumes
screen probes: denser
shadow probes: adaptive 4x4 near variance, 8x8 elsewhere
glossy: CWBVH fallback + ReSTIR candidates
reservoirs: on
```

## Debug Views and Counters

Views:

```text
GI output split: diffuse, glossy, indirect
trace source: screen/CWBVH/SDF/sky
trace hit distance
trace confidence
world light grid occupancy
world light grid overflow
world radiance cache occupancy
world radiance cache age
world radiance cache confidence
world radiance cache direct radiance
world radiance cache indirect radiance
irradiance volume cascade index
irradiance volume probe validity
irradiance volume visibility
screen probe validity
shadow probe placement
shadow probe raw visibility
shadow probe reprojection validity
shadow disocclusion mask
shadow reconstruction confidence/history length
shadow reconstructed visibility
shadow temporal visibility
shadow denoised visibility
diffuse SH L0/L1
glossy hit distance/confidence
reservoir selected source type
```

Counters:

```text
screen trace hits/misses
CWBVH hits/misses
SDF hits/misses
sky misses
material fallback count
SDF surface-cache low-confidence count
world cache touched entries
world cache shaded entries
world cache lookup failures
world cache collisions/evictions
light grid overflow count
emissive source count
reservoir accepted/rejected samples
```

## Implementation Phases

### Phase 1: Standalone Skeleton

- Add `worldgi` runtime/shader directories.
- Add output textures and `WorldGIComposePass`.
- Add settings and debug toggles.
- Wire GI outputs without changing direct lighting when GI is disabled.

### Phase 2: Shared Hit Surface

- Define `GITraceSurface` CPU/WGSL layouts.
- Build `WorldGIHitResolvePass` over screen, CWBVH, and SDF trace results.
- Include roughness, metallic, emissive, source kind, and confidence.
- Add source/debug views.

### Phase 3: Cascaded Light Grid

- Build standalone `WorldGILightGridUpdatePass`.
- Store lights, reflection probes, decals, and emissive ranges per world cell.
- Add occupancy and overflow debug.
- Both the Grid and Probes will need to move using toroidal remapping, we already do that in VSM.
- Add settings to IMGUI such that we can control the size of the base cascade grid and grid resolution. We can use same defaults as VSM.
- Add debug options in imgui to overlay the grid cells on the final image like we do for VSM light pages. 

### Phase 4: World Radiance Cache

- Add spatial hash cache buffers.
- Add touch and active-list passes.
- Shade active entries with direct diffuse, emissive, and simple previous-frame bounce.
- Add cache query helpers for later passes.
- Check https://interplayoflight.wordpress.com/2025/11/23/spatial-hashing-for-raytraced-ambient-occlusion/ for spatial hashing
- Plug this into trace pipeline, on ray hits, for sdf and cwbvh, we should check if we have radiance hits in the cache, if we do and it is still alive (TTL of 120 frames) we reuse, if not we enqueue the hit for shading again and update the cache at the end. We can add a new update cache pass after hit shading results. 
- Screen hits should also update the cache, but I would like a setting to disable this to have a better view of how world traces are behaving.

### Phase 5.1: Light BVH
- We need to build a good structure to answer for every position what lights can cause changes into that pixel.

- We can implement a SceneLightManager. Every transform applied to lights is abstracted into this manager. When we perform an update, we record that a change occurred and perform a refit or rebuild depending on how many changes have occurred already and how bad the current tree is. We update the BVH only at the beginning of the frame using last frame information and upload it to the GPU for the next frame buffers.

### Phase 5.2: Shadow rays
- Replace the current "tile splat then denoise" prototype with a sparse shadow probe/cache pipeline.
- Default trace order for this phase: screen-space contact trace -> SWRT/CWBVH -> optional SDF disabled.
- One probe per 8x8 tile by default. Debug modes should allow 4x4, 8x8, 16x16, and forced full-rate.
- Probe spawn chooses a representative visible pixel using blue noise/per-tile seed, not a fixed box pattern.
- Each probe stores receiver position, normal, depth, light id, scalar visibility, variance, confidence, and history length.
- Trace only probes that need fresh data:
  - invalid or disoccluded history
  - high local visibility variance
  - low confidence/history length
  - light or receiver changed
  - periodic refresh
- Keep the total ray budget stable. Use disoccluded/high-variance tile queues to steal rays from stable tiles instead of increasing the frame cost.
- For local/area lights, store the selected light sample and PDF. Later this can grow into a small light-domain visibility map per probe.
- Miss policy:
  - hit before the light = shadowed
  - miss before the light distance = visible for finite lights
  - ray leaves the scene for ambient/sky queries = configurable ambient visibility
  - invalid ray/depth = no sample, not "lit"
- Debug output must include raw traced probe visibility, not only reconstructed/denoised visibility.

Pass breakdown:

```text
WorldGIShadowProbeSpawnPass
  input: depth, normal, motion, previous probe info, previous depth/normal
  output: current probe info, spawn mask, per-probe random seed, trace queues

WorldGIShadowProbeTracePass
  input: trace queues, light BVH/grid, depth/normal
  trace: screen contact first, then SWRT/CWBVH
  output: raw probe visibility samples

WorldGIShadowProbeReprojectPass
  input: previous probe info/visibility, motion vectors, current depth/normal
  output: reprojected probe visibility + validity/confidence

WorldGIShadowProbeFillPass
  input: reprojected probes, raw traced probes, spawn mask
  output: stable current probe cache
```

Acceptance criteria:

- No white default for unsampled pixels.
- No moving rectangular artifacts when the camera moves.
- Probe debug shows stable reprojection on static receivers.
- Fully shadowed regions do not randomly contain lit pixels unless the light-domain sample actually sees the light.
- SDF disabled + SWRT enabled still produces valid raw visibility.

### Phase 5.3: Shadow reconstruction and temporal accumulation
- Reconstruct full-resolution visibility from the probe cache with geometry-aware weights.
- Use screen distance, depth agreement, normal agreement, plane distance, light id, and object/material id where available.
- Never average across incompatible receiver planes.
- Produce these textures:
  - `WorldGIShadowRawProbeVisibility`
  - `WorldGIShadowProbeValidity`
  - `WorldGIShadowReconstructedVisibility`
  - `WorldGIShadowTemporalVisibility`
  - `WorldGIShadowDenoisedVisibility`
  - `WorldGIShadowDebugTiles`
- Temporal accumulation must validate history using motion vectors, depth, normal, receiver plane, and light id.
- Confidence controls alpha:

```text
new/disoccluded probe: confidence = 0, alpha = 1
stable probe: confidence approaches 1, alpha approaches 0.05
```

- Clamp history with neighborhood min/max from compatible probes/pixels.
- Use spatial denoise only after temporal accumulation. Denoise is not allowed to invent missing visibility before reconstruction.
- Use VSM/hard shadow maps as the hard direct-shadow base; sparse RT is a soft/contact/refinement layer until the probe cache is proven stable.

### Phase 5.4: MegaLights-like stochastic many-light shadows
- Replace single-light or directional-biased probe tracing with a fixed-budget stochastic direct-lighting path.
- Keep the total rays per frame stable. Quality comes from better light selection, temporal reuse, reconstruction, and denoising, not from silently increasing the ray count.
- Use the cascaded light grid for local candidate lookup and the light BVH for far/large lights.
- Start with current-frame RIS/reservoir-lite selection per probe. Do not enable full spatial ReSTIR until temporal reprojection and debug views are stable.
- The output of this phase is colored direct-light visibility/radiance per probe, not a black/white scalar.

Per-probe sample budget:

```text
low:      1-2 rays per active probe
medium:   4 rays per active probe
high:     8 rays per active probe
debug:   force one ray per lane or full-rate pixels
```

Light candidate generation:

```text
for each active shadow probe:
    gather nearby light ids from world light grid cell
    gather optional far/large candidates from light BVH
    reject lights outside range/cone/channels
    compute approximate unshadowed contribution
    build M stochastic candidates using blue noise + low-discrepancy sequence
    select N lanes with weighted reservoir sampling
```

Candidate weight:

```text
weight =
    luminance(lightColor * intensity)
  * attenuation
  * saturate(dot(receiverNormal, lightDirection))
  * approximateSolidAngle
  * receiverCompatibility
```

Reserved lanes:

```text
lane 0: best directional light candidate
lane 1: best finite local light candidate
lane 2+: weighted candidates from all valid lights
```

This prevents a strong sun from hiding point/spot/area lights during early development. Once the weighted sampler is proven, the reservation can become a debug/quality option.

Pass breakdown:

```text
WorldGIMegaLightCandidatePass
  input: shadow probes, cascaded light grid, light BVH, frame blue noise
  output: per-probe candidate lights with target weights and PDFs

WorldGIMegaLightSamplePass
  input: candidates, previous reservoirs, previous directional atlas
  output: selected sample lanes / reservoir-lite payload

WorldGIMegaLightTracePass
  input: selected sample lanes
  trace: screen contact first, then SWRT/CWBVH, SDF disabled by default
  output: visible/occluded sample radiance with hit distance and flags

WorldGIMegaLightProbeIntegratePass
  input: traced samples, previous probe atlas
  output: current 8x8 directional/light-domain probe atlas, confidence, age
```

Probe atlas requirements:

- Store 8x8 directional or light-domain radiance, not a scalar replicated across the tile.
- Store hit/light distance for parallax-aware temporal reprojection.
- Store confidence and age per atlas cell so untouched samples decay instead of persisting forever.
- Store light id/type per sample or per reservoir lane for validation and debug.
- Point, spot, rect/area, and directional lights should all contribute colored radiance using the same path.

Temporal reuse:

```text
for each previous atlas cell:
    validate previous receiver surface against current probe
    reconstruct previous sample hit/light point when distance is available
    reproject direction into current probe atlas
    reject if normal/plane/depth/light validation fails
    decay age/confidence for reused but unrefreshed samples
```

Do not only copy the previous tile. Per-sample reprojection is required to avoid camera-rotation ghosting and rectangular history artifacts.

ReSTIR upgrade path:

- Phase 5.4a: current-frame RIS only.
- Phase 5.4b: temporal reservoir reuse with strict receiver and light validation.
- Phase 5.4c: spatial reservoir reuse only between compatible nearby probes.
- Phase 5.4d: optional full ReSTIR DI normalization/MIS once the non-ReSTIR path is stable.

Debug outputs:

- candidate light count per probe
- selected light id/type per lane
- selected sample PDF and target weight
- raw traced visibility per lane
- 8x8 colored probe atlas
- per-cell confidence/age
- reservoir reuse accepted/rejected reason
- point/spot/area/directional contribution isolation
- final reconstructed many-light shadowed direct lighting before denoise

Acceptance criteria:

- A scene with one directional light and one point light shows both lights in the raw probe atlas and filtered result.
- Raw probe debug displays directional 8x8 content, not solid grayscale probe tiles.
- Camera rotation does not produce tile-stuck ghosting.
- Fully shadowed regions do not receive lit samples unless the selected light sample has real visibility.
- Disabling SDF while keeping SWRT/CWBVH enabled still produces correct visibility.
- Denoised output is smoother than raw output without erasing contact shadow detail.

### Phase 5.5: Checkerboard per-pixel shadow ray debug path
- Add an alternate shadow path that traces per-pixel checkerboard shadow rays instead of probe-based shadows.
- This is a validation and comparison path for Phase 5.4. It should make it easier to distinguish probe-cache artifacts from ray tracing, denoising, or light-selection artifacts.
- When this output is selected, disable the probe-based shadow passes so stale probe output cannot mix with the checkerboard result:
  - `WorldGIShadowProbeSpawnPass`
  - `WorldGIShadowProbeTracePass`
  - `WorldGIShadowProbeFillPass`
- Keep the light grid and light BVH enabled because the checkerboard ray generator still needs light selection.
- Keep SDF disabled by default for this path. Use SWRT/CWBVH first so the result is comparable to the current shadow probe backend.

Sampling pattern:

```text
checker = (pixel.x + pixel.y + frameIndex) & 1
if checker == 0:
    select one or more shadow light samples
    spawn shadow rays
else:
    mark pixel as untraced this frame with confidence = 0
```

Pass breakdown:

```text
WorldGIShadowCheckerboardRayGenPass
  input: scene depth, geometry normals, lights, light grid/BVH, frame index
  output: checkerboard shadow ray buffer, per-ray pixel/light metadata, current-frame confidence mask

WorldGIShadowCheckerboardTracePass
  input: checkerboard shadow rays
  trace: SWRT/CWBVH, SDF disabled by default
  output: ray hit/miss

WorldGIShadowCheckerboardResolvePass
  input: ray hits, ray metadata, current depth/normal
  output: raw full-resolution shadow radiance/visibility
  notes:
    traced pixels write current result with confidence = 1
    untraced checkerboard pixels write confidence = 0
    invalid depth pixels write zero confidence

WorldGIShadowCheckerboardTemporalPass
  input: raw checkerboard shadow, current confidence, current depth/normal, previous shadow history, previous depth/normal, motion/reprojection data
  output: temporally accumulated checkerboard shadow, updated history length/confidence
  notes:
    traced pixels blend current result with valid history
    untraced checkerboard pixels may reuse valid history
    disoccluded pixels reset history and confidence
    stable locked-camera pixels should approach a low alpha, around 0.05

WorldGIShadowCheckerboardDenoisePass
  input: temporally accumulated checkerboard shadow, history confidence, depth, normals
  output: denoised full-resolution checkerboard shadow
```

Temporal accumulation requirements:

- Run temporal accumulation before spatial denoise.
- Store checkerboard shadow history as radiance/visibility plus history length or confidence.
- Validate history using reprojected depth, normal, receiver plane distance, and motion/reprojection bounds.
- If object/material ids are available, include them in the validation.
- Treat confidence = 0 checkerboard holes as missing current data that can be filled by valid history, not as black.
- Use high current alpha for new or disoccluded pixels and gradually reduce alpha as history length grows.
- Clamp history to a compatible neighborhood around current traced pixels to avoid preserving stale dark or bright outliers.

Denoising requirements:

- Reuse the existing denoiser in the project first, before writing a bespoke filter.
- Use G-buffer depth and normal edge stopping.
- Treat confidence = 0 checkerboard holes as missing data, not as real black shadow.
- Consume the temporal pass output, not the raw checkerboard output.
- The denoised result should converge when the camera is locked and should not leave checkerboard structure visible after a few frames.

Debug outputs:

- raw checkerboard traced pixels
- checkerboard confidence/mask
- selected light id/type per traced pixel
- raw hit/miss visibility
- denoised checkerboard shadow
- history length/confidence

Acceptance criteria:

- Selecting `WorldGIShadowCheckerboard` disables the probe-based shadow passes.
- Raw debug clearly shows the checkerboard sampling pattern.
- Denoised debug fills untraced pixels without bleeding across depth or normal discontinuities.
- A locked camera visibly converges over multiple frames.
- Camera movement does not show probe-tile ghosting.
- The result can be compared directly against Phase 5.4 probe shadows for the same lights and SWRT backend.

### Phase 6: Irradiance Volumes
- We only need probes near surfaces.
- When we hit a surface we check if there is a probe nearby in the cells, if not we reuse a previously deallocated probe and allocate one in the position.
- We need to add an offset to avoid light leaking, there are modern approaches of solving this problem
- Add standalone cascaded and local irradiance volume resources.
- Generate probe visibility rays.
- Trace, resolve hits, touch world cache.
- Integrate cached radiance into probe atlases.
- Add volume sampling helpers.
- Use 8×8 octahedral map
- We need a cascaded probe system, maybe we can use one probe per cascaded grid cell. 
- Maybe add an option in imgui to draw one sphere (3d sphere) per probe in the world. They should not move with the camera; I should see new probes being allocated once the camera moves and the cascade allocates new probes using toroidal remapping. We can draw the 8x8 encoded radiance on each sphere. It should use the frame depth buffer to draw only visible probes.
- We need a budget for dirty probes per frame, we can also compute variance per region to decide what probes to consider dirty.
- When the camera moves we can use toroidal remapping to move the probes.
- Probes can use Ray guiding and other techniques to get most results from each ray.
Each frame:
  1. Pick active / dirty probes.
  2. Trace N rays per selected probe.
  3. Shade hit points using direct lighting + previous GI cache.
  4. Integrate samples into the probe’s octahedral irradiance map.
  5. Also update hit-distance / visibility moments.
  6. Blend with previous probe state using hysteresis.

A good default range:

Low cost:      8–32 rays per updated probe
Medium:       32–96 rays per updated probe
High quality: 128–288 rays per updated probe

### Phase 7: Diffuse Final Gather

- Generate low-res cosine-weighted rays.
- Trace and query screen cache/world cache/irradiance volume/sky.
- Store 2-band SH.
- Denoise, temporal accumulate, upscale.
- Output `GIDiffuseLighting`.

### Phase 8: GI Screen Probes

This phase reuses the shadow probe infrastructure for radiance instead of visibility.

- Add screen radiance cache with ping-pong probe buffers.
- Reproject/validate screen probe history using the same depth, normal, material/object, and plane tests as shadows.
- Use screen probes as the first final-gather lookup.
- Start each screen probe with screen traces, then SWRT/CWBVH fallback, then world cache/irradiance volume/sky fallback.
- Allocate one probe per 8x8 tile by default, with 16x16 for low quality and adaptive 4x4 where probe coverage fails.
- Use the local Brixelizer GI implementation as the template for:
  - spawn screen probes
  - reproject screen probes
  - fill screen probes
  - project/interpolate screen probes
  - per-tile random seed/spawn mask resources

Implementation notes:

```text
trace probes placed on visible pixels
adaptive downsample by changing probe tile size
interpolate only within compatible receiver planes
jitter placement grid over frames
temporally accumulate probe cache, not just final pixels
```

Debug requirements:

- screen probe placement overlay
- screen probe reprojection validity
- screen probe radiance/octahedral atlas
- screen probe confidence/history length
- adaptive tile size/priority reason

### Phase 9: Glossy GI

- Add glossy ray generation and resolve.
- Implement SSR first, CWBVH fallback second, probe/sky fallback last.
- Add roughness-aware denoise/upscale.
- Output `GIGlossyRadiance`.

### Phase 10: Reservoir Sampling

- Add ReGIR world reservoirs for cache shading and emissive/light selection.
- Add ReSTIR screen reservoirs for diffuse/glossy only after base signals are stable.
- Keep reservoir usage behind sampling interfaces.

## First Milestone

The first useful standalone milestone:

```text
screen/SWRT-CWBVH traces produce GITraceSurface
SDF backend remains optional and disabled by default
WorldGILightGridUpdatePass builds world light cells
WorldGIRadianceCacheTouchPass hashes trace hits
WorldGIRadianceCacheShadePass writes direct diffuse + emissive radiance
WorldGIShadowProbeSpawn/Trace/Reproject/Fill produce stable sparse visibility
WorldGIShadowReconstruct/Temporal/Denoise produce full-res shadow visibility
WorldGIIrradianceVolumeUpdatePass consumes the cache
WorldGIDiffuseResolvePass final-gathers cache/volume lighting into SH
WorldGIComposePass outputs a toggleable GIDiffuseLighting
```

This proves the architecture without DDGI or surfels: traces feed the world cache, sparse shadow probes reconstruct full-resolution direct visibility, the world cache feeds irradiance volumes, final gather uses caches, and the diffuse output is independent from glossy and final composition.
