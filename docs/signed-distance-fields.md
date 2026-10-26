# Signed Distance Fields

> Current per-instance sparse SDF design: see [Sparse Instance SDF](sparse-instance-sdf.md). That newer design keeps every scene instance as its own SDF surface and uses a TLAS sparse octree for world traversal instead of building a merged global SDF. The rest of this file describes an older global-SDF plan.

## Goal

Build a Lumen-style signed distance field path where static mesh SDFs are baked offline, streamed at runtime, and merged quickly into a runtime global SDF. The runtime should keep the current brixelized global SDF infrastructure for tracing and dynamic updates, but add a faster path that stamps prebuilt mesh SDF bricks instead of rebuilding every static mesh from triangles.

The target system has four cooperating parts:

- offline mesh SDF assets
- a runtime global SDF built from sparse clipmap bricks
- surface or surfel pages used to shade SDF ray hits
- feedback-driven page streaming similar to virtual textures

The current engine already has much of the runtime foundation:

- `src/runtime/gpusigneddistancefields/GPUSignedDistanceFieldsManager.hpp`
- `src/runtime/gpusigneddistancefields/rendering/GPUSignedDistanceFieldsPass.hpp`
- `assets/shaders/gpusigneddistancefields/wgsl/gpusigneddistancefields-build.wgsl`
- `assets/shaders/gpusigneddistancefields/wgsl/gpusigneddistancefields-combine.wgsl`
- `assets/shaders/gpusigneddistancefields/wgsl/gpusigneddistancefields-trace-common.wgsl`
- `src/runtime/globalIllumination/SDFHitShadingResolvePass.hpp`
- `src/runtime/globalIllumination/SDFRayTraceEvaluateMaterialPass.hpp`
- `src/runtime/virtualgeometry/VirtualTextureSystem.hpp`
- `src/runtime/globalIllumination/surfels/SurfelPersistenceTypes.hpp`

This document rewrites the old notes into an implementation design and a phased plan.

## Lumen Model To Borrow

The local SIGGRAPH 2022 Lumen presentation describes the same basic split we want:

- Mesh SDFs are generated on mesh import.
- Mesh SDFs are stored as sparse virtual volume textures, using 8^3 bricks and narrow-band distances.
- Runtime software tracing sphere-traces SDFs and computes normals from distance gradients.
- The global SDF merges mesh distance fields into camera-centered clipmaps.
- Static and dynamic global SDF data are cached separately, then composited for tracing.
- Surface cache cards provide material and lighting at SDF hit points because an SDF hit only gives position and normal.
- GPU feedback requests missing pages, then CPU streams the requested pages into a fixed physical pool.

The important lesson is that distance fields are the visibility representation, while cards/surfels are the shading representation. We should keep that separation. The global SDF should not carry full material parameters per voxel.

## Current Engine Baseline

The current GPU SDF path is already a brixelizer-style cascaded global SDF:

- `GPUSignedDistanceFieldsManager::Settings` owns cascade count, first voxel size, voxel scale, brick capacity, and surface cache toggles.
- Cascades are 64^3 voxels.
- Each occupied voxel points at an 8^3 brick in `sdfAtlas`.
- The build path has separate `Static`, `Skinned`, and `Combined` paths.
- Static and skinned paths build their own brick maps, then `GPUSignedDistanceFieldsCombineCascadesPass` merges them.
- The current build path consumes fallback mesh triangles, voxelizes references, emits SDF bricks, compresses bricks, runs Eikonal propagation, builds brick AABBs, and builds the trace AABB tree.
- Tracing uses the combined brick maps and AABB trees.
- Surface cache material references already exist at brick-face granularity:
  - `surfaceCacheMaterialReferenceAtlas`
  - `surfaceCacheMaterialReferenceBasisAtlas`
  - `surfaceCacheMaterialReferenceScores`

This means the new work should not replace the global SDF. It should add an alternate static build source:

```text
offline mesh SDF asset
    -> stamp/resample into static cascade bricks
    -> current static/skinned combine path
    -> current combined global SDF tracing path
```

## Direct Answers

### Can We Combine Offline SDFs Fast?

Yes, if the offline SDF assets are stored in a format that matches the runtime brick model closely enough.

Fast combination should be done by resampling precomputed mesh SDF bricks into dirty global SDF bricks, then taking the minimum signed distance among contributors. The cost becomes proportional to affected global brixels and overlapping SDF instances, not proportional to mesh triangle count.

The fast path is:

1. CPU or GPU builds a list of dirty global voxels/bricks per cascade.
2. GPU culls offline SDF instances against those dirty bricks.
3. For each dirty global brick, GPU samples each overlapping asset SDF in instance-local space.
4. GPU writes the minimum signed distance into the global brick.
5. The existing Eikonal, AABB, and combine passes repair the runtime representation for tracing.

This is much faster than triangle voxelization for static assets because each brixel does a small number of volume samples instead of triangle reference scans.

The main caveat is removal. Min-composition is easy to add but impossible to subtract in-place. When a static instance moves, unloads, or changes scale, the old affected bricks must be invalidated and rebuilt from all remaining contributors. That is the same dirty-brick rebuild model Lumen uses for cached global SDF updates.

### Should We Generate SDFs On The GPU At Runtime?

For skinned meshes, procedural meshes, and assets without baked SDFs, yes. Reuse the current brixelizer build path.

For normal static meshes, no. Offline generation is better because it avoids runtime triangle scanning, supports higher-quality sign generation, and can precompute mips plus surface pages. Runtime GPU generation should remain the fallback path, not the default static path.

### Can Surface Surfels Generate The SDF?

Not as the primary visibility representation. Surfels/cards are good shading samples, but they do not reliably provide closed-volume sign, watertight occlusion, or thin-feature coverage. A surfel cloud can seed an approximate unsigned distance field or update a local dynamic cache, but it should not replace mesh SDF import for robust tracing.

Use surfel pages to shade SDF hit results. Use mesh SDFs for hit finding.

## Target Runtime Trace Policy

Different GI paths should use SDF data differently:

- Diffuse indirect GI can use the global SDF for broad, stable world traces.
- Rough glossy can trace screen first, then use short instance SDF traces, then global SDF or world radiance cache on miss.
- Mirror-like glossy should prefer screen/CWBVH/HWRT and treat SDF as low-confidence.
- Probe/world radiance traces can use the global SDF for distant visibility.
- Local contact and very short rays should prefer screen traces or instance SDFs where available.

Suggested trace order:

```text
screen trace
    -> CWBVH or hardware ray tracing when enabled
    -> instance SDF for short/local glossy rays
    -> global SDF for broad fallback and diffuse/world probes
    -> sky/probe/radiance cache miss
```

The global SDF does not need full material allocation. Hit shading should query surface pages, material virtual textures, surfel radiance pages, or world probes.

## Offline Scene SDF Assets

Add an offline asset next to virtual geometry output. The concrete extension can be decided later; this document uses `.vsdf`.

The `.vsdf` file should be a scene-capable container, not only a single mesh SDF. Importing a scene like Sponza should be able to produce one `.vsdf` with shared mesh SDF data plus multiple instance records. The header owns full-scene metadata, while tables describe meshes, instances, SDF chunks, cards, and shading-cache images.

### Asset Contents

```text
VSDFSceneFile
  header
    magic/version
    full scene bounds
    unit scale
    source asset path/hash
    mesh count
    instance count
    material count
    card count
    shading image count
    sdf chunk count
    table offsets

  mesh table
    local bounds
    distance units and quantization metadata
    mip count
    brick resolution = 8
    narrow band distance range
    per-mip page table ranges
    compressed SDF chunk ranges
    surface cache card ranges

  instance table
    mesh index
    material remap range
    local to world
    world to local
    previous local to world optional
    world bounds
    flags

  card and page tables
    card descriptors
    card lookup grids
    virtual page descriptors
    fallback page links

  shading image table
    shared UV/page layout id
    parameter kind
    format
    width/height
    mip count
    payload offset/size

  compressed SDF payloads
  shading image payloads
```

Distances should be stored in a normalized narrow band. A good first implementation can use `R8_SNORM` or `R16_SNORM` source payloads and expand to the current runtime `r32float` atlas when stamping. Later, the runtime atlas can be compressed or changed if memory pressure demands it.

SDF brick payload chunks should be LZ4-compressed on disk:

```text
VSDFSdfChunk
  meshIndex
  mip
  firstBrick
  brickCount
  uncompressedSize
  compressedSize
  compression = LZ4
  payloadOffset
```

Use chunked compression rather than one huge blob so streaming can load a mip range or brick range without decompressing the whole file.

The offline mip hierarchy should be explicit. Runtime stamping needs to choose the source mip based on target cascade voxel size and instance scale.

### Shading Parameter Images

The offline shading cache should write one image file or image payload per shading parameter, and every parameter image must share the same card/page UV space.

The cache should support both shared mesh pages and per-instance pages:

```text
shared mesh card pages
  used when instances share the same mesh and material bindings

instance card pages
  used when an instance has unique material remaps, baked material parameters,
  or needs instance-specific debug/output visibility
```

For a scene container such as Sponza, the `.vsdf` should have one instance table for the whole scene and each instance should point at either shared mesh surface pages or an instance-specific surface page range.

Example parameter set:

```text
albedo
normal
roughness
metallic
emissive
opacity
depth
validity
materialId
uvBasis
```

All images use the same virtual page layout:

```text
SurfaceCacheImageSet
  layoutId
  pageSize
  maxOfflinePageResolution = 256 | 512 | 1024
  card page table
  images:
    albedo      same width/height/mips
    roughness   same width/height/mips
    metallic    same width/height/mips
    normal      same width/height/mips
    depth       same width/height/mips
    validity    same width/height/mips
```

This means a card texel maps once:

```text
assetId/cardId/mip/pageX/pageY/localTexel
```

Then every parameter samples the same atlas coordinate or offline sheet coordinate.

For debugging and validation, the importer should be able to export these parameter images as ordinary image files:

```text
scene.vsdf
scene_surface_albedo.png
scene_surface_roughness.png
scene_surface_metallic.png
scene_surface_normal.png
scene_surface_validity.png
scene_surface_depth.exr or r16 image
```

Those debug images should show the packed card/page layout for all baked instance pages in the file. If some instances share mesh pages, the debug overlay should annotate or dump the instance-to-page mapping so it is clear which instances reuse the same texel regions. The final `.vsdf` can either embed the payloads or reference sidecar images, but the first implementation should prefer embedding or adjacent deterministic sidecars so the scene container remains reproducible.

### Brick Layout

Use the same logical brick size as the runtime:

```text
brickResolution = 8
stored samples = 8 x 8 x 8
distance range = [-band, +band] in local/object units
brick occupancy = present only when narrow band intersects the brick
```

If we later need Lumen-style border samples, add a border in the offline asset and resolve into the runtime 8^3 brick during stamping. Do not require border samples in phase one.

### Offline Build Algorithm

For each virtual geometry mesh or fallback mesh:

1. Choose SDF voxel size from mesh bounds and quality settings.
2. Build a dense or tiled temporary distance grid.
3. Compute unsigned distance to nearest triangle.
4. Compute sign using robust ray parity or winding. A CPU Embree-style point query is ideal when available; otherwise use the engine's mesh acceleration or a conservative offline BVH.
5. Quantize to narrow-band signed distance.
6. Emit sparse 8^3 bricks for all cells intersecting the narrow band.
7. Build mips by min/max aware distance downsampling, not naive averaging.
8. Generate surface pages/cards for material and lighting lookup.
9. Store a pinned coarse fallback level.

The current fallback mesh builder already emits SDF proxy data for runtime triangle builds. The offline builder should reuse virtual geometry import data and fallback mesh generation rather than introduce a second source of mesh truth.

### Small Triangle And Thin Surface Emission

Small triangles are a special case because a voxel-center SDF can miss them entirely. If a triangle is thinner than the sample spacing or lies between samples, the field may never cross zero and sphere tracing can leak through it.

Classify triangles during SDF emission:

```text
cellSize = target voxel size for this mip
triangleExtent = max edge length or projected bbox extent
triangleAltitude = min altitude against its longest edge

isSmallTriangle =
    triangleExtent < 2 * cellSize ||
    triangleAltitude < 1 * cellSize ||
    projected area covers less than one texel in the dominant projection
```

For normal triangles, emit distances from the exact triangle into every brick sample touched by the triangle's narrow-band bounds.

For small triangles, emit a conservative swept surface:

```text
expandRadius = 0.5 * voxelDiagonal
expandedBounds = triangleBounds expanded by expandRadius

for each sample in expandedBounds:
    d = distance(samplePosition, triangle)
    emittedDistance = d - expandRadius
    write min(existingDistance, emittedDistance)
```

This makes tiny triangles visible to the SDF even if no sample lands exactly on the original triangle. The expansion is a visibility fix; hit shading still uses the surface cache/card data to recover material and normals.

Use the same idea for thin walls and open sheets:

- expand by at least half a voxel diagonal at the target mip
- clamp negative regions to the narrow band so open meshes do not create infinite interiors
- keep the original closest triangle/material reference for surface cache lookup
- pull the shading point back toward the original surface before sampling material pages

Offline SDF mips must preserve small features conservatively. Do not average distances in a way that erases thin geometry. Downsample with a min-distance rule and keep an occupancy/coverage bit when any child brick contains expanded surface coverage:

```text
parentDistance = min(childDistances)
parentCoverage = any(childCoverage)
```

If a feature is too small for a coarse mip, the coarse mip should still contain a conservative occluder or mark the page as requiring a finer resident mip near the camera.

The runtime brixelizer fallback already has a `TriangleSmallFlag` path that stamps a tiny triangle by center into the nearest brixel. Keep that as the emergency path, but the offline builder should prefer the swept-triangle emission above. A later runtime improvement should replace center-only small-triangle stamping with the same expanded-bounds stamp used offline.

### SDF Mip Selection

At runtime, source mip should be selected from:

```text
targetWorldSampleSpacing = globalCascadeVoxelSize / 7
localSampleSpacing = targetWorldSampleSpacing / conservativeInstanceScale
sourceMip = coarsest resident mip whose local voxel spacing <= localSampleSpacing
```

For non-uniform scale, distances should be conservative. The first version can multiply sampled local distance by the minimum axis scale. This underestimates distance under non-uniform scale, which is safer for occlusion but increases ray steps. A later version can use gradient-aware correction.

## Global SDF Design

Keep `GPUSignedDistanceFieldsManager` as the global SDF owner.

The current runtime data model is already close to the target:

```text
CascadeInfoGPU
  gridMin/gridMax
  voxelSize
  clipmapOffset
  initialization and wrapping state

PathResources
  instances
  cascade brick maps
  cascade AABB trees
  jobs
  scratch buffers

SharedResources
  sdfAtlas
  brick voxel map
  brick AABB
  brick free list
  dirty/merge lists
  cascade infos
  surface cache atlases
```

Add a new static-offline ingestion path, either as a new path enum or as a mode inside the current static path:

```text
Path::StaticTriangleBuild   existing static brixelizer path
Path::StaticOfflineSdf      new baked SDF stamp path
Path::Skinned               existing dynamic/skinned path
Path::Combined              existing trace path
```

If adding a new enum causes too much churn, keep `Path::Static` and add an SDF source mode in the build pass settings. The end state should still produce the same static `brickMaps`, `sdfAtlas` bricks, and AABB data consumed by the combine pass.

### Fast Offline Stamping Passes

Add a composite pass similar to `GPUSignedDistanceFieldsBuildPass`, but for baked SDF assets:

```text
GPUSignedDistanceFieldsOfflineSdfBuildPass
  clear stamp counters
  collect dirty bricks
  cull offline SDF instances to cascades
  cull offline SDF instances to dirty bricks
  allocate or clear target bricks
  stamp/resample offline SDF samples into target bricks
  update brick AABBs and surface page references
  enqueue dirty bricks for Eikonal/AABB repair
```

The key shader is the stamp kernel:

```text
for each dirty global brick:
    clear target samples to +band
    for each overlapping offline SDF instance:
        for each 8^3 brixel:
            worldPosition = global brick brixel center
            localPosition = inverse(instanceTransform) * worldPosition
            sourceDistance = sample asset SDF mip at localPosition
            worldDistance = sourceDistance * conservativeScale
            targetDistance = min(targetDistance, worldDistance)
            record material/surface page candidate if close to surface
    write target brick
    update brick AABB from near-surface samples
```

For pure union composition of signed distance fields, `min(a, b)` is the operation we want. Narrow-band quantization and clamping can still create edge cases around overlapping interiors, so phase one should add validation cases for overlapping closed meshes, touching meshes, and one mesh fully inside another.

### Dirty Brick Tracking

Dirty regions come from:

- clipmap scrolling
- newly loaded offline SDF pages
- instance transform changes
- instance add/remove
- asset quality/mip residency changes
- skinned/dynamic invalidation

For static offline instances, cache the last stamped world bounds per cascade. On movement/removal:

```text
dirtyBounds = union(previousStampedBounds, currentStampedBounds)
mark all overlapping global bricks dirty
rebuild dirty bricks from current contributors
```

The current `GPUSignedDistanceFieldsSceneManager` already tracks instance build state and clipmap exposed slabs. Extend that model to emit offline SDF jobs.

### Static And Dynamic Separation

Keep static and dynamic SDFs separate until final combine.

```text
static offline/triangle SDF
    -> static brick map

skinned/runtime brixelizer SDF
    -> skinned brick map

static + skinned
    -> combined brick map
    -> combined AABB tree
    -> tracing
```

This makes dynamic updates cheaper and keeps static cache invalidation localized.

### Reusing Brixelizer Structures

Reuse these current structures:

- `CascadeInfoGPU`
- `InstanceGPU` for transforms and bounds, or a new smaller `OfflineSdfInstanceGPU`
- cascade brick maps
- `sdfAtlas`
- `contextBrickVoxelMap`
- `contextBrickAabb`
- free/dirty/merge lists
- combined AABB trees
- trace shaders
- debug atlas/trace passes

Add these structures:

```text
OfflineSdfAssetGPU
  pageTableOffset
  mipOffset
  localBoundsMin
  localBoundsMax
  voxelSizeMip0
  mipCount
  flags

OfflineSdfInstanceGPU
  modelMatrix
  inverseModelMatrix
  previousModelMatrix
  worldBounds
  assetIndex
  conservativeScale
  flags

OfflineSdfPageTableEntryGPU
  physicalBrickId
  residentMip
  flags

OfflineSdfPhysicalBrickGPU
  assetId
  mip
  brickCoord
  physicalAtlasOffset
```

The physical offline SDF brick pool can initially be a buffer instead of a sampled 3D texture. The stamp shader can read quantized brick samples from a storage buffer and write expanded floats into the existing runtime `sdfAtlas`.

## Instance SDF Acceleration

The global SDF is best for broad visibility, but short glossy rays benefit from tracing individual mesh SDFs before falling back to the lower-resolution global SDF.

Add a small top-level acceleration structure for instance SDFs:

```text
SdfInstanceGrid
  cascades: 4 to 6
  cells per cascade: 8^3 or 16^3
  cell list: compact instance ids
  instance table: transform, inverse transform, bounds, asset id
```

Trace flow for short instance SDF rays:

1. Traverse grid cells along the ray.
2. Fetch candidate instances in each cell.
3. Intersect ray with each instance local bounds.
4. Sphere-trace the mesh SDF asset directly.
5. Return nearest hit with asset id, instance id, local UVW, and normal.
6. If no hit within the short max distance, trace the global SDF or sample world radiance cache.

This is close to the Lumen two-level software tracing design: a flat instance list on top, mesh SDFs at the bottom.

Phase one can skip this and use only the global SDF. Add it once global SDF stamping and hit shading are stable.

## Surface And Surfel Pages

SDF hit results need shading data. The hit provides:

- world position
- world normal
- hit distance
- cascade and brick id for global hits
- local UVW for instance SDF hits

That is not enough to evaluate material and lighting. We need surface pages.

### Two Page Types

Use two related but distinct page concepts:

```text
Surface material pages
  stable material lookup
  generated offline when possible
  stores material id, UV basis, normal/roughness hints, opacity/emissive flags

Surface radiance pages
  runtime lighting cache
  updated over time
  stores filtered radiance/irradiance for SDF hit shading
```

This mirrors Lumen: mesh SDFs solve visibility; surface cache cards solve material and lighting.

### Offline Surface Pages

At import time, generate surface cards or surfel pages per mesh asset.

For a first implementation, use six direction-aligned card families per mesh or per SDF brick. The current global SDF already stores per-brick face material references, so we can evolve from that:

```text
brick id + face id + page texel
    -> material id
    -> UV origin
    -> UV basis/window
    -> quality score
```

The existing shader path writes:

- `SurfaceCacheMaterialReferenceAtlas`
- `SurfaceCacheMaterialReferenceBasisAtlas`
- `SurfaceCacheMaterialReferenceScores`

For baked SDFs, generate equivalent references offline and stamp/copy them into the runtime surface cache when global bricks are stamped. Later, replace the brick-face layout with asset-level cards if we need higher resolution.

### Surface Cache Generation Algorithm

Generate surface shading caches from mesh data in two layers:

- an offline, asset-local card set that captures material and geometry parameters
- a runtime, world-space indirection grid that lets global SDF hits find candidate cards without knowing instance identity

The first implementation should use conservative axis-aligned cards. Lumen tried arbitrary cards and settled on axis-aligned cards because generation and lookup are simpler.

Offline generation:

```text
input:
  virtual geometry mesh or fallback mesh
  material ids
  vertex positions, normals, uvs
  optional baked mesh SDF

output:
  SurfaceCacheAsset
    local bounds
    card descriptors
    card page table
    offline page sheets with max resolution 256, 512, or 1024
    low-res fallback pages
    material/depth/normal/validity payloads
```

Build steps:

1. Simplify the mesh into axis-aligned surfels.
2. Cluster surfels into cards.
3. Assign each card a local-space frame, bounds, projection axis, and resolution.
4. Allocate each card into one or more bounded offline page sheets.
5. Rasterize or ray project mesh triangles into each card page.
6. Store material references, UV basis, depth, normal, opacity/validity, and optional emissive hints.
7. Build fallback mips and pin the coarsest pages.

Offline page resolution is per asset or per instance cache, not the runtime 16k atlas size:

```text
SurfaceCacheBuildSettings
  maxOfflinePageResolution = 256 | 512 | 1024
  cardPageSize = 64 or 128
  minCardResolution = 8
  maxCardResolution = maxOfflinePageResolution
  texelsPerWorldUnit or texelsPerSdfVoxel
```

The offline builder packs card payloads into small page sheets:

```text
SurfaceCacheOfflineSheet
  width <= maxOfflinePageResolution
  height <= maxOfflinePageResolution
  pageSize
  pageCountX
  pageCountY

SurfaceCacheOfflinePage
  sheetId
  pageX
  pageY
  mip
  cardId
  flags
```

The runtime 16k atlas is only the global physical cache where these smaller offline pages are uploaded when requested.

A practical surfel generation method:

```text
for each major axis pair:
    create a 2d grid over the mesh bounds
    for each grid cell:
        cast N rays through the mesh along the remaining axis
        collect hit intervals and hit triangles
        spawn one surfel for stable covered surfaces
        store:
            local position
            local normal
            coverage
            dominant material id
            triangle id or UV basis
            projection axis
```

`N = 16` rays per cell is enough for phase one. Increase toward `64` for final quality or difficult meshes. The coverage value decides whether the surfel is useful enough to keep and later helps card clustering.

Card clustering:

```text
group surfels by projection axis and normal sign
split by disconnected regions and large depth discontinuities
cluster nearby surfels with similar normals/material ranges
for each cluster:
    fit an axis-aligned card plane
    compute 2d bounds in card space
    choose resolution from projected size and texel density
    discard tiny cards below LOD threshold
```

Card capture:

```text
for each card:
    for each texel:
        project texel center into mesh local space
        find closest covered triangle/hit along card projection axis
        write depth in card space
        write validity/opacity
        write material id
        write UV origin and UV gradients or compact UV window
        write geometric normal and shading normal
        write roughness/metallic/emissive hints if cheap
```

Store material identity and UV mapping, not fully evaluated material color, unless the source material is static and cheap to bake. Runtime virtual textures can still provide final base color and other material data when the page is captured or updated.

Card page allocation:

```text
for each card:
    projectedSize = card bounds in card space
    desiredResolution = projectedSize * texelDensity
    desiredResolution = clamp(desiredResolution, minCardResolution, maxCardResolution)

    if desiredResolution fits one offline sheet:
        allocate rectangle from sheet packer
    else:
        split card into tiles no larger than maxOfflinePageResolution
        allocate one page range per tile

    build mip chain down to resident fallback
```

If the asset exceeds its offline cache budget:

```text
reduce tiny card resolution first
merge nearby cards with similar projection axis and depth range
drop cards below screen-size or SDF-voxel-size threshold
keep the coarse fallback pages
record dropped coverage for diagnostics
```

The cache can also be generated directly from the SDF build data. When baking the mesh SDF, each near-surface voxel/brixel can store the closest triangle and barycentrics. Those references are useful for card validation and for bootstrapping brick-face material references:

```text
for each near-surface SDF sample:
    closestTriangle
    barycentrics or UV
    surfaceNormal
    signedDistance
    sourceBrick

use samples to:
    seed surfels
    score card coverage
    fill sparse card texels when raster coverage is missing
    build SDF-brick to surface-card lookup hints
```

Quality rules:

- Invalid texels must be explicit. Missing surface cache coverage should lose energy or fall back, not project unrelated lighting through walls.
- Depth is required. It rejects samples that project onto the wrong surface behind the hit.
- Normals are required. They choose the best cards and avoid stretched projections.
- Coarse fallback pages are required. Missing high-res pages should still shade approximately.
- Cards should have LOD metadata so distant cards can be removed or merged.

Runtime page capture/update:

```text
for each resident surface page selected for update:
    render/capture the owning mesh, card group, or cached material references
    resolve virtual material textures
    write material payloads
    trace or gather direct/indirect lighting for radiance payloads
    temporally blend radiance
    mark page valid
```

This can start with CPU-built material reference pages and GPU-updated radiance pages. Later, card capture can render the mesh from the card's orthographic projection, which is closer to Lumen's model and handles complex materials better.

### Lumen-Style Virtual Page Layout

Offline generation creates virtual pages, not runtime physical atlas allocations. The mesh is not split directly into pages. Instead:

```text
mesh triangles
    -> surfels
    -> cards
    -> card-local 2d domains
    -> virtual card pages
    -> bounded offline source sheets
    -> runtime physical atlas residency
```

Each card owns a 2d parameterization:

```text
SurfaceCacheCard
  localToCard
  cardToLocal
  localBounds
  cardRectMinMax
  projectionAxis
  projectionNormal
  resolutionX
  resolutionY
  mipCount
  pageTableOffset
  lookupGridMask
  flags
```

The card domain is split into fixed virtual pages:

```text
pageSize = 64 or 128
pagesX = ceil(cardResolutionX / pageSize)
pagesY = ceil(cardResolutionY / pageSize)

virtualPageKey:
  assetId
  cardId
  mip
  pageX
  pageY
```

The offline sheet only stores source payload for those virtual pages:

```text
SurfaceCacheOfflinePageTableEntry
  sheetId
  sheetPageX
  sheetPageY
  cardId
  mip
  cardPageX
  cardPageY
  fallbackPageIndex
  flags
```

At runtime, the page table maps the same virtual key to a physical page in the selected runtime atlas. The atlas can be 4k, 8k, or 16k depending on quality settings and binding budget:

```text
SurfaceCacheRuntimePageTableEntry
  physicalPageId
  fallbackPageIndex
  resident
  requestedMip
  flags
```

Fallback is flattened when page tables are built or updated. If a high-resolution virtual page is not resident, its runtime lookup should directly resolve to a resident lower-resolution fallback page. Shaders should not recursively walk the mip chain.

```text
requested virtual page
    -> resident physical page if loaded
    -> otherwise precomputed resident fallback physical page
```

This gives one shader page-table lookup per card sample.

### Local Hit To Page Mapping

When tracing an instance SDF or mesh SDF, the ray hit can usually be expressed in mesh local space. That path can use the asset card lookup directly.

Offline, build a compact card lookup grid per mesh:

```text
SurfaceCacheCardLookupGrid
  gridResolution: 8^3 or 16^3
  localBounds
  cellOffsetCount[]
  cardIdList[]
```

Populate it from card bounds:

```text
for each card:
    find lookup cells overlapped by card local bounds
    append cardId to those cells
compact lists
```

Local hit sampling:

```text
SampleSurfaceCacheLocal(assetId, hitPositionLocal, hitNormalLocal):
    cell = lookupGrid.cell(hitPositionLocal)
    candidateCards = lookupGrid.cardIds(cell and optional neighbors)

    for each cardId:
        card = cards[cardId]
        normalWeight = saturate(dot(hitNormalLocal, card.projectionNormal))
        if normalWeight <= threshold:
            continue

        cardPos = card.localToCard * hitPositionLocal
        uv01 = cardPos.xy
        hitDepth = cardPos.z
        if uv01 outside card rect:
            continue

        texel = uv01 * card.resolution
        pageX = floor(texel.x / pageSize)
        pageY = floor(texel.y / pageSize)
        localTexel = texel - vec2(pageX, pageY) * pageSize

        pageKey = assetId/cardId/mip/pageX/pageY
        physicalPage = resolve pageKey through runtime page table
        atlasTexel = physicalPage.origin + localTexel

        sample depth, validity, normal, material, radiance
        reject bad depth/validity samples
        accumulate weighted result
```

Sample several cards, normally the best 3 by normal and depth agreement:

```text
weight = normalWeight * depthWeight * validity * mipQuality
```

This is the local equivalent of Lumen's lookup:

```text
mesh index -> card grid
card grid + local position -> candidate cards
normal -> best cards
card projection -> virtual page
virtual page table -> physical atlas
```

### Runtime Page Allocation Algorithm

The runtime allocator is demand driven:

```text
for each frame:
    keep coarse GI pages resident around camera
    process surface-cache feedback from ray hits
    allocate requested high-resolution pages within budget
    evict old optional pages
```

Page classes:

```text
pinned:
  root fallback page for each loaded card set
  low-resolution GI pages around camera

optional:
  high-resolution card pages requested by ray hits
  reflection/detail pages
  radiance pages that can be regenerated
```

Allocation:

```text
requestPage(pageKey, desiredMip, priority):
    entry = pageTable[pageKey]
    if entry is resident:
        touch physical page
        update requested mip and priority
        return

    if desired page source is not loaded:
        request offline source sheet/page
        use flattened fallback
        return

    if free physical page exists:
        physicalPage = pop free list
    else:
        physicalPage = evict optional page with oldest lastUsedFrame

    copy/decode offline source page to runtime physical atlas
    update pageTable[pageKey]
    patch dependent flattened fallback entries if needed
```

Eviction:

```text
chooseEvictionCandidate():
    reject pinned pages
    reject pages touched this frame
    choose optional page with smallest lastUsedFrame
```

This is the behavior to emulate from Lumen: allocate low-res pages predictably for GI, allocate high-res pages sparsely from feedback, evict by last-used age, and make missing high-res pages resolve to resident fallback pages in one lookup.

Quality settings can bias the requested mip before allocation:

```text
desiredMip = pageFeedbackMip + qualityMipBias
desiredMip = clamp(desiredMip, minAllowedMip, maxResidentFallbackMip)
```

Lower quality modes should upload lower mip pages first. Higher mip/detail pages can still be requested by feedback, but they should only be allocated when the upload and residency budgets allow it.

### Runtime Surface Pages

Add a virtual-page system for runtime shading cache data:

```text
SdfSurfacePageKey
  assetId or globalBrickId
  cardId or faceId
  mip
  pageX
  pageY

SdfSurfacePageTableEntryGPU
  physicalPageId
  resident
  fallbackPageId
  flags

SdfSurfacePhysicalPageGPU
  key
  lastUsedFrame
  pinState
```

Physical atlases:

```text
materialReferenceAtlas  rgba32uint
materialBasisAtlas      rgba32uint
normalRoughnessAtlas    rgba16f or rgba8
radianceAtlas           rgba16f
irradianceAtlas         rgba16f or packed SH
validityAtlas           r8uint or flags buffer
```

Use one global runtime physical atlas per SDF surface cache payload, not one runtime atlas per mesh. This is separate from the offline 256/512/1024 page sheets. The atlas resolution should be configurable so we can bind all required payload atlases in one pass without paying unnecessary memory or binding costs.

```text
pageSize = 128
atlasResolution = 4096 | 8192 | 16384
physicalPagesPerAxis = atlasResolution / pageSize
physicalPageCount = physicalPagesPerAxis * physicalPagesPerAxis

physicalPageId -> pageCoord
  x = physicalPageId % physicalPagesPerAxis
  y = physicalPageId / physicalPagesPerAxis

pageTexelOrigin = pageCoord * pageSize
```

All SDF surface payload textures use the same runtime physical page id and page coordinate for a resident page. That means one runtime virtual page allocation can populate matching rectangles in `materialReferenceAtlas`, `materialBasisAtlas`, `normalRoughnessAtlas`, `radianceAtlas`, `irradianceAtlas`, and `validityAtlas`.

Suggested quality tiers:

```text
Low
  atlasResolution = 4096
  pageSize = 128
  physicalPages = 32 * 32 = 1024
  qualityMipBias = +2
  low-res GI pages only, sparse high-res pages disabled or very limited

Medium
  atlasResolution = 8192
  pageSize = 128
  physicalPages = 64 * 64 = 4096
  qualityMipBias = +1
  high-res pages allowed for nearby hits and rough reflections

High
  atlasResolution = 16384
  pageSize = 128
  physicalPages = 128 * 128 = 16384
  qualityMipBias = 0
  high-res pages allowed for GI, glossy, and reflection detail
```

Different payloads can use different tiers. For example, material/validity pages may use 8k or 16k, while radiance can use 4k or 8k plus a packed format. The binding model should keep the atlas count small enough that `SDFHitShadingResolvePass` can bind all required atlases in one pass.

The page table is global and virtual:

```text
SdfSurfaceVirtualTexture
  pageTableOffset
  pageCount
  mipCount
  pageSize
  sourceKind
  assetId or global namespace id

SdfSurfacePageTableEntryGPU
  physicalPageId
  residentMip
  fallbackPageTableIndex
  flags
```

The runtime physical atlas is only the cache. The virtual page table can describe more pages than any selected physical atlas across all assets, cards, bricks, and mips. Only resident pages consume physical slots. Offline source pages remain in their bounded 256/512/1024 sheets on disk or CPU-side loaded asset data.

Do not blindly allocate every payload as a full high-precision texture. At 16k x 16k, memory is large:

```text
r8                 256 MB
rgba8 / r32uint      1 GB
rgba16f             2 GB
rgba32uint          4 GB
```

So 16k should be treated as the maximum physical address space, not the default for every payload. Use it for low-byte payloads or when memory budget allows it. For radiance, prefer one of these:

- smaller physical atlas, such as 8k x 8k
- packed radiance format, such as `rgb9e5`, `rgba16f` only where needed, or SH packed across fewer layers
- separate radiance page pool with a lower `physicalPagesPerAxis`
- update budget that keeps only actively hit radiance pages resident

Hit shading should:

1. Compute a surface page key from the hit.
2. Look up the virtual page table.
3. If resident, sample material/radiance.
4. If missing, enqueue a page request and fall back to coarse page, probes, receiver material, or sky.
5. Mark the page as touched by writing `lastUsedFrame = currentFrame`.

### Page Feedback

Implement SDF surface page feedback just like virtual textures:

```text
SdfSurfaceFeedbackHeaderGPU
  requestCount
  overflowCount

SdfSurfaceFeedbackRequestGPU
  pageKey
  desiredMip
  hitCount or priority
  sourceKind
```

The GPU writes requests during:

- SDF hit shading resolve
- SDF material evaluate pass
- debug visualization when page residency is inspected

The CPU streaming manager:

1. Reads back feedback.
2. Deduplicates page keys.
3. Scores by hit count, recency, mip, and ray type.
4. Uploads up to a fixed budget.
5. Updates page table entries.
6. Evicts old non-pinned pages using oldest `lastUsedFrame`.

Use `VirtualTextureSystem` as the implementation model. Do not force SDF pages into the material virtual texture system unless the page dimensions and formats line up cleanly; the policy is reusable, but the payload formats are different.

### Physical Page Allocation

SDF surface pages should use a global allocator with pinned fallback pages, a free list, and strict last-used eviction.

CPU-side state:

```text
SdfSurfaceAtlasAllocator
  settings
    pageSize = 128
    atlasResolution = 4096 | 8192 | 16384
    physicalPagesPerAxis = atlasResolution / pageSize
    maxUploadsPerFrame
    maxEvictionsPerFrame
    qualityMipBias
    maxResidentMip

  freePhysicalPages
  physicalPages[physicalPageCount]
  pageTableEntries
  virtualPageToPageTableIndex
  dirtyPhysicalPages
  dirtyPageTableEntries
  frameIndex
```

Physical page metadata:

```text
SdfSurfacePhysicalPage
  virtualPageKey
  pageTableIndex
  lastUsedFrame
  lastUploadedFrame
  pinState
  payloadMask
```

Allocation flow:

```text
allocatePage(pageTableIndex, pageKey):
    if page is already resident:
        touch physical page
        return existing physicalPageId

    pageKey.mip = chooseResidentMip(pageKey.mip, qualityMipBias, maxResidentMip)

    if freePhysicalPages is not empty:
        physicalPageId = pop freePhysicalPages
    else:
        physicalPageId = choose non-pinned page with oldest lastUsedFrame
        evict old page table entry

    assign physicalPageId to pageTableIndex
    mark page table entry resident
    copy/decode from offline 256/512/1024 source sheet into every enabled runtime atlas layer
    mark physical page and page table dirty for GPU sync
```

Eviction flow:

```text
evict(physicalPageId):
    oldEntry = physicalPages[physicalPageId].pageTableIndex
    pageTableEntries[oldEntry].resident = false
    pageTableEntries[oldEntry].physicalPageId = InvalidPhysicalPage
    pageTableEntries[oldEntry].fallbackPageTableIndex remains valid
    clear physical page metadata
```

Pinned pages:

- root fallback pages for every registered asset/card set
- debug pages when inspecting residency
- optionally the coarsest global brick-face pages around the camera

The allocator should upload all payload layers for a page together when possible. Partial payload residency is allowed through `payloadMask`, but the first implementation should keep page residency atomic: a page is resident only when its required material and validity payloads are uploaded. Radiance can be stale or missing independently.

Lower-mip upload policy:

```text
if requested mip is too expensive or unavailable:
    upload the nearest resident-allowed coarser mip
    point the requested page table entry at that fallback
    keep the original high-res request alive for a later quality/budget upgrade
```

This lets quality settings reduce bandwidth and atlas pressure without changing shader lookup behavior.

### World SDF Hit Surface Lookup

Global SDF hits usually do not know the original instance or triangle. They need a world-space lookup that maps a hit point and normal to candidate surface cache cards.

Add a cascaded surface cache indirection grid:

```text
SdfSurfacePageGrid
  cascades or clipmaps
  cells per cascade: 16^3 first, 32^3 later if needed
  cell table: offset + count
  cell item list: compact card/page ids
  card table: world bounds, card frame, page table range, quality flags
```

Grid update:

```text
for each visible or near-camera instance:
    for each card in instance SurfaceCacheAsset:
        transform card bounds to world
        choose grid cascades overlapped by the bounds
        append card id to overlapped cells
compact cell lists
```

The card id should reference both the asset card and the instance transform:

```text
SdfSurfaceCardInstanceGPU
  assetId
  cardId
  instanceId
  worldToCard
  cardToWorld
  worldBoundsMinMax
  normal
  pageTableOffset
  flags
```

Sampling algorithm:

```text
SampleSdfSurfaceCache(hitPositionWS, hitNormalWS, rayDir):
    cell = lookup surface grid cell for hitPositionWS
    candidates = card ids in cell and neighbor cells

    for each candidate card:
        normalWeight = saturate(dot(hitNormalWS, card.normalWS))
        if normalWeight <= threshold:
            continue

        cardPos = card.worldToCard * hitPositionWS
        uv = cardPos.xy
        hitDepth = cardPos.z
        if uv outside card bounds:
            continue

        page = resolve virtual page for uv and desired mip
        if page missing:
            enqueue feedback request
            page = fallback page

        cachedDepth = sample depth page
        valid = sample validity page
        depthWeight = exp(-abs(hitDepth - cachedDepth) * depthScale)
        texelWeight = normalWeight * depthWeight * valid

        sample material/radiance/irradiance
        accumulate weighted result

    if accumulated weight > threshold:
        return blended surface sample

    return fallback sample from world radiance cache, probes, receiver material, or sky
```

Use 3 to 6 candidate cards. Lumen samples multiple cards and blends valid samples; we should do the same because a single projection is brittle around corners and thin geometry.

The returned data should be:

```text
SdfSurfaceSample
  valid
  confidence
  baseColor
  shadingNormal
  roughness
  metallic
  emissive
  radiance
  irradianceSH or irradiance
  pageKey
  flags
```

Feedback should record the most important sampled or missing page:

```text
pageKey
desiredMip
hitWeight
source = global_sdf_hit
lastUsedFrame
```

This is the bridge between global SDF visibility and surface-cache shading. The global SDF remains merged and identity-free, while the surface grid provides the local card candidates needed to shade the hit.

### Ray Hit Shading Resolve

The current `SDFHitShadingResolvePass` can produce `TraceShadingParameters` from SDF hits. Extend it in this order:

1. Keep the existing fallback material path.
2. Read current brick-face material references when available.
3. Add `SdfSurfacePageGrid` lookup for global SDF hits.
4. Add card projection, depth/normal/validity tests, and blended page sampling.
5. Add surface page table lookup and feedback.
6. Add radiance page sampling.
7. Add confidence/debug flags for missing pages and fallback sources.

For missing high-resolution pages, use this fallback order:

```text
resident requested page
    -> resident coarser page
    -> current brick-face material reference
    -> world radiance/probe cache
    -> receiver albedo fallback
```

## Streaming Offline SDF Pages

Offline mesh SDF pages also need streaming. Large meshes should not upload all mips and bricks at once.

### Residency Model

Each mesh SDF asset has virtual volume pages:

```text
asset id
mip level
brick coord
```

Runtime keeps:

```text
OfflineSdfPageTableBuffer
OfflineSdfPhysicalBrickBuffer or texture
OfflineSdfFeedbackBuffer
OfflineSdfReadbackBuffer
OfflineSdfFreeList
OfflineSdfLruState
```

SDF stamping and instance SDF tracing can request missing offline SDF bricks. Coarse fallback bricks should be pinned so the system always has something to sample.

### Feedback Sources

Generate offline SDF page requests from:

- global SDF stamping jobs
- instance SDF tracing
- debug views
- prefetch based on camera distance and instance bounds

The strongest prefetch signal should come from global SDF stamping. If a global cascade brick needs an asset mip, request that asset page before or during stamping. If the page is missing, stamp from a coarser resident mip and mark the brick for refinement after upload.

### Upload And Eviction

The physical pool should be fixed-size.

```text
try free physical brick
else evict oldest non-pinned brick
upload decoded brick payload
update page table entry
mark dependent global bricks dirty if this improves source mip
```

Evicting an offline SDF page does not necessarily invalidate the global SDF immediately, because the global SDF contains a stamped copy. It does affect future restamps and instance SDF tracing. If an evicted fine page was used by a resident global brick, the global brick can stay until it becomes dirty for another reason.

## GPU Runtime SDF Build With Surfels

A GPU runtime SDF generated from surfels is useful as a supplement, not as the main path.

Good use cases:

- very dynamic procedural geometry
- temporary occluders
- skinned meshes without stable baked SDFs
- local contact shadows where approximate unsigned distance is acceptable
- emergency fallback when offline SDF pages are missing

Bad use cases:

- closed-scene global visibility
- robust interiors
- thin walls and reliable leak prevention
- material identity

If we add it, use the existing brixelizer structures:

```text
surfel or triangle source
    -> dirty runtime bricks
    -> emit approximate SDF
    -> Eikonal propagation
    -> skinned/dynamic path
    -> combined global SDF
```

Do not let surfel-generated SDF overwrite the static offline SDF cache. Composite it as a dynamic layer.

## Implementation Plan

### Phase 0: Stabilize And Document Existing SDF

Deliverables:

- Keep this document current with the real code path.
- Add debug labels for static, skinned, combined, and surface cache residency.
- Add tests that validate current pass ordering:
  - `GPUSignedDistanceFieldsPassScheduleTests`
  - `GPUSignedDistanceFieldsBuildPassKernelTests`
  - `GPUSignedDistanceFieldsSceneBridgeTests`

Success criteria:

- Current brixelizer path still builds and traces.
- Debug trace can show combined SDF hits and brick ids.
- Existing surface material reference atlas behavior is documented by tests.

### Phase 1: VSDF Scene Container And SDF Bake

Deliverables:

- Add `.vsdf` as a scene-capable file format with full-scene header metadata.
- Add mesh, instance, material, SDF chunk, card, page, and shading image tables.
- Support multiple instances in one file, for example one Sponza `.vsdf` with many mesh instances.
- Extend virtual geometry import to optionally bake mesh SDFs and scene instance transforms.
- Store local bounds, world bounds, mips, sparse brick tables, quantized brick payload metadata, and instance transforms.
- Compress SDF brick payload chunks with LZ4.
- Add conservative small-triangle and thin-surface SDF emission using half-voxel expansion.
- Add a CLI validation tool that dumps scene bounds, instance count, mesh count, brick count, mips, compression ratio, and memory.

Initial builder:

- CPU implementation is acceptable.
- Use virtual geometry fallback meshes first.
- Add higher-quality triangle distance/sign later.

Success criteria:

- A multi-instance scene can produce one valid `.vsdf` file.
- Unit tests can load the file, enumerate instances, decompress LZ4 SDF chunks, and sample known distances.
- Coarse fallback mip is always present.
- Header metadata validates against the table offsets and payload sizes.
- Test meshes with sub-voxel triangles and thin walls remain visible in the baked SDF.

### Phase 2: Offline Card Generation And Shading Parameter Images

Deliverables:

- Add `SurfaceCacheAsset` data with card descriptors, local lookup grids, virtual page tables, fallback pages, and offline page sheets.
- Add an offline card/surfel generator for static meshes and scene instances.
- Add `SurfaceCacheCardLookupGrid` for mesh-local hit to card lookup.
- Add virtual page layout for cards: `assetId/cardId/mip/pageX/pageY`.
- Add offline sheet packing with 256/512/1024 max sheet resolution.
- Generate one shading image or payload per parameter:
  - albedo
  - normal
  - roughness
  - metallic
  - emissive
  - opacity
  - depth
  - validity
  - material id
  - UV basis
- Ensure every parameter image shares the exact same packed card/page UV space.
- Export optional debug images for each parameter so the baked layout can be inspected.
- Store or reference these parameter images from the `.vsdf` shading image table.
- Add flattened fallback page-table entries so missing high-resolution pages resolve to lower-resolution pages in one lookup.

Success criteria:

- A scene with multiple instances bakes cards and shared-layout parameter images.
- Opening the debug albedo/roughness/metallic/normal images shows the same packed card/page layout.
- Local instance SDF hits can map from local hit position to candidate cards, then to a virtual page and offline sheet texel.
- Missing card pages have valid coarse fallback pages.
- Tests cover 256, 512, and 1024 max offline sheet resolutions.

Implementation order:

1. Define `SurfaceCacheAsset`, `SurfaceCacheCard`, card lookup grid, offline sheet, shading image, and virtual page table structs.
2. Build a CPU card generator from fallback mesh triangles.
3. Add card domain tiling and offline sheet packing with test assets at 256, 512, and 1024 limits.
4. Capture depth, validity, normal, material id, UV basis, albedo, roughness, metallic, opacity, and emissive into same-layout parameter images.
5. Build the mesh-local `SurfaceCacheCardLookupGrid`.
6. Write the card/page/image metadata into `.vsdf`.
7. Export debug parameter images next to the `.vsdf`.
8. Add validation that all parameter images have matching dimensions, mips, page layout, and UV mapping.

### Phase 3: Offline SDF And Surface Page Runtime Streaming

Deliverables:

- Add `OfflineSdfStreamingManager`.
- Add `SurfaceCacheStreamingManager`.
- Add page tables, physical pools, feedback buffers, and readback for SDF pages and surface pages.
- Add CPU upload policy modeled after `VirtualTextureSystem`.
- Add strict last-used eviction using `lastUsedFrame`.
- Add configurable runtime surface atlas sizes: 4k, 8k, and 16k.
- Upload/decode offline 256/512/1024 surface source pages into the selected runtime atlas.
- Add quality settings that bias requested shading pages toward lower mip levels.
- Pin SDF fallback mips and surface cache fallback pages.

Success criteria:

- Runtime can register a multi-instance `.vsdf`.
- GPU or CPU can request SDF pages and surface parameter pages.
- Requested pages become resident within a fixed upload budget.
- Eviction never removes pinned fallback pages.
- Old optional pages are evicted by oldest `lastUsedFrame`.
- Runtime page table resolves missing high-resolution surface pages to resident lower-resolution pages.
- Low/medium quality modes upload lower mip pages and run with smaller atlases.

### Phase 4: Stamp Offline SDFs Into Static Global SDF

Deliverables:

- Add `OfflineSdfInstanceGPU` and asset tables.
- Add dirty brick collection for offline SDF instances.
- Add GPU stamp shader.
- Reuse `sdfAtlas`, static brick maps, brick AABBs, dirty lists, and combined trace path.
- Add settings to choose:
  - current triangle brixelizer
  - offline SDF stamping
  - hybrid fallback

Success criteria:

- Static meshes with baked SDFs build the global SDF without triangle voxelization.
- Moving/removing a static instance invalidates old and new dirty regions.
- Combined global SDF tracing sees stamped static data.
- Missing fine pages use coarse SDF pages and later refine.

### Phase 5: Surface Page Runtime Lookup And Material Resolve

Deliverables:

- Add `SdfSurfacePageGrid` and `SdfSurfaceCardInstanceGPU` runtime tables.
- Add runtime registration that creates virtual page table entries and pinned fallback pages.
- Add a CPU-side allocator that uploads offline sheet pages into the runtime atlas.
- Add WGSL helpers for local hit to card page sampling.
- Add WGSL helpers for global SDF hit to world-space card-grid sampling.
- Extend `SDFHitShadingResolvePass` to find candidate cards for global SDF hits, project hit positions into card space, depth-test samples, and prefer surface page material data.
- Add feedback readback, request ranking, and last-used eviction for optional high-resolution pages.
- Preserve current fallback material behavior.

Success criteria:

- Local instance SDF hits can map from local hit position to candidate cards, then to a virtual page and physical atlas texel.
- Global SDF hits can shade without instance identity by using the world-space surface page grid.
- SDF ray hits can resolve material id and UV from baked pages.
- Missing pages generate feedback instead of hard failure.
- Debug views show material page residency and fallback source.
- Debug views can show selected cards/pages for a world SDF hit.

### Phase 6: Runtime Radiance Pages

Deliverables:

- Add physical radiance atlas for SDF surface pages.
- Add page update pass that shades resident pages over time from card texels.
- Add direct-light and indirect-light gathering for surface page texels.
- Add temporal blending and validity/confidence for radiance pages.
- Add page feedback from SDF hit results.
- Add last-used eviction and fixed update budget.

Success criteria:

- SDF hits can fetch cached radiance through `SampleSdfSurfaceCache`.
- Missing radiance falls back to world probes or receiver fallback.
- Radiance page update cost is bounded per frame.
- Surface page radiance can propagate multi-bounce lighting over multiple updates.

### Phase 7: Instance SDF Tracing

Deliverables:

- Add cascaded `SdfInstanceGrid`.
- Add mesh SDF instance trace shader.
- Add trace policy for short glossy rays.
- Return hit records compatible with `TraceHitGPU` or `GITraceSurface`.

Success criteria:

- Short glossy rays can hit instance SDFs before global SDF fallback.
- Instance SDF tracing requests missing SDF and surface pages.
- Debug output can compare screen, instance SDF, and global SDF hits.

### Phase 8: Dynamic And Skinned Hybrid

Deliverables:

- Keep current brixelizer for skinned/runtime meshes.
- Composite dynamic SDF layer with static offline layer.
- Add optional surfel-generated approximate SDF for special dynamic sources.

Success criteria:

- Skinned meshes still contribute to global SDF through the existing path.
- Static offline SDF updates do not force skinned rebuilds.
- Dynamic SDF can be disabled independently for debugging.

### Phase 9: Quality, Memory, And Performance

Deliverables:

- Tune source mip selection.
- Add brick/page budgets to UI.
- Add quality settings for atlas resolution, page upload budget, max resident mip, and quality mip bias.
- Add profiler counters:
  - offline SDF pages requested/uploaded/evicted
  - surface pages requested/uploaded/evicted
  - surface page last-used eviction count
  - requested mip vs uploaded mip
  - atlas pressure per payload
  - dirty global bricks
  - stamp time
  - trace steps
  - fallback rate
- Compare triangle brixelizer vs offline stamping.
- Validate that 4k and 8k atlases can be bound with all required payloads in `SDFHitShadingResolvePass`.
- Compare center-stamped small triangles against swept-triangle emission for leak resistance.

Success criteria:

- Static global SDF update cost scales with dirty bricks, not triangle count.
- Page feedback converges quickly after camera cuts.
- Global SDF memory stays within fixed budgets.
- Missing page artifacts degrade to coarse occlusion and probe shading, not leaks or black hits.

## Risks And Decisions

- Asset format: decide whether `.vsdf` is standalone or embedded in `.virtualgeometry`.
- Scene ownership: decide when repeated instances share mesh card pages and when they get instance-specific baked pages.
- Compression: add LZ4 dependency and chunk-size limits so streaming does not decompress too much data per request.
- Quantization: `R8_SNORM` is memory efficient but may be too coarse for near-field instance tracing. Start with `R16_SNORM` option.
- Non-uniform scale: conservative min-axis scaling is safe but slower. Add better correction later.
- Overlapping signed fields: validate union behavior for interiors and narrow-band clamping.
- Small triangles: center-only stamping is not enough for the offline path; use swept/expanded emission and validate against thin walls.
- Removal/invalidation: never rely on subtracting from a min-composited SDF. Rebuild dirty bricks from contributors.
- Surface cache coverage: cards/pages can miss. Missing coverage should lose energy or fall back to probes, not leak visibility.
- Streaming feedback latency: coarse fallback pages must always be resident.
- Memory contention: offline SDF pages, global SDF bricks, surface pages, and radiance pages need separate budgets.

## Near-Term Code Changes

Start with the least disruptive path:

1. Add `.vsdf` scene container loading and tests without touching render passes.
2. Add LZ4-compressed SDF chunk read/decompress tests.
3. Add multi-instance scene metadata tests using a small Sponza-like fixture.
4. Add offline card generation and same-layout shading parameter image export.
5. Add validation/debug output for albedo, roughness, metallic, normal, depth, and validity images.
6. Add offline SDF and surface cache tables to `GPUSignedDistanceFieldsManager`.
7. Add a stamp pass that writes into the existing static path brick maps and `sdfAtlas`.
8. Gate the new path behind settings, keeping the current brixelizer default.
9. Extend existing SDF debug passes before integrating GI shading.
10. Add runtime surface page feedback and streaming after offline visibility and card images are stable.

The main architectural rule is simple: bake and stream mesh-local detail, but trace a runtime global SDF that uses the current brixelized clipmap structures.
