# Streaming heightmaps — implementation plan

Written 2026-08-17. Every number here is measured on this machine or read out of
the code; anything inferred says so.

---

## The thesis

The CBT gives geometry at any resolution for free. What it cannot give is
*data*: the paper's own footnote is that storing the fully subdivided mesh of an
Earth-sized planet would take exabytes, "orders away from what a modern SSD can
store" (Benyoub & Dupuy 2024, §1). The geometry is procedural and bounded by a
bisector pool; the elevation that shapes it is not.

So the split this plan implements is:

- **CBT** decides *where* detail is needed — it already does, per frame, from
  projected area.
- **Streaming** decides *what data* backs it — a bounded working set of
  heightmap tiles resident on CPU and GPU, paged against the same signal.

Today `SparseVoxelPlanetSurfaceAtlas` uploads **one immutable pyramid for the
whole world**, sized up front. That is why detail is capped: at 2048/face the
atlas is 288 MB and 77 m texels, and the only way to get finer is to make the
whole thing bigger. Measured on the reference planet:

| face resolution | atlas VRAM | bake | texel at R=100 km |
|---|---|---|---|
| 1024 | 72 MB | ~1.1 s | 153 m |
| 2048 | 288 MB | ~4 s | 77 m |
| 4096 | 1.15 GB | ~18 s | 38 m |
| 8192 | 4.6 GB | ~68 s | 19 m |

Streaming replaces that column with a constant. A fixed residency budget buys
whatever resolution the camera can actually see, at any world size, and makes
"multiple heightmaps" a matter of registering more sources rather than
multiplying the atlas.

---

## What already exists

This is deliberately not a from-scratch design. Four pieces are already in the
tree and this plan is mostly about connecting them.

**The package is already a streamable quadtree.** `PlanetPackage.hpp` has
`PlanetTileAddress{face, level, x, y}`, `planetTileChild`/`planetTileParent`,
`planetTileMorton`, and `planetTileLevelForTargetEdge(radius, targetEdge)`.
Tiles are `tileSizePixels = 256` with `gutterPixels = 1`, LZ4-compressed, stored
per channel. The gutter matters more than it looks: it is what lets a tile be
bilinearly sampled without reaching into a neighbour that may not be resident.

**Ancestor fallback already exists.** `findBestRasterBlock(id, requested)`
returns the requested tile *or its closest stored ancestor*. This is the single
most important property for streaming: there is always something to draw, so a
miss degrades resolution instead of punching a hole. The whole design below
leans on it.

**There is a working GPU-driven paging precedent.** The voxel streaming path
already does request-bits → priorities → instance IDs → CPU fulfilment
(`sparse_voxel_streaming.wgsl`, `pageRequestBits`, `pageRequestPriorities`,
`pageRequestInstances`). Mirror its shape rather than inventing a second idiom.

**The CBT already computes the LOD signal.** `projectedLodError()` in
`sparse_voxel_planet_cbt_update.wgsl` samples `heightMinMax` at
`planetCbtTextureMip(depth, mipCount)` and projects the displaced triangle to
pixels. The mip it *wants* is the tile level we should stream. That is the
feedback signal, already computed, currently thrown away.

---

## Architecture

### Heightmap sources

"Multiple heightmaps" becomes a registry of independent sources, each with its
own tile pyramid, transform and channel set. A planet is one source; a continent
is one source; an island chain is several. They share one residency budget and
one GPU atlas, and differ only in how a world position maps to a tile address.

```
HeightmapSource
  id                     stable, survives reload
  domain                 Cube (6 faces) | Planar (1 face)
  reader                 PlanetPackageReader
  toTileAddress(pos, level) -> PlanetTileAddress
```

`domain` is what keeps the planar continent and the spherical planet on one
path: the cube case addresses six faces, the planar case is the degenerate
single-face quadtree. Nothing else in the system needs to know which it is.

The future procedural-placement maps use the same source transform, tile
address and ancestor fallback. Elevation, uplift and density weights are
filterable channels; region/biome IDs and topology masks are nearest-sampled
integer channels. They may share page requests, but not necessarily physical
atlases or compression formats. This extension is reserved by the map bundle
schema; it is not part of the first heightmap-streaming implementation.

### Residency

One LRU-with-pinning cache, keyed by `(sourceId, channel, PlanetTileAddress)`,
holding decompressed tiles. Two floors that must not be negotiable:

- **Every ancestor of a resident tile is also resident.** This is what makes
  fallback total: any miss walks up and always terminates. Cheap, because the
  pyramid above a tile is a third of its own cost.
- **Level 0 of every source is pinned forever.** A few hundred KB per source
  that guarantees the world is never blank.

Eviction is LRU over unpinned tiles, but only down to a low-water mark, and
never within one frame of a tile being requested — evicting a tile the CBT is
about to ask for again is the classic way to build a thrash loop.

### GPU representation

A physical tile atlas plus an indirection table, not one texture per source.

- **Physical atlas** — a 2D array of `256 + 2 gutter` slots per channel.
  Budgeted in slots, not megabytes: 4096 slots of 258² RG16F is ~1.1 GB, so the
  practical default is smaller and configurable.
- **Indirection table** — a buffer mapping `(source, face, level, x, y)` to a
  physical slot, plus the level actually resident there. The shader reads the
  indirection, discovers it got level `n-2` instead of `n`, and adjusts its UV
  accordingly. This is where ancestor fallback becomes free at sample time.

Sparse/virtual textures were considered and rejected: MoltenVK support is the
constraint, and an explicit indirection table is portable and debuggable. Same
reasoning that put an explicit page table in the VSM.

### The feedback loop

Per frame, in the order the graph already runs:

1. **CBT update** computes per-bisector desired level (it already computes the
   mip; expose it).
2. **Request pass** marks desired tiles in a request bitfield with a priority
   from projected area — mirroring the voxel streaming pass.
3. **Readback** one frame later. Never stall.
4. **CPU fulfilment** on a worker: decompress the highest-priority misses, cap
   the per-frame upload budget in bytes so a camera cut cannot spike the frame.
5. **Upload** into free atlas slots; patch the indirection table.

Latency is deliberately several frames. With ancestor fallback that is invisible
— the surface is briefly coarse, not missing.

---

## Phases

Each phase has to be independently verifiable, because the failure mode of a
streaming system is that it silently serves stale or wrong data and looks
merely blurry. See the `[[metric-noise-floor-before-verdict]]` lesson: decide
what the number is *before* running it.

### Phase 0 — instrumentation

Log resident tile count, atlas occupancy, per-frame uploaded bytes, and miss
rate. Nothing streams yet. This is the baseline every later phase is judged
against, and it is what makes a thrash loop visible instead of merely slow.

**Done when** the counters exist and read zero.

### Phase 1 — CPU residency, no GPU change

Build the cache, the ancestor invariant and the LRU. Keep uploading the full
atlas as today, but serve `buildSurfaceMaps` through the cache. Pure refactor.

**Done when** the surface atlas is byte-identical to today's — hash it — and
the ancestor invariant holds under a randomized request fuzz.

### Phase 2 — GPU atlas and indirection

Introduce the physical atlas and indirection table. Still resident-everything,
still no eviction. The sampling path changes; the contents do not.

**Done when** a rendered frame is identical to phase 1 within tolerance, with
the atlas physically laid out differently. Any visible seam here is a gutter or
UV-adjust bug, and it is far cheaper to find now than after eviction lands.

### Phase 3 — the feedback loop

Wire CBT request → readback → fulfilment → upload. Enable eviction.

**Done when** the working set stabilises under a moving camera, and the
`[[svraster-dolly-camera-harness]]` flythrough shows no growth in resident
tiles over repeated passes. A static camera proves nothing here — the whole
failure mode is motion.

### Phase 4 — multiple sources

Register more than one source and confirm the budget is shared, not multiplied.

**Done when** two continents plus a planet stay inside one budget, and
unregistering a source frees every slot it held.

---

## What will go wrong

Written in advance, because each of these has a cheap check and an expensive
discovery.

**Thrashing at a subdivision boundary.** A bisector oscillating across a level
boundary requests and evicts the same tile forever. Hysteresis — the CBT already
has `refineMultiplier`/`mergeMultiplier` for exactly this on geometry — plus the
minimum-residency rule above.

**The gutter is one texel.** That is enough for bilinear and *not* enough for
anisotropic filtering or a wider reconstruction kernel. If sampling ever widens,
the gutter must widen with it, and the symptom is a faint seam grid that is easy
to blame on the atlas layout instead.

**Level 0 pinning is not optional.** Without it, a fast camera cut can evict
everything the fallback chain depends on and blank the world for several frames.

**Readback latency reads as a bug.** A tile arriving three frames late looks
like popping. Resist fixing it by stalling on the readback; that trades a
cosmetic artifact for a frame-rate one.

**Determinism.** Streaming must not change generated output. Re-run the
three-run package hash after any change that touches the bake — it has silently
broken before.

---

## What this does not solve — and must be sequenced before it

The streaming system moves data efficiently; it does not create detail. At
R=100 km the stream-power erosion runs on a 256-cell cube face — **614 m
cells** — and the Voronoi graph is ~785 m. Streaming a 19 m atlas over a
surface eroded at 614 m serves interpolation at higher precision.

**Raise the simulation resolution and quality first, or this phase buys nothing
you can see.**

### The erosion model is the real ceiling

McDonald & Cordonnier, *Stochastic geomorphological transport for terrain
erosion simulation* (ACM TOG 45(4), 2026) diagnoses precisely the artifacts
being seen here. Two of its claims apply directly to our solver:

- Eulerian grid discretization "induces salient directional artifacts and **a
  strong dependence on resolution**". Our solver is exactly that — a fixed
  cube-face grid — which is why the terrain reads as pixelated rather than
  merely soft.
- Stream Power Law implementations rely on "graph traversal strategies that
  impose restrictive assumptions, notably that water flow strictly follows the
  steepest topographic gradient". Ours does: `PlanetStreamPowerSolver` carries a
  per-cell receiver-direction buffer. The paper notes this "limits the
  expressiveness of the model and its ability to reproduce complex phenomena
  such as meandering rivers, braided rivers, deltas".

That list is the complaint verbatim: rivers reading as flat pixels, mountain
ranges reading as bad. They are not texture problems. They are consequences of
a single-receiver Eulerian solver on a 614 m grid.

The paper's replacement is a stochastic, particle-based Lagrangian scheme that
transports water, sediment and debris with **momentum conservation**, is
explicitly "massively parallel", and yields meanders, braids, deltas, debris
flow scars and alluvial fans as emergent features. Reference implementation:
`https://github.com/erosiv/geotransport`.

**It is defined on an open 2D domain Ω ⊂ ℝ² in a 2.5D setting.** That is not
incidental: it fits the planar-continent direction exactly, and would need
adaptation to run on a cube-sphere. The pivot to a flat field and the adoption
of this model are the same decision, which is a strong argument for taking them
together.

Being deliberately cautious about scope: the momentum-conserving particle
formulation is a research-grade rewrite of the solver, not a parameter change.
Phase 3's `transportK`/`weatheringRate`/`depositionK` remain uncalibrated, and
those constants do not transfer.

### Suggested order

1. **Planar continent field** — 361 km at 4096², 88 m cells, no cube seams.
2. **Port the existing stream power to that flat field** — the cheap win, 614 m
   → 88 m, mostly deleting `planetCubeFieldNeighbour` seam handling. Judge the
   result before going further; it may be enough.
3. **Geomorphological transport** — only if step 2's single-receiver artifacts
   are still the limiting factor. Prototype against the reference
   implementation before committing, and calibrate θ on the cube domain the way
   phase 1 did.
4. **This streaming plan** — once there is detail worth streaming.

Doing streaming before 1–2 would deliver coarse data faster.
