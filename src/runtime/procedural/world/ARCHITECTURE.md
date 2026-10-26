# World generation: architecture

How a world is built, what the pieces are, and where to add a new one.

The generator is a **hierarchy of small classes**. Each one takes a region of
the world, does one thing to it, and hands what it produced to the classes
below. There is no stage that knows about the whole world, and no class that
can reach outside the region it was given.

---

## 1. The three structures

These are separate on purpose, and conflating them is the mistake this
architecture exists to avoid.

| Structure | What it says | Where it lives |
|---|---|---|
| **Containment** | world → cell → settlement → district → block → lot → building → floor | `Region::parent` |
| **Adjacency** | which regions share an edge | `Region::portals` |
| **Dependency** | which products a generator reads | the generator's declared role and scope |

A road depends on many regions and is contained by none of them. A merged
parcel has several sources and one parent. Land use, political owner and biome
are independent labels over the same region: turning farmland into town does
not change its kingdom.

---

## 2. The layers

```
  procedural/geom2        polygons, booleans, offsetting, partition, sampling
  procedural/geom3        prisms, extrusion, triangulation
        │
        ↓  every generator below builds on these and never re-implements them
        │
  world/stages            the world map: 13 small stage classes
        │  GeneratedWorld: cells, elevation, rivers, climate, regions, contours
        ↓
  world/hierarchy         Region, Edge, the three generator interfaces,
        │                 the registry, the policy, the composer
        ↓
  world/*.{hpp,cpp}       the settlement, city, rural and runtime kernels
```

### 2.1 `geom2` and `geom3` — the geometry library

Everything geometric goes through here, so that there is one tolerance policy
and one definition of "valid polygon" rather than twelve.

| Unit | What it gives |
|---|---|
| `Quantise` | the fixed micrometre grid every coordinate is snapped to |
| `Polygon` | rings with holes, exact predicates, `polygonFault` as *the* validity definition |
| `Arrangement` | the planar sweep: segments in, a half-edge graph and its faces out |
| `Boolean` | union, intersection, difference, xor, and arbitrary winding predicates |
| `Offset` | grow, shrink, ribbons from centrelines, and cutting into N pieces |
| `Partition` | Voronoi, power diagrams, Lloyd relaxation |
| `Sample` | blue-noise sampling of a region, and thinning a candidate set |
| `geom3::Prism` | a footprint plus a height: extrusion, walls, ear-clipped caps |

Two rules hold throughout:

- **Concave is the baseline.** No type is called `ConvexRing`, and nothing
  rejects a shape for being concave. Convexity is *measured* and used only to
  pick a faster path. The shapes this generator produces — river corridors,
  road-graph faces, wall outlines, coastlines — are concave.
- **Exactness fixes decisions, not constructions.** Every sign test goes
  through the adaptive predicates on integer coordinates. Anything that
  *builds* a new coordinate (an offset, a circle) is plain double arithmetic,
  and the grid is what makes the result reproducible.

### 2.2 `world/stages` — the world map

Thirteen classes, run in order, each advancing one `GeneratedWorld`. See §3.

### 2.3 `world/hierarchy` — the generator framework

`Region`, `Edge`, three interfaces, a registry, a policy and a composer. See §4.

---

## 3. The world map stages

One class per stage in [`stages/WorldStage.hpp`](stages/WorldStage.hpp). Each
declares a stable `name()`, a display `phase()`, a `progress()` and a `run()`.

| # | Class | Owns |
|---|---|---|
| 0 | `PartitionStage` | A ladder of Voronoi partitions, coarsest first, each cut inside the one above. The finest is what every later stage measures. |
| 1 | `LandmassStage` | Which cells are land. Grown over the coarse partition, or cut from an analytic field when the brief names an archetype. |
| 2 | `CoastStage` | Clips cells to the world extent and traces the shoreline. |
| 3 | `ElevationStage` | Elevation and uplift, then a *probe* drainage run on a copy so plains and floodplains can be conditioned before the real rivers. |
| 4 | `FluvialStage` | Stream-power erosion against the uplift the elevation stage authored. Opt-in. |
| 5 | `PlateauStage` | Plateau blocks, and the site index. Before drainage: flattening afterwards makes rivers run uphill across the block. |
| 6 | `BaseClimateStage` | Climate before rivers, because precipitation drives them. |
| 7 | `RiverStage` | The definitive drainage pass, lake filling, and a re-traced coast. |
| 8 | `StylizeStage` | Ridges and a monotone relief remap. After drainage, so it cannot break it. |
| 9 | `SurfaceStage` | The continuous elevation field and its contours. Published after the last scalar mutation, so every consumer sees one surface. |
| 10 | `FinalClimateStage` | A second climate pass: riparian strips and lake shores the first pass could not know about. |
| 11 | `RegionStage` | Named regions over the finished map. |
| 12 | `SummaryStage` | Summarises leaves back up the ladder and measures the result against the brief. |

The ordering constraints above are not stylistic. Each of the "before/after"
notes is a property that breaks if the order changes.

**Adding a stage**: write the class, add one line to `worldMapStages()`. The
stage-name list is derived from the classes, so a name and an implementation
cannot drift apart.

---

## 3b. The settlement fabric stages

`composeSettlementFabric` builds a settlement on an accepted territory. It is
being split the same way as the world map, but it could not be split in one
move: unlike the map stages, its phases share twenty-five locals and seven
lambdas that capture them by reference.

So it is being split **from the tail forward**. The last phases touch nothing
the earlier ones left on the stack, so they moved out first; each phase that
leaves takes its shared state with it into `FabricBuild`, which shrinks the
remainder and makes the next phase separable in turn.

| Class | Owns |
|---|---|
| `PopulationStage` | Fits residents to the layout's plots: households, tenancy, employment, services, with explicit terrain and water eligibility. Then decides jettying per house, which needs the street width in front of it and what the facade opposite already projects. |
| `CompoundStage` | Every accepted non-housing district except the plaza gets a castle, manor, church or civic hall, planned inside the layout's compound inset. Each inhabited wing is a dwelling brief, so rooms and stairs come from the same rules as a house. |
| `NeighbourhoodStage` | Measures each district from what the director actually placed: houses, residents, workers, shops, jettied facades. Measured, not authored. |
| `InstanceStage` | Meshes city massing on the terrace and farm buildings on the real ground — different surfaces on purpose, because the terrace is what the streets were levelled onto and the fields were not. |

`FabricBuild` is what a stage may touch, stated explicitly rather than
inherited from an enclosing scope. Two of its fields were **found by moving the
phases, not by reading them**:

- `acceptedJetty` — written by the population stage, read by the neighbourhood
  ledger two stages later. On one enormous stack that coupling was invisible.
- the terraced surface versus the untreated ground, which the instance stage
  needs kept apart.

A stage fails only on a broken invariant. Ordinary shortfalls — a district with
no room for its compound, a programme the population could not fill — are
recorded on `fabric.rejections` and the settlement still stands.

Status: the tail is extracted; `composeSettlementFabric` is down from 2,054
lines to 1,722, and the phases above (urban polygon, gate routes, walls,
terrace, arteries, layout) are next.

---

## 3c. The city layout: two housing strategies

`generateCityLayout` turns a settlement's land into streets, wards, walls and
buildings. About half of it was one `if (s.blockHousing)` branch of 2,146 lines
against a 332-line else — not two cases of one algorithm, but two complete and
independent ways of filling a street network, chosen by a boolean.

| Class | Fills blocks by |
|---|---|
| `BlockHousingStrategy` | Extracting the faces of the road arrangement as blocks, keeping their interiors as shared courts, cutting lots against the frontage and fitting a regular shell in each. What a town or city uses, where the ground is worth subdividing carefully. |
| `FrontageHousingStrategy` | Walking each street by arc length, placing the widest shell the remaining frontage allows and recording an explicit gap where none fits. What a village uses, where buildings follow the road rather than the road enclosing blocks. |

`LayoutBuild` states what a strategy may touch — the layout, ward cells, road
ribbons, wall masks, usable land, reservations, wall ring, gates, bounds, and
the terrain and randomness callbacks. Bound by reference at the branch point:
the preamble builds these on its own stack, and naming them is what turns
"everything in scope" into a contract.

`citylayout/LayoutGeometry.hpp` holds the fourteen planar predicates the layout
is written in — `cross`, `area`, `clip`, `inside`, `intersection`, `subtract`,
`hull`, `merge`, `convex`, `dist`, `mix`, `ribbon`, `segmentDistance`,
`segmentsMeet`. They were file-local, which meant a strategy could not leave
the file without taking a copy of them.

**They are moved verbatim, not re-expressed through `geom2`.** `geom2` has its
own tolerances and its own definition of validity; swapping them in would
change which lots exist, and that belongs in its own change with its own
evidence. The same rule as everywhere else here: a move is a move.

`WorldCityLayout.cpp` is 2,388 lines, down from 4,950.

---

## 3c'. The market square

`generator/PlazaPlanner` plans what stands on the square the layout reserved
(`CityLayout::market`): a gate where each street arrives, an arcade of piers
and round arches between the gates, a dais of concentric steps at the centre
with a fountain, cross or well on it, and - when the square has the depth -
rows of houses behind the arcade on every side (shop-houses with an inn, a
pub and a bakehouse), for which the arcade is the covered walk; a smaller
square keeps a parapet under every arch and the shambles and market hall
instead. The lots are `CityPlazaLot`s with a block lot role name. `composeCitySkeleton` runs it
with the entries taken from the streets that end on the square, publishes the
plan as `CitySkeleton::plazas`, the square as a Court public space and the
stalls and hall as lots of the square's region; `buildCityModel` meshes the
plan through `meshPlaza`. A plaza block in `CityBlockPlanner` delegates to the
same planner, so the standalone `--block --plaza 1` demo and the city agree.

## 3d. The rural hinterland: two ways to be a farm

`growRuralPatches` had the same shape as the city layout: one
`if (regions.empty())` against its `else`, two different answers to "what is a
farm", chosen by whether the caller supplied the world's own cells.

| Class | Decides a farm is |
|---|---|
| `GridGrowthStrategy` | A bounded number of broad cores seeded on the analysis grid and grown outward in competition. Farms come out roughly even and separated by the configured spacing — a pattern, which is what you want when there is no map to follow. |
| `RegionGrowthStrategy` | One supplied world cell. Whole cells form adjoining countryside with no artificial spacing; the grid only audits terrain and supplies access. This keeps a holding's boundary on the world's own cell edge, so a field and the map agree about where it ends. |

`GrowthContext` names the three per-cell verdicts decided before either runs —
which cells may be farmed, which source each belongs to, what each costs to
reach. Both strategies need them and neither should decide them.

`rural/RuralGeometry.hpp` holds `cross`, `area`, `cellPoly` and `outline`, for
the same reason the city layout needed its own header: a strategy cannot leave
a file while its geometry stays behind.

---

## 4. The generator framework

### 4.1 `Region` — the one geometric hand-off

```cpp
struct Region
{
  RegionId id, parent;
  Frame frame;              // world-frame origin of its local coordinates
  Polygon boundary;         // outer ring plus holes, in the local frame
  Cover cover;              // open, forest, rock, water, urban, road, building
  Attributes attributes;    // elevation, slope, flood, fertility, defence, demand
  std::vector<Portal> portals;  // shared edges to siblings: the adjacency graph
  std::string producer;     // which generator made it
  std::vector<RegionId> sources;  // lineage, which is not containment
  bool convex;              // measured, never required
};
```

A parent emits regions that exactly tile it. A child receives one and must not
read outside it. That is what makes a region the unit of parallelism.

Every region works near its own origin because children keep absolute
tolerances: a lot is planned to the centimetre whatever continent it is on.

### 4.2 `Edge` — roles are a set, width makes a region

```cpp
struct Edge
{
  EdgeId id;
  std::vector<Point> path;
  std::vector<EdgeRoleEntry> roles;  // a set, not an enum
  std::vector<Band> bands;           // empty: a zero-width cut
};
```

**Roles are a set** because the same curve can be a river bank *and* a parish
border *and* a farm-lane boundary. A role carries the producer that claimed it,
so revoking the farm producer's lane leaves the parish border standing on the
same curve.

**Width makes the ribbon a region.** A zero-width edge splits a region in two.
A widthed one splits it in three — side A, the ribbon, side B — and the ribbon
is a region with a cover and attributes, because that is what lets anything
stand on it, collide with it, or answer "am I on a road". Generalised to a
stack of bands:

```
  road:   Ditch | Shoulder | Road      | Shoulder | Ditch
  river:    –   | RiverBank| RiverBed  | RiverBank|   –
```

### 4.3 The three interfaces

```cpp
class EdgeGenerator   { bool propose (const Request&, std::vector<Edge>&,   Ledger&, std::string*); };
class RegionPopulator { bool populate(const Request&, std::vector<Object>&, Ledger&, std::string*); };
class FieldGenerator  { bool evaluate(const RegionTree&, const Context&, std::vector<Region*>&, Ledger&, std::string*); };
```

**Why three and not four.** An earlier draft separated *partition* from
*network*. That was wrong: a partition and a network produce the same thing —
edges — and the child regions are what you get by extracting faces afterwards.
A chord from a lot splitter and a segment from a road L-system are one
operation at different scopes. The old code proved it by extracting faces from
a road graph and then forgetting that the face boundary *is* the road.

**Why not one.** Placing a house in a lot does not subdivide the lot, and
measuring slope changes no geometry at all. Forcing those through an
edge-producing interface is what made the previous `Generator<Input, Output>`
get bypassed.

`Scope` says how far a generator may look: `Region` (the parallel grain),
`Neighbourhood` (across its portals), `Network` (roads, rivers), `World`.

### 4.4 `Registry` and `Policy` — selection by identity

Several generators bind to one role; a policy picks between them **per region**.

```cpp
registry.bind(std::make_shared<StripLots>());
registry.bind(std::make_shared<RingLots>());

plan.policy.defaults[Role::Lots] = "lots.strip";
plan.policy.rules.push_back({Role::Lots, "lots.ring",
    [](const Region &r) { return r.attributes.demand > 500; },
    "high demand ward"});
```

This replaces selection by boolean. The settlement generator historically
decided which lot generator ran from nineteen booleans across two settings
structs, several documented by precedence — a dispatch table written in prose.
Adding a generator is now a binding, never a new flag with a new precedence
rule, and the choice *and its reason* go in the ledger so a render can be
explained.

Per region, not per settlement: that is what lets a merchant ward be gridded
while the quarter beside it stays organic.

### 4.5 `Composer` — the director

Runs a `Plan` of `Step`s from a root region downward. For each region, it asks
the policy which generator owns that role, runs it, turns edges into children,
and queues the children for the next step.

**The work list grows as regions appear.** That is the thing the older typed
director could not do: it takes a fixed task list and infers dependencies
before execution, so "one task per region, N discovered while running" is
inexpressible — which is why every parent in the older code loops over its
children with static `::plan()` calls instead of registering them.

Where two widthed edges cross, the junction goes to the **first** ribbon that
claims it, so the children stay an exact tiling of their parent. (A junction as
its own region is the natural refinement; claiming it twice is simply wrong,
and the area-conservation test is what says so.)

### 4.6 `Ledger` — why something is missing

Typed entries — `Rejected`, `Omitted`, `Capacity`, `Repaired` — each with the
region id. This replaces free-form `std::vector<std::string> rejections`, so a
review or a render can group them and answer "why is this block empty".

---

## 5. The generators

### 5.1 Streets — `Role::Streets`

| Class | Shape |
|---|---|
| `GridStreets` | A road every *spacing* along two perpendicular axes, each widthed, so the roads come back as regions. |
| `RadialStreets` | Spokes from the centre plus a ring road partway out. |

### 5.2 Lots — `Role::Lots`

| Class | Shape |
|---|---|
| `StripLots` | Strips cut perpendicular to the block's longest axis. Zero-width cuts: a lot boundary divides without consuming ground. |
| `RingLots` | The block copied inward by the lot depth, the band between cut radially, and the copy kept as a common court — a region like any other, so something can be put in it. |

### 5.3 Buildings — `Role::Buildings`

`HouseFootprints` (a `RegionPopulator`): insets the lot by its setback and
places a house, skipping roads, water and lots too small to hold one.

### 5.4 Terrain — `Role::Terrain`

`SlopeField` (a `FieldGenerator`): samples the terrain callback at the region's
ring vertices and writes elevation, slope and buildability. **Touches no
geometry** — which is the distinction this interface exists to make.

---

## 6. Determinism

The project's core discipline, and the reason for several decisions above.

- **Randomness is purpose-addressed.** `Context::unitRandom` derives from the
  world seed, the generator id, the region id and a purpose — never from
  traversal order. Shuffling the work list cannot change the result.
- **Geometry is decided on an integer grid.** Snap once on entry; equality is
  then equality and welding needs no tolerance.
- **The gates are runnable.** Every geometry and hierarchy suite is checked
  byte-identical at `-O0`, `-O2`, `-O2 -ffp-contract=off` and
  `-O3 -march=native`; the arrangement is checked identical under permuted
  input order.

---

## 7. Adding things

**A new lot grammar.** Write a class implementing `EdgeGenerator` with
`Role::Lots`, bind it, and name it in a policy rule. Nothing else changes; no
existing file is edited.

**A new kind of thing placed in a region.** Implement `RegionPopulator`.

**A new measured property.** Implement `FieldGenerator`. Do not change
geometry from one; that is what the other two interfaces are for.

**A new world map stage.** Write a class in `stages/`, add one line to
`worldMapStages()`.

**A new housing grammar.** Implement `HousingStrategy` in `citylayout/` and
bind it. Nothing else changes.

**A new geometric operation.** Add it to `geom2`, not to the caller. If you are
about to write a `cross`, an `area` or a point-in-polygon test, it already
exists — that duplication is the problem the library was built to end.

**Never re-express a moved kernel while moving it.** Rewriting `appendTriangle`
during an extraction silently dropped a normal and crashed a consumer three
stages downstream. Move it verbatim; change it in its own commit.

---

## 8. What is not migrated yet

Honest status. The framework and the world map are done; the settlement chain
is not.

Where the big functions stand, measured:

| File | Was | Now |
|---|---|---|
| `WorldFinalPatches.cpp` | 420 | **66** — fully decomposed (§3a note) |
| `WorldCityLayout.cpp` | 4,950 | **1,684** — housing strategies, outer wards and ground publishing are out; what remains is wall faceting, ward planning, street derivation and the circulation graph |
| `WorldRuralPatches.cpp` | 1,223 | **781** — growth strategies are out; field and track emission remain |
| `WorldSettlementFabricAdapter.cpp` | 2,470 | **2,148** — the tail is four stages; the ~1,700-line head is still to move |
| `WorldSettlementSiteAdapter.cpp` | 2,553 | **2,553** — untouched |
| `WorldSettlementDirector.cpp` | 940 | **940** — untouched; `finish` is ~770 of it |

Still to do, in the order that pays best:

- `selectWorldSettlementSite` (~1,950 lines in one function).
- The settlement fabric head: urban polygon, gate routes, walls, terrace,
  arteries, layout retry ladder.
- The city layout's remaining preamble.
- `WorldSettlementDirector::finish`.
- Eleven places still **reject** concave input with `must be convex CCW`.
- A district is still stored as `pieces` — a convex decomposition — with six
  places carrying defensive "internal seams are NOT streets" logic. A genuinely
  concave district cannot currently be reassembled.
- Three separate arrangement implementations (`arrangeSurfaceContours`,
  `RoadGraph`, `MapEdges`) remain to be retired onto `geom2`.

This work is expected to **change generated output** — retiring
`LotSplitter::convexLots` alone changes the city, because a concave block is
currently cut into more lots than its area warrants purely to satisfy a
downstream convexity requirement. Determinism gates stay absolute; parity gates
are "no unexplained change".

Plan and phase table: `docs/architecture/generator-structure-and-refactor.md`.

### History-aware dwelling styles

`generator/DwellingStylePlanner.hpp` is a native planning pass. A
`CellDwellingBrief` can explicitly choose Vernacular, Roman, or HighFantasy and
supply `architecturalAgeYears`. Historical city lots derive age from the first
construction version. The default age/identity heuristic selects some Roman
revival houses after 40 years and Roman/fantasy legacy styles after 180 years;
these thresholds are art direction, not a claim about real historical periods.

Roman planning reserves an enclosed atrium before room subdivision and partitions
its surrounding mass into four wings before the existing door/stair solvers run.
Small or unsuitable plots omit the atrium. Generous rectangular courts receive a
covered colonnade with capped polygonal columns. Interior ceilings receive timber
coffers, excluding outdoor rooms and stair opening bounds. Low roofs use the
existing roof solver. Weathering affects masonry and column ornament, never
removes inhabited floors or load-bearing walls. Fantasy uses pointed openings,
steeper roofs and age-dependent ornament. Accepted composition contracts and
compound castle wings retain their separately authored structural plans.

The viewer exposes architecture, construction age and weathering beside the room
blueprint. `WorldSettlementPreview --architectural-gallery <output-directory>`
produces a bounded three-building native fixture with authored ages (not a
simulated history). `GothicCastleTests --dwelling-style` runs the focused planning,
column closure/winding, determinism and compact-plot checks.

### World history, exterior styles, and surface ownership

The world summary stage can now publish an immutable `WorldCivilization`,
containing the selected kingdom, one annual story graph and every settlement's
dated records. Set `WorldPipelineSettings.historyYears` to enable it; zero retains
the map-only API. The world generator application and world settlement preview enable 200 years by default.
Settlement selection binds the selected kingdom cell to its historical
households, institutions, building ages and enclosures. Explicit snapshot inputs
remain authoritative. The preview command `--world-history <directory>` exercises
this native path and exports all settlement histories without generating every
building mesh simultaneously.

Roman houses reserve frontage space for optional porticos. The `roman-temple`
programme reserves a tall cella and a four-column pediment. Columns use closed,
fluted shafts, plinths, collars, capitals and a stone architrave. Exterior members
must fit inside the recorded lot. These are Roman-inspired fantasy structures.

Terrain ownership is emitted after successful lot caps and entrance steps have
reserved their footprints. Surface subtraction runs in bounded batches; failed
building plans do not erase their ground. Structural terrace decks also replace
the underlying ground skin. The WebGL viewer uses logarithmic fragment depth to
preserve small surface separation at long distances; this complements geometry
ownership rather than concealing coplanar duplicate geometry.

### Compact dwelling panels

`CityModelSettings::instanceDwellingPanels` enables native compact panels during
city assembly. The solid converter recognizes convex planar quads and matching
adjacent atlas triangles. Accepted panels bypass polygon triangulation, position
welding and per-triangle material/UV/ground-weight allocation. Their four corners,
metric UVs, visibility group and material form one `CityModelPlan::QuadInstance`.
Non-planar warped faces, concave polygons and unmatched triangles keep the normal
mesh path. Grid planning, room boundaries, aperture cuts and historical identities
are unchanged. One material resolver handles both output paths.

Native preview tools enable this path by default; `CITY_FLAT_MESH=1` produces the
expanded comparison export. Other native consumers remain opt-in until they can
consume `quadInstances`. `CityMeshInstances.hpp` streams 88-byte float records to
a separate binary file with a double-precision local origin. Export happens before
moving the model into history inspection. `load_city_mesh` expands records for
legacy CPU consumers, or preserves them for the WebGL exporter. Binary mesh input
is memory-mapped; unfiltered reads do not copy each complete attribute array.

The WebGL viewer shares a six-vertex quad topology through `drawArraysInstanced`,
batching instances by 64 m spatial cell and visibility category. UVs and materials
are instance attributes; material changes do not create extra draw calls. Picking
and room-camera probes keep compact position-only records. Roof toggles, cutaways
and chunk culling cover both paths. This shares surface topology, not entire
building meshes; the dwelling skeleton and its temporary `Solid` are still built,
and irregular terrain remains an ordinary mesh.

Columned entrances reserve their frontage during `planCellDwelling`, including
standalone dwellings. The realised dwelling plan owns `entranceLanding` and
`entranceFlight` footprints alongside the portico's columns, architrave, roof and
soffit. All plinths fit inside the landing; the separate broad flight fits inside
the same legal lot. Standalone meshes consume the planned access slabs. City
meshes omit those local support faces and fit the same reserved footprints to
terrain heights, replacing the generic nearest-road stoop and reserving the
result against terrain and clutter. A flight too short for the rise is rejected
instead of producing shallow, unwalkable treads.

Door leaf count survives from `CellDoor` into `DwellingDoor` and the facade
structure, rather than being inferred solely from grid-bay width. Temple fronts
use paired round-headed doors; sufficiently tall high-fantasy entrances use
pointed doors. Door cuts, frames and leaves share the normalized spring profile
so their crowns stay aligned while retaining clearance.


### Annual world story graph

The inhabited world uses `WorldStoryGraph`, backed by the engine's
`EdgePageStore`. This path does not instantiate the city chronicle or economic,
household, construction or defence ledgers, and retains no yearly/weekly
population snapshots. The separate legacy city-history fixtures still exist.

World generation seeds exact role-based populations (hamlets 24–99, villages
100–499, towns 500–1,000, cities 1,001–2,400, capitals 1,500–3,200), inherited
homes, farms, workshops and civic buildings. Legendary founding and charter
stories are labeled assumptions before year 1000. The world application and
preview default to 200 annual steps. Population caps are explicit and an
oversized founding census fails before simulation.

Simple annual rules update living residents, jobs, ancestry, new households,
leadership succession, harvest hardship and rebuilding. Recovery and succession
stories link to their causal events. Civic vacancies are filled from working
residents. Food need is a current graph fact, not an inventory or money account.
Story nodes and dead ancestors remain in the graph; only current relationships
such as employment and residence are removed or replaced. Progress and
cancellation occur annually.

After the run, current residents and occupied homes/workplaces plus civic
buildings become the settlement geometry contract at the actual selected world
sites. Empty historical houses remain in the story graph without requesting
thousands of unused meshes. Building dates drive architectural age.
`--world-history <directory> [years]` exports the evolving graph's final state
as `story-graph.json` and current settlement projections. The regular world
preview uses the same generated graph to build geometry, with no second
simulation. `KingdomHistoryTests --centuries` checks the graph over 200 years.
