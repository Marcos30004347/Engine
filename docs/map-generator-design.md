# Map generator — design

Written 2026-08-17. A Perilous Shores-style symbolic map generator, built as the
*first* stage of terrain generation. The map is the authored layout; heightmaps
and the other runtime maps are derived from it later (§7).

Companion documents: `terrain-system-design.md` (the consumer),
`heightmap-streaming-plan.md` (runtime delivery).

---

## 1. What this produces, and what it deliberately does not

**Produces:** a cell-partitioned map with land/water classification, coastlines,
river courses, per-region content types (mountain, wood, swamp, desert, plain),
settlements, names, and a derived contour graph once elevation intent exists.

**Does not produce:** a final dense, eroded terrain heightmap. It does carry
continuous elevation and uplift intent in a bounded map-resolution surface and
derives sparse elevation contours from that same field. This is deliberate: the
surface and contours describe the authored layout; the later land generator
still owns fine terrain, erosion and material deposition.

### Evidence base

Read from the compiled `Perilous.js` (638 class names survive compilation;
method bodies are minified, so structure is reliable and fine detail less so).
The top-level controller is `com.watabou.perilous.model.Blueprint`, constructed
from `(seed, tags)` and reading `seed`/`tags`/`w`/`h` from the URL. Its entry
point is exactly four calls:

```
Blueprint.build() = buildCoast() → buildRivers() → spawnTerrains() → spawnTowns()
```

**There is no heightmap and no erosion in it.** The only `height` is the output
image dimension. What stands in for elevation is `face.data.level`, a *discrete
per-cell integer*, used solely by `buildRivers` to pick a descent direction
across the half-edge graph. Mountains are `model.content.Mountain` assigned to
regions and drawn as `mapping.sprites.MountainSprite` — illustration symbols,
not terrain.

That is the correct architecture for a map that gets drawn. Ours has one extra
obligation: it must also be *convertible into terrain*. §6 is about that gap and
is the part with no reference implementation to copy.

---

## 2. Data model

Mirrors the reference where it is sound.

**Half-edge mesh (DCEL).** Poisson-disk sampling → Delaunay triangulation →
Voronoi dual → DCEL. The reference uses `geom.PoissonPattern`,
`geom.Delaunator`, `geom.VoronoiPattern`, `geom.DCEL`, with `Face`, `HalfEdge`,
`Vertex`. Poisson-disk rather than uniform random matters: it gives evenly sized
cells without a lattice, and the engine already has blue-noise sampling and a
CDT from the procedural generation port.

**Per-face data**, following the reference's `face.data`:

```
  land        bool     above sea level
  coastal     bool     touches water
  shallow     bool     water adjacent to land
  level       int      discrete elevation tier
  region      ref      owning region
  content     enum     Mountain | Hill | Wood | Swamp | Desert | Plain | Rocks
```

**Regions** group faces into named areas with a shared content type. **Rivers**
are polylines over the mesh graph. **Sites** are settlements with a tier.

**One deviation from the reference, and it is not optional: the map is in
metres, not pixels.** `Blueprint` carries `width`/`height` as image dimensions
(`w=1800&h=1800` in the URL). Ours carries a world extent in metres and a
separate render scale. Everything downstream — cell size, river width, erosion
grid, settlement spacing — is a physical quantity, and a generator that thinks
in pixels will silently produce a world whose features have no real size.

---

## 2A. The map is a hierarchical graph

The single most important structural decision: **the map is a graph of nodes at
multiple scales, not one flat cell partition.** A node owns an area, carries its
type and intent, and can be *refined* into a finer subgraph on demand.

```
  L0   world      landmasses, seas, mountain systems      ~50 km nodes
  L1   region     provinces, ranges, basins, forests      ~5 km nodes
  L2   locale     valleys, ridges, bays, town hinterlands ~500 m nodes
  L3+  detail     refined only where needed               ~50 m nodes
```

Every node carries the same shape of data — type, elevation intent, uplift
intent, relief amplitude, boundary conditions — so refinement is the same
operation at every level: partition this node's area into a finer Voronoi
subgraph, inherit and constrain from the parent.

This is what makes three otherwise separate problems one problem:

- **Multi-gigabyte maps.** Nothing is generated at full detail up front. The
  world exists at L0–L1 always; deeper levels are produced on demand. Total
  storage is a function of what has actually been visited, not of world area.
- **"Run complex algorithms on a selected region."** Expensive per-region
  generation — high-resolution erosion, multi-layer soil, valley carving — is
  refinement of one node, at a resolution that would be unaffordable globally.
- **Streaming.** The node *is* the streaming unit. It already has a bounded
  area, a parent to fall back to, and a natural LOD. `heightmap-streaming-plan.md`
  assumed tiles; nodes are strictly better because they carry semantics.

### 2A.1 Boundary contracts — the hard part

If two sibling nodes refine independently, their shared boundary must still
agree, or the world cracks along every region edge. This is the central
correctness problem of the whole design and it needs to be explicit, not
emergent.

**The parent owns the boundary.** Before any child refines, the parent fixes, on
each shared edge: elevation profile, river crossing points and discharge,
material/biome class, and slope. Children treat these as *immutable boundary
conditions* and may only refine the interior.

Consequences worth stating plainly:

- Refinement is a **pure function** of (node, its boundary conditions, seed). Two
  runs, in any order, on any machine, must produce the same node. This is what
  makes on-demand generation safe and reproducible.
- A river's course across a boundary is decided by the parent, not negotiated
  between children. Same reasoning as basin decomposition in
  `terrain-system-design.md` §2.1 — global drainage is resolved coarsely first,
  then honoured locally.
- Refining a node must never invalidate a sibling. If it would, the thing being
  changed belongs to the parent.

---

## 3. Pipeline

```
  1  Brief            tags + seed + extent          -> parameters
  2  Partition        Poisson -> Delaunay -> Voronoi -> DCEL
  3  Raise            archetype template            -> land/water per face
  4  Coast            outline extraction            -> coastline polylines
  5  Elevation        from cell type + neighbours   -> elevation + uplift intent
  5A Contours         isolines over welded cells    -> elevation contour graph
  6  Rivers           descent over the graph        -> river network + discharge
  7  Regions          group faces, assign content   -> named regions
  8  Sites            settlements + roads           -> towns, routes
  9  Names            toponymy                      -> labels
  9A Map export       registered world-data bundle -> graph + maps + manifest
 ---  the map is complete and exportable here (§7D) ---
 10  Rasterise        intent fields at sim resolution -> initial heightmap
 11  Erosion          against sustained uplift       -> eroded surface
 12  Derive           material, biome, landform, soil layers
 13  Terrain export   refined height/material tiles + graph
```

Stages 1–9 are the map. Stages 10–13 are the terrain derived from it, and they
run **per node**, at whatever level has been refined.

Stage 5 is called out separately because the reference folds it in implicitly
and we need it explicit — see §6. Note it now produces *both* elevation and
uplift intent: assigning elevation from cell type is what makes a mountain cell
high, and assigning uplift is what keeps it high once erosion runs.

### 3.1 Dendry replaces erosion on the critical path

**Gaillard, Benes, Guérin, Galin, Rohmer & Cani, *Dendry: A Procedural Model for
Dendritic Patterns*, I3D 2019.** Reference implementation:
`github.com/mgaillard/Noise`.

Dendry is a **locally computable procedural function** returning `f(p) = d(p, T)`
— the distance from a point to an implicitly constructed branching tree. It
generates ridge and river networks, and its stated application is "the generation
of terrain heightfields with consistent river networks".

This changes the plan materially, because it delivers the one thing plain noise
cannot. The paper is explicit about why fBm fails here: "because of its lack of
structuring landforms, fBm cannot generate hydrologically consistent terrains".
Dendry gives "globally consistent branching patterns while still being locally
computable".

Four properties make it fit this architecture almost exactly:

- **Locally computable.** Evaluable at a point with no global state. This is
  what §2A's on-demand node refinement needs, and it **largely dissolves the
  boundary-contract problem for drainage** (§2A.1): two sibling nodes evaluating
  the same function agree at their shared edge by construction, rather than by
  negotiation. Boundary contracts remain necessary for authored intent; they stop
  being necessary for river topology.
- **Driven by a global control function** `c: ℝ² → [0,1]` that "defines the
  overall shape and can be used, for example, to initialize local minima". **Our
  map is that control function.** The layout supplies `c`; Dendry supplies
  consistent branching detail underneath it. The paper hands us precisely the
  hook this design needs.
- **Multi-scale by subdivision.** "Each subdivision level adds more detail and
  the overall shape is inherited through the iterations" — the same shape as the
  L0→L3 hierarchy.
- **Trivially parallel**, small memory footprint, scales linearly with cores.

**Consequence: complex erosion leaves the critical path.** The
geomorphological-transport solver was the highest-risk item in
`terrain-system-design.md`; Dendry obtains hydrologically consistent drainage
without simulation. Erosion becomes an optional later enhancement rather than a
prerequisite, which is a large reduction in risk.

**What Dendry does not give**, stated so it is not discovered later. It produces
the *pattern* — branching valleys and ridges. It does not produce sediment:
no alluvial fans, no deltas, no floodplain deposition, no concave-up river
profiles from incision, no multi-layer soil. If those become important, erosion
returns — but as an enhancement to an already-working terrain, which is a far
better position than depending on it.

**Cost, from the paper:** ~10 s for a 512×512 grid on a quad-core CPU. That is
roughly 38 µs per point on four cores, so a naive 4096² would be minutes. This is
fine *because of* §2A — evaluation is per node, on demand, at the level actually
needed, never globally at full resolution. It is also trivially parallel and a
GPU implementation should improve on the paper's CPU figure substantially.
Measure before assuming.

### 3.2 One generator per cell type

Each cell type owns a generator that turns its area and intent into a
heightfield. This is the natural extension point: a new terrain character is a
new generator, and nothing else changes.

| cell type | generator |
|---|---|
| Mountain | Dendry, high branching, ridged, large relief amplitude |
| Hill | Dendry, lower amplitude, heavier smoothing |
| Plain | gentle fBm, very low amplitude, broad wavelength |
| Valley | Dendry drainage as primary structure, concave cross-profile |
| Swamp | near-flat, high-frequency low-amplitude, water table near surface |
| Desert | dune fields — anisotropic, wind-aligned periodic patterns |
| Coast | beach profile blended between land generator and sea datum |
| Rocks | high-frequency ridged, exposed bedrock |

Two rules keep this from fragmenting:

- **Every generator is a pure function of (area, intent, boundary, seed).** Same
  contract as node refinement (§2A.1), so any generator can be evaluated on
  demand, in any order, reproducibly.
- **Blending between types is the shared machinery, not each generator's
  problem.** Neighbouring cells' outputs are blended by the same weighted scheme
  as §7A's colour blending, with per-transition sharpness — so a mountain-to-plain
  boundary can be an abrupt escarpment while hill-to-plain diffuses.

### 3.3 Noise, used correctly

Noise initialises; it does not structure. Two legitimate uses:

- **Seeding the initial surface** in stage 10, so erosion has roughness to bite
  into. A perfectly smooth seed erodes into unnaturally regular drainage.
- **Sub-texel detail** below the simulation grid, at render time.

Blue noise / Poisson for point distribution, and a ridged or domain-warped
fractal for the seed surface, are the right defaults. What noise must never do
is decide where a mountain range is — that is stage 3–5's job, and a noise-placed
range is exactly the "generated-looking" result the layout-first approach exists
to avoid.

### 3.1 Archetypes ("raisers")

The reference implements archetypes as a class family:
`templates.Raiser` with `IslandRaiser`, `ArchiRaiser`, `BayRaiser`,
`CoastRaiser`, `FjordRaiser`, `HeadRaiser`, `LakeRaiser`. A raiser decides which
faces are land. This is the world-brief mechanism, already proven in a shipping
generator, and we adopt it directly.

Each raiser takes the brief's tags and produces a land mask over the faces.
Adding a world shape means adding a raiser, which is the right extension point.

### 3.2 Tags can be measured, not only authored

The reference's `Blueprint.fromProto` derives a template tag from geometry: the
land fraction of border faces below 0.2 gives `island` (or `archipelago` above
three islands), below 0.9 gives `coast`, no water faces at all gives `land`,
otherwise `lake`. Worth adopting as a *validator*: generate, measure, and check
the result matches the requested archetype. A brief that asks for an archipelago
and measures as `coast` is a generator bug, and this catches it for free.

---

## 4. Coastlines

The reference fractalizes the outline polyline — `fractalize(outline, null, 0.4)`,
then a second `fractalize` when `rugged` or Chaikin smoothing otherwise.

**Adopt this for the map, and do not carry it into the terrain.** Midpoint
displacement on a polyline is right for an illustration and wrong for a 3D
surface: its inlets connect to nothing inland, which is the most recognisable
tell of a procedural world once you can walk on it. The map's coastline is a
*draft*; §7 replaces it with the contour where the derived terrain meets the sea
datum.

---

## 5. Content and settlements

`spawnTerrains` assigns content types to regions; `spawnTowns` places
settlements. The reference's content set — `Mountain`, `Plain`, `Swamp`, `Wood`,
`Desert`, `SeaRocks`, plus `Landmark` and `Danger` — is a good gameplay-facing
vocabulary and maps cleanly onto our 26-class biome enum later.

Settlement placement should follow the suitability-then-selection split from
`terrain-system-design.md` §5B rather than the reference's approach, because we
have more to work with: once §7 produces real terrain we get slope, drainage and
arable potential as inputs. In the map-only phase, place from cell content,
river adjacency and coast access, and treat it as provisional.

---

## 6. The gap: what our map must carry that the reference's does not

This is the part with no reference implementation, because Watabou's map is
terminal and ours is intermediate. A symbolic map is not sufficient input for a
heightmap; these are the additions.

| addition | why |
|---|---|
| **Continuous elevation intent** per face, not just `level` | a discrete tier cannot be interpolated into a surface without terracing |
| **Uplift intent** per face | so erosion sustains ranges instead of flattening them — the key idea in `terrain-system-design.md` §1.1 |
| **River discharge / stream order** | course alone gives no width, no valley scale, no confluence hierarchy |
| **Relief amplitude** per region | "mountain" must distinguish 400 m hills from 3000 m peaks |
| **Slope intent at boundaries** | escarpment vs gradual transition between neighbouring regions |
| **Physical scale in metres** | §2 |

Without these, stage B has to invent them, and inventing them is where authored
intent gets lost.

Broad elevation is now composed from an independent coarse **landform province
diagram**. Each deterministic province is plain, rolling country, plateau or
highland; it supplies a regional floor, a small internal tilt, local-relief
activity and a multiplier for any orogenic seam crossing it. This removes the
old visual rule that every land cell rose monotonically with distance from the
shore. A plain can remain low and quiet deep inland, a plateau can be high but
locally flat, and the same structural range becomes subdued through a plain and
dramatic through highlands. Province edges blend over a metric width and are
independent of political regions, tectonic blocks and the partition hierarchy,
so their boundaries do not all coincide. Positive tectonic blocks can also be
classified as plateaus when their measured shape agrees.

Before definitive hydrology, a map-scale conditioning pass relaxes only the
interiors of plain and plateau landforms. It never averages across coasts,
mountain fronts, or plateau rims. A disposable preliminary drainage network
then identifies major channels; nearby cells are graded toward each channel's
monotone filled elevation with a shallow bank slope. This yields broad,
buildable floodplains while preserving longitudinal drainage. The preliminary
network is discarded and rivers are rebuilt from the conditioned surface, so
the final descent and mass-balance guarantees remain intact.

The current drainage implementation uses priority-flood depression filling,
single-flow descent and physical runoff accumulation. Channel heads are chosen
with a discharge–slope stream-power proxy; once initiated, a course remains
connected across its downstream floodplain. Strahler order and discharge remain
on the graph for terrain constraints and width selection. Lakes are selected
per connected filled basin, starting from the deepest cell and stopping at an
absolute area or catchment-share cap. Waterfalls are spaced, high-gradient
knickpoints rather than every steep channel cell.

---

## 7. Bridge to terrain (stage B, later)

Sketch only; specified properly in `terrain-system-design.md`.

1. **Rasterise intent.** Cell elevation intent, uplift intent and river channels
   become continuous fields at the simulation resolution, smoothed across cell
   boundaries so the surface is not faceted.
2. **Seed + uplift, not just seed.** The initial heightmap comes from elevation
   intent; the uplift field comes from uplift intent. Erosion runs against the
   uplift, so ranges persist because they are being pushed up.
3. **Rivers as constraints.** Laid-out courses become low-elevation seed
   channels plus a drainage bias, so the solver's own network chooses to follow
   them rather than being overwritten.
4. **Erosion**, then re-derive coastline, landforms, climate and biomes from the
   eroded surface.
5. **Agreement check.** Compare authored content type (stage 7) against measured
   landform (post-erosion). Divergence means the synthesis was too weak or the
   erosion overran the layout. This is the cheapest quality signal available and
   should be a standing gate.

---

## 7A. Map export — coloured cells with blended transitions

The map is exportable as an image long before any terrain exists, and this is
the primary way it gets judged during phases 1–4.

**Colour by cell type, blend across boundaries.** A flat fill per cell reads as
the Voronoi diagram it is, and hard edges are the most recognisable procedural
tell after noisy coastlines. Instead each raster texel takes a *weighted blend*
of nearby cells' colours.

Weighting by inverse distance to the nearest few cell sites gives smooth
transitions; a sharpening exponent controls how crisp a boundary is, so the same
mechanism produces both a soft forest-to-grassland ecotone and a hard
escarpment. **Boundary sharpness is a property of the transition, not a global
constant** — coast and escarpment should stay crisp while biome edges diffuse.

Two things this earns beyond a nicer picture:

- It is the same operation the biome ecotone blending needs
  (`terrain-system-design.md` §5), so it is written once and used twice.
- Every cell is independent, so it is embarrassingly parallel and runs per node
  at whatever refinement level is loaded.

Worth being straight about cost, measured on the current system: per-texel
classification is 16 ms at 1024 and 63 ms at 2048 — under 1% of generation. This
is a **quality** change, not a performance one. It replaces a nearest-cell
lookup with a continuous function; do not expect it to speed anything up.

Export targets: a raster image for inspection, SVG for the stylised map (the
reference uses `mapping.SvgExporter`), and the versioned world-data bundle in
§7D. The serialized graph remains authoritative; textures are registered caches
and runtime inputs, not a second independently editable world model.

## 7B. Topographic view and GPU renderer

Elevation contours are a **derived graph**, not pixels painted onto an export.
`MapSurfaceField` first samples elevation, uplift, relief and roughness from the
graph's smooth Shepard field onto one bounded regular grid. Each grid square has
a fixed diagonal, making the surface a watertight piecewise-linear triangle
field. Heightmap rendering, GPU mesh vertices, uplift rendering and contour
extraction all sample this same object; `flatCells` affects categorical regions,
not these scalar channels.

Contour extraction marches those exact grid triangles. Equal-elevation
crossings join by their shared triangle-edge identity, producing deterministic
closed loops without a fragile positional weld or hierarchy T-junctions. Raw
polyline points therefore lie on the same scalar surface rendered by the
heightmap. The loops are then sampled with centripetal Catmull-Rom spans for
smooth display. Every polyline retains its elevation and whether it is a major
index contour (100 m / every fifth line by default).

The rounded display line is deliberately **not** inverted to recreate height.
Contour intervals discard extrema and inverse reconstruction would introduce
terraces. The authoritative order is graph intent -> shared continuous surface
-> exact isolines -> rounded display geometry. Terrain and contours consequently
agree without making a lossy presentation artifact the source of truth.

That representation is intentionally renderer-independent. It supports:

- a topographic overlay today;
- GPU line expansion and antialiasing without rerunning generation;
- elevation-band, snow-line, terrace and distance-to-contour effects in the
  voxel world later;
- SVG export with real elevation metadata.

Contours are not a replacement for the height field. Voxel displacement and
surface normals should sample the continuous terrain function; the contour graph
is the sparse semantic/index representation for effects and inspection.

The current preview presents through Vulkan, while map tessellation, line-graph
construction and headless rasterisation remain on the CPU. The interactive
renderer now uses dedicated fill and analytic-line pipelines with persistent
buffers; ImGui remains the CPU reference and owns labels, selection and the
world frame. The GPU migration preserves the `MapGraph` and contour APIs and
replaces only their presentation:

1. **Working:** upload stable buffers after generation. Cell triangles carry
   all display-layer material channels plus scalar elevation; all boundary,
   river and contour segments are persistent.
2. **Working:** draw cell triangles with a dedicated map fill pass. Pan and zoom
   are uniforms and do not rebuild CPU or GPU geometry. Layer selection and
   per-fragment elevation ramping are shader-side and no longer rebuild or re-upload the
   mesh. Categorical Shepard influence is baked into a registered semantic
   field; its guttered pages are resolved and sampled per fragment, and only
   rebuild when blend settings change.
3. **Working:** draw rivers, coastlines, cell/region boundaries and minor/major
   contours with one instanced analytic-line pass. Widths stay in screen space;
   analytic coverage provides antialiased edges and round caps. Closed contour
   loops upload their final-to-first span explicitly.
4. **Partly working:** hillshade derives surface gradients from interpolated
   elevation in the fragment shader. Semantic biome pages are merged there as
   well, including ancestor fallback. Exact per-fragment Shepard evaluation
   would still require uploading the site grid and node material table. Keep
   the existing CPU PNG path as the deterministic reference.
5. Only move contour extraction to compute if profiling says construction is a
   bottleneck. It currently is not; rendering and per-pixel field evaluation are
   the expensive work.

Curved region/coast boundaries stay upstream of both renderers. Boundaries are
welded once, displaced through a shared domain warp, and curve-sampled as chains
with interpolating cubic Bezier spans. Internal region borders use a much lower
warp amplitude than coasts and low-pass only ordinary two-edge bends; coastline
vertices and true multi-region junctions remain authoritative. Region growth
prefers leaf siblings under the same larger hierarchy parent, which removes
cell-scale sawteeth without discarding river and ridge resistance. Because both
adjacent cells consume the same path, smoothing cannot open cracks along a
region edge.

## 7C. Erosion and ground materials

The shared field is also the hand-off point for a future terrain simulation:

```
conditioned map graph
  -> MapSurfaceField (elevation, uplift, relief, roughness)
  -> hydraulic and thermal erosion at the selected node resolution
  -> rebuild drainage, lakes, coast constraints and contours
  -> derive ground-material and soil layers
```

Uplift remains a separate channel so erosion can expose and maintain mountain
systems rather than only smoothing an initial heightmap. Erosion must run before
the final drainage and contours: both water paths and isolines describe the
post-erosion surface, not the seed.

Ground material should be derived from lithology, slope, water, erosion and
deposition rather than assigned as random biome decoration. A useful first
vocabulary is bedrock, scree/talus, colluvium/debris, alluvium, sand, clay/silt,
loam, peat, salt crust and glacial till. Biome then consumes those substrate
properties alongside temperature and moisture. The map-resolution field only
provides the interface; expensive transport and layered soil remain local,
refinable terrain work.

**The parent refinement field is now working.** `MapTerrainRefinement` performs
a deterministic priority flood over the registered raster, builds a total D8
receiver graph, accumulates moisture-weighted flow, applies bounded
stream-power incision and capacity-limited deposition, and derives substrate,
soil depth and bedrock/regolith/alluvium/organic weights. Sediment mass balance
is tested. This country-scale result is guidance and an ancestor fallback for
regional terrain; metre-scale hydraulic/thermal iterations and detailed
lithologic beds still run only when a node is refined.

## 7D. World-data export for future procedural placement

The map generator exports **placement inputs, never final prop instances**. This
follows the useful separation in Jaap van Muijden's GDC 2017 presentation,
*GPU-based Procedural Placement in Horizon Zero Dawn*: streamed 2D world-data
maps feed rule layers, density evaluation and later object generation. Their
shipping data was metre-scale; copying that globally would be prohibitive for
our world. We keep the semantic separation but evaluate fine density only for a
loaded/refined terrain node.

The L0 country export defaults to one registered 1024x1024 texture set. At the
current 1800 km extent this is 1.76 km/texel: enough for biome, range, watershed
and broad vegetation intent, never individual trees or rocks. Later regional
LOD tiles inherit the same channel schema and parent boundary conditions. The
runtime streaming system may physically page these into smaller tiles; that is
an implementation detail of the consumer, not a different map format.

Every export is a versioned bundle:

```
world-map/
  manifest.json              schema, seed, world transform, units, hashes
  graph.bin                  authoritative nodes and vector features
  preview.png / map.svg      inspection only
  pages/<field>/             independently stored planar-quadtree pages
```

The first schema reserves the following fields. A field may be unavailable, but
its manifest entry must still exist; `available: false` distinguishes a planned
empty layer from a genuinely generated all-zero map. Loaders may bind one
canonical zero texture for unavailable fields, so integrations get an empty map
without storing a redundant zero-filled file for every world.

| field | representation | filtering | map-stage source |
|---|---|---|---|
| base elevation | R16 UNORM + scale/offset, or R32F authoring | linear | `MapSurfaceField` |
| uplift | R16F | linear | `MapSurfaceField` |
| relief + roughness | RG16F | linear | `MapSurfaceField` |
| temperature + moisture | RG16F | linear | climate |
| region / biome / landform IDs | integer channels | nearest only | classifiers |
| biome mixture IDs + weights | RGBA8 UINT + RGBA8 UNORM | paired sparse-mixture sampling | blended classifiers |
| ocean/lake mask + water level | typed mask + R16F | nearest / linear | hydrology |
| river distance + discharge | RG16F plus vector graph | linear | river network |
| coast distance | R16F | linear | coastline |
| vegetation/rock potential | RGBA8 sets | linear | biome + substrate + climate |
| refined elevation | R16 UNORM + scale/offset | linear | parent terrain refinement |
| erosion wear + flow / deposition | RG16F + R16F | linear | priority-flood drainage and sediment transport |
| substrate ID | R8 UINT | nearest | slope, biome, wear and deposition classifier |
| soil depth / soil layers | R16F + RGBA8 | linear | refined substrate model |
| roads + settlement/activity | empty reserved maps plus vectors | nearest | future map stage |
| object exclusion/topology | empty reserved map | nearest | future authored/runtime input |

Continuous and categorical fields must not share filtering rules. Region IDs
must never be linearly interpolated. Rivers, roads and contours remain vector
graphs as well as optional raster distance/mask caches, because their topology
cannot be recovered reliably from a low-resolution image.

The future runtime consumes this bundle through two separate systems:

1. A **placement palette** maps an ecotope and local conditions to weighted
   asset entries. An entry can reference either a voxel model or a conventional
   low-poly mesh/material and carries footprint, spacing, scale range, slope,
   altitude, moisture, substrate and LOD constraints.
2. A **GPU placement pass** evaluates density for the loaded terrain tile,
   generates deterministic blue-noise/dithered candidates, resolves footprint
   and exclusion conflicts, then emits instance buffers.

Map generation still ends at deterministic export of the graph, existing
fields, declared empty future fields and their coordinate/format contract. A
runtime-neutral CPU reference for the first boundary now lives in
`MapPlacement.{hpp,cpp}`: it validates opaque voxel/mesh palette references,
samples resident map pages, and emits deterministic, seam-stable candidates.
Asset loading, terrain collision/projection, GPU instance emission and spawning
remain deferred runtime work. This keeps roads and prop placement additive
instead of forcing an export-format migration later.

**Export schema v4, working.** `MapExport.{hpp,cpp}` writes a deterministic
little-endian bundle with `WMG1` graph data, `WMF1` coast/river/contour/region
features, a JSON manifest and independently LZ4-compressed 256x256 page
pyramids with one-texel gutters. Twenty fields are populated: the previous
eighteen plus top-four biome IDs and normalized weights. Roads, settlement
activity and object exclusion are the three declared unavailable fields.
Placement potential now combines biome intent with soil depth and exposed
bedrock.

The two biome-mixture fields form one `biome_mixture` sampling group. A texel's
four IDs correspond slot-for-slot to its four weights; active weights sum to
255 and the dominant ID agrees with `biome_id`. They must not be hardware
filtered independently. A consumer fetches neighboring pairs, multiplies each
weight by its spatial interpolation coefficient, accumulates equal IDs, then
renormalizes. Parent pages perform this merge during export before selecting
their strongest four contributors, so ancestor fallback remains semantic.

`MapExportSampler` is the executable reference for consumers of this contract.
It converts normalized or world-space positions to quadtree addresses, samples
cell-centred nearest and linear fields through page gutters, decodes stored
scale/offset values into physical units, and resolves absent pages to their
nearest resident ancestor. Biome IDs and weights always resolve to one common
ancestor; the four bilinear footprints are merged by ID and renormalized rather
than interpolating integer IDs.

The interactive GPU renderer now implements that contract. Its material texels
carry packed top-four biome IDs and weights, and upload as 256x256 quadtree
pages with one-texel gutters plus a logical-to-physical page table. The fragment
shader resolves missing addresses through at most the current two ancestors,
uses nearest lookup for categorical layers, and performs the same sparse merge
for biome presentation. A live resident-LOD control can hide detailed pages to
exercise fallback without rebuilding or uploading the map.

`MapExportStreamingCache` is the runtime-neutral residency planner in front of
that page-table contract. A camera supplies normalized visible bounds, a target
LOD, a guard band, requested fields and a byte budget. The planner pins every
requested root, prioritizes visible pages from the view centre outward, and
returns deterministic load/eviction transactions. Fields in a sampling group
are admitted and evicted atomically, so biome IDs can never become resident
without their companion weights. Missing detail resolves to the group's common
ancestor and budget pressure degrades to roots rather than producing holes.
The cache deliberately performs no file I/O or GPU writes: an asynchronous
loader applies its transaction to physical slots at a safe frame boundary.

`MapExportPageCache` is that loader-side execution layer. Given the planner's
resident target, `readMapExportManifest` first loads only addressing, format and
hash metadata—every raster/page payload stays empty. One worker then reads only
the requested page files, validates the stored hash, decompresses LZ4, validates
the decoded hash, and queues complete sampling units. The render thread calls
`publish` with a per-frame byte cap at its fence-safe boundary. Publication
assigns stable bounded physical slots, evicts stale detail, and returns uploads
followed by the complete logical-to-physical table. The table is intentionally
last: a shader can never discover a slot before its bytes exist. Superseded
camera targets discard stale prepared work, roots must fit before a target is
accepted, and biome ID/weight pages remain atomic through preparation,
throttling, slot assignment and eviction.

This is renderer-neutral map infrastructure. The map preview already exercises
the matching GPU page-table shader; the future voxel runtime can map publication
slots to a buffer or texture array without changing the export or residency
policy.

`MapExportGpuPageCache` now provides the concrete RHI buffer consumer. It packs
all available field formats into one fixed-stride, byte-addressable physical
pool sized by the largest page, and builds a dense field-major quadtree table.
The accompanying `MapExportGpuPageLayout` validates every mapping and payload
before device mutation: logical addresses must exist in the manifest, physical
slots must be unique and bounded, decoded byte counts must match the field
format, and sparse groups must be complete. `apply` writes physical slots first
and commits the indirection table last. A caller either owns one cache per frame
resource or invokes it only after the cache buffers' fence completes; the class
does not hide synchronization with a global device stall.

`map_export_pages.wgsl` is the matching portable shader consumer. A third,
immutable metadata buffer precedes the page table and physical pool in the
binding contract; it records page geometry and one descriptor per available
field (format, filter, table offset, texel stride and physical-unit
scale/offset). The include performs nearest and gutter-safe bilinear sampling,
ancestor fallback, decoding for every schema-v4 storage format and paired
sparse-biome reconstruction at one common LOD. A dedicated compute shader is
compiled with the runtime shader suite, while the interactive map imports the
same Morton/quadtree addressing functions. Consumers select fields by the
stable indices exposed by `MapExportGpuPageLayout::fieldIndex`; unavailable
future maps have no descriptor and therefore cannot be mistaken for zero data.

`WorldMapGpuTests` closes the execution gate instead of treating successful
shader compilation as proof of the contract. It builds a deterministic paged
bundle containing every schema storage format, publishes root pages through the
real RHI cache, binds the metadata/page-table/physical buffers to Vulkan,
requests a missing LOD-1 page and reads the compute results back. The device
results verify root fallback, physical-unit transforms, half and unorm decoding,
exact 32-bit categorical IDs and normalized duplicate-aware biome merging. This
is still map-generator validation: no prop instances or runtime asset catalog
are introduced.

The sample command is:

```
WorldGeneratorSample --export <directory> --export-resolution 1024
```

On the current default 1800 km country, the 1024 bundle has 420 pages across
levels 0–2. It stores 76.0 MiB of decoded pyramid data (58.0 MiB at full
resolution) in 19.7 MiB and exports plus readback-verifies in roughly 2.9 s on
the development machine. Compression is selected independently for each page
and retained only when smaller. The loader validates the stored hash,
decompresses, validates the decoded-page hash, reconstructs each full field,
and validates that hash too. Every field pins level zero; a missing regional
address walks to its closest stored ancestor. Page gutters reproduce adjacent
interiors byte-for-byte, categorical parents use majority reduction, and
continuous parents use component-aware averaging. Sparse biome parents merge
weights by biome ID and renormalize instead of averaging unrelated slots.

---

## 8. Verification

- **Determinism** — same seed and tags, identical map. Hash the serialized
  model. Cheap here and much harder to retrofit later.
- **Archetype agreement** — §3.2's measured classifier matches the requested
  tag.
- **Topology** — every river reaches the sea or a lake; no cell is
  simultaneously land and water; no orphan regions.
- **Coverage** — no content type dominates; every major landmass has at least
  one settlement.
- **Look at it.** The map is an image. Read it. This phase is uniquely easy to
  judge by eye, and that advantage should be used while it lasts.

---

## 9. Phasing

| phase | deliverable | gate |
|---|---|---|
| 1 | Partition + raisers + coast, rendered flat | recognisable island/archipelago/coast from tags |
| 2 | Levels + rivers | rivers descend, reach water, branch believably |
| 3 | Regions + content + names | a readable fantasy-map-style output |
| 4 | Sites + roads | settlements survive inspection |
| 5 | §6 intent fields + contour graph | map carries enough to build terrain; isolines are deterministic and connected |
| 6 | GPU map presentation (§7B) | pan/zoom/layer changes do not rebuild CPU geometry; GPU and reference PNG agree |
| 7 | Versioned map world-data export (§7D) | graph and co-registered fields round-trip; unavailable roads/placement maps are explicit |
| 8 | Sites, roads and authored topology fields | placeholder maps become generated without changing the schema |
| 9 | Dendry + per-type generators (§3.1–3.2) | heightmap with consistent drainage; terrain matches the map that authored it |
| 10 | *Optional:* erosion for sediment features | fans, deltas, floodplains — only if §3.1's gap actually matters |
| runtime | density evaluation, collision and voxel/mesh prop spawning | deliberately deferred; consumes §7D without changing the map generator |

Phases 1–7 are the current map-generator scope. Phase 7 freezes the consumer
contract before roads and runtime placement exist. Later phases fill reserved
fields or derive local terrain; the runtime row is a separate project.

---

## 10. Open questions

1. **Level spacing.** §2A proposes ~50 km / ~5 km / ~500 m / ~50 m. The ratios
   between levels matter more than the absolute numbers: too coarse a step and
   refinement has to invent most of the detail; too fine and the hierarchy has
   more levels than anything needs. Worth fixing before phase 1, because it sets
   every downstream resolution.
2. **Fine soil calibration.** The parent raster now closes sediment mass
   balance and exports normalized bedrock/regolith/alluvium/organic layers.
   Regional metre-scale transport still needs calibration against plausible
   incision, fan and delta dimensions before it may create detailed soil beds.
3. **Hex mode.** The reference supports a hex grid (`Hex`, `Hexes`,
   `LoadedHexes`) alongside Voronoi. Relevant if the game wants tile-based play.
4. **Editability.** The reference has `RegionEditor` and edit forms — the map is
   authorable after generation. If we want that, the model must be serializable
   and re-runnable from an edited state, which constrains stage B to be a pure
   function of the map.

---

## 11. Licensing — binding constraint on implementation

*Not legal advice. If anything here is load-bearing for shipping, get it
reviewed by someone qualified.*

Both reference implementations we have been discussing are **GPL-3.0**:

| project | licence | our use |
|---|---|---|
| `github.com/mgaillard/Noise` (Dendry) | **GPL-3.0** | reference only — see below |
| `github.com/watabou/TownGeneratorOS` | **GPL-3.0** | reference only |
| Perilous Shores | not published | observed architecture only |
| `github.com/erosiv/geotransport` | check before use | not yet verified |
| `github.com/AnisB/large_cbt` | check before use | not yet verified |

GPL-3.0 is strong copyleft. Incorporating or linking that code into the engine
would require the resulting work to be released under GPL-3.0. For this codebase
that is almost certainly unacceptable, so the constraint is hard.

**What is and is not restricted.** Copyright covers the *expression* — the source
code. It does not cover the *algorithm* described in the paper. Dendry is
published at I3D 2019 specifically so it can be implemented. So:

- **Permitted and intended:** implement Dendry from the paper, in our own code.
- **Not permitted:** copy, translate, or adapt code from the GPL repository into
  the engine. Translating C++ to another language is still a derivative work.
- **Separate question:** patents are independent of copyright. Not investigated.
  Worth a check if this becomes a shipping dependency.

### 11.1 DECISION: clean-room implementation from the paper

**Decided 2026-08-17. No code from `github.com/mgaillard/Noise`, or from any
GPL-3.0 reference, enters this codebase. Dendry is implemented fresh from the
I3D 2019 publication.**

The offline-tool alternative — GPL obligations attach to distributing derivative
works, not to output data, so a separate baking tool shipping only baked
heightmaps would be permissible — was considered and **rejected**, because it
forfeits on-demand local evaluation inside the runtime (§2A), which is the
property that made Dendry the right choice.

Process discipline, binding on whoever implements it:

- **Work from the paper only.** Do not open the GPL repository. Having read it
  makes an independent-creation claim much harder to sustain and invites
  unconscious copying, including of structure and naming.
- **No translation.** Porting the algorithm's *code* from C++ to another
  language is still a derivative work. Implementing the algorithm's *description*
  is not.
- **Reference use, if any, is black-box only.** Run it, compare our output
  statistically. Never read it.
- **Record provenance.** The implementation should cite the paper, state that it
  is an independent implementation, and note that no reference source was
  consulted. Cheap now, and the only useful evidence later.
- **Attribution is still owed.** Clean-room removes the licence obligation, not
  the academic one. Cite Gaillard et al. 2019 in the source and in any
  documentation of the terrain system.

*Provenance note for this document: the Dendry description in §3.1 was written
from the publication (title page and §1–2) only. The
`github.com/mgaillard/Noise` repository was never fetched or read during this
work.*
