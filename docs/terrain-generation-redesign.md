# Terrain generation redesign — realistic heightmaps, game-worthy landmasses

Written 2026-08-17. Measured numbers are marked as such; everything else is a
proposal and says so.

---

## 1. The decision this document makes

**Neither a refactor nor a rewrite: a new spine with the proven organs
transplanted.**

The parts that are wrong are the *domain* and the *erosion model*. The parts
that work — the Voronoi graph, the biome classifier, the package format, the
CBT renderer — are wrong only in what feeds them. Rewriting those would discard
work that took a long time to get right (the CDT port alone cost 14 defect
fixes including a constraint livelock) and would put determinism back at risk.

| component | verdict | why |
|---|---|---|
| Cube-sphere domain (`PlanetCubeField`) | **replace** | exists only to dodge the equirect polar singularity; a game continent has no poles |
| Equirect export atlas | **replace** | anisotropic, wastes rows at poles, and was undersampling the graph |
| Stream-power solver | **replace** | single-receiver Eulerian; see §3 |
| Voronoi graph + CDT | **keep** | works, deterministic, expensive to re-derive |
| 21-class biome enum + classifier | **keep, extend** | 19 of 21 classes exposed on the reference planet |
| Package format | **keep** | already a streamable quadtree with gutters and ancestor fallback |
| CBT renderer | **keep, retarget** | paper's method is defined on arbitrary halfedge meshes; planar roots are supported, not a hack |
| Voxel strata (phase 4) | **stays dropped** | surface-only realism, no terrain editing |

The spherical planet path is not deleted. It keeps working; it stops being the
thing we invest in.

---

## 2. What is actually wrong today

All measured this session on the reference planet (seed 9127364512, R=100 km).

The pipeline has a **resolution inversion**. Detail is destroyed in the middle
of the chain and then interpolated back up:

| stage | resolution |
|---|---|
| Voronoi graph, 204k regions | 785 m |
| Stream-power erosion (256 cells/face) | **614 m** |
| Export atlas, was 512 wide | 1227 m ← coarser than the graph |
| Surface atlas, 2048/face | 77 m |

The export atlas has been fixed (now 2048 → 307 m). The erosion has not. **The
process that actually carves terrain runs at 614 m.** No texture, atlas or
subdivision resolution can recover detail that was never simulated. This is the
whole explanation for "everything looks pixelated".

Scale was never the problem. R=100 km is 125,664 km² of surface; England is
130,279 km². The world is already the right size — the budget is spent badly.

---

## 3. Erosion: the real ceiling

McDonald & Cordonnier, *Stochastic geomorphological transport for terrain
erosion simulation* (ACM TOG 45(4), 2026) names both of our defects:

- Eulerian grid discretization "induces salient directional artifacts and a
  **strong dependence on resolution**".
- Stream Power Law implementations impose "restrictive assumptions, notably that
  water flow strictly follows the steepest topographic gradient", which "limits
  the expressiveness of the model and its ability to reproduce complex phenomena
  such as **meandering rivers, braided rivers, deltas**".

`PlanetStreamPowerSolver` is a fixed-grid solver with a per-cell receiver
direction. It makes exactly both assumptions. Meanders, braids and deltas are
the features that read worst today, and they are the features the assumption
forbids.

Their scheme is stochastic particle-based Lagrangian transport with momentum
conservation, massively parallel, on an **open 2D domain Ω ⊂ ℝ² in a 2.5D
setting**. Reference implementation: `github.com/erosiv/geotransport`.

That 2D domain is the point of leverage: adopting this model and moving to a
flat field are the same decision.

**Sequencing, deliberately conservative.** The particle scheme is a
research-grade rewrite. Port the *existing* solver to a flat field first — it is
mostly deleting `planetCubeFieldNeighbour` seam handling, and takes cells from
614 m to 88 m. Judge that. Only adopt the particle model if single-receiver
artifacts still dominate after the resolution fix. Note also that phase 3's
`transportK`/`weatheringRate`/`depositionK` are still uncalibrated and those
constants will not transfer.

---

## 4. Target world

Proposal, not measurement:

- **Extent** 361 × 361 km (≈130,000 km², England) as the default preset, with
  the extent a parameter rather than a constant.
- **Simulation field** 4096² → **88 m cells**. Stream power at this size
  extrapolates to ~20 s from the measured 459 ms / 393k cells / 512 iterations.
- **Voronoi graph** ~800 m site spacing (≈203k regions, matching the density
  already proven on the sphere). Measured planar scaling: 20k sites in 4.1 s,
  81k in 20.3 s, roughly O(n^1.16) — so ~203k lands near a minute.

The division of labour matters more than either number: **the graph sets
structure** (landmasses, plates, ranges, drainage basins) and **the solver
carves detail** (valleys, meanders, fans) at 88 m below it. Trying to get detail
by refining the graph is the expensive way and is what makes generation slow.

---

## 5. Landmasses: realistic is not the same as interesting

This is the part that is a game design problem, not a simulation problem, and it
deserves saying plainly: **Earth is mostly boring.** Real continents have vast
low-relief interiors. A pure tectonic simulation faithfully reproduces that, and
faithfully reproducing it is how you get a world that is realistic and dull.

Proposed structure — a **guided** generator, not a purely emergent one:

1. **Macro layout (authored or seeded).** A coarse control map — a handful of
   landmass seeds with target area, and a coastline complexity parameter. This
   is the knob a designer actually wants: "one continent with a large inland
   sea", "an archipelago", "a single island with a mountainous spine".
2. **Tectonics on top of that layout.** Plates are fitted to the authored
   landmasses rather than the landmasses emerging from plates. Convergent
   boundaries still produce ranges — but where the design wants them.
3. **Coastline enrichment.** Fjords, rias, barrier islands and peninsulas come
   from erosion and sea-level interaction at the coast, not from noise. A
   coastline should be a *consequence* of the terrain meeting the datum, which
   is what makes it look earned.
4. **Interest density pass.** An explicit, inspectable metric — navigable water
   reaching inland, chokepoints, buildable-land fraction, distance from any
   point to a biome transition. Reject-and-reseed or locally perturb against it.

Point 4 is the one most likely to be skipped and most likely to matter. Without
a stated metric, "interesting" is unfalsifiable and the generator drifts toward
whatever the last person eyeballed.

---

## 6. Biomes

The classifier keeps the existing 21-class `PolygonTerrainMaterial` enum
(Ocean … SubtropicalDesert). Three extensions:

**Ecotones instead of hard edges.** Today each cell takes one class. Blending
neighbouring classifications gives graded transitions, which is both more
realistic and more legible in play.

This is where the **per-region classify-then-blend** idea belongs. Classify once
per Voronoi region — which `SphericalTerrain.cpp:2057` already does under
`parallelFor`, independently per region — then have each raster texel blend the
nearby regions' classes by a cheap weight. Two things worth being straight
about:

- As a *performance* change it is not worth doing. `raster classify` is 16 ms at
  1024 and 63 ms at 2048 — 0.4% and 0.8% of generation. The cost is in the graph
  (78.9%) and erosion (13.3%).
- As a *quality* change it is worth doing, because it turns a nearest-region
  lookup into a continuous function and removes the region-shaped seams.

**Orographic realism.** Rain shadows already have the inputs (`windExposure`,
`precipitation`). Worth verifying they actually produce dry leeward basins on
the new domain rather than assuming it.

**Sub-biome variation.** Within-class variation (soil, vegetation density,
rockiness) so a large biome does not read as one flat colour. This is where the
lithology field earns its place.

---

## 7. Verification

The failure mode of a generator is that it produces something plausible and
wrong, so each of these is a gate, not a report.

- **Determinism.** Three runs, one hash. Re-run after *every* solver or
  classifier change — it was verified once, assumed for three phases, and had
  broken by the time anyone looked (17% of cells differing, up to 1856 m).
- **Drainage scaling.** Slope-area θ measured on the simulation domain, never
  the resampled atlas. Phase 1 got θ = −0.532, r² = 0.996; the atlas reported
  −1.12 for the same field.
- **Mass balance.** Bedrock lost vs uplift vs export, closing to within a stated
  tolerance. Phase 3 closes to 7.6% and that is not good enough to ship on.
- **Biome coverage.** No class above ~25%, and a stated minimum number of
  classes present. Reference planet: 19 of 21, largest 14.9%.
- **Interest metrics** from §5.4, once defined.
- **Look at it in a window.** Every number in the baseline doc came from
  headless runs, and twice this session a change was reported working while the
  app rendered nothing.

---

## 8. Phasing

Each phase ends in something judgeable on screen.

| phase | work | gate |
|---|---|---|
| 1 | Planar continent domain; port existing stream power to a flat field | 614 m → 88 m cells; θ holds; determinism holds |
| 2 | Judge phase 1 in the 2D map view **before building any renderer** | rivers and ranges legibly better, or stop and reconsider |
| 3 | Planar CBT preview (20-triangle root mesh, `PlanarDomain` lifecycle flag) | subdivides on zoom; bisector count tracks camera |
| 4 | Guided landmass layout + interest metrics | designer can specify a world shape and get it |
| 5 | Biome ecotones + sub-biome variation | coverage gate; no region-shaped seams |
| 6 | Geomorphological transport, *if* phase 2 showed it is needed | meanders/braids/deltas appear; mass balance closes |
| 7 | Heightmap streaming (see `heightmap-streaming-plan.md`) | working set stable under a moving camera |

Phase 2 is a real gate, not a formality. If the resolution fix alone makes the
terrain acceptable, phases 6 and 7 may not be worth their cost, and finding that
out costs one afternoon.

---

## 9. Notes and risks

**The CBT port is smaller than it looks.** The core is a geometry-agnostic
binary triangle tree over a halfedge mesh — the paper's contribution is
precisely that it takes arbitrary polygon meshes. Only three things in
`sparse_voxel_planet_cbt_update.wgsl` are spherical: the `normalize()` closing
`decodeTriangle`, `planetCbtCubeAtlasTexel()`, and the radial
`displaced[corner] *= 1.0 + height * reliefScale`. A planar mesh of exactly 20
triangles yields exactly 60 halfedges, preserving `kPlanetCbtRootBisectorCount`
and all 16 of its uses untouched. A planar mode rides on the existing
`lifecycleFlags` bitfield (bits 0–2 used), so it needs **no new uniform field**
and cannot trip the WGSL alignment trap.

**Voxels stay out of the generator.** Per the standing architecture note: the
generator produces height, material and lithology maps; voxels *read* them when
building bricks. Runtime detail belongs to the voxel renderer, not baked
per-vertex.

**The micro-material palette ceiling is unresolved.** One byte per voxel indexes
a 256-entry per-brick palette, and eroded terrain overflows it — hence
`--disable-terrain-micro-materials`. Finer erosion will make this worse, not
better. It needs the planned per-voxel index into a per-instance atlas before
eroded terrain can ship with micro-materials.

**Open question — world shape.** Whether the shipped world is a single
361 km continent, an archipelago, or several continents streamed as separate
sources changes §5 substantially and is a design call, not a technical one.
