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
                SDF trace
                    if valid:
                        sourceKind = SDF
                    else:
                        sourceKind = sky/miss
```

Screen trace is a speed path, not an authority. CWBVH is the main world-hit path. SDF is the low-frequency fallback path for broad occlusion and approximate distant surface data.

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
glossy: SSR/probes only
reservoirs: off
```

Normal:

```text
diffuse gather: half resolution, 1 ray/pixel
irradiance volume rays: 32 per active probe
irradiance volume grid: 16^3
screen probes: on
glossy: SSR + CWBVH fallback for smooth surfaces
reservoirs: optional for world cache shading
```

High:

```text
diffuse gather: half resolution, 1 to 2 rays/pixel
irradiance volume rays: 64 per active probe
more local volumes
screen probes: denser
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
- Both the Grid and Probes will need to move using toroidal remaping, we already do that in VSM.
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

### Phase 5: Irradiance Volumes

- Add standalone cascaded and local irradiance volume resources.
- Generate probe visibility rays.
- Trace, resolve hits, touch world cache.
- Integrate cached radiance into probe atlases.
- Add volume sampling helpers.
- Use 8×8 octahedral map
- We need a cascaded probe system, maybe we can use one probe per cascaded grid cell. 
- Maybe add an option in imgui to draw one sphere (3d sphere) per probe in the world. They should not move with the camera, I should see new probes being allcoated once the camera moves and the cascade allocate new probes using toroidal remaping. We can draw the 8x8 encoded radiance on each sphere. It should use the frame depth buffer to draw only visible probes.

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

### Phase 6: Diffuse Final Gather

- Generate low-res cosine-weighted rays.
- Trace and query screen cache/world cache/irradiance volume/sky.
- Store 2-band SH.
- Denoise, temporal accumulate, upscale.
- Output `GIDiffuseLighting`.

### Phase 7: Screen Probes

- Add screen radiance cache.
- Reproject/validate screen probe history.
- Use screen probes as first final-gather lookup.
- This is where we will generate the rays. 
- Each screen probe should start firing the rays in screen space with world fallbacks as we are doing.
- We may allocate one probe per 8x8 screen tile, or 16x16. 

● Trace from probes placed on pixels (screen probes)
○ Adaptive downsampling
● Interpolate to pixels within the same plane
● Jitter placement grid and temporally accumulate

Our approach is Screen Space Radiance Caching. We trace from probes which are placed on pixels on the screen, which we call screen
probes.
This is effectively adaptive downsampling. You can see on the left that the probes are placed uniformly, except where there’s more detailed
geometry, where we place more probes.
After we place the probes and trace from them, we interpolate their radiance to any other pixels within the same plane, which limits leaking
from the interpolation to pixels in the same plane, which is hard to notice.
We jitter the placement grid of the probes over frames and temporally accumulate to get good coverage.

Check Brixelizer Screen traces for how it its jittered. it is under ignore/FidelityFX-SDK-V1.1.4

- Need debug outputs to shows the placements of all probes in the screen. 
- Need debug outputs to display the irradiance of all screen probes. 
- We also need a probe priority, we will use temporal reprojection for the screen probes to make sure they accumulate, new probes needs more rays than probes that are active for a time, for example in newly disocluded areas and screen corners during camera rotation.
- Like lummen, we may need to add more probes in some screen regions, for example, maybe the probes does not give good coverage for the whole screen tile, we can take those regions and instead of 16x16 window, just in these regions we try 8x8 or 4x4, and we stop there and use flood fill. 

### Phase 8: Glossy GI

- Add glossy ray generation and resolve.
- Implement SSR first, CWBVH fallback second, probe/sky fallback last.
- Add roughness-aware denoise/upscale.
- Output `GIGlossyRadiance`.

### Phase 9: Reservoir Sampling

- Add ReGIR world reservoirs for cache shading and emissive/light selection.
- Add ReSTIR screen reservoirs for diffuse/glossy only after base signals are stable.
- Keep reservoir usage behind sampling interfaces.

### Phase 10: SDF improvements
- Improve SDF clip map generation such that we generate the whole cascade not just the regions that are not overlaping with finner ones.
- Allow Indirecg GI to trace rays on a coarse clip map. we need settings.
- Check if the current build path can handle small triangles, compare it with brixelizer

## First Milestone

The first useful standalone milestone:

```text
screen/CWBVH/SDF traces produce GITraceSurface
WorldGILightGridUpdatePass builds world light cells
WorldGIRadianceCacheTouchPass hashes trace hits
WorldGIRadianceCacheShadePass writes direct diffuse + emissive radiance
WorldGIIrradianceVolumeUpdatePass consumes the cache
WorldGIDiffuseResolvePass final-gathers cache/volume lighting into SH
WorldGIComposePass outputs a toggleable GIDiffuseLighting
```

This proves the architecture without DDGI or surfels: traces feed the world cache, the world cache feeds irradiance volumes, final gather uses caches, and the diffuse output is independent from glossy and final composition.
