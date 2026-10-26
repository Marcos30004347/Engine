# World-streamed polygon terrain plan

## Outcome

Add a deterministic, world-coordinate terrain system built from the existing
Poisson-disk sampler and constrained Delaunay triangulator.

```text
coarse world grid -> continents, plates, uplift, macro climate hints
                 -> blue-noise sites -> Delaunay mesh -> Voronoi dual graph
                 -> land/water, elevation, drainage, rivers, wind, climate, biomes
                 -> continuous world-space terrain field
                 -> surface-only voxel chunks -> streamed brick renderer
```

The terrain is not a fixed 1 km map. One kilometre is the approximate
camera-centred render/residency window. A larger, finite world is generated
**offline** into tiled height/material textures. At runtime the engine only reads
those baked tiles and regenerates disposable voxel chunks from their samples.

Two samples deliver the work:

1. `PolygonTerrainSample` is an offline baker. It reads terrain configuration,
   generates the whole configured world, and writes the terrain package and its
   source configuration alongside it.
2. `TerrainVoxelSample` loads that package, streams its texture tiles on a few
   worker threads, voxelizes surface chunks, and renders a moving window.

The original polygon-map article is the algorithmic reference. Red Blob's
[Mapgen2 overview](https://www.redblobgames.com/maps/mapgen2/) describes how the
same graph algorithms can serve different map styles. Detail noise is a later
surface layer, following the separation described in
[Terrain from noise](https://www.redblobgames.com/maps/terrain-from-noise/).
For larger worlds, the flat-map generator adapts the region/plate hierarchy from
Red Blob's
[planet generation experiment](https://www.redblobgames.com/x/1843-planet-generation/):
pick coarse regions, assign plates with a randomized fill, then let movement and
boundary rules produce continents, coasts, rifts, and mountains. The current
source-world implementation has a spherical/equirectangular preview generator;
the planar package path can later bake or project selected rectangular windows
from that planet source.

### World Orogen reference pipeline

[World Orogen](https://www.orogen.studio/) and its public
[planet generator source](https://github.com/raguilar011095/planet_heightmap_generation)
are the closer visual reference for the spherical world path. Its useful lesson
is the separation of topology, sculpting, and display resolution:

1. Generate plates and continents on a fixed roughly 20K-region Fibonacci /
   Delaunay sphere, independent of export resolution.
2. Project those assignments onto a separate high-resolution irregular sphere;
   do not make the equirectangular atlas the simulation mesh.
3. Construct tectonic relief from boundary classification, propagated stress,
   and spherical distance fields for mountains, coasts, shelves, and ocean
   features.
4. Warp the complete elevation lookup in the local tangent plane. Warping only
   the final detail noise leaves obvious plate-shaped and hard-angle bands.
5. Run bilateral smoothing, priority-flood outlet resolution, iterative implicit
   stream-power erosion, thermal transport, ridge sharpening, and soil creep on
   the high-resolution graph. Rebuild drainage as erosion changes elevation.
6. Rasterize equirectangular textures only as export/streaming products. The
   editor display sphere renders the graph-native dual mesh and node attributes
   directly, so changing export atlas dimensions does not affect generation or
   preview cost.

The Orogen repository is GPL-3.0. This engine therefore uses it as a behavioral
and architectural reference, not as code to copy. The implementations here are
independent versions of published techniques such as priority-flood drainage
and implicit stream-power erosion.

The current graph payload stores one `SphericalTerrainNode` per stable region:
position, post-erosion elevation, plate, continent, tectonics, mountain score,
river discharge, temperature, precipitation, wind, plate motion, and the final
biome/material index. Region adjacency is retained in CSR arrays. A geodesic
distance-to-ocean field keeps active relief behind a configurable coastal-plain
transition. Signed crust diffusion and multi-band spherical noise move the
shoreline away from the underlying plate-cell boundary to form shelves, bays,
capes, and island chains, while seamless province noise varies continental
interiors between broad plains, rolling hills, and old uplands. Priority-flood
depressions with sufficient accumulated runoff are retained as lakes at their
spill elevation. Continental distance, descending wind, latitude, and regional
noise create deserts and steppes before final biome classification. Interactive generation sets
`rasterizeMaps=false`; pressing Save performs the deterministic atlas bake for
the requested export dimensions. This is also the migration path toward a
runtime format that streams graph nodes and adaptive patches directly without
requiring global atlases.

### Planet region identity and adaptive surface hierarchy

The generated Voronoi graph and the runtime render hierarchy solve different
problems and deliberately keep different identities:

| Identity | Lifetime | Purpose |
| --- | --- | --- |
| `sourceRegionId` | baked world lifetime | Stable link back to the offline Voronoi region, plate, climate, biome, and future gameplay metadata. |
| `(rootHalfedge, depth, path)` | stable across frames | Spatial address for a spherical surface patch, terrain chunk, cache key, and LOD ancestry. |
| CBT pool slot | one transient residency interval | Recycled GPU storage/work index for a currently active bisector. It is never serialized or used as a chunk key. |

The offline baker rasterizes `sourceRegionId` into an unfiltered R32_UINT atlas
alongside height and material. Every adaptive patch maps its spherical direction
to the baked atlas and reads the original region ID with nearest sampling. A
large coarse patch may cover several source regions; it discovers those IDs from
the resident atlas tiles instead of pretending that one Voronoi cell equals one
render chunk. A later region metadata table will use the same IDs to store
centroid, plate, climate, biome, neighbours, and gameplay tags without changing
the height/material package.

Runtime subdivision follows the stable-bisector formulation from
[Concurrent Binary Trees for Large-Scale Game Components](../../../Desktop/2407.02215v1.pdf):

1. Load a small closed triangular halfedge sphere. Each halfedge creates one
   root bisector whose vertices are the halfedge endpoints and face centre.
2. Address children as `(root, depth + 1, path * 2 + child)`. Applying the two
   fixed bisection matrices reconstructs a leaf at any depth without storing the
   complete binary tree.
3. Project the reconstructed leaf to the sphere, estimate screen-space error,
   and request split/merge operations. Refinement follows the compatibility
   chain and merging is conservative so the active mesh remains conforming.
4. Use a compact CBT as allocation pool and parallel scheduler. A 64-bit
   bisector address supports far deeper terrain than the 32-bit pool has active
   slots; active capacity and spatial precision are independent limits.
5. At the selected leaf, convert directions to atlas coordinates, request the
   necessary LZ4 tiles/mips, and sample height, material, and `sourceRegionId`.
   Height uses filtered sampling; integer region/material fields use nearest
   sampling.

The display sphere, simulation graph, and runtime base mesh are also separate.
The high-resolution Voronoi graph remains the offline macro-simulation domain.
The runtime base mesh should be a small regular polyhedron (an icosahedron is a
good first engine default), not one root per simulation cell. This keeps the CBT
root count small while the atlas preserves all generated detail.

For streaming, choose a fixed `streamingDepth` below or equal to maximum render
depth. The stable ancestor of a visible bisector at that depth is the chunk key:

```text
planet chunk key = { planetId, rootHalfedge, streamingDepth, pathPrefix }
pathPrefix       = renderPath >> (renderDepth - streamingDepth)
```

Many fine render leaves therefore share one loaded chunk. Render LOD may change
each frame without invalidating I/O requests. Texture mip is selected from the
leaf angular footprint; longitude wraps, and polar samples use direction-based
lookup so no vertex or chunk identity depends on equirectangular pole topology.

The eventual GPU implementation uses the paper's staged update model:
reset counters, cache pointers, reset indirect commands, generate split/merge
commands, reserve blocks, fill blocks, update neighbours, update the CBT
bitfield, and run its sum reduction. The CPU `PlanetSurfaceHierarchy` module is
the deterministic reference for halfedge conversion, stable addresses,
bisection, atlas coordinates, and source-region lookup; it is not yet the full
nine-pass GPU allocator.

## Scales and conventions

| Item | Initial value | Reason |
| --- | ---: | --- |
| Terrain coordinates | world-space metres | Chunk data is independent of generation order. |
| Voxel size | 0.20 m | Requested resolution. |
| Terrain chunk width | 32 m | 160 terrain columns and 20 finest 8-voxel brick widths. |
| Resident window | 33 x 33 chunks | 1,056 m square: about 1 km and centred with a guard band. |
| Visible surface columns | 27,878,400 | `33 * 33 * 160 * 160`; one voxel maximum per column. |
| Baked texture tile | 128 m | 640 x 640 columns; contains 4 x 4 terrain chunks. |
| Polygon site spacing | 12-20 m | Configurable; graph scale, not final heightmap scale. |
| Terrain vertical range | 256 m initially | 1,280 height steps at 20 cm. |
| Raster workers | 64 by default | Offline baking prefers throughput; explicit configs can lower it. |

`x` and `z` are horizontal and `y` is up. Height is quantized only once on the
world grid, before voxelization. Thus two chunks querying a shared border receive
the same surface voxel.

The 33 x 33 set is a residency target. Frustum and brick occlusion culling still
remove invisible bricks from the draw.

## Scope

Required:

- Deterministic offline generation of height, water, river, moisture, wind,
  climate, terrain-form, forest, settlement-suitability, and biome fields for a
  configured world extent.
- Tiled baked output with seamless texture-tile and render-chunk boundaries.
- Surface-only terrain voxels: no filled terrain volume or underground cells.
- Incremental CPU generation and GPU upload on camera movement.
- A terrain-specific streamed integration for the brick G-buffer renderer.

Deferred:

- Caves, terrain editing, overhangs, vegetation, structures, collision, and
  water simulation.
- Erosion beyond an optional shallow river-bed carve.
- Runtime procedural map generation. The offline package is runtime source data;
  voxel chunks are disposable derived data.
- Coarse terrain LOD across chunk boundaries until the exact finest-brick path
  is stable.

## Module layout

Add `src/runtime/procedural/terrain/`:

```text
TerrainSettings
TerrainSiteSource             whole-map Poisson sites
PolygonTerrainGraph           Delaunay + clipped Voronoi graph and attributes
TerrainBaker                  configuration -> tiled terrain package
TerrainPackageWriter          package index, texture tiles, checksums
TerrainChunkSurface           quantized 160 x 160 heights + material indices
```

Add `src/voxel/svraster/` integration:

```text
TerrainBrickBuilder           TerrainChunkSurface -> finest surface bricks
TerrainBrickChunkPool         GPU range allocation and fence-safe reclamation
TerrainBrickStreaming         camera window planning and asynchronous jobs
TerrainPackageReader          metadata, tiled texture I/O, decompression, LRU
```

The baker is the only consumer of the polygon graph. Runtime voxelization reads
the baked package through `TerrainPackageReader`; it must not construct a graph,
evaluate noise, or depend on Vulkan map-generation code.

## One whole-map polygon graph

The v1 offline baker follows the reference algorithm directly: it generates one
finite polygon graph for the entire configured map. It must never run Poisson or
Delaunay independently per render chunk or per texture tile.

1. Generate sites for the configured world rectangle with `PoissonDiskSampler`.
   Poisson distribution is an explicitly supported modern replacement for the
   article's initial random-points-plus-Lloyd-relaxation step.
2. Add the four map corners to the site set and run
   `ConstrainedDelaunayTriangulator` once for the whole site set. Because all
   Poisson sites are inside that rectangle, the corners establish its convex
   hull without exercising the current constraint-insertion path.
3. Build the Voronoi dual from Delaunay triangle circumcentres, retaining the
   two linked graphs: polygon centres/Delaunay adjacency and polygon
   corners/Voronoi shape edges.
4. Clip only the outer map boundary. There are no internal graph-tile borders,
   halo retries, or synthetic drainage outlets.

The existing triangulator exposes live triangles through `collectTriangles()`.
The new graph builder supplies the dual construction, reciprocal adjacency, and
the combined centre/corner/edge relationships described by the reference.

The baker validates the configured maximum site count before allocating. A world
that exceeds the measured practical limit of the current triangulator fails with
an actionable configuration error; introducing a tiled graph algorithm is a
separate scalability project, not an untested shortcut in v1.

## Terrain pipeline

All graph IDs and tie-breaks are stable; generation order must not change output.

1. Construct region/corner/edge adjacency from the Delaunay dual and validate
   reciprocal neighbours and non-degenerate cells.
2. Build a hierarchical macro grid over the whole world. Coarse cells are seeded
   into continent/ocean regions with a randomized fill, then sampled by the
   Voronoi graph. Small worlds blend toward an island profile; larger worlds
   blend toward the macro continent field.
3. Assign macro tectonic plates. Small worlds keep a central island/volcanic
   plate layout; larger worlds choose plate centres across the macro grid. Plate
   movement and region boundaries create uplift, rifts, coasts, and mountain
   candidates.
4. Apply the macro land field to mark initial water/land. Flood fill from the
   outer boundary distinguishes ocean from inland lakes.
5. Assign elevation to **Voronoi corners** as distance from the coast, then
   redistribute sorted land elevations to bias the map toward low coastlands and
   a smaller high mountain area. Assign polygon-centre elevation as the average
   of its corners.
6. Assign each land corner its steepest downhill corner. The elevation scheme
   guarantees that following downhill reaches the ocean; a stable ID breaks equal
   elevation ties.
7. Choose seeded high-elevation corner sources automatically from world size,
   graph spacing, land area, and available highland candidates, then accumulate
   river flow once in downhill elevation order. Merged paths reuse the same
   downstream flow instead of being walked again per source; lake levels remain
   flat. A non-zero river-source count remains only as a debug override.
8. Spread and redistribute local moisture from water and river sources.
9. Simulate climate on the polygon network. A seeded prevailing wind sorts sites
   from upwind to downwind; each site receives moisture from upwind neighbours,
   adds nearby ocean/lake/river sources, loses water through orographic rainfall,
   and records wind exposure for debug previews. Temperature comes from latitude,
   elevation, and local variation.
10. Select a Whittaker-style biome from temperature and precipitation, with
   explicit ocean, lake, beach, and river classes.
11. Rasterize the graph fields. Add bounded, low-amplitude detail only after this
   topology is complete, fading it near rivers and coasts so it cannot alter
   drainage or shorelines.

## Offline terrain package

The baker writes one package for the finite world extent in its configuration.
For medium maps it may rasterize the polygon field at final terrain-column
resolution in memory, but RPG-scale maps are written as independently streamable
texture tiles. Runtime does not evaluate the Delaunay graph, drainage, noise, or
biome rules; it only streams baked tiles.

The configuration is an authored `terrain-config.json` supplied to the baker.
The exact canonicalized configuration is copied beside the baked output and its
hash is embedded in the package header. This makes it possible to identify the
settings that produced every terrain package without asking runtime to regenerate
anything.

Current output layout:

```text
terrain-config.json
terrain-package.json
tiles/
  x_00012_z_00007.terrain-tile.lz4
  ...
```

`terrain-package.json` contains package/version information, world bounds, voxel
size, vertical quantization range, tile dimensions, material-palette version,
the configuration hash, channel formats, and one tile record per compressed
blob. Tile records include the relative path, dimensions including apron,
channel byte offsets, uncompressed byte count, compressed byte count, and an
FNV-1a checksum of the uncompressed payload. Tiles are independently
LZ4-compressed so several reader threads can seek, read, and decode them without
serializing on a monolithic image.

Each 128 m baked tile contains 640 x 640 terrain columns plus a one-column apron
for border sampling. The current version-2 `.terrain-tile.lz4` payload begins with a tiny
little-endian header (`TERR`, version, dimensions, channel offsets, channel byte
counts), followed by the required runtime channels:

- `height`: unsigned 16-bit height over the package vertical range.
- `material`: biome/water/river palette index used directly by terrain bricks.
- `sourceRegion`: unsigned 32-bit original Voronoi region ID. It is sampled as
  discrete data and maps runtime patches back to generated world metadata.

Optional diagnostic/offline channels include `river-map.pgm`, `region-map.pgm`,
`terrain-form-map.pgm`, `temperature-map.pgm`, `precipitation-map.pgm`,
`wind-map.pgm`, `forest-map.pgm`, `city-suitability-map.pgm`,
`continent-map.pgm`, `tectonic-map.pgm`, and, for spherical previews,
`mountain-map.pgm`. They are not loaded by the terrain renderer unless a debug
mode asks for them. The apron values are authored from the same global
rasterization pass, not duplicated later by a runtime filter.

The baker also writes `source-region-map.r32` for direct inspection and tooling.
It has a 16-byte little-endian header (`PRID`, version, width, height) followed
by one R32_UINT Voronoi region ID per atlas texel. `region-map.pgm` is only a
hashed colour preview and must never be used as persistent identity.

The final baker streams raster rows and tile writes, so a large map does not
require a full float heightmap and every auxiliary map to exist in memory
simultaneously. The current sample still owns full preview maps for interactive
inspection, and writes the same LZ4 tile package from those maps.

## Sample 1: `PolygonTerrainSample`

Add `samples/polygonTerrain/` and register it in `samples/CMakeLists.txt`.

Responsibilities:

- Read `terrain-config.json`, validate its finite world bounds, site-count limit,
  and bake settings, then generate the one whole-map polygon graph.
- Rasterize and write every texture tile, package index, copied canonical
  configuration, checksums, previews, and graph diagnostics.
- Generate terrain from metre-scale tectonic plates, a continent/shelf field,
  deterministic lake basins, and chained mountain ranges with foothills, rather
  than relying on a simple radial island mask. Feature counts and noise
  wavelengths should be world-scale, so larger maps reveal more structure
  instead of stretching the same normalized island.
- Generate large worlds from a hierarchical macro grid first. Macro cells choose
  continent/ocean regions and coarse uplift before detailed Voronoi sites are
  sampled, so the generator can produce continents, archipelagos, and island
  chains instead of only one island-shaped landmass.
- Maintain a full-sphere source generator alongside the planar baker. The sphere
  generator uses equirectangular cells as the preview/storage grid, randomized
  plate-region fill with wrapped longitude, angular plate movement on sphere
  vectors, boundary uplift/rifts, explicit mountain maps, latitude/elevation
  climate, precipitation/runoff accumulation, and automatic downhill river
  routing. Planar terrain packages should later bake/project selected windows
  from this planet source instead of regenerating local continents per chunk.
- Drive the default island shape from an explicit island profile: seeded
  centre, rotated elliptical radius, coastal shelf, and central peak/highland
  strength. A central continental/volcanic plate is surrounded by inward-moving
  plates so convergence produces interior spines and island-arc ranges instead
  of decorative mountains detached from the landform.
- Mix landform-specific noise into graph elevations: ridged metric noise for
  mountain spines, billow noise for rolling hill country, and domain-warped
  lowland noise for plains that should not look like flat masks. Raster detail
  uses metre-scale wavelengths so larger worlds gain more terrain frequency
  instead of only magnifying one normalized preview.
- Simulate lightweight climate before biome assignment: latitude and elevation
  drive temperature; a network wind pass transports moisture across neighbouring
  Voronoi sites; water sources, orographic lift, arid latitude belts, and rain
  shadow drive precipitation. Biome/material selection should use temperature
  plus precipitation so deserts and ice regions are causal map features rather
  than hand-painted thresholds.
- Preview terrain forms separately from final biome colour: plains, valleys,
  hills, mountains, and peaks use different tones so generator shape can be
  debugged without confusing it with material assignment.
- Preview forest suitability and city suitability separately. Forests come from
  temperature, precipitation, elevation, and landform; city suitability favours
  flat temperate plains with water access while avoiding mountains, wetlands,
  deserts, and dense forest.
- Accept seed, world bounds, site radius, water level, mountain count, optional
  river-source debug override, vertical range, texture-tile size, and
  compression settings.
- Report site count, triangles, graph cells, ocean/lake count, resolved river
  sources, river edges, timing, tile count, compression ratio, and output bytes.
- Run preview generation asynchronously from the UI, showing phase/progress and
  elapsed time while keeping ImGui input responsive. Cancellation is cooperative:
  it is checked at generator phase boundaries and during raster worker loops.

This is an offline baker and visual reference for proc-gen, independent of the
voxel renderer. It must fail the bake when topology, downhill drainage, tile
checksums, or border-apron validation is unresolved.

Current implementation note: `PolygonTerrainSample` can now preview either the
older finite polygon/Voronoi baker or the spherical planet source. In spherical
mode it generates a 2:1 equirectangular atlas with height, plates/regions,
continents, tectonics, mountains, river, terrain-form, climate, forest, and
settlement diagnostic maps. These maps are still CPU-authored for deterministic
package output, but the interactive preview window now uses the engine
Vulkan/RHI stack: SDL creates a Vulkan surface, `VulkanRHI` owns the swapchain
and preview textures, CPU diagnostic maps are uploaded through `textureWrite`,
and Dear ImGui is rendered through a Vulkan overlay render pass.

The next preview iteration can move expensive preview construction into real
compute passes for fast iteration:

1. Upload sphere or planar macro buffers: plate ids, plate velocities,
   continent/ocean flags, uplift/rift fields, and low-resolution graph
   attributes.
2. Run compute passes into storage textures:
   `planet-macro`, `plate-boundaries`, `height-preview`, `climate-preview`,
   `river-preview`, and `debug-colour`.
3. Draw those storage textures in ImGui through the RHI renderer, with tabs
   matching the existing CPU previews.
4. Keep CPU generation as the authoritative offline baker until GPU passes have
   byte-for-byte or tolerance-bounded validation against the CPU path. Early GPU
   passes are for interactive preview, not for package authoring.

GPU preview is for designer feedback. The saved runtime package remains the
CPU-authored deterministic source of truth unless a later GPU baker implements
the same tile format and validation. The current CPU preview path already runs
the raster and preview-colour conversion on worker threads; Delaunay/topology is
still the serial bottleneck for very large worlds.

## Surface-only terrain voxelization

For a 32 m chunk, read/extract 160 x 160 world-grid columns from the required
baked texture tile(s):

```text
surfaceY = floor(heightMetres / 0.20 m)
occupied(x, surfaceY, z) = true
material(x, z) = biome palette index
```

Current incremental implementation: the polygon baker already writes a
`terrain-package.json` manifest and independently readable
`tiles/x_<column>_z_<row>.terrain-tile.lz4` files. Every tile has a one-texel
clamped apron, height/material/source-region channel offsets, byte counts, and
an uncompressed payload checksum in the manifest. A runtime
`TerrainPackageReader` and streamed
voxel sample remain required before the renderer consumes the package directly.

No voxel below `surfaceY` is occupied. Differences between adjacent heights show
as voxel step faces; equal-height neighbours hide their shared side. The initial
sample supports above-ground terrain views, not visible terrain undersides.

`TerrainBrickBuilder` groups columns directly into 8 x 8 x 8 bricks. Do not
manufacture a dense volume or generic source octree merely to recover surface
cells. Add a fixed terrain-palette mode to `SparseVoxelBrickHierarchy` so biome
indices remain exact and avoid unnecessary per-chunk palette clustering.

## Streamed brick integration

The current `SparseVoxelBrickHierarchy` is built as one fully resident hierarchy
and explicitly has no streaming. Terrain needs independent chunk hierarchies plus
a pool, not a global rebuild on camera movement.

`TerrainBrickChunkPool` owns reusable slots. A slot has its own nodes, occupancy,
material indices, child indices, and root indices. GPU buffers are range-allocated;
the renderer draws a root-index list rather than assuming roots are
`nodes[0..rootCount)`.

Required renderer work:

1. Add root-index indirection to brick culling.
2. Rebase a chunk's local offsets into its assigned GPU ranges at upload.
3. Keep replaced ranges alive until a graphics fence/timeline proves no submitted
   frame can read them.
4. Publish a chunk's roots only after all of its buffer writes are available.
5. Initially render finest terrain bricks at chunk borders, preferably everywhere.
   Independent coarse cuts can crack at chunk seams; cross-chunk LOD constraints
   are a later optimization.

Relevant existing code:

- `src/voxel/svraster/SparseVoxelBrick.hpp`
- `src/voxel/svraster/SparseVoxelBrick.cpp`
- `src/voxel/svraster/rendering/SparseVoxelBrickPasses.hpp`
- `samples/sparseVoxelRenderer/SparseVoxelRendererSample.cpp`

## Sample 2: `TerrainVoxelSample`

Add `samples/terrainVoxel/`. It opens a `TerrainPackageReader`, owns a
`TerrainBrickStreaming` controller, and uses the brick G-buffer renderer. It
does not link or execute the polygon-map generation pipeline.

Camera-window scheduling:

1. Convert camera position to a world chunk coordinate.
2. Form the desired 33 x 33 coordinate set.
3. Reuse all slots whose coordinates remain in that set.
4. On a one-chunk move, evict and request only an entering/leaving strip of 33
   chunks, never all 1,089 chunks.
5. Prioritize incoming chunks by frustum relevance and then distance.
6. A small bounded worker pool reads and LZ4-decodes the needed 128 m terrain
   texture tiles into an LRU cache. One tile supplies up to 4 x 4 chunks.
7. Workers extract height/material columns from cached tiles and build their
   surface-only brick payloads. This is the only terrain generation at runtime:
   conversion from baked textures to disposable render chunks.
8. Tag every request `(worldChunk, slot, generation)`; discard stale completions
   before upload.
9. Publish completed chunks on the render thread and retire prior ranges after
   the GPU fence.

Expose chunk state (`missing`, `generating`, `queued`, `uploading`, `resident`,
`retiring`), active brick counts, upload bandwidth, and resident-memory usage.

## Tests and acceptance gates

Terrain:

- Fixed seed gives identical sites, topology, heights, and biomes across runs and
  generation orders.
- No accepted site pair violates the configured Poisson radius.
- The whole-map Delaunay/Voronoi graph has reciprocal centre/corner/edge links
  and no invalid boundary cells.
- Baked texture aprons and render-chunk border samples are bit-identical.
- Downhill pointers have no cycles; non-lake land reaches a valid lower outlet;
  flow accumulation is deterministic.
- Package metadata, checksums, tile records, and the copied configuration hash
  match the bake settings; exported maps match requested quantization.

Bricks and streaming:

- A 160 x 160 chunk emits exactly one surface voxel per non-empty column and no
  below-surface occupancy.
- Neighbouring chunks have identical border material/height data and no finest
  level visual seam.
- Rebuilding a chunk from the same baked texture columns is byte-identical.
- A one-chunk camera move schedules only an entering/leaving strip.
- Stale worker results cannot overwrite a reassigned slot.
- GPU ranges are recycled only after the final reader frame retires.
- A smoke test renders a seeded terrain window through the brick G-buffer path.

Measure whole-map site/graph generation, terrain rasterization, bake compression
ratio, runtime tile-read/decompression time, cache hit rate, chunk build time,
upload bytes, active brick/root counts, brick culling time, GPU memory, and
terrain frame time before tuning the default window, vertical range, or site
spacing.

## Implementation order

1. Define `terrain-config.json`, package/tile binary schemas, versioning, and
   configuration hashing.
2. Add terrain settings/types, stable hashes, whole-map Poisson sampling, and
   site-count-limit tests.
3. Build the whole-map Delaunay/Voronoi dual graph and its topology tests.
4. Add land/water, elevation, drainage, rivers, moisture, biomes, and the
   offline `PolygonTerrainSample` baker.
5. Add streamed map rasterization, package writing, independent tile compression,
   border aprons, package-validation tests, and preview exports.
6. Add `TerrainPackageReader`, asynchronous tiled I/O/decompression, LRU cache,
   `TerrainChunkSurface`, and CPU surface-brick tests.
7. Add brick root indirection, fence-safe range allocation, and the asynchronous
   33 x 33 `TerrainVoxelSample` streaming path.
8. Add profiling, chunk-edge LOD constraints, river carving, and detail tuning
   only after exact finest-level rendering is stable.

The terrain package and its copied configuration are normal generated build/game
artifacts. Whether a particular baked world is checked into source control is a
content-delivery decision, not a runtime requirement; runtime always consumes the
package and never reconstructs it procedurally.
