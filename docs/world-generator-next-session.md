# World generator — next session handoff

Rewritten 2026-08-18. Everything below is measured or verified unless it says
otherwise.

---

## IN FLIGHT — read this first

### 0. Topographic graph and curved rendering — WORKING

`MapSurface.{hpp,cpp}` now turns the graph's smooth Shepard elevation, uplift,
relief and roughness intent into one bounded, watertight triangle-grid field.
GPU mesh vertices, the reference heightmap and uplift view all sample that same
field. Scalar channels remain smooth even when categorical regions use the
`flatCells` presentation mode.

`MapContours.{hpp,cpp}` marches this exact shared field. Crossings join by
triangle-edge identity and every bounded-domain isoline is emitted as a closed
loop; there is no positional weld and no hierarchy T-junction. The default world
currently produces 84/84 closed contours. Each loop retains its elevation and
major/index-contour flag, then receives centripetal Catmull-Rom sampling to
remove the sharp marching-triangle joins. Raw isolines are authoritative;
rounded curves are presentation geometry and are not inverted into a terraced
height field. The graph remains renderer-independent and suitable for later
voxel masks or distance effects. The sample displays 100 m contours with every
fifth line heavier in both interactive and headless output.

`MapEdges` now completes the curved-boundary path that was present but unused:
the relaxed shared vertices remain authoritative, visible coast/region chains
run through an interpolating cubic Bezier pass, and overlay geometry consumes
every point in the curved path instead of drawing a straight chord between old
endpoints. Region-only vertices receive three conservative Laplacian passes;
coasts and true three-region junctions stay fixed.

The renderer path specified in `map-generator-design.md` §7B now retains CPU
generation, uploads stable cell/boundary/river/contour buffers, and replaces
ImGui fill and line geometry with dedicated GPU fill and analytic-line passes.
Do not port contour construction to compute before profiling; the measured
bottleneck is raster presentation.

### 0A. GPU renderer migration — FILL + LINE PASSES WORKING

`WorldMapGpuPass` and `WorldGeneratorMap.wgsl` are the first completed migration
slice. Generated map triangles carry renderer-neutral content, landform, uplift,
biome, region and scalar-elevation channels in persistent GPU vertex/index
buffers. Categorical layer selection happens in the vertex shader; elevation
ramping and hillshade run per fragment over interpolated elevation, so sun bearing,
sun altitude, vertical exaggeration and display-relief changes are uniform-only.
Cell boundaries, rivers, minor/major contours, coasts and region boundaries
share one persistent line-instance buffer and one analytic line shader with
screen-space widths, antialiased edges and round caps. Pan and zoom update only
uniforms; visibility toggles choose instance ranges without rebuilding geometry.
The pass draws into the existing Vulkan render pass with a scissor matching the
map panel, then ImGui draws controls, labels, selection and the world frame on
top.

There are live `GPU fill` and `GPU lines` controls for A/B comparison with the
CPU reference, and the stats panel reports upload time and buffer counts. Upload
waits for idle only when replacing a map or rebuilt layer; ordinary frames do
not recreate or upload geometry. The material vertex is 32 bytes. A current
default live run generated 12,962 cells and uploaded 294,829 fill vertices,
848,064 indices, 211,371 line instances and five material pages (10.16 MiB) in
6.84 ms. Runtime validation covered the
biome and derivative-based heightmap fragment paths, both pipeline creations,
generated-map upload, instanced rendering and swapchain recreation without
Vulkan errors.

Categorical influence is still evaluated on the CPU when the registered
material field is built, but it now emits semantic biome IDs/weights and a
guttered page pyramid. The GPU resolves pages and reconstructs the biome per
fragment. Changing blend sharpness/neighbour count, flat-vs-blended mode or
shore tint therefore rebuilds and uploads material pages; changing layers,
lighting or the preview residency cut does not.

**Next:** profile whether exact per-fragment Shepard blending is worth uploading
the site grid and node material table. Keep labels, selection and the
deterministic PNG reference on the CPU; only consider a GPU text path after
profiling shows those residual overlays matter.

### 0B. Map-data export contract — PAGED SCHEMA WORKING, RUNTIME PLACEMENT DEFERRED

`MapExport.{hpp,cpp}` now writes the versioned world-data bundle specified in
`map-generator-design.md` §7D and `world-generator-spec.md` §6.6. It exports a
canonical graph binary, vector coast/river/contour/region features and twenty
co-registered fields for the scalar surface, climate, classification,
hydrology, refined elevation, erosion, soil and coarse vegetation/rock
potential, including paired top-four biome ID/weight fields. Roads,
settlement/activity and object exclusion remain explicit
`available: false` fields. Every field declares its components, units,
scale/offset, filtering and decoded hash; every page declares compression plus
stored and decoded hashes.

Headless usage is `--export <directory> --export-resolution 1024`. Measured on
the default 1800 km country: 420 pages, 19.7 MiB stored from 76.0 MiB of decoded
pyramid data (58.0 MiB full resolution), and about 2.9 s to build, write and
readback-verify. The full map test verifies deterministic graph, feature, field,
page and manifest bytes, gutters, categorical/continuous reduction, compressed
and raw modes, both hashes, unavailable placeholders and path containment.

Do not place trees or rocks in this phase. The Horizon GDC 2017 reference is
useful for its boundary—streamed world-data maps -> rule/density evaluation ->
candidate generation -> placement—not for its metre-scale storage budget.
Runtime will eventually refine a loaded node, resolve density and collision,
and select voxel or low-poly mesh assets from a placement palette. That asset
catalog and GPU placement pipeline are deliberately a separate consumer.

Export gate: deterministic bundle hash; graph round-trip; identical extent and
texel registration for every field; correct nearest/linear/sparse-mixture
sampling metadata;
and explicit unavailable-state handling so an empty future road map cannot be
mistaken for a generated road-free world.

Transport and semantic round-trip are working: WMG1 reconstructs a validated
`MapGraph`, while WMF1 reconstructs coasts, complete river state, contours,
regions and names. Schema v4 stores 256x256 interiors with one-texel gutters at
quadtree levels 0–2; every available channel has a pinned root and missing
addresses resolve to the nearest ancestor. Per-page LZ4 transport validates
compressed bytes, decoded pages and reconstructed GPU-ready fields.
Biome mixture parents merge equal IDs, retain the strongest four contributors
and quantize normalized weights to an exact sum of 255; the paired fields use
the manifest's `sparse-mixture` sampling rule rather than invalid independent
linear filtering.
`MapExportSampler.{hpp,cpp}` now supplies the renderer-neutral consumer
reference: normalized and world-space lookup, cell-centre nearest/linear
filtering through gutters, physical-unit decoding, streaming-address selection,
ancestor fallback, and paired sparse-biome merging at one common resolved LOD.
Tests cover exact texels, page seams, asymmetric residency and unavailable
fields. The map preview implements the same contract:
`WorldGeneratorMap.wgsl` resolves logical material
addresses through a GPU page table, falls back through the current level-0..2
tree, reads guttered physical pages, and merges the packed semantic biome pair
per fragment. The Vulkan pass uploads separate page-table and physical-page
storage buffers. The UI's `GPU material resident LOD` control forces fallback
for inspection, while its stats report page count and resident bytes.
`MapExportStreamingCache.{hpp,cpp}` now supplies the camera-facing residency
policy: normalized view bounds, target LOD, guard pages and a byte budget become
a deterministic load/eviction transaction. Roots stay pinned, detail is chosen
from the view centre outward, missing pages fall back to resident ancestors,
and biome ID/weight pages load and evict as one unit. Tests cover camera moves,
minimal transactions, root-only degradation, insufficient budgets, oversized
guards and repeatability. The planner intentionally does not perform I/O or GPU
slot writes. `MapExportPageCache.{hpp,cpp}` now supplies that execution layer:
its worker reads and validates individual raw/LZ4 pages, its bounded cache keeps
stable physical slots, and `publish` emits evictions, byte-capped uploads and
the final logical page table for application at a fence-safe frame boundary.
Superseded targets are discarded and semantic sampling groups stay atomic
through throttling and eviction. Backend-specific descriptor-set creation and
command binding remain the consumer's responsibility, preserving the boundary
with the later voxel runtime; shared shader decoding is supplied below.
`readMapExportManifest` makes this genuinely on-demand: it loads the page
address/format/hash contract but no graph, feature, field or page payloads.
`MapExportGpuPageCache.{hpp,cpp}` is the concrete RHI consumer. It allocates one
bounded fixed-stride physical storage pool plus a dense per-field quadtree
table, validates the entire publication before touching either buffer, uploads
page bytes first and writes indirection last as the commit point. Tests cover
slot offsets, logical addressing, split sparse groups, physical aliases and
truncated uploads. Apply it only to a frame-local copy whose fence completed or
at another globally safe boundary; it deliberately never calls `waitIdle`.
It now publishes the shader ABI as a third immutable metadata buffer containing
page geometry plus each available field's format, filter, table offset, stride
and physical-unit transform. `map_export_pages.wgsl` binds metadata/page
table/physical words, performs ancestor resolution, nearest or gutter-safe
bilinear filtering, decodes all seven schema formats, and reconstructs paired
sparse biome mixtures at one common LOD. The generic compute consumer and the
preview's shared addressing include both compile as build targets. Tests cover
the complete C++ metadata layout and reject lookup of unavailable fields.
`WorldMapGpuTests` now binds the cache's real RHI buffers to the generic compute
consumer and validates device readback. Its deterministic fixture covers every
storage format, scale/offset, exact `R32Uint`, paired sparse-biome merging and a
LOD-1 request falling back to root-only residency. This closes the map-stage GPU
transport/sampling gate without starting runtime prop placement.
`MapTerrainRefinement` also exports the post-erosion surface, wear/flow,
deposition, substrate, soil depth and normalized soil layers. Re-encoding is
byte-identical. Runtime prop placement remains the next separate consumer.

Two older threads remain open. Everything described further down (phases 1–3)
is complete, tested and working; these two are not.

### 1. Hierarchical Voronoi — WORKING

`MapHierarchy.{hpp,cpp}`: a ladder of Voronoi partitions, coarsest first, each
level cut inside its parent's polygon. Measured on a 361 km world:

| ladder | level 0 | level 1 | level 2 | end to end |
|---|---|---|---|---|
| 20 km / 5 km | 195 | 2,830 leaves | — | 78 ms |
| 80 / 20 / 5 km | 12 | 178 | 2,706 leaves | 78 ms |
| 60 km / 12 km | 22 | 519 leaves | — | — |

Every coarse cell refines; the graph `validate()`s; `--levels 1` is unchanged.
`refineNode` refines one leaf on demand seeded only from (node, world seed), so
refinement is a pure function. `aggregateToParents` summarises leaves upward, so
a coarse cell knows its land fraction, dominant landform and content — the hook
for assigning realms.

**Four defects found and fixed getting here. None was obvious and all four are
worth not reintroducing:**

1. **NaN in a Dijkstra does not terminate.** `interiorSweep` divides a cell's
   coast distance by the landmass's deepest distance. A cell the coast flood
   never reached has an *infinite* distance, and infinity over an infinite scale
   is NaN; `cost >= distance[n]` is false for NaN, so every neighbour is
   re-pushed forever. This is what hung generation at two or more levels — found
   by sampling the stuck process, not by reading the code. Both quantities are
   now guarded with `isfinite`.
2. **A parent yielding no usable cells must stay a leaf, not fail the world.**
   `refineNode` returned false and aborted the build; two levels failed outright
   with "partition produced no cells".
3. **The dart-throwing sampler is unreliable in the small.** Its yield against
   the ideal count is 0.58-0.60 for radii of 20 km and below on a 361 km world,
   then 0.39 at 40 km, 0.44 at 80 km and **0.25** at 60 km. A coarse level asked
   for 36 cells got nine sites. Coarse levels now sample at 0.45x the radius —
   well inside the reliable regime — and thin to the wanted count by
   farthest-point selection. `buildPartition` is untouched, so `--levels 1` is
   bit-identical to before.
4. **Hull sites have unbounded duals and were silently dropped.** With hundreds
   of sites that costs a few border cells; with the fifteen inside one parent it
   is most of them, and a level came out with a third of the cells it asked for.
   `buildVoronoiCells` now mirrors every site across every window edge as a
   ghost, which bounds every real cell exactly at the window. Took the 20/5
   ladder from 2,110 to 2,830 leaves.

Also: `buildVoronoiCells` extracted from `buildPartition`, `clipPolygonToConvex`
added to `MapCoast`, every later stage made leaf-aware.

**Connectivity is good**, which was worth checking rather than assuming: 0% of
land leaves are unreachable from water at 1, 2 or 3 levels, mean 5.95 neighbours
at every level count. The cross-parent linking works.

**Not started, both asked for:** realms/kingdoms assigned to coarse cells, and
graph analysis to extract named mountain ranges. Both are now unblocked.

### 2. Dendry — field works, height construction does not

`land/Dendry.{hpp,cpp}` is new and clean-room from the paper (provenance note at
the top of the header; `mgaillard/Noise` was **not** opened, and that must stay
true). It builds a multi-scale downhill tree and returns distance plus the
nearest channel's control value, at **0.14–0.25 µs/sample**.

**What is not met:** the phase-4 gate is hydrologically consistent relief, and
the height construction fails it. Carving `exp(-d/width)` valleys into the
control gives **0.36% pit cells against fBm's 0.005%** on the same control at
the same amplitude — the opposite of the paper's claim, and a real defect in the
*carve*, not in the field. The exponential incision manufactures closed
depressions. There are no Dendry tests yet.

Also measured: taking the nearest channel's own elevation as the surface steps
across the medial axis between two channels and reads as terracing. That is why
the carve is into the smooth control instead.

### 3. Landmass shape — grown over the coarse partition

`MapLandmass.{hpp,cpp}` replaces the analytic archetypes for continent-shaped
worlds (`--grow 1`, now the default; `raiseLand` is untouched and still serves
the eight named shapes).

The archetypes could not make a shape that is not convex. They are analytic — a
disc, a half-plane, a union of cores — and however hard the domain was warped
the result still read as a smoothed blob. Growing over the coarse Voronoi graph
makes the shape a sequence of discrete decisions over irregular cells, and that
is what produces peninsulas, isthmuses, gulfs and inland seas.

Three stages: grow `continents` fronts over the coarsest level from one shared
queue, with a per-cell cost that varies over the map so the front is ragged;
push down to the leaves and displace the coastline against its signed distance,
re-cut by ranking so the land fraction stays exact; then drop land components
below `minimumIslandFraction`.

**One defect worth not repeating: detail belongs below the scale of the thing it
details.** The coastline displacement was first scaled to the *coarse* cell —
about 48 km of wobble on a coastline grown at 80 km. That does not detail a
shape, it dissolves it: the render came out as an archipelago of mid-sized
fragments where a continent had been grown. Scaled to the leaf cell (a few cells
of movement, with the noise frequency matched to the amplitude) it is a ragged
coast on an intact landmass.

### 4. Mountain ranges — bounded

`maximumRangesPerLandmass` (default 2) and `cellsPerRange` (900). Branches were
one per 400 cells with **no ceiling**, so a large continent grew a range system
that filled it and there was nowhere for a plain to be.

### 4A. Mixed landform provinces — WORKING

The broad elevation term is no longer one coast-distance ramp. A separate
jittered Voronoi diagram, at `landformSpacingMetres`, assigns plains, rolling
country, plateaus and highlands. Its profile controls the regional floor, the
small remaining inland tilt, local detail and how much of a passing orogenic
range survives. Edges blend across `landformEdgeMetres`; the diagram deliberately
shares neither political regions, tectonic blocks nor hierarchy parents.

Defaults draw 42% plain, 20% plateau and 18% highland provinces, leaving 20%
rolling country. `landformContrast` fades continuously from the former uniform
inland shelf to the complete province profiles. Plateau is now an explicit
`CellType`, coloured separately in the landform layer; a high floor does not
make it a mountain. Range strength also gates uplift and internal range relief,
so setting it to zero produces genuinely quiet non-orogenic terrain.

### 5. Heightmap layer

`MapLayer::Heightmap` — hypsometric tint under a Lambertian hillshade, lit from
315°/45° (the cartographic convention; relief lit from below reads as inverted).
Sun bearing, altitude and vertical exaggeration are on the UI; `--layer
heightmap` headless. The mesh path bakes the shade into vertex colours from four
field samples; the raster path finite-differences its own elevation buffer,
which is cheaper and measures the gradient at exactly the scale being drawn.

### 6. World and continent sizing

A continent is sized by **span**, not by land fraction: `continentSpanMetres`
turns a width into an area with a compact landmass's 0.6 fill factor. France is
~950 km, Italy ~700. Measured: 540,866 km^2 and 293,993 km^2 against the real
551,695 and 301,340.

The default world is now 1,800 km at 12 km cells, with one 1,250 km landmass.
That yields about 13,000 generated cells and 921,000 km² of land in roughly a
third of a second: enough room for multiple landform and climate provinces
without turning the map into a continent-scale atlas.

**Length and area settings rescale with the extent** (`rescaleForExtent`). Their
shipped defaults were tuned against 361 km, and on a France-sized world they are
absurd - a 22 km range width is a hairline on a 950 km continent, and a
2,500 km^2 region gives two hundred labels on one map. This is the third time a
scale-dependent default has bitten; assume any metre-valued default is wrong at
a new world size until it is checked.

### 7. Land types from continuous climate fields — WORKING

`MapClimate.{hpp,cpp}`. A 26-value `Biome` vocabulary (tundra, taiga, temperate
and tropical rainforest, savanna, steppe, mediterranean and montane forest,
cold/rocky/sandy desert, salt flat, wetland, swamp, mangrove, alpine meadow,
bare rock, …) stored on `MapNode::biome`, plus `--layer biome`.

**Coherent neighbours come from the fields being continuous, not from a
propagation rule.** Temperature is a cosine zonal profile in latitude minus a
6.5 C/km lapse rate; moisture is maritime reach decaying inland, times what a
rain shadow removed, plus orographic gain and a riparian term. Both are
continuous, so neighbouring cells get near-identical values and therefore
compatible types **by construction**. The adjacency table is a **gate**, not a
fixer: `countIncompatibleBorders` returns zero on a temperate world, and a
non-zero count means a *field* has a discontinuity, which is the bug worth
finding.

Heuristics that carry the realism, in order of how much they show:

- **Rain shadow.** March upwind from each land cell, remember the highest ground
  crossed, and halve the moisture per `rainShadowHalfHeightMetres` of barrier.
  This is the single heuristic that most makes a biome map look real - one side
  of a range wet, the other a steppe.
- **Latitude band.** A world is a window onto a globe; `--latitude` and
  `--latitude-span` decide whether it has tundra or rainforest. Without it every
  world is the same temperate middle.
- **Lapse rate**, which is what puts ice on a tropical summit.
- **Orographic gain** on windward slopes, and a **riparian** strip along
  channels - why a dry country has a green line through it.

**The gate did its job twice, and both times I had guessed wrong first.** It
reported 182 bad borders on an equatorial world; my first fix (the warmth axis)
changed nothing. Making the diagnostic report *which pairs* fail - now
`incompatibleBorderPairs`, kept - said `temperate forest | rainforest` x97: the
classifier stepped across two warmth zones at the 21 C line because there was no
warm *and* wet biome between them. Adding subtropical forest took it from 101 to
2. Residue is 0-3, all at hard rain-shadow edges, which is a real hard edge.

**Do not replace this with a per-region draw plus a repair pass.** The repair
pass is what you need when the underlying field is not continuous; the field
being continuous is the whole design.

### 8. What the *window* actually showed — three defects behind one complaint

"Radial islands, all grey, one mountain range in the centre." Reproduced by
running the headless path with **no flags at all**, which is exactly the state
the window starts in. Worth remembering as a technique: every render I had been
judging used flags, and none of them was the default view.

1. **The default layer was `Content`** - the older region-scored cover, the one
   already known to come out as a single landform across a landmass. That was
   the grey. Default is now `Biome`.
2. **The growth was radial**, for two reasons that both had to go. The
   resistance noise ran at a fixed frequency of 3.5 in normalized coordinates,
   which on a 1,400 km world is a 400 km wavelength - the front sees an almost
   constant field and grows a ball. And five octaves of fBm concentrate near
   0.5, so the spread between the cheapest and dearest direction was under two.
   Now: wavelength tied to the coarse cell, two octaves, squared for contrast.
3. **The border penalty was a bowl.** 6x over 12% of the extent reached a
   seventh of the way in from every side, and a front climbing out of a bowl
   grows a circle whatever else is done to it. Now 2.5x over 5%.

**And one seed grows a disc, whatever the noise does** - the cost is accumulated
distance, so the front is a distance ball. `cratonsPerContinent` (5) grows each
continent from several blocks that fuse, which is both what really happened and
what produces a lobed outline.

Over-corrected once on the way: placing every craton by farthest-point over the
whole world put them in opposite corners and each grew its own island - an
archipelago where a continent was asked for. Continent seeds are placed apart;
each continent's cratons are clustered within 0.32 of its span.

**Ranges now sit on the craton sutures.** `MapNode::craton` records which of
the growth's blocks claimed each cell, and `markSutures` puts the spine on the
seams between them - a range is a collision, not the middle of a landmass. The
long-axis double sweep survives as the fallback for a landmass grown from one
block, or one built by the analytic archetypes.

Two things this needed:

- **Not every seam is orogenic.** Marking all of them draws a closed loop around
  each block, which walls the interior into pockets - the "unreachable pieces of
  land" worry, made real. A deterministic draw per *pair* keeps about six seams
  in ten, so the ranges are lines rather than cages.
- **The flank has to be narrow enough that a seam reads as a chain.** At the
  85 km the 361 km island wanted, a 250 km seam came out as a dome and the map
  had three isolated massifs on it. 9 km before scaling.

Result: two separate range systems, off-centre, with plains between them.

### 9. "The whole map is just rivers" — and the four defects under it

**Rivers.** `riverThresholdCells` was 5, which drew a channel through about one
cell in ten. On a country-sized map that is a blue net over everything and
nothing else is visible. The current default is 35, applied to a
discharge–slope stream-power proxy rather than catchment alone. A qualifying
head retains its complete downstream course across low-gradient plains, while
independent one-cell coastal trickles are removed. Four Catmull-Rom display
segments per drainage edge round the site-to-site path without changing its
topology, and discharge drives a clearer headwater/tributary/trunk width scale.

**Lakes exist now, and they could not have before.** The elevation this stage
builds is *monotone* - a sum of distance-to-coast and distance-to-spine terms -
so it has no interior minimum anywhere and the flood found one pit in a whole
country. Basins and plateaus are now placed as layout features on interior
ground away from the ranges.

**A basin only holds water if its rim rises faster than the ground already
slopes to the sea.** The first attempt was 315 m over a 175 km radius: 1.8 m/km
against a regional 5.8 m/km, so it was a dip in a hillside, not a basin. Small
and deep, not wide and shallow - and **not scaled with the world**, because a
lake basin is the size it is whatever continent it sits on. Same for plateaus:
scaled, one reached 350 km across a 950 km country and rendered as a single ice
cap.

**Deserts.** `dryBeltStrength` adds the subtropical high near 27 degrees. That
belt is why the Sahara, the Kalahari and the Atacama are where they are; without
it a world has no deserts unless a mountain happens to shade one.

**Snow.** The glacier threshold was -8 C, which put the snowline above every
mountain this generator makes. It is now "below freezing *and* wet enough to
accumulate" - both halves matter: at 0 C alone cold dry lowlands glaciate and
central Siberia is -10 C and is taiga; at a 0.3 moisture threshold a high
plateau came out as an ice cap, and Tibet is cold desert.

**Flat cells.** `MapShading::flatCells`, on by default. The blend was built to
hide the Voronoi diagram and it does; it also smears every land type into its
neighbours, which on a map whose point is *which cell is what* costs more than
the smooth edges are worth.

### 10. `content` is now derived from `biome`

The region stage used to score cover from relief, uplift and drainage with a tag
bias and a jitter. That was a second, worse answer to a question the climate
stage already answers: the two disagreed on the same ground - a region named
"the Fubrik Desert" sitting on green forest - and the scoring collapsed to a
single cover whenever the terrain changed under it. `buildRegions` now reads the
dominant biome. The climate stage runs *before* regions.

### 11. Two more scale-dependent defaults, and a lesson about guessing

`coastalMoistureRangeMetres` at 240 km put the whole 361 km test world within a
fifth of a moisture length of the sea, so the orographic term dominated and the
interior came out wetter than the coast. It is a length; it scales.

That one took **four wrong guesses** - lakes resetting the maritime distance
(real, fixed, not the cause), latitude (real, fixed, not the cause), the
orographic term being additive (real, fixed, not the cause) - before measuring
the actual magnitudes. The pattern this whole session: guessing costs more than
instrumenting. When a number is wrong, print the numbers.

(The maritime distance *is* now measured from the ocean rather than from any
water, and the orographic term *is* now multiplicative. Both were genuine bugs
found on the way to the real one.)

### 12. Basins and plateaus come from a tectonic block diagram

Placing them as radial features worked and looked wrong: a radial feature is a
circle, so the map got round basins and round plateaus, which is not what either
is. They now come from **an independent Voronoi diagram at its own scale** whose
blocks rise and fall - the user's own suggestion of "another diagram", applied
to elevation.

A cell takes its block's offset, blended with the neighbouring block's across
`blockEdgeMetres`. That gives polygon-edged basins and, where two blocks differ,
an **escarpment** - which is what a plateau edge actually is. The diagram is
deliberately independent of the cell hierarchy so tectonic boundaries do not
fall on province boundaries.

Two numbers that had to be right, both found by measuring rather than by taste:

- **The offset must beat the regional slope.** At 55 km spacing and a regional
  fall near 6 m/km the ground already drops ~330 m across a block, so a block
  that moves less than that cannot close a basin. `blockReliefFraction` 0.30 of
  a 3,000 m budget is 900 m, comfortably over.
- **The transition must be narrow.** At 14 km against a 70 km spacing most of
  every block was in transition and the field came out as one broad swell rather
  than as blocks. 5 km - a few cells - is an escarpment.

Result before lake-footprint limiting: filled pits went 1 -> 14 and lakes 0 ->
12 on the default world. Basin-aware filling now groups those cells, starts at
the deepest floor, and grows only a connected core under a 450 km² absolute cap
and an 8% catchment-area cap. The current default retains 3 basin-filled cells
instead of flooding every qualifying shelf; pre-existing enclosed water is
reported separately in the total lake-cell count.

The draw is `x^3` over [-1,1] rather than uniform, so most blocks barely move
and a few move a long way. A uniform draw gives every block a different height
and the map reads as noise; what is wanted is mostly-level country with the
occasional basin or plateau in it.

### 13. The pipeline reordered: climate drives the rivers

Runoff was uniform, so a desert basin grew the same river a wet one did. It is
now per cell from the moisture field, and channel initiation is a
**discharge–slope stream-power proxy** rather than catchment area alone. On area
a desert basin the size of a wet one still makes a river; on discharge alone a
broad flat collector starts at the same scale as a steep upland tributary.

That inverts a dependency, so the stages are now:

    elevation -> drainage probe -> planar/floodplain conditioning
              -> climate -> definitive rivers -> lakes -> climate again -> regions

Climate first because precipitation drives the rivers; climate again afterwards
because riparian strips and lake shores are local moisture the first pass cannot
know about. Measured: a temperate world has 63 channel cells and a
subtropical-dry one has 20.

### 13A. Planar ground and settlement corridors

`MapTerrain.{hpp,cpp}` is a deterministic post-process between authored
elevation and definitive climate/hydrology. Four conservative same-type
Laplacian passes remove cell-scale slope noise inside plains and plateaus while
leaving their rims, mountains, coasts and landform boundaries fixed. A
disposable uniform-runoff drainage probe finds the major channels; ground within
`floodplainWidthMetres` is blended toward the channel's filled downstream grade
plus a shallow bank gradient. Relief amplitude and roughness fall with the
blend, explicitly marking these as buildable surfaces for the future settlement
stage.

The drainage probe mutates only a graph copy. Rivers are then regenerated over
the conditioned real graph using climate-scaled runoff, so every published
course still descends and reaches water. The default 1,800 km country conditions
878 planar cells and 790 floodplain cells. UI controls expose planar strength,
floodplain width and grading strength.

The default latitude window is 22–38° rather than the former narrow temperate
band. It crosses the subtropical dry belt at a physically plausible 16° over
the 1,800 km map, allowing deserts, steppe, mediterranean woodland, forests and
riparian wetlands or swamps to coexist in one country. Riparian moisture now
reaches the first bank cell and lake margin rather than only the channel centre.

**Lakes are gated on humidity, inflow and area.** A basin only holds a lake if
the climate can keep it full and something drains into it—the same hollow in a
desert is a salt flat. Accepted basins keep a connected deep core rather than
turning every cell below the spill surface into water.

**Block spacing is a length and scales with the world.** At a fixed 55 km a
bigger continent got proportionally more subsided blocks and therefore more
lakes: measured at 0, 42 and 140 lakes for continents of 700, 950 and 1,500 km.
Basins are also 0.45 as deep as plateaus are high, because a subsiding block
fills with sediment and a rising one does not.

**Ranges have their own diagram now.** They sat on the seams between the blocks
the *landmass* grew from, which was geologically right and practically wrong:
that diagram also decides whether the land holds together, so spreading its
blocks to spread the ranges fragmented the continent - 5 lakes became 120 at a
craton spread of 0.40. `orogenSpacingMetres` is a separate plate diagram and is
the dial that means "more mountain ranges".

**Waterfalls mark steep reaches, not falls, and the code says so.** A waterfall
is a sub-cell feature; at 12 km cells the gradient between two channel cells is
an average over 12 km. Measured on the default world, channel gradients run
median 0.0074, p90 0.016, **max 0.025** - so the first threshold tried, 0.045,
was above every gradient on the map and found nothing. Thresholds picked from a
distribution, not from taste. Candidates also need a 25 m drop and meaningful
discharge; adjacent candidates on the same stem collapse to the strongest
knickpoint. The map draws these as pale cross-channel bars.

### 14. Boundaries: a shared edge structure, then one domain warp

`src/runtime/procedural/world/MapEdges.{hpp,cpp}`. Welded vertices, edges shared
by the two cells either side, a loop of edge references per cell, and properties
on both - an edge knows whether it is a coastline, a region border, a world
border, and how long it is; a vertex knows how many cells meet there.

**This is the structure that makes the whole problem go away.** Cells carry
their outlines as independent polygons, which is fine to store and wrong to draw
from: two cells' copies of a boundary are separate arrays of points, so anything
decorating one has to be made to agree with the other by hand.

`relaxEdges` then displaces everything through a **domain warp** - one smooth
vector field of position. A displacement that is a pure function of position
moves every copy of a shared point to the same place, whoever is asking. No
canonicalisation, no shared random stream, no special cases.

Region boundaries no longer inherit coastline-strength noise. Their warp scale
is 0.18 of the base amplitude, ordinary two-edge junctions are low-pass
filtered, and cubic Bezier handles provide tangent-continuous rendering between
true junctions. Region growth also makes travel within one hierarchy parent
cheaper than crossing to the next, so the larger partition guides silhouettes
without replacing leaf-level ridge and river constraints.

**What this replaced, and why.** Red Blob Games' noisy edges (midpoint
displacement over the quad of two Voronoi corners and two sites) was implemented
first, including the agreement guarantee the article does not describe - the
path canonicalised so both cells derive the same one. It worked, and it had two
faults that were not worth fixing in place:

- **No momentum.** Each subdivision drew independently, so the path reversed at
  every step and read as static rather than as a coastline. The domain warp's
  field is smooth, so neighbouring points move together and a line bends and
  holds a direction.
- **Edges with no partner.** Across a hierarchy parent boundary the two children
  were clipped independently, their vertices do not coincide, and neither can
  find the other - there is no shared quad, so those edges were left straight
  and it showed. A position-based displacement needs no partner at all.

`MapNoisyEdges` and its tests were deleted rather than left as a second
mechanism. The technique is still where the idea came from and is worth citing.

**Both renderers work from one geometry.** `MapMesh::cells` carries the drawn
boundary; the window fan-triangulates it and the headless path scanline-fills
it. Before this the exported image resolved cells per pixel by nearest site -
a second algorithm, free to disagree, and unable to show a shaped boundary at
all.

Tested: every interior edge belongs to **exactly two** cells (one means a weld
failed, which is what leaves a hairline), edge properties agree with the cells
they came from, the world's rim is not displaced, interior boundaries are
continuous per edge, and two edges meeting at a vertex land on the same point.

### Still open, and all of it asked for

- **`content` and `biome` now say different things about the same cell.**
  `biome` is the climate stage's answer and is the one to trust; `content` is
  the older region-scored cover and still comes out as a single landform across
  a landmass. They should be reconciled - most likely by deriving `content` from
  the dominant `biome` of a region and deleting the cover scoring.
- **The separate land-type Voronoi is not built.** The climate fields made it
  unnecessary for coherence, but a diagram at its own scale would still break up
  the smoothness of purely field-driven types.
- **Plateaus.** Not started.
- **Kingdoms on the coarse cells.** `aggregateToParents` gives every coarse cell
  its land fraction, dominant landform and content; nothing consumes it yet.
  This is the one the whole hierarchy was built for — the user's stated end is
  simulating kingdoms growing and shrinking.
- **Named mountain ranges by graph analysis.** Not started.

---

## Where things stand

**Phases 1, 2 and 3 are complete and all three gates are met.**
`samples/worldGenerator/` produces a recognisable island, archipelago, coast,
bay, fjord or headland from the tag alone, with a range system, elevation and
uplift intent, a drainage network that descends and branches, and named regions
with content types rendered as a readable labelled map.

**The ask now is phase 4: Dendry (clean-room) plus the heightmap view.** Gate:
hydrologically consistent relief under the map. Spec is
`world-generator-spec.md` §5.1 and §9; the licensing terms are
`map-generator-design.md` §11.1 and they are not optional —
`github.com/mgaillard/Noise` is **GPL-3.0** and must not be opened, read or
translated. As of this handoff nobody has, and that must stay true.

Phase 4 is where Dendry has to prove itself. If its cost or quality
disappoints, that is worth knowing before anything depends on it — phases 1–3
are a complete, useful map generator on their own.

---

## What exists and is verified

`src/runtime/procedural/world/` — reached by the recursive source glob in
`src/runtime/CMakeLists.txt`, including `land/`, so no CMake edit is needed to
add files there.

| file | contents |
|---|---|
| `WorldBrief.{hpp,cpp}` | `WorldBrief`, `Tag` (8 shape + 6 character), `CellType` (11), tag resolution, brief hashing |
| `MapGraph.{hpp,cpp}` | `MapNode`, `NodeIntent`, `BoundaryConditions`, hierarchy, `nodeAt` picking, `signature()`, `validate()` |
| `MapPartition.{hpp,cpp}` | `buildPartition` — Poisson → Delaunay → Voronoi dual, symmetric neighbour links |
| `Raisers.{hpp,cpp}` | `raiseLand` (8 archetypes), coastal/shallow derivation, `borderLandFraction`, `measuredShape` |
| `MapCoast.{hpp,cpp}` | `clipCellsToExtent` (Sutherland-Hodgman), `extractCoastline` (chained polylines) |
| `MapElevation.{hpp,cpp}` | `classifyWaterBodies` (ocean vs lake), `coastDistances`, `cellArea`, `assignElevation` — range spines, elevation and uplift intent, landform types |
| `MapRivers.{hpp,cpp}` | `buildRivers`, `RiverNetwork` — priority-flood filling, steepest descent, drainage area, discharge in cumecs, Strahler order |
| `MapRegions.{hpp,cpp}` | `buildRegions`, `RegionMap`, toponymy (`placeName`, `namedPlace`, `namedRiver`) — watershed-bounded regions, cover selection, names |
| `land/Blend.{hpp,cpp}` | `SiteIndex` (uniform grid), `blendAt`, `transitionSharpness` — **the shared weighting, written once and used twice** |

`samples/worldGenerator/` — `WorldGeneratorSample.cpp` (app + headless),
`WorldGeneratorViews.{hpp,cpp}` (RHI, overlay pass, mesh build, PNG render),
`WorldGeneratorSample.cmake`.

`tests/procedural/WorldMapTests.cpp` — **54 groups, all passing.**

```sh
cmake --build build-release --target WorldMapTests WorldGeneratorSample -j 8
./build-release/tests/WorldMapTests
cd build-release && ctest -R "Planet|SphericalTerrain|PolygonTerrain|VoxelMaterial|WorldMap"
# 21/21 at handoff
```

### The headless path is how you check your work without a display

```sh
./build-release/samples/WorldGeneratorSample --headless \
    --tags coast --seed 4 --layer elevation \
    --output /tmp/map.png --resolution 640
```

`--layer` takes `content`, `landform`, `elevation`, `uplift` or `region`.

Prints cell/land counts, border land fraction, requested vs measured shape and
timings; writes a PNG you can actually look at. Exit code 2 means the map did
not read as the family that was asked for. `--help` lists every flag.

**Use it.** Both real defects found so far — the bay that was an inland sea, and
the range that was a single cone — were invisible to the test suite and obvious
in the image within seconds.

---

## Decisions already made — do not relitigate

The phase-0 list still stands: layout first, noise never decides structure, land
fraction by ranking not thresholding, `RandomThrows` over `FillVoids`,
integer-only hashing, Dendry clean-room from the paper (`mgaillard/Noise` is
GPL-3.0 and must not be opened), erosion off the critical path, the parent owns
every boundary. Reasons are in `world-generator-spec.md` §9 and
`map-generator-design.md` §11.

Added in phase 3:

**`content` is a separate field from `type`.** Landform is what the ground does;
content is what is on it. Scoring them in one competition — the obvious
reading of the reference's vocabulary — lets mountain and hill win everywhere,
because their scores rise with relief and uplift and that dwarfs any vegetation
term. The arid tag produced zero desert. Covers now compete only with each
other and with a "bare" option, and bare falls back to the measured landform.

**Nothing physical chooses forest against desert yet, and the code says so.**
The tags are the signal, terrain suitability soft-gates what cannot fit, and the
jitter distributes the rest. When a climate model arrives it replaces the
jitter, not the structure.

**Region boundaries are watersheds, so the growth cost is on *climbing*.**
Charging for occupying high ground does nothing — measured slightly worse than
no resistance at all. Charging for elevation gain makes a region fill its basin
and stall at the crest.

**Labels need collision rejection and a naming budget.** The first labelled
render put ~80 names on one island. Regions default to 2,500 km², rivers are
named only at order ≥ 2 and 15% of the largest discharge, and placement is
greedy by importance.

Added in phase 2:

**The range spine follows the landmass's long axis.** Taking the maxima of the
distance-to-coast field looks natural and is wrong: a convex island has exactly
one, so the first version produced a central cone with radial drainage — a
volcano, not a continent. The spine is now the least-cost path between the two
furthest interior points, plus a branch per 400 cells. Still a fact about the
shape of the land, so the layout decides the mountains.

**Depression filling is what makes the river gate structural.** Priority-flood
plus an epsilon, then steepest descent on the filled surface. Every land cell
then has a strictly lower neighbour, so no course can cycle or dead-end. Do not
replace this with a patch-up pass over a raw descent.

**Landform type reads the range's share of the local height, not the height.** A
tilted plain that reaches 3,000 m is a plain. This is what `rangeStrength = 0`
produces no mountains tests, and it is the check that keeps the classification
from quietly becoming a contour.

**The channel threshold decides what is drawn, not what is computed.** Discharge
is drainage area times runoff, in cumecs, and there is a test that moving the
threshold does not move it.

Added in phase 1:

**The blend's exponent is a symmetric bilinear form, and that is load-bearing.**
Taking the per-transition sharpness from the nearest cell's type flips at the
midline between two cells, and the colour jumps exactly where the blend is most
visible. `Σ wᵢ wⱼ sharpness(tᵢ, tⱼ)` over a first neutral pass is continuous.
`transitionSharpness` is symmetric by construction and there is a test that
holds it there.

**The support radius is the distance to the first *excluded* site.** Plain
inverse-distance over the nearest k steps by the dropped neighbour's whole
weight every time the neighbour set changes. Modified Shepard against that
radius fades it to zero instead. `testBlendIsContinuous` bounds the worst step
at 0.05 over a 4000-sample diagonal.

**The map is draw list geometry, not a texture.** Fan per cell, vertex at every
edge midpoint, vertex colour from the blend at that position. Neighbours share
those vertices so the fill is continuous across boundaries with no per-pixel
work, it is crisp at any zoom, there are no GPU resources to churn on a pan, and
the sample owns no SPIR-V at all.

**Shore and shelf shading is a view decision and switchable.** `coastal` and
`shallow` are derived flags, not cell types. Until phase 2 gives land more than
`Plain`, they are the only variation the blend has — turn them off and the map
is two flat colours.

---

## Traps that cost real time

The phase-0 ones all still apply: `build/` is a stale Debug tree and everything
goes to `build-release/`; **clangd lies here** (no CMake defines, stale index,
phantom errors in files you never touched — trust the build); adding a file to a
sample's `.cmake` needs an explicit `cmake -S . -B build-release`; svraster
passes resolve SPIR-V relative to the *executable*; Bash working directory
persists between calls, so use absolute paths; and floating point on a boundary
is not on the boundary.

New in phase 3:

**A test that only runs on the easy shape passes through the bug.** The region
coverage test ran on `Tag::Island` and was green while farthest-point seeding
was leaving whole islands of an archipelago with no seed, no region and no
name — obvious in the render as two unlabelled islands. Run topology tests over
more than one archetype; the archipelago is the one that has the awkward cases.

New in phase 2:

**Surface roughness is not a drainage dial.** Sweeping `detailReliefFraction`
from 0.04 to 0.40 across three wavelengths moved the river mouth count by under
10% and the Strahler order not at all — but manufactured depressions, 0 filled
pits at 0.04 and 42 at 0.40. If the drainage looks wrong, the roughness is not
where to look.

**Maximum Strahler order measures the sampling, not the network.** Same island:
5 km cells reach order 3, 2.5 km order 4, 1.5 km order 4, and lowering the
channel threshold raises it at every size. The branching test asserts Horton's
bifurcation ratio instead — 2.1 and 3.6 at every resolution measured. An
"order ≥ N" assertion is an assertion about the cell size.

**A test world can be too small to have the property under test.**
`smallPartition()` is 59 cells; on its 27 land cells the largest catchment is
under four cell areas, so no channel ever crosses the initiation threshold and
every river test failed on the world rather than on the code. Anything about
drainage uses `riverPartition()`, the shipped 3,003 cell default, which runs in
30 ms.

New in phase 1:

**A coarse classifier passing is not the archetype working.** `measuredShape`
reads the *border* land fraction. A bay and a detached inland sea give the same
border, so the archetype-agreement test passed for months while the bay
archetype produced an inland sea. The map view found it immediately.
`testBayOpensOntoTheSea` now floods water from the border and bounds the
enclosed fraction, with the lake archetype as the contrast that keeps the check
non-vacuous.

**`measuredShape` names only four things** — island, coast, lake, landlocked.
Comparing it directly against the requested tag reports a mismatch for
archipelago, bay, fjord and headland while all four are correct. Compare
families; the app has `expectedMeasuredShape` for this.

**ImGui indices are 16 bit and `PrimReserve` only starts a new vertex offset
between reservations.** A single reservation over 64k vertices wraps silently.
`MapMesh` is split into batches under 60k, closed on cell boundaries. Verified
at 19,355 cells (~5 batches) running stable in the window.

---

## Known rough edges, none blocking

- **The archipelago archetype reads as a grid** at low coast complexity — the
  interfering sinusoids show their rectilinear structure. Cosmetic, and it will
  matter more once cells carry types.
- **A circular island's range system reads as a star**, because the branches all
  return to the middle of the main axis. Irregular landmasses look right; a disc
  does not.
- **The range crest is one cell wide**, since the spine is a graph path. The
  exponential flank softens it, but a real range has a wide crest zone.
- **Region shapes are blobby.** Watershed growth gives sensible boundaries but
  no coastline-following or elongation, so regions read as patches rather than
  as provinces.
- **Sites and roads follow the initial export milestone.** The first bundle
  declares their fields unavailable, so consumers can integrate without
  mistaking a placeholder for a generated road-free world.
- **Prop instances are not a map-generator output.** Trees, vegetation and
  rocks remain a future runtime placement concern.
- **The headless font is 5×7 uppercase only.** Fine for inspection, not a
  typographic map render.
- **The fjord's inlets are evenly spaced**, because they come from a pure
  `sin(38 nx)`. Real fjords are not periodic.
- **The lake archetype's water is typed `Ocean`**, since the lake reclassification
  pass does not exist yet. It renders as sea.
- **`landFraction` and the lake/landlocked archetypes fight**, by design: shape
  and amount are independent, so "landlocked at 35% land" is honoured literally.

---

## Open questions

Unchanged and still worth settling early:

1. **Level spacing** — ~50 km / ~5 km / ~500 m / ~50 m is proposed, not settled.
   Ratios matter more than absolutes; it sets every downstream resolution.
2. **World shape** — one 361 km continent, an archipelago, or several streamed
   continents.
3. **Is the world runtime-deformable?** Far cheaper to design in than to retrofit.
4. **Cell count at L1.** Measured here: 3,003 cells run the whole pipeline —
   partition, raise, coast, elevation, rivers, blend and mesh — in 0.04 s, and
   19,355 cells in 0.15 s. That is comfortably interactive well past the 5 km
   spacing default.

---

## Documents

| doc | what it is |
|---|---|
| `world-generator-spec.md` | **the build spec**; §9 is live status |
| `map-generator-design.md` | rationale, Perilous Shores evidence, licensing (§11) |
| `terrain-system-design.md` | the wider terrain system, erosion, landforms, settlements |
| `heightmap-streaming-plan.md` | runtime streaming, deferred until there is detail to stream |
