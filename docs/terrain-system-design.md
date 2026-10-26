# Terrain system — clean-slate design

Written 2026-08-17. Supersedes `terrain-generation-redesign.md`, which assumed
the existing pipeline had to be preserved. This one does not.

Numbers marked **(measured)** were taken on this machine this session.
Everything else is proposal.

---

## 0. Why start over

The current pipeline is not badly implemented. It is built on a shape that the
game does not need, and that shape costs it everywhere:

- A cube-sphere domain exists to dodge the equirectangular polar singularity. A
  continent-scale game world has no poles to dodge.
- Because the domain is six faces, every field needs seam transforms, every
  atlas needs a ×6 multiplier, and the erosion grid is derived from the export
  atlas width (`width / 4`) rather than stated. That derivation is how erosion
  ended up running at **614 m cells (measured)** without anyone choosing it.
- Terrain elevation is carried on a Voronoi graph, so terrain detail is bounded
  by graph density (**785 m at 204k regions, measured**) and getting more detail
  means more regions, which is the expensive axis.

Each of those is defensible alone. Together they produce a system where the
thing that makes terrain look like terrain — erosion — runs at the coarsest
resolution in the chain. That is not a bug to fix; it is what the architecture
asks for.

---

## 1. Design commitments

Five decisions everything else follows from. These are the parts worth arguing
about before any code is written.

### 1.1 Layout first, simulation second, noise last

*Revised. An earlier draft of this document said "simulation-first". That was
wrong for a game: a pure simulation gives you a plausible world, not a composed
one, and composition is what makes a map worth playing on.*

Three stages, in this order, each with a job the others cannot do:

1. **Layout decides composition.** A Voronoi partition, cells classified into
   region types — mountain, hill, valley, plain, marsh, forest, coast — plus
   river courses and the coastline. This is where the map becomes *interesting*,
   because it is the only stage a designer can direct. Reference:
   Watabou's Perilous Shores, whose `tags=island,lowland,difficult,barren`
   are precisely the world brief of §4.2.
2. **Simulation decides plausibility.** Erosion takes the layout's blocky,
   authored structure and makes it behave like rock and water: graded slopes,
   connected drainage, valleys that widen downstream, alluvium where the
   gradient drops. This is where the map stops looking generated.
3. **Noise decides texture.** Sub-texel roughness below the simulation grid,
   and nothing else. Never structure — structure comes from stages 1 and 2.

The ordering matters because each stage can only fix the layer above it.
Erosion cannot invent an interesting continent; layout cannot invent a
believable hillside; noise cannot invent either.

**The tension this creates, and the resolution.** Erosion is landscape
*evolution*: run it on an authored heightmap and it will flatten the mountains
you placed and fill the valleys you cut. Left naive, stage 2 destroys stage 1.

The fix is that **the layout must produce an uplift field, not only an initial
height**. A cell classified "mountain" becomes a region of sustained uplift;
erosion then runs against it and reaches a dynamic equilibrium where the range
persists *because it is still being pushed up*, while its valleys, spurs and
drainage emerge from the physics. That is how real ranges work, and it is what
makes authored structure and simulated detail agree instead of fight.

Rivers get the same treatment: a laid-out river is a low-elevation seed channel
plus a drainage bias, so the solver's own network *chooses* to follow it rather
than being overwritten by it.

### 1.2 Simulation resolution is stated, never derived

The single most expensive defect in the current system was a derived resolution
nobody chose. In the new system the simulation grid is an explicit parameter,
validated at startup against the smallest feature the design requires, and
reported in metres in the UI. If the grid cannot resolve the feature, that is a
loud failure, not a soft blur.

### 1.3 Fields are independent layers

Not one `Maps` struct of parallel arrays. Each field — bedrock, sediment, water,
temperature, precipitation, biome, lithology — has its own resolution, lifetime
and owner. Climate does not need the erosion grid; biome does not need to exist
during erosion. This is what lets the expensive stage run at 88 m while cheap
stages run coarser, and it is what makes each stage independently testable.

### 1.4 The graph carries structure, never elevation

Voronoi stays, but demoted. It is good at irregular natural partitions — plates,
provinces, regions — and that is all it does. Elevation lives in the field, and
detail comes from erosion, not from graph density. This decouples "how detailed
is the terrain" from "how many regions are there", which are currently the same
question and should never have been.

### 1.5 Interest is a measured property

"Interesting landmasses" is unfalsifiable unless it is measured. The generator
carries explicit metrics (§4.4) and can reject or perturb against them. Without
this the generator drifts toward whatever was last eyeballed, and quality
regressions are invisible.

---

## 2. Scale hierarchy

Three tiers, each with a different generator and a different resolution. Cost
lives almost entirely in the middle tier.

| tier | span | resolution | produces |
|---|---|---|---|
| **Macro** | whole world, 361 km | ~2–5 km | landmass layout, plates, uplift field, climate zones |
| **Meso** | 10–50 km | **88 m** | eroded bedrock, sediment, drainage, coastline |
| **Micro** | < 1 km | sub-texel | surface detail, scatter, material variation |

Micro is explicitly **not baked**. It is evaluated at render time by the voxel
renderer and the CBT displacement. Baking it multiplies storage for detail that
is only visible up close, and the standing architecture note already says
runtime detail belongs to the renderer.

Reference target: **361 × 361 km** (≈130,000 km², England-sized) at **4096² =
88 m cells**. For calibration, R=100 km sphere surface is 125,664 km² and
England is 130,279 km² **(measured)** — the current world is already the right
size, it just spends the budget badly.

### 2.1 Multi-gigabyte maps: generation must go out-of-core

The reference target is not multi-gigabyte — 16.8M cells at ~20 bytes of fields
is ~336 MB. Gigabyte scale means one of:

| configuration | cells | fields at ~20 B |
|---|---|---|
| 361 km at 88 m (4096²) | 16.8 M | 336 MB |
| 361 km at 22 m (16384²) | 268 M | **5.4 GB** |
| 1000 km at 61 m (16384²) | 268 M | **5.4 GB** |

Past roughly 8192² the whole-field-in-RAM assumption breaks, and that is an
architectural constraint, not a tuning problem. Two consequences:

**Fields become tiled and paged during generation, not just at bake.** Each
stage streams its input tiles and writes output tiles, with a halo wide enough
for its stencil. Cheap for local stages (slope, curvature, classification).

**Erosion is the hard case, because drainage is globally non-local.** Discharge
at a point depends on its entire upstream basin, which may span the map. A naive
grid tiling cuts drainage networks and produces visible seams along tile edges —
rivers that stop at a boundary.

The resolution: **decompose by drainage basin, not by grid.** Basins are
hydrologically closed — water does not cross a divide — so they are natural
independent units. Proposed scheme:

1. Solve drainage on a coarse global field (e.g. 2048², cheap, fits in RAM) to
   extract basins and divides.
2. Erode each basin independently and in parallel, at full resolution, out of
   core. A basin's boundary condition is its outlet elevation, which the coarse
   pass already fixed.
3. Reconcile along divides, where by construction there is no flow to reconcile
   — only continuity of the surface, which is a narrow blend.

This makes erosion scale with total area rather than with what fits in memory,
and it parallelises across basins with no communication. The risk to watch is
that a basin's *size distribution* is heavy-tailed: one continental basin can
dominate the map and become the critical path. Splitting a very large basin at
an interior confluence, with the sub-basin outlet pinned, is the escape hatch.

**This should be designed in from phase 1 even if early worlds fit in RAM.**
Retrofitting out-of-core erosion onto an in-core solver means rewriting it.

---

## 3. Pipeline

Each stage takes fields and returns fields. No stage reaches backwards.

```
  --- layout: composition, cheap, directable ---
  1  Brief         -> archetype + tags (island, lowland, barren, ...)
  2  Voronoi       -> cell partition at region scale (~2-5 km)
  3  Classify      -> per-cell region type: mountain | hill | valley
                      | plain | marsh | forest | coast | water
  4  Rivers        -> courses over the cell graph, mouth to headwater
  5  Coastline     -> land/water boundary from the cell mask

  --- synthesis: layout becomes fields ---
  6  Height seed   -> initial heightmap from cell types (blocky, honest)
  7  Uplift field  -> sustained uplift where the layout wants relief
                      *** this is what stops erosion erasing the layout ***

  --- simulation: plausibility ---
  8  Erosion       -> bedrock, sediment, drainage, water   [the expensive one]
  9  Sea level     -> coastline refinement, ocean/lake classification

  --- derivation: everything else reads the eroded field ---
 10  Climate       -> temperature, precipitation           (needs relief)
 11  Biome         -> class + blend weights
 12  Landform      -> valley/plain/ridge/plateau (§5A), measured not assumed
 13  Lithology     -> exposed rock from suite + current height
 14  Settlements   -> suitability then siting (§5B)
 15  Noise         -> sub-texel texture only
 16  Bake          -> tiled LOD pyramid
```

Note stages 3 and 12 are **different things with similar names, and the
difference is the point**. Stage 3 is the *authored intent* — "this cell should
read as mountain". Stage 12 is the *measured outcome* — "this cell now has high
TPI, convex curvature and low accumulation, so it is a ridge".

Comparing them is the cheapest quality signal in the whole pipeline: where
intent and outcome disagree, either the synthesis was too weak to express the
layout or the erosion overran it. A per-class agreement score between stage 3
and stage 12 should be a standing gate.

Two orderings that are easy to get backwards:

Two orderings matter and are easy to get wrong:

- **Climate after erosion.** Orographic precipitation depends on relief, so
  rain shadows cannot be computed before the mountains exist. Getting this
  backwards produces deserts in the wrong places and is hard to spot.
- **Lithology after erosion, not before.** Rock exposed at the surface is a
  function of the *current* height against the bedding, so a valley that incises
  exposes the layer beneath. Baking one rock index per cell up front freezes the
  pre-erosion geology and no amount of incision reveals anything.

### 3.1 Erosion — the stage that matters

This is where realism is won or lost. Model: stochastic particle-based
Lagrangian transport with momentum conservation, per McDonald & Cordonnier,
*Stochastic geomorphological transport for terrain erosion simulation* (ACM TOG
45(4), 2026); reference implementation `github.com/erosiv/geotransport`.

Why this and not Stream Power Law: SPL implementations impose "restrictive
assumptions, notably that water flow strictly follows the steepest topographic
gradient", which the authors note "limits the expressiveness of the model and
its ability to reproduce complex phenomena such as meandering rivers, braided
rivers, deltas". Grid-based Eulerian schemes additionally show "salient
directional artifacts and a strong dependence on resolution". Meanders, braids
and deltas are exactly the features that read worst today.

The paper's domain is an **open 2D domain Ω ⊂ ℝ² in a 2.5D setting** — it fits a
flat continent natively and would need adaptation for a sphere. It is
"massively parallel" and particle-based, so it maps onto compute.

**Honest cost note.** This is a research-grade implementation, not a parameter
change. It is the highest-risk item in this document. A cheap fallback exists:
a conventional multi-receiver flow-routing erosion at 88 m would already be a
large improvement over 614 m single-receiver, and can serve as the phase-1
target while the particle solver is prototyped against the reference code.

---

## 4. Landmasses

### 4.1 The problem

Earth is mostly boring — vast low-relief interiors, coastlines that are long
stretches of not much. A faithful tectonic simulation reproduces that
faithfully. Realistic and dull is the default outcome, and it is what a purely
emergent generator will converge on.

### 4.2 Guided, not emergent

The generator takes a **world brief**: an archetype plus constraints, rather
than only a seed.

```
WorldBrief
  archetype        SingleContinent | Archipelago | InlandSea | Peninsular
  landFraction     target, e.g. 0.35
  coastComplexity  smooth headlands .. deep fjords
  reliefBudget     total elevation range
  rangeCount       how many major mountain systems
```

Plates are **fitted to** the briefed landmasses rather than the landmasses
emerging from plates. Convergent boundaries still generate ranges through
sustained uplift against erosion — the physical mechanism is intact — but they
occur where the design asked for them. This is the difference between a
generator and a lottery.

### 4.3 Coastlines are earned, not drawn

A coastline is where terrain meets the sea datum. Fjords come from glacial or
fluvial incision below the waterline; rias from drowned valleys; barrier islands
from sediment deposition. All of these fall out of erosion plus sea level if the
erosion is good enough. **Do not generate coastline with noise** — noisy
coastlines are the most recognisable tell of a procedural world, because the
inlets do not connect to anything inland.

### 4.4 Interest metrics

Computed after erosion, before acceptance:

| metric | why |
|---|---|
| navigable water penetration | can you reach the interior by boat |
| chokepoint count and quality | passes, isthmuses, straits |
| buildable-land fraction | slope below a threshold, contiguous |
| distance to nearest biome transition | variety density; large uniform regions read as empty |
| coastline fractal dimension | smooth blob vs over-noisy |
| basin count and size distribution | distinct regions with identity |

These gate acceptance and are reported per generation. A world that fails is
reseeded or locally perturbed. Crucially they also make quality **regressions
visible** — the thing eyeballing cannot do.

---

## 5. Biomes

Whittaker-style classification from temperature × precipitation, modified by
slope, altitude and drainage. Keep a compact stable class index — the existing
21-class set (Ocean, Lake, Beach, Grass, Rock, Snow, River, Marsh, Ice, Tundra,
Bare, Scorched, Taiga, Shrubland, TemperateDesert, TemperateRainForest,
TemperateDeciduousForest, Grassland, TropicalRainForest,
TropicalSeasonalForest, SubtropicalDesert) is a good set and worth keeping on
merit, not history.

**Ecotones.** A cell stores a dominant class *and blend weights*, not a single
index. Hard class boundaries are the second most recognisable procedural tell
after noisy coastlines.

Implementation: classify per Voronoi region — independent and embarrassingly
parallel — then blend the nearby regions' classes per texel with a cheap
distance weight. Two honest notes:

- As **performance** this is not worth doing. Raster classification measures
  16 ms at 1024 and 63 ms at 2048 **(measured)** — 0.4% and 0.8% of generation,
  against 78.9% in the graph and 13.3% in erosion.
- As **quality** it is, because it replaces a nearest-region lookup with a
  continuous function and removes region-shaped seams.

**Sub-biome variation.** Vegetation density, rockiness and soil vary *within* a
class from lithology and local slope, so a large biome does not read as one flat
colour. This is what stops a correct classification from still looking cheap.

---

## 5A. Landforms

Valleys, plains, ridges, plateaus, basins and escarpments are **derived, not
generated**. If the erosion is right they are already present in the heightmap;
the job is to name them so that gameplay, biomes and settlement siting can
reason about them.

Standard geomorphometry, all local and therefore cheap and tileable:

| input | from |
|---|---|
| slope | first derivative |
| curvature (profile / planform) | second derivative — concave collects, convex sheds |
| topographic position index (TPI) | height minus neighbourhood mean, at two radii |
| drainage accumulation | already produced by erosion |
| relief amplitude | local max minus min |

TPI at two radii is the workhorse: small-radius TPI separates local ridges from
local hollows, large-radius TPI separates uplands from lowlands, and the pair
classifies the standard set:

```
  valley       low TPI both radii, high accumulation, concave
  plain        low relief amplitude, low slope, mid TPI
  plateau      low slope, high large-radius TPI, bounded by escarpment
  escarpment   high slope, sharp large-radius TPI gradient
  ridge        high TPI both radii, convex, low accumulation
  basin        low large-radius TPI, internally drained
  foothill     mid TPI, moderate slope, adjacent to range
```

Two design notes. **Landform is a separate field from biome** — a valley can be
forest or desert, and collapsing the two loses exactly the distinction siting
needs. And **classification thresholds are world-relative**, scaled to the
world's own relief budget, or a low-relief world classifies as one giant plain.

**Emergent, not authored.** Alluvial fans, terraces, floodplains and deltas
should appear because the erosion produced them, and the classifier should find
them. If a landform can only be produced by generating it directly, that is
evidence the erosion model is too weak — the same argument as coastlines in
§4.3.

## 5B. Settlements and cities

Two stages, deliberately separated: a continuous **suitability field**, then
discrete **site selection**. Conflating them is why suitability maps rarely
translate into believable towns.

### Suitability

A weighted combination, all inputs already available by this point:

| factor | rationale |
|---|---|
| fresh water adjacency | river, lake, spring — historically decisive |
| buildable land | slope under threshold, contiguous area |
| landform | valley floors, plains, river confluences, sheltered coast |
| arable potential | biome, soil from lithology, precipitation |
| coastal access | natural harbour: sheltered water plus buildable shore |
| defensibility | local elevation advantage, chokepoint proximity |
| resources | ore from lithology, timber from forest cover |
| flood risk | **negative** — floodplain and low terrace penalised |

### Site selection

Suitability alone yields blobs, not settlements. Selection needs:

- **Spatial competition.** Settlements suppress rivals nearby — a Poisson-disk
  or greedy-with-exclusion pass over suitability, radius scaled by tier.
- **Tiers.** Hamlet / town / city, with different radii, thresholds and counts.
  A city needs more of everything; hamlets fill the gaps.
- **Connectivity.** Sites should be mutually reachable at sane cost. Compute
  least-cost paths over slope and water crossings; these double as the road
  network and feed §4.4's chokepoint metric.
- **Regional quotas.** At least one meaningful settlement per major basin, so no
  large region reads as empty.

Roads falling out of the same least-cost pass matters: roads that follow valleys
and cross rivers at fords are a strong realism cue, and they come free from
having landforms and a cost field rather than being drawn separately.

**These are proposals for placement, not a simulation of history.** Real
settlement patterns come from centuries of path dependence. This produces
plausible sites, which for a game is the requirement — but do not expect it to
explain itself the way a real map does.

## 6. Output and runtime

**Bake to a tiled LOD pyramid.** Arrived at independently, this is the same
shape the existing package uses, and the existing design is sound: fixed-size
tiles (256²), a one-texel gutter so a tile can be filtered without its
neighbours, per-channel compression, and — most importantly — **ancestor
fallback**, so a missing tile degrades resolution instead of punching a hole.
Reuse the format on merit. The one change: it needs a single-face planar mode
alongside the six-face cube mode.

**Streaming** per `heightmap-streaming-plan.md`: bounded residency, GPU atlas
plus indirection table, CBT-driven request feedback. Multiple heightmaps become
multiple registered sources sharing one budget.

**Procedural props are a future runtime consumer, not a terrain-bake output.**
The map bundle supplies coarse ecotope weights, vegetation/rock potential and
topology/exclusion fields. A loaded terrain node refines those with local
height, slope, substrate, moisture, erosion and authored overrides, then a GPU
placement pass turns density into deterministic candidates and resolves their
footprints. Only then does an asset palette choose a voxel model or conventional
mesh/material representation. The palette owns asset paths, weights, spacing,
scale, environmental limits and LOD policy; none of those asset details belong
in region or biome textures.

This keeps the useful data/density/placement separation described in the GDC
2017 Horizon procedural-placement presentation without adopting its global
metre-scale storage cost. Roads, activity space and object-exclusion maps are
reserved by the map-export schema now and populated later. Until that runtime
project starts, the generator stops at the registered maps and vector graphs.

**Rendering: CBT on a planar mesh.** Adaptive subdivision driven by projected
area, displaced from the streamed height tiles. Per Benyoub & Dupuy 2024, the
method takes **arbitrary halfedge meshes**, so a planar root mesh is the method
working as designed. A mesh of exactly 20 triangles yields exactly 60 halfedges,
matching the existing root-bisector count.

---

## 7. Verification

Gates, not reports. A generator's failure mode is producing something plausible
and wrong.

- **Determinism** — three runs, one hash, re-checked after *every* solver or
  classifier change. It was verified once, assumed for three phases, and had
  broken by the time anyone looked: 17% of cells differing, up to 1856 m.
- **Drainage scaling** — slope–area θ measured **on the simulation domain, never
  a resampled atlas**. The same field measured θ = −0.532 on the cube domain and
  −1.12 on the equirectangular atlas **(measured)**.
- **Mass balance** — sediment produced, transported and deposited must close to
  a stated tolerance. The current 7.6% is not good enough.
- **Biome coverage** — no class above ~25%, minimum class count present.
- **Interest metrics** — §4.4, gating acceptance.
- **Feature-size assertion** — simulation grid resolves the smallest briefed
  feature, checked at startup.
- **Look at it in a window.** Every baseline number came from headless runs, and
  twice this session a change was reported working while the app rendered
  nothing, because headless cannot see a gated draw call.

---

## 8. Phasing

| phase | deliverable | gate |
|---|---|---|
| 0 | **Layout only** — Voronoi, cell classification, rivers, coastline, rendered as a flat 2D map | a readable map in the Perilous Shores sense; composition judged before any terrain exists |
| 1 | Planar field domain, height seed + uplift, conventional multi-receiver erosion at 88 m | terrain legibly better than today; stage 3 vs stage 12 agreement above threshold |
| 2 | Climate, biomes with ecotones, lithology | coverage gate; no region seams |
| 3 | Landform classification (§5A) | valleys/plains/ridges match what the hillshade shows |
| 4 | Planar CBT preview | subdivides on zoom; bisector count tracks camera |
| 5 | World brief + interest metrics | a stated archetype reliably produces that world |
| 6 | Settlement siting + roads (§5B) | sites survive inspection; every basin populated |
| 7 | Basin-decomposed out-of-core erosion (§2.1) | a >8192² world generates; no seams at divides |
| 8 | Particle-based geomorphological transport | meanders, braids, deltas; mass balance closes |
| 9 | Tiled bake + streaming | stable working set under a moving camera |
| runtime | Ecotope density evaluation and voxel/mesh prop placement | deferred consumer of the map/terrain bundle |

Phase 3 lands early on purpose: landform classification is cheap, is the
strongest available check that the erosion is producing real structure, and
everything in phases 5–6 depends on it.

**Phase 1 is the honest test of this whole document.** If moving erosion from
614 m to 88 m with multi-receiver routing produces terrain that looks right,
phase 5 is optional and a large amount of risk disappears. That is worth finding
out before committing to the research-grade solver.

---

## 9. Open questions

1. **World shape** — one 361 km continent, an archipelago, or several continents
   as separate streamed sources? Changes §4 substantially. Design call.
2. **Is the world static?** Runtime terrain deformation (craters, digging,
   construction) changes everything about baking and streaming, and is far
   cheaper to design in now than to retrofit.
3. **Voxel coupling** — the generator emits height, material and lithology; the
   voxel renderer reads them. The existing micro-material palette is one byte
   into a 256-entry per-brick palette and eroded terrain already overflows it.
   Finer erosion makes that worse. Needs resolving before eroded terrain ships
   with micro-materials.
4. **Rivers as geometry** — biome-classified river cells give you rivers on the
   map, but not banks, meander geometry or navigable water volume. If rivers
   matter to play, they likely need to be extracted as explicit curves rather
   than left as a raster class.
