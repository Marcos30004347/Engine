# World generator — build specification

Written 2026-08-17. The consolidated, implementation-ready spec: the map
generator, the land (heightmap) generator, and the ImGui application that drives
and displays both.

Companions: `map-generator-design.md` (rationale and evidence),
`terrain-system-design.md` (the wider terrain system),
`heightmap-streaming-plan.md` (runtime delivery).

---

## 1. Scope

Two generators and one application.

**Map generator** — a Perilous Shores-style symbolic map: a hierarchical graph
of Voronoi cells with land/water classification, coastlines, rivers, region
content types, settlements and names. It also produces broad parent elevation
and refinement fields, but not metre-scale terrain geometry.

**Land generator** — turns a map node into a heightmap and a material map, using
Dendry for hydrologically consistent branching structure and a per-cell-type
generator for local character. Runs per node, on demand.

**Application** — an ImGui tool that runs both, displays the map and the
resulting heightmaps, and lets a node be selected and refined.

**Original v1 exclusions:** metre-scale erosion, runtime streaming, voxels and
spherical worlds. Schema v3 now supplies paged parent data and bounded
country-scale refinement; runtime residency and detailed node simulation remain
separate systems.

---

## 2. Architecture

```
   WorldBrief  (tags, seed, extent in metres)
        |
        v
   [ Map Generator ]  ------------------------> MapGraph
        |                                        L0 world
        |                                        L1 regions
        |                                        L2 locales
        v                                        L3+ detail
   [ Land Generator ]  <--- select node ---
        |
        +--> Heightmap (R32F)
        +--> MaterialMap (R8U)
        +--> Derived: slope, landform, biome
        |
        v
   [ ImGui Application ]
        map view | heightmap view | layers | node inspector
```

The map is authored structure; the land generator is a **pure function** of
(node, boundary conditions, seed). That purity is what makes on-demand
refinement reproducible and is the single most important invariant in the
system.

---

## 3. Data model

### 3.1 Graph and nodes

```cpp
struct WorldBrief {
  uint64_t seed = 0;
  std::vector<Tag> tags;          // island, archipelago, coast, lowland, ...
  double extentMetres = 361000.0; // world is square; extent is a side
  double seaLevelMetres = 0.0;
};

enum class CellType : uint8_t {
  Ocean, Lake, Coast, Plain, Hill, Mountain, Valley,
  Wood, Swamp, Desert, Rocks,
};

struct NodeIntent {          // what the layout wants; not yet a surface
  double elevationMetres;    // continuous, not a discrete tier
  double upliftRate;         // sustains relief if erosion is ever enabled
  double reliefAmplitude;    // 400 m hills vs 3000 m peaks
  double roughness;          // 0..1, feeds generator parameters
};

struct MapNode {
  uint32_t id;
  uint8_t  level;            // 0 = world .. 3+ = detail
  uint32_t parent;
  std::vector<uint32_t> children;   // empty until refined

  CellType   type;
  NodeIntent intent;
  Polygon    area;           // in world metres
  bool       land;
  bool       coastal;

  BoundaryConditions boundary;  // fixed by the parent; see 3.3
};
```

**Every quantity is in metres.** The reference generator works in pixels
(`w=1800&h=1800`); a generator that thinks in pixels produces a world whose
features have no real size, and every downstream quantity — cell size, river
width, settlement spacing, simulation grid — is physical.

### 3.2 Mesh

Poisson-disk sampling → Delaunay → Voronoi dual → half-edge (DCEL). The engine
already has blue-noise sampling and a CDT from the procedural generation port;
reuse them rather than adding a dependency.

Per-face data mirrors the structure observed in the reference: `land`,
`coastal`, `shallow`, `level`, `region`, `content`.

### 3.3 Boundary contracts

**The parent owns every shared boundary.** Before a node refines, the parent
fixes, on each shared edge: elevation profile, river crossing points and
discharge, material class, and slope. Children treat these as immutable and
refine only the interior.

```cpp
struct BoundaryConditions {
  std::vector<EdgeSample> edges;   // elevation + slope along each shared edge
  std::vector<RiverCrossing> rivers;  // point + discharge + direction
};
```

Rules, non-negotiable:

- Refinement is a **pure function** of (node, boundary, seed). Two runs, any
  order, any machine, identical output.
- Refining a node must never invalidate a sibling. If it would, the thing being
  changed belongs to the parent.

Dendry's local computability (§5.1) makes drainage agree across boundaries by
construction, so these contracts carry *authored intent*, not river topology.
That is a large simplification and it is why Dendry was chosen.

---

## 4. Map generation

```
  1  Brief         tags + seed + extent
  2  Partition     Poisson -> Delaunay -> Voronoi -> DCEL
  3  Raise         archetype -> land/water per face
  4  Coast         outline extraction -> coastline polylines
  5  Elevation     cell type + neighbours -> elevation + uplift intent
  6  Rivers        descent over the graph -> network + discharge
  7  Regions       group faces, assign content -> named regions
  8  Sites         settlements + roads
  9  Names         toponymy
```

**Archetypes ("raisers").** One class per world shape, following the structure
observed in the reference (`IslandRaiser`, `ArchiRaiser`, `BayRaiser`,
`CoastRaiser`, `FjordRaiser`, `HeadRaiser`, `LakeRaiser`). A raiser decides which
faces are land. Adding a world shape means adding a raiser — the right extension
point.

**Rivers carry discharge, not just a course.** Stream order and discharge set
valley width and confluence hierarchy downstream; a bare polyline does not.

**Coastline caveat.** The reference fractalizes the outline polyline. Adopt that
for the *map*, but do not carry it into the terrain: midpoint displacement
produces inlets that connect to nothing inland, which is the clearest tell of a
procedural world once you can walk on it. The map's coast is a draft; the land
generator's coast is where the surface meets the sea datum.

---

## 5. Land generation

### 5.1 Dendry

**Gaillard, Benes, Guérin, Galin, Rohmer & Cani, *Dendry: A Procedural Model for
Dendritic Patterns*, I3D 2019.**

A locally computable procedural function returning `f(p) = d(p, T)`, the distance
from a point to an implicitly constructed branching tree. Parameters: branching
level `n`, seeding `s`, local smoothing `ε`, disturbance `Δ`, branching angles.
Driven by a global control function `c: ℝ² → [0,1]` that "defines the overall
shape and can be used to initialize local minima".

**Our map is `c`.** Rasterised elevation intent becomes the control function;
Dendry supplies consistent branching detail beneath it. The paper provides
precisely the hook this architecture needs.

Why not plain fBm, in the authors' words: "because of its lack of structuring
landforms, fBm cannot generate hydrologically consistent terrains."

**Cost.** The paper reports ~10 s for 512×512 on a quad-core CPU — about 38 µs
per point on four cores. A naive 4096² would run into minutes. This is acceptable
only because evaluation is per node and on demand, never globally at full
resolution. It is trivially parallel; a GPU implementation should improve on the
CPU figure substantially. **Measure before assuming.**

**Limits, stated so they are not discovered late.** Dendry produces the
*pattern* — branching valleys and ridges. It produces no sediment: no alluvial
fans, no deltas, no floodplains, no concave-up incision profiles, no layered
soil. Erosion remains available later as an enhancement to working terrain
rather than a prerequisite.

**Licensing: clean-room, decided.** Implement from the publication only. The
GPL-3.0 reference at `github.com/mgaillard/Noise` must not be read, copied or
translated. Full terms in `map-generator-design.md` §11.1.

### 5.2 One generator per cell type

```cpp
class ICellGenerator {
public:
  virtual ~ICellGenerator() = default;
  // Pure: same inputs, same output, any order, any thread.
  virtual double heightAt(Vec2 positionMetres,
                          const MapNode &node,
                          const BoundaryConditions &boundary,
                          uint64_t seed) const = 0;
  virtual MaterialId materialAt(Vec2 positionMetres,
                                double height, double slope) const = 0;
};
```

| cell type | generator |
|---|---|
| Mountain | Dendry, high branching, ridged, large amplitude |
| Hill | Dendry, low amplitude, heavier smoothing |
| Valley | Dendry drainage as primary structure, concave cross-profile |
| Plain | gentle fBm, low amplitude, broad wavelength |
| Swamp | near-flat, high-frequency low-amplitude |
| Desert | anisotropic wind-aligned dune patterns |
| Coast | beach profile blended from land generator to sea datum |
| Rocks | high-frequency ridged, exposed bedrock |
| Ocean / Lake | bathymetry below datum |

**Blending is shared machinery, not each generator's problem.** Neighbouring
cells' outputs blend by inverse-distance weights over the nearest few sites,
with a **per-transition sharpness exponent** — so mountain-to-plain can be an
abrupt escarpment while hill-to-plain diffuses. The same routine serves the map
view's cell colouring (§6.3), so it is written once and used twice.

### 5.3 Noise

Two legitimate uses and no others: seeding the initial surface so structure has
roughness to sit on, and sub-texel detail below the grid. Noise must never
decide where a mountain range is — that is stages 3–5's job, and a noise-placed
range is precisely the generated-looking result this architecture exists to
avoid.

### 5.4 Outputs

| output | format | notes |
|---|---|---|
| Heightmap | R32F | metres above datum |
| Material | R8U | index into the 21-class enum |
| Slope | R16F | derived |
| Landform | R8U | derived: valley/plain/ridge/plateau via two-radius TPI |
| Normal | RG16F | derived, for display |

---

## 6. ImGui application

A new sample: `samples/worldGenerator/`. Follows the proven structure of
`PolygonTerrainSample` — raw Vulkan RHI, ImGui overlay, worker-thread generation
with main-thread polling — because that pattern already works in this codebase.

### 6.1 Layout

```
+----------------+-------------------------------------------+
|                |  [Map] [Height] [Material] [Slope] [Biome] |
|   CONTROLS     |                                           |
|                |                                           |
|  Brief         |                                           |
|   tags         |             VIEWPORT                      |
|   seed         |        map or heightmap render            |
|   extent       |        pan / zoom / click-to-select       |
|                |                                           |
|  Generation    |                                           |
|   [Generate]   |                                           |
|   progress     +-------------------------------------------+
|                |  NODE INSPECTOR                           |
|  Cell types    |   id, level, type, intent                 |
|   per-type     |   area, neighbours                        |
|   parameters   |   [Refine]  [Regenerate]  [Edit type]     |
|                |                                           |
|  Display       +-------------------------------------------+
|   layer, blend |  STATUS / STATS / TIMINGS                 |
+----------------+-------------------------------------------+
```

### 6.2 Controls

**Brief** — tag multi-select (island, archipelago, coast, lowland, barren, …),
seed with randomise, world extent in km, sea level.

**Generation** — Generate runs the map on a worker thread; progress bar per
stage. Land generation is per node and triggered from the inspector, so the map
stays responsive.

**Per-cell-type parameters** — a collapsible section per `CellType` exposing that
generator's parameters (Dendry branching level, smoothing, disturbance, relief
amplitude, roughness). Changing one invalidates only nodes of that type.

**Display** — active layer, blend sharpness, contour interval, hillshade
direction, show rivers / sites / labels / cell edges.

### 6.3 Views

**Map view.** Cells filled by type colour, **blended** across boundaries by the
§5.2 weighting rather than flat-filled — a flat fill reads as the Voronoi
diagram it is, and hard edges are the clearest procedural tell after noisy
coastlines. Overlays: rivers (width by discharge), coastline, settlements,
labels, and an optional cell-edge wireframe for debugging.

**Heightmap view.** Hillshaded relief with a configurable sun direction, plus a
selectable colour ramp and optional contour lines. Hillshade matters: a raw
greyscale heightmap hides exactly the drainage structure we most need to judge.

**Material / slope / landform views.** Categorical or continuous ramps as
appropriate.

All views share pan/zoom state so switching layers does not lose the camera.

### 6.4 Node interaction

Click selects the node under the cursor at the current display level. The
inspector shows id, level, type, intent, area and neighbours, and offers:

- **Refine** — subdivide into a finer subgraph, honouring boundary conditions.
  This is the "run complex algorithms on a selected region" path.
- **Regenerate** — re-run this node with a new local seed, leaving siblings
  untouched. If a sibling changes, that is a boundary-contract bug and the app
  should say so loudly rather than silently.
- **Edit type** — override the cell type and regenerate, so a designer can fix a
  map by hand.

### 6.5 Async and threading

Generation runs on a worker; the main thread polls, exactly as
`PolygonTerrainSample::pollGeneration` does. Two rules learned there:

- **Never gate the whole view on a generation flag.** A failed land generation
  must leave the map on screen. A preview failure once took the entire planet off
  screen because the draw was gated on `state.generated`.
- **GPU work belongs to the main thread's device.** If a generator uses the GPU,
  give it its own device rather than driving the display device from a worker.

### 6.6 Export

Export a versioned, co-registered world-data bundle rather than a single image:

- serialized graph and vector coastline/river/contour features as the
  authoritative map;
- 1024x1024 L0 elevation, uplift, relief/roughness, climate, hydrology,
  region/biome/landform, top-four semantic biome mixtures and
  placement-potential fields;
- PNG/SVG renders for inspection only;
- generated refined-elevation, erosion, substrate and soil fields, plus
  manifest entries for future roads, settlement/activity and object exclusion
  emitted with `available: false` until their producers exist.

The manifest records seed, extent, coordinate transform, units, channel format,
filtering rule, scale/offset, no-data value, producer version and content hash.
All textures share texel centres and extent. Integer region/class fields require
nearest filtering; continuous fields may filter linearly. A missing producer is
not represented by a silent zero texture because zero may be valid data. The
loader may still expose a shared zero fallback when `available` is false.

This bundle is the future procedural-placement input, not its output. It contains
coarse biome/ecotope and vegetation/rock potential, but no tree, plant or rock
instances. Runtime will refine density from local terrain, select from voxel or
mesh asset palettes, resolve footprints/exclusions and emit instance buffers.
That runtime system is explicitly outside the current map-generator scope.

Implementation status: schema v4 is working in `MapExport.{hpp,cpp}` with
little-endian GPU-ready pixels, independently compressed 256x256 pages,
one-texel gutters, planar quadtree addresses, pinned roots and ancestor
fallback, plus `WMG1`/`WMF1` vector data. The bundle has 20 populated and three
unavailable fields. A 1024 export of the default world contains 420 pages and
stores 76.0 MiB of decoded pyramid data in 19.7 MiB, taking about 2.9 s
including readback, all hash validation and semantic decoding.

`MapExportSampler` is the runtime-neutral reference implementation for reading
these pages. It maps normalized or world positions to streaming addresses,
applies cell-centre nearest/linear filtering through gutters, decodes physical
units, and falls back to resident ancestors. The semantic biome pair resolves
at one common LOD and merges weighted IDs before normalization; consumers must
not interpolate the ID texture independently. GPU consumers are required to
match this behavior.

The interactive map GPU path now matches it for categorical preview data:
top-four biome IDs/weights are packed into each material texel, reduced into
semantic parent pages, uploaded as separate page-table and guttered-page storage
buffers, and merged in the fragment shader after ancestor resolution. The
preview exposes a resident-LOD cut so root and intermediate fallback can be
checked directly. Runtime voxel placement remains a separate consumer.

`MapExportStreamingCache` now turns camera-visible normalized bounds into
deterministic page loads and evictions under a byte budget. Requested roots are
pinned, a configurable page guard anticipates movement, and view-centred detail
is prioritized. Sparse sampling groups are one atomic residency unit and use a
common ancestor, preserving valid biome mixtures during partial residency. The
planner owns policy and page lookup only; asynchronous transport, physical GPU
slot allocation and safe-frame descriptor/page-table publication are the next
runtime integration layer.

That execution layer is now implemented by `MapExportPageCache`. It can stream
individual raw or LZ4 pages from an export directory on a worker, checking both
stored and decoded hashes. A bounded slot cache retains stable mappings across
camera moves and emits a safe-frame publication containing evictions, physical
uploads and the final logical page table in application order. Upload byte caps
defer whole semantic units, never one half of the biome pair. Backend-specific
descriptor-set creation and command binding remain with the consuming renderer,
which is the intended boundary between the map generator and the later voxel
runtime; the shared decoding implementation is described below.
`readMapExportManifest` supplies its metadata without eagerly reading graph,
feature, field or page payloads, so startup cost does not silently scale with
the complete world-data set.

`MapExportGpuPageCache` consumes those publications through the renderer-neutral
RHI. It owns a bounded fixed-stride storage buffer and a dense field-major
quadtree indirection buffer. Its layout encoder rejects absent logical pages,
slot aliases, bad byte counts and partial semantic groups before any device
write. At a fence-safe boundary, physical payloads are written first and the
page table is the final commit write. Per-frame replication versus a globally
safe update remains a renderer scheduling choice, not a hidden `waitIdle`.

The GPU cache also owns an immutable metadata storage buffer. Bind metadata,
page table and physical words in that order and include
`assets/shaders/procedural/wgsl/map_export_pages.wgsl`. The shader contract
resolves ancestors, reads gutters, decodes every exported integer, unorm and
half-float format, applies each field's scale/offset and refuses to sample one
half of a sparse mixture independently. Its paired biome path merges equal IDs
across four bilinear taps and returns the strongest normalized four, matching
the renderer-neutral sampler. The generic consumer has a compiled compute
smoke target; the map preview shares its quadtree/Morton addressing include.

`WorldMapGpuTests` runs that generic consumer on Vulkan with the actual three
buffers owned by `MapExportGpuPageCache`. Its fixture publishes only roots but
requests LOD 1, then checks readback for ancestor fallback and all seven schema
formats, including an exact high-bit `R32Uint` value and paired sparse-biome
normalization. Shader compilation alone is therefore not the acceptance gate.

`MapPlacement` is the runtime-neutral reference contract at the next boundary.
Its versioned palette keeps voxel and mesh resource references opaque, filters
them by biome mixture, Phase 8 rock/vegetation potential maps, legacy placement
potential, elevation, slope, moisture, soil, substrate and river distance, then
generates stable jittered-lattice candidates with deterministic cross-tile
spacing. `SparseVoxelTerrainPlacement` keys those candidates by world-data page,
terrain page, seed, palette signature and terrain-output revision, applies
opaque footprint feedback, and emits ordinary sparse-voxel or virtual-geometry
streamed-instance requests. It consumes ancestor pages when detail is absent
and enforces explicit work/output budgets. It deliberately does not load assets,
query runtime collision, project onto voxel detail or spawn/render instances;
those remain responsibilities of the runtime placement and asset-streaming
systems.

---

## 7. Module layout

```
src/runtime/procedural/world/
  WorldBrief.hpp            brief, tags, archetypes
  MapGraph.hpp/.cpp         hierarchical graph, nodes, boundaries
  MapPartition.hpp/.cpp     Poisson -> Delaunay -> Voronoi -> DCEL
  Raisers.hpp/.cpp          archetype land masks
  MapRivers.hpp/.cpp        network + discharge
  MapRegions.hpp/.cpp       content assignment, naming
  MapSites.hpp/.cpp         settlements + roads
  MapExport.hpp/.cpp        versioned graph + registered world-data fields
  MapExportSampler.hpp/.cpp renderer-neutral semantic page sampling
  MapExportStreaming.hpp/.cpp visible-page residency transactions
  MapExportPageCache.hpp/.cpp async I/O, physical slots and publication
  MapExportGpuCache.hpp/.cpp RHI pool and indirection-table publication
  land/
    Dendry.hpp/.cpp         clean-room implementation
    CellGenerators.hpp/.cpp per-type generators
    LandBake.hpp/.cpp       node -> heightmap + material
    Blend.hpp/.cpp          shared weighted blending

samples/worldGenerator/
  WorldGeneratorSample.cpp
  WorldGeneratorViews.cpp/.hpp
  WorldGeneratorSample.cmake

tests/procedural/
  WorldMapTests.cpp         determinism, topology, archetype agreement
  WorldMapGpuTests.cpp      live streamed-page shader/readback parity
  DendryTests.cpp           statistical properties, locality
```

The corresponding reusable shader files are
`assets/shaders/procedural/wgsl/map_export_addressing.wgsl` and
`map_export_pages.wgsl`.

Build into `build-release/`. There is a stale Debug tree in `build/` that looks
identical and is not.

---

## 8. Verification

Gates, not reports.

| gate | check |
|---|---|
| **Determinism** | same brief → identical graph hash, 3 runs |
| **Locality** | a node's output is unchanged by refining a sibling |
| **Purity** | same node generated twice, any thread order → identical |
| **Boundary continuity** | no elevation discontinuity across a shared edge above tolerance |
| **Archetype agreement** | measured land fraction matches the requested tag |
| **Topology** | every river reaches sea or lake; no cell both land and water |
| **Drainage consistency** | height decreases monotonically along every river |
| **Coverage** | no content type dominates; every landmass has a settlement |
| **Dendry statistics** | branching angle and spacing distributions match the paper |

Determinism must be re-checked after **every** generator change. On the current
system it was verified once, assumed for three phases, and had broken by the time
anyone looked — 17% of cells differing, up to 1856 m.

And look at it in the window. Every baseline number on the current system came
from headless runs, and twice a change was reported working while the app
rendered nothing.

---

## 9. Phasing and status

**Status as of 2026-08-18. Phases 1, 2 and 3 are complete.**

### Done

| item | where | verified |
|---|---|---|
| `WorldBrief`, tags, `CellType` | `src/runtime/procedural/world/WorldBrief.{hpp,cpp}` | tag resolution, hash order-independence, hash sensitivity |
| `MapGraph`, `MapNode`, `NodeIntent`, `BoundaryConditions` | `src/runtime/procedural/world/MapGraph.{hpp,cpp}` | structural validation, hierarchy, picking, signature determinism |
| `buildPartition` — Poisson → Delaunay → Voronoi dual | `src/runtime/procedural/world/MapPartition.{hpp,cpp}` | builds, determinism, seed sensitivity, bad-settings rejection, sites inside extent |
| `raiseLand` — 8 archetypes, coastal/shallow derivation, `measuredShape` classifier | `src/runtime/procedural/world/Raisers.{hpp,cpp}` | land fraction exact for 7 archetypes × 3 fractions, graph stays valid, coastal flags derived, determinism, measured shape matches request, bad-settings rejection |
| `clipCellsToExtent`, `extractCoastline` | `src/runtime/procedural/world/MapCoast.{hpp,cpp}` | every cell bounded, interior cells unchanged, clipping idempotent, coastline follows the land/water boundary, no coast without water |
| Test suite | `tests/procedural/WorldMapTests.cpp` | **21 groups, all passing** |

`ctest -R "Planet\|SphericalTerrain\|PolygonTerrain\|VoxelMaterial\|WorldMap"`:
**21/21 passing**, so nothing in the shared procedural code regressed when the
new sources joined the `Engine` target.

Further decisions made while implementing the raisers:

- **Land fraction is applied by ranking, not by thresholding the field.** Cells
  are scored by the archetype's scalar field, sorted, and cut at the requested
  fraction. This is what makes archetype (*shape*) and land fraction (*amount*)
  independent — a fixed threshold drifts with the archetype, the noise and the
  cell count, so "archipelago at 60% land" would otherwise be unachievable.
  Ties break on index so the sort is stable rather than implementation-defined.
- **Noise warps the archetype field, it does not add to it.** Adding lets noise
  invent land in open sea; warping only perturbs where the coast falls. This is
  §5.3's rule enforced at the one place it would otherwise be violated first.
- **The hash is integer-only.** Value noise is built on a 64-bit integer mix
  rather than anything using square roots, so it is bit-identical across
  targets — the same reason `RandomThrows` was chosen over `FillVoids`.
- **`coastal` and `shallow` are derived, never authored**, from neighbour land
  state after raising.
- **`measuredShape()` runs the reference's classifier in reverse** as a gate:
  generate, measure the border land fraction, and confirm it matches the
  requested archetype.

Decisions made while implementing, worth knowing:

- **Tag conflicts resolve, they do not fail.** A brief with two shape tags, or
  none, still generates — deterministically. The UI lets both be ticked, and a
  generator that refuses to run is worse than one that picks canonically.
- **Brief hashing is order- and duplicate-independent**, so the same world does
  not regenerate differently depending on the order tags were clicked.
  Negative zero is normalized for the same reason.
- **`validate()` enforces the invariants that fail silently**: symmetric
  neighbour links (asymmetry makes blending and boundary contracts apply to one
  side only), land flag agreeing with cell type, parent/child back-links, and
  level following the parent.
- **`nodeAt()` descends the hierarchy** rather than scanning, so picking stays
  cheap as refinement deepens and the inspector selects the deepest refined node
  rather than the root.

The partition uses `PoissonDiskSampler` with `Method::RandomThrows`, chosen
because it is **bit-identical across compiler settings** (that file records
verification at -O0, -O2 and -O2 -ffp-contract=off), whereas `FillVoids`
constructs sample positions from circle intersections and square roots and is
not reproducible across targets — 454/448/425 samples under those same three
settings. Determinism is a gate here, so the method choice is load-bearing.

### Border clipping — CLOSED

The unbounded-hull-cell gap recorded at the partition step is fixed.
`clipCellsToExtent` runs Sutherland-Hodgman against the four world half-planes,
which is exact rather than approximate because Voronoi cells are convex.

**One real bug, found by the idempotence test rather than by inspection.**
Clipping was not idempotent: `intersect` computed the boundary crossing as
`a + (b - a) * t`, which is algebraically exactly on the boundary but in
floating point lands a rounding step to either side. A vertex a hair *outside*
was re-clipped on the next pass, so the world changed slightly every time
clipping ran. Fixed by snapping the clipped coordinate exactly onto the
boundary value.

Worth noting the shape of this: the failure was invisible geometrically — the
polygons looked right — and would have surfaced much later as a world that
drifts on reload. The test that caught it exists only because idempotence was
written down as a property before the code was.

### The ImGui application — DONE. Phase 1 is closed.

`samples/worldGenerator/` builds and runs. Brief controls, worker-thread
generation with main-thread polling, blended map view, coastline overlay, node
inspector, and a headless PNG path.

| item | where | verified |
|---|---|---|
| `SiteIndex`, `blendAt`, `transitionSharpness` | `src/runtime/procedural/world/land/Blend.{hpp,cpp}` | weights sum to one, pure at a site, continuous across boundaries, sharpness monotone, table symmetric, determinism, bad-settings rejection |
| App, views, headless render | `samples/worldGenerator/` | window runs at 3k and 19k cells with no Vulkan validation errors; every archetype renders and passes the family gate |
| Test suite | `tests/procedural/WorldMapTests.cpp` | **28 groups, all passing** |

`ctest -R "Planet|SphericalTerrain|PolygonTerrain|VoxelMaterial|WorldMap"`:
**21/21 passing.**

**The gate is met.** Island, archipelago, coast, bay, fjord and headland each
produce a recognisable map from the tag alone.

Decisions made while implementing the app:

- **Blending is a two-pass weighting, and the exponent is a symmetric bilinear
  form over the contributing weights.** Modified Shepard with the distance to
  the first *excluded* site as the support radius, so a contributor fades to
  zero as it leaves the neighbour set rather than stepping out. The
  per-transition sharpness the spec asks for cannot be taken from the nearest
  cell's type — that flips at the midline between two cells and the colour
  jumps there. Taking it as `Σ wᵢ wⱼ sharpness(tᵢ, tⱼ)` over a first neutral
  pass is continuous, which is why `transitionSharpness` is symmetric by
  construction and tested for it.
- **The map is ImGui draw list geometry, not a texture.** Each cell is a fan
  from its site with a vertex at every edge midpoint, coloured by the blend at
  its own position; neighbours share those vertices, so the fill is continuous
  across a boundary with no per-pixel work. It also means the sample owns no
  pipeline and no SPIR-V, so it cannot hit the shader-path trap that catches
  binaries living outside `tests/`.
- **The headless path is the reference.** `renderMapImage` evaluates the same
  field exactly, per pixel, and writes a PNG through miniz. The mesh is a
  piecewise-linear sampling of it. This is what makes the phase gate judgeable
  without a display.
- **Shore and shelf shading is a view decision, switchable.** `coastal` and
  `shallow` are derived flags rather than cell types, and until the elevation
  and region stages give land more than one type they are the only variation the
  blend has. Without them the map is two flat colours and there is nothing to
  judge the blending by.
- **The app's `coastComplexity` default is 0.25, not the `RaiseSettings` 0.5.**
  At 0.5 the warp displaces the coast by roughly 15% of the world extent, which
  turns the island archetype's disc into an unrecognisable blob.

### Phase 2 — elevation, uplift and rivers. DONE.

| item | where | verified |
|---|---|---|
| `classifyWaterBodies`, `coastDistances`, `cellArea`, `assignElevation` | `src/runtime/procedural/world/MapElevation.{hpp,cpp}` | land above the datum and water below, mountains higher and further inland than the rest, no ranges means no mountains, character tags modulate relief, determinism, bad settings rejected, lakes split from ocean and idempotent |
| `buildRivers`, `RiverNetwork` | `src/runtime/procedural/world/MapRivers.{hpp,cpp}` | every land cell descends strictly to water, no cycles, discharge monotone downstream, exact mass balance, Strahler rule, bifurcation ratio, enclosed-basin mouths, determinism, threshold changes drawing not physics, bad settings rejected |
| Elevation / uplift layers, river overlay, terrain controls | `samples/worldGenerator/` | window runs; every archetype renders with drainage |
| Test suite | `tests/procedural/WorldMapTests.cpp` | **46 groups, all passing** |

**The gate is met.** Rivers descend, reach water, and branch: a coast at seed 4
produces 348 channel cells in 25 catchments, largest 72 cumecs, Strahler order 4.

Design decisions:

- **The range spine follows the landmass's long axis, laid out explicitly.**
  Taking the maxima of the distance-to-coast field seemed natural and is wrong:
  a convex island has exactly *one* such maximum, so the first version produced
  a single central cone with perfectly radial drainage — a volcano, not a
  continent. The spine is now the least-cost path between the two furthest
  interior points (a double sweep), with a branch range per 400 cells grown from
  whatever interior point is currently furthest from the range. Both are facts
  about the shape of the land, so the layout still decides where the mountains
  are.
- **Depression filling makes the gate a property of the algorithm.**
  Priority-flood with an epsilon, then steepest descent on the filled surface.
  After filling, every land cell has a strictly lower neighbour, so a course
  cannot cycle and cannot dead-end inland. "Rivers reach water" is then
  structural rather than something to check for and patch up.
- **Landform type reads the range's *share* of the local height, not the
  height.** A tilted plain that happens to reach 3,000 m is a plain. This is
  also what makes `rangeStrength = 0` produce no mountains at all, which is the
  test that keeps the classification honest.
- **Discharge is in cumecs, from drainage area times a runoff depth.** The
  channel threshold decides what is *drawn*; it must not move what is computed,
  and there is a test for that.
- **River width is drawn from the square root of discharge**, which is what
  hydraulic geometry gives. A linear scale makes every trunk a slab and every
  headwater invisible.

### Phase 3 — regions, content, names, readable render. DONE.

| item | where | verified |
|---|---|---|
| `buildRegions`, `RegionMap`, `placeName`, `namedPlace`, `namedRiver` | `src/runtime/procedural/world/MapRegions.{hpp,cpp}` | regions cover and partition the land on island *and* archipelago, every region connected, mean area tracks the target, ridge resistance measurably keeps regions off the ranges, tags bias content without flattening it, names stable and 90%+ distinct, named river stems run mouth to source without overlap, determinism, bad settings rejected |
| Content / landform / region layers, labels, region edges | `samples/worldGenerator/` | window runs; every archetype renders a labelled map |
| Test suite | `tests/procedural/WorldMapTests.cpp` | **54 groups, all passing** |

**The gate is met.** A verdant island and a barren archipelago both render as
readable labelled maps with named regions and rivers.

Design decisions:

- **`content` is a separate field from `type`.** Landform is what the ground
  does; content is what is on it. The first attempt scored them in one
  competition and mountain and hill won everywhere — their scores rise with
  relief and uplift, which on any world with ranges dwarfs anything a vegetation
  term can say, and the arid tag produced *zero* desert. Now covers compete only
  with each other and with a "bare" option, and a region that draws bare keeps
  the landform its cells measured.
- **Be straight about what chooses content.** With no climate model there is no
  physical basis for forest against desert. The tags are the signal, terrain
  suitability is a set of soft gates that stop an implausible cover being drawn
  at all, and the jitter distributes what is left. In a neutral brief the cover
  of any one region really is a draw.
- **Region boundaries are watersheds, so the cost is on climbing.** Charging for
  *occupying* high ground — the obvious first reading of "ranges resist" — does
  nothing: it delays when the front reaches a range but never stops a region
  spanning one, and it measured slightly *worse* than no resistance at all.
  Charging for elevation gain makes a region fill its basin and stall at the
  crest, which is what a watershed is.
- **Labels need collision rejection or the render is worthless.** The first
  labelled map put ~80 names on one island and was a wall of text. Fewer, larger
  regions (2,500 km²), rivers named only at 15% of the largest discharge and
  order ≥ 2, and greedy rejection by importance.
- **The headless render carries its own 5×7 bitmap font.** ImGui 1.92's atlas is
  not reachable from a process with no context, and a map with no place names
  cannot be judged as a map.

### One more defect the picture found

**Farthest-point seeding leaves whole islands unseeded.** When the seed budget
runs out before a small island is picked, that island gets no seed, no region
and no name — visible immediately as two unlabelled islands in an archipelago
render. Every connected landmass now gets at least one seed. The coverage test
ran only on `Tag::Island` and passed throughout; extended to the archipelago it
fails without the fix, which was checked by reverting it.

### Two measurements worth not repeating

**Surface roughness is not a drainage dial.** Sweeping `detailReliefFraction`
from 0.04 to 0.40 across three wavelengths on a 3,003 cell world moved the river
mouth count by under 10% and the Strahler order not at all. What it did do was
manufacture depressions: 0 filled pits at 0.04, 42 at 0.40. The noise is kept
small and honest; it is not what makes rivers branch.

**Maximum Strahler order is a property of the sampling, not of the network.**
Same island, three cell sizes: 5 km reaches order 3, 2.5 km order 4, 1.5 km
order 4, and lowering the channel threshold raises it at every size. So the
branching test asserts **Horton's bifurcation ratio** instead, which is
scale-invariant and came out at 2.1 and 3.6 at every resolution measured.
Asserting "order ≥ N" would be asserting the resolution.

### One real defect, found by the map view and invisible to the tests

**The bay archetype did not make a bay.** It centred its bite at a fixed
position rather than on the coastline, so at any land fraction the bite landed
inland and produced a detached inland sea. `archetypeField` now takes
`landFraction`, because where the coastline ends up is the one thing the ranking
cannot tell it, and for the monotone coastal fields that is `ny = landFraction`.

Worth noting how it survived: `measuredShape` reads the *border* land fraction,
and an inland sea and a real bay give the same border. The archetype-agreement
test passed throughout. `testBayOpensOntoTheSea` now floods water from the
border and asserts the enclosed fraction is under 10% at three land fractions,
with the lake archetype as the contrast that keeps the check from being vacuous.

**`measuredShape` names only island, coast, lake and landlocked.** Comparing it
directly against the requested tag reports a mismatch for archipelago, bay,
fjord and headland while they are all working. The app compares families
(`expectedMeasuredShape`); anything else reading that classifier should too.

### Remaining phases

| phase | deliverable | gate |
|---|---|---|
| 1 | ~~App skeleton, brief controls, partition + raisers + coast, map view~~ **DONE** | ~~recognisable island/archipelago/coast from tags~~ **met** |
| 2 | ~~Elevation + uplift intent, rivers with discharge~~ **DONE** | ~~rivers descend, reach water, branch believably~~ **met** |
| 3 | ~~Regions, content types, names, blended map render~~ **DONE** | ~~a readable fantasy-map output~~ **met** |
| 4 | Dendry (clean-room) + heightmap view | hydrologically consistent relief under the map |
| 5 | Per-cell-type generators + blending | each type reads distinctly; no seams at boundaries |
| 6 | Hierarchy: node selection, refinement, boundary contracts | refining a node never disturbs a sibling |
| 7 | Versioned map-data export; reserved fields may be unavailable | deterministic bundle round-trips and validates registration |
| 8 | Sites, roads, material/landform layers | reserved layers become available without a schema migration |
| runtime | Density rules, asset palettes and voxel/mesh prop spawning | separate consumer project, not part of this build spec |

Phases 1–3 are a working map generator with no terrain at all, and are judgeable
entirely by eye. Phase 4 is where Dendry has to prove itself; if its cost or
quality disappoints, that is known before anything depends on it.

---

## 10. Open questions

1. **Level spacing.** ~50 km / ~5 km / ~500 m / ~50 m is proposed. Ratios matter
   more than absolutes: too coarse and refinement invents everything, too fine
   and the hierarchy has more levels than anything uses. **Settle before phase 1
   — it sets every downstream resolution.**
2. **Cell count at L1.** Measured on the current planar generator: 20k sites in
   4.1 s, 81k in 20.3 s, roughly O(n^1.16). That curve bounds what is
   interactive.
3. **Hex mode.** The reference supports hexes alongside Voronoi. Relevant only if
   the game wants tile-based play.
4. **Editability.** If maps are hand-editable after generation, the graph must be
   serializable and re-runnable from an edited state — which requires the land
   generator to be a pure function of the map, as specified. Cheap if designed
   in now, expensive later.
5. **Fine soil calibration.** Country-scale transport now closes mass balance
   and emits parent soil layers. Metre-scale regional transport still needs
   calibration before it authors detailed stratified beds.
