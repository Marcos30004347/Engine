# Planet generator refactor: GPU erosion, lithology, and regolith

> **Status, 2026-08-17.** Phases 0, 1 and 2 are done. Phase 3 is implemented but
> opt-in and uncalibrated, and does not meet its gate. Results and the measured
> calibration are in [planet-generator-baseline.md](planet-generator-baseline.md).
> Decisions settled: D1 cube faces, D2 the graph keeps tectonics and the solver
> owns fluvial relief, D3 bit-exactness verified, D4 the pass lives in
> `src/runtime/procedural/rendering/`. World scale is now a 200 km-diameter
> planet (R=100 km) with 3 km of authored relief. Phase 4 is not started.
>
> Two departures from the plan below:
> - D1's cross-face gutter kernel was replaced by exact cross-face neighbour
>   addressing, which needs no extra kernel, no gutter state, and cannot go
>   stale. `PlanetCubeFieldTests` covers the transform.
> - Phase 2 stores a rock *suite* plus a bedding phase rather than one
>   `PlanetLithology` per texel, and resolves the exposed rock from the current
>   surface height. A baked rock index froze the pre-erosion geology in place, so
>   incision never revealed a layer and almost no landforms appeared.
> - Phase 3 exposed two defects in the phase 1 solver that are now fixed on the
>   default path: incise_main discarded the accumulation it had just been given,
>   and the carried accumulation can compound once deposition breaks the strict
>   monotonicity it assumes. The first changes the measured channel exponent to
>   -0.362 at r2 0.994.

Plan, 2026-08-16. Moves the planet's fluvial relief off the CPU and onto a
stream-power solver, then splits the single `material` byte into a real geology
model (surface cover + rock type + soil depth) so the terrain can differentially
erode and so digging into it reveals something.

Sources analysed: `ignore/StreamPowerErosion` (H-Schott),
`ignore/WebGPU-Erosion-Simulation` (GPU-Gang), and McDonald & Cordonnier,
*Stochastic geomorphological transport for terrain erosion simulation*,
SIGGRAPH 2026.

---

## 1. What the two repos actually are

**They are the same algorithm** — not merely the same method, but the same code:
the WGSL is a line-for-line transliteration of the GLSL, with identical
`Read`/`Write`/`Slope`/`Laplacian`/`GetFlowSteepest`/`WaterSteepest`
decomposition, the same neighbour table in the same winding, and the same three
`erosionMode` branches. Both implement Schott et al. 2023's GPU stream power
erosion. `StreamPowerErosion` is the reference C++/GLSL; the WebGPU repo is the
port with a painting UI.

The only substantive divergence is parameter tuning, and one of the changes
matters: the WebGPU port drops `p_sl` (the slope exponent `n`) from 2 to 1,
making incision linear rather than quadratic in slope, which materially weakens
valley deepening. It also drops `k` 0.0005 → 0.05 and `dt` 50 → 2. Read those
as compensation for its 8-bit height field, not as better defaults — **take the
GLSL's values** (`k=0.0005, p_sa=0.8, p_sl=2, dt=50`) as our starting point.

**Both operate on a single flat 2D scalar grid, and neither uses a mesh at all.**
`StreamPowerErosion`'s state is `ScalarField2 : public Array2` — a 2D array of
doubles — and it renders with `glDrawArrays(GL_TRIANGLE_STRIP, 0, 4)` into
`heightfield_raytrace_330.glsl`: four vertices, a screen quad, raymarched. The
WebGPU repo does the same via `TerrainQuad extends Quad` and
`terrainRaymarch.wgsl`. There is no terrain geometry in either codebase.

This is the load-bearing gap. The *solver* transplants cleanly — it is
grid-local, 8-neighbour, ping-pong, and indifferent to what the grid represents.
The *domain* does not: a square patch with hard zero-clamped borders is not a
closed sphere, and our terrain is voxel/Tree64 rather than a height field. That
mismatch is what decision D1 resolves, and it is why the ocean mask has to
become the outlet boundary condition in place of their domain edge.

- `ignore/StreamPowerErosion/data/shaders/spe_shader.glsl` — 182 lines, one
  compute kernel, five SSBOs, driven by `code/spe_shader.cpp` (138 lines) as a
  bare ping-pong loop: bind 5 buffers, dispatch, `glMemoryBarrier`, swap. That
  is the entire simulation.
- `ignore/WebGPU-Erosion-Simulation/src/sample/terrainX/erosion.wgsl` — 353
  lines, the same kernel plus brush/paint editing of the uplift field.

The kernel, per cell, per iteration:

1. D8 steepest-descent receiver `d` over the 8 neighbours.
2. Drainage area by **gather**: sum `stream[q]` over the 8 neighbours `q` whose
   own steepest receiver is me. No atomics, no sort, no graph reduction.
3. Stream power incision `E = k · A^m · S^n` (`p_sa = m ≈ 0.8`, `p_sl = n ≈ 2`).
4. Optional hillslope diffusion `−k_h · ∇²z` and debris term `−k_d · S`.
5. `z = max(z, z_receiver)` — monotone, so depressions cannot form and no
   depression-routing pass is ever needed.
6. `z += dt · U · upliftMap[cell]`.

**The cost model follows from step 2.** Drainage area propagates exactly one
cell per iteration, so a domain of side N needs O(N) iterations before the
downstream half of the field has seen its true catchment. This is why the
per-iteration cost is trivial and the iteration count is not. The paper's
comparison table puts it precisely: FastFlow routes an exact 1000×1000 flow
graph in 2 ms via O(log n) graph reduction; SPE needs a number of iterations
proportional to the longest flow path.

### What to copy, and what not to

Copy: the kernel structure, the gather-based accumulation, the `max(z, z_recv)`
monotonicity trick, the uplift map as the primary authoring control, and the
three erosion modes as a runtime switch.

Do **not** copy these four things:

- **`rgba8unorm` elevation.** The WebGPU port stores the height field in an
  8-bit storage texture because that was the only WebGPU storage format
  available (their README section 6 discusses this for the stream buffer and
  never fixes it for elevation). 256 height levels planet-wide is not usable.
  Use `f32` storage buffers.
  Note that they tried this first and abandoned it:
  `src/sample/terrainX/streamPowerErosion.compute.wgsl` is an `array<f32>`
  storage-buffer transliteration, header-commented *"not in use, just for
  reference"*, not imported by `main.ts`, and not actually valid WGSL — it
  declares `var<storage, write>` and overloads `ToIndex1D`. Their dead end is
  our starting point, and it is dead for reasons that do not apply to us: we
  are not bound by WebGPU's storage-format restrictions.
- **The steepest-flow buffer race.** `erosion.wgsl:143` — `GetFlowSteepest(p, id)`
  writes `steepestFlowBuffer[id]`, but `WaterSteepest` calls it eight times with
  *neighbour* coordinates and *my* `id`, so each neighbour's direction is
  written into my slot. The read path `GetFlowSteepestFast` is behind
  `if (false)` at line 162 — the optimisation their README section 2 reports as
  a large win is disabled in shipped `main`. Get the win properly by splitting
  into two kernels (see phase 1).
- **The `workgroupBarrier()` at line 317.** No workgroup memory is used; it is
  decorative.
- **Zero-clamped borders.** `z = 0`, `A = cellDiag` on the domain edge is
  correct for a square patch and wrong for a closed sphere.

### What neither repo has

No sediment. SPE is a **bedrock-only detachment-limited** model: eroded mass
leaves the system entirely. Mass is not conserved, so there are no alluvial
fans, no valley fill, no deltas, no floodplains. It carves excellent drainage
networks and produces nothing that accumulates. Also: no soil, no rock types,
no spherical topology.

### The paper is the tier above

McDonald & Cordonnier's stochastic geomorphological transport is the method
that fixes exactly that gap — Lagrangian particles carrying water, sediment and
**momentum**, yielding meanders, braids, deltas and debris-flow scars, with
explicit deposition (Eqs. 4, 7) and a mass/momentum-conserving derivation.
Their measured cost is 51 ms per full iteration at 1024² on an RTX 5070 Ti,
over 1000–4000 timesteps: 1–10 minutes per planet-scale run. That is ~25× SPE's
per-iteration cost. Their open implementation is `erosiv/geotransport`.

Treat it as phase 6, not phase 1. Its deposition model, however, informs the
sediment layer in phase 3, and its parameter table (Appendix C) is a good source
of physically-scaled defaults.

---

## 2. Where the planet generator stands today

### It is entirely on the CPU, and it erodes twice with two different models

`src/runtime/procedural/SphericalTerrain.cpp` is 2,797 lines of single-file
CPU generation. It runs erosion in two unrelated places:

| | file:line | model | default iterations |
|---|---|---|---|
| graph | [SphericalTerrain.cpp:1340](src/runtime/procedural/SphericalTerrain.cpp#L1340) `erodeSphericalGraph` | implicit SPL on the Voronoi graph, + thermal + ridge sharpen + soil creep | 10 |
| raster | [SphericalTerrain.cpp:618](src/runtime/procedural/SphericalTerrain.cpp#L618) `erodeSphericalRasterMaps` | **ad-hoc**, not stream power | 10 |

The graph pass is legitimate: it is an implicit SPL relaxation in
drainage-descending order, which is the correct serial formulation.

The raster pass is not a physical model. It keys incision off the *river mask*
(`smoothstep((river - 0.035) / 0.70)`, line 662) rather than off drainage area,
adds a deposition term that is a function of slope alone, and clamps every land
height into `[0.025001, 0.995]` — a hard ceiling that silently flattens any
relief that reaches it. Drainage is computed separately and beforehand by
`routeRainfallRunoff` ([:434](src/runtime/procedural/SphericalTerrain.cpp#L434)),
so incision and routing are never coupled within an iteration.

**Ten iterations at 2048×1024 is not convergence, it is a texture.** The sample
defaults are `raster.width = 2048` ([PolygonTerrainSample.cpp:204](samples/polygonTerrain/PolygonTerrainSample.cpp#L204))
and `planetHydraulicIterations = 10` ([:154](samples/polygonTerrain/PolygonTerrainSample.cpp#L154)).
Ten iterations moves drainage information ten cells across a 2048-cell domain.

### There is already a GPU erosion pass — and the planet does not use it

`samples/polygonTerrain/PolygonTerrainHydraulicErosion.wgsl` (181 lines) is a
complete Mei et al. shallow-water pipe model: three kernels (`flux_main`,
`hydraulic_main`, `transport_main`), ping-pong state and flux textures, a
climate texture, sediment capacity, deposition, and integrated discharge.

It is wired to the **flat** polygon-terrain path in the sample
([PolygonTerrainSample.cpp:2709](samples/polygonTerrain/PolygonTerrainSample.cpp#L2709)),
never to `SphericalTerrainGenerator`. Two things in it are worth more than the
rest of the file and must survive this refactor:

- `atlas_coord` (lines 15–37) — correct pole reflection for an equirectangular
  atlas: crossing a pole mirrors latitude and advances longitude by 180°.
  Clamping instead, as the naive version does, builds a horizontal wall at each
  pole and produces the angular flow artifacts around them.
- `longitude_metric` (lines 54–58) — the `cos(latitude)` cell-width correction,
  floored at 0.20.

That floor is worth noting: at 2048 wide the polar rows are ~5× narrower than
the metric admits, so flow within roughly 11° of each pole is quantitatively
wrong. It is a bound on artifacts, not a fix. See the decision in phase 1.

### `material` is a biome byte; there is no geology

`PolygonTerrainMaterial` ([PolygonTerrain.hpp:42](src/runtime/procedural/PolygonTerrain.hpp#L42))
is 21 values — `Ocean, Lake, Beach, Grass, Rock, Snow, River, Marsh, Ice,
Tundra, Bare, Scorched, Taiga, Shrubland, TemperateDesert, …, SubtropicalDesert`.
Every one is **land cover**. `Rock` and `Bare` are the only entries that gesture
at substrate, and both are assigned by a Whittaker climate lookup, not by
geology: `classifyGraphMaterial`
([:167](src/runtime/procedural/SphericalTerrain.cpp#L167)) branches on
temperature and precipitation with elevation as a tiebreak.

Consequences, all of them visible:

- Erodibility is uniform. There are no cliffs, no benches, no hogbacks, no
  resistant caprock, no differential valley cross-sections.
- A voxel column is one material all the way down. Digging into a grass hill
  yields grass-coloured voxels forever.
- Deposits and bedrock are indistinguishable, because there are no deposits.

The package format mirrors this: 18 raster channels
([PlanetPackage.hpp:56](src/runtime/procedural/PlanetPackage.hpp#L56)) with
nothing for rock type, regolith depth, or sediment thickness.

### The two hooks we need already exist

This is the part that makes phase 4 cheap rather than a rewrite:

- `VoxelMaterialContextField::DepthBelowSurface`
  ([VoxelMaterialContext.hpp:17](src/voxel/material/VoxelMaterialContext.hpp#L17))
  is already a declared, plumbed context field. Stratigraphy is a consumer of
  it, not a new mechanism.
- `PlanetVoxelGeneratedMaterialField { Grass, Dirt, Variation }`
  ([PlanetVoxelChunk.hpp:135](src/voxel/svraster/PlanetVoxelChunk.hpp#L135))
  plus `VoxelMaterialFieldSource::PlanetRegion`
  ([VoxelMaterialField.hpp:29](src/voxel/material/VoxelMaterialField.hpp#L29))
  is an existing per-region R8 field channel from the planet into the material
  atlas. Soil/lithology fields are new *semantics* in an existing pipe.

Note the ceiling: `kVoxelMaterialFieldCount = 4`. Three slots are spoken for.
Phase 4 has to either fit in one slot or raise that constant — decide it there,
explicitly.

---

## 3. Decisions to settle before coding

Three of these change the shape of the work. I state my recommendation; the
first two are worth your explicit sign-off before phase 1 lands.

**D1. Erosion domain: equirectangular atlas, or cube faces?**
The package is already a cube quadtree, and the six-face domain has no polar
singularity — only edge seams, which the baker already handles with gutters
(`buildSphericalTerrainCubePackage`). Equirect is less work now and keeps
`atlas_coord` as-is, but bakes in the 11°-polar error permanently and wastes
~35% of the texels on oversampled polar rows.
**Recommend: cube faces, six 2D domains with a 2-cell cross-face gutter,
exchanged between iterations.** It is one extra kernel (gutter exchange) and it
retires the `longitude_metric` floor entirely.

**D2. Who owns fluvial relief — the graph, or the raster?**
Today both do, in sequence, with different models. Running an ad-hoc raster pass
on top of a converged graph SPL solve is how you get relief that answers to
neither model.
**Recommend: the Voronoi graph keeps tectonics, plates, climate and lakes; GPU
SPE owns all fluvial and hillslope relief.** Delete `erodeSphericalRasterMaps`
rather than porting it. Keep `erodeSphericalGraph` only as the CPU reference
for the parity test in phase 1.

**D3. Determinism and the package content hash.**
`PlanetPackageMetadata::contentHash` and the whole `PlanetVoxelChunk` transient
cache assume regenerating a planet yields identical bytes. GPU float reduction
order is not guaranteed stable across drivers or even dispatch shapes. The
gather in step 2 sums at most 8 values in fixed neighbour order, so this is
tractable — but it must be *tested*, not assumed.
**Recommend: bit-exactness is a phase-1 gate, not a phase-1 hope.** If it fails,
quantise the height field to a fixed-point grid at the end of every iteration.

**D4. Where does the erosion pass live?**
`samples/polygonTerrain/` is a sample. The planet baker calls
`writeSphericalTerrainPackage` from that same sample
([:567](samples/polygonTerrain/PolygonTerrainSample.cpp#L567)), so a GPU device
is already available at bake time — but the pass itself belongs in the runtime.
**Recommend: `src/runtime/procedural/rendering/` with the WGSL under
`assets/shaders/procedural/wgsl/`, matching the `wgsl2spirv` convention.** The
sample keeps only the UI.

---

## 4. The plan

### Phase 0 — Baseline and noise floor

Nothing here is optional. Every later phase is judged against these numbers,
and past work on this codebase has been voided by skipping this step.

- Wall-clock `SphericalTerrainGenerator::generate` at 2048×1024 and 4096×2048,
  split into graph / raster / erosion / climate / classification.
- **Run each three times and record the spread before quoting any single
  number.** Prior sweeps on this engine were invalidated by a 3× run-to-run
  variance that nobody measured first.
- Freeze a reference planet (fixed seed) and dump: hypsometric curve, drainage
  area histogram, slope–area log-log plot (the diagnostic that tells you whether
  a fluvial model is doing anything real: a converged SPL field shows a clean
  break between the hillslope and channel regimes), and the height field itself
  as a PNG for eyeballing.

Deliverable: `docs/planet-generator-baseline.md` with the numbers and plots.
Gate: none. This phase cannot fail; it can only be skipped, which is the failure.

### Phase 1 — GPU stream power erosion

Port SPE into the runtime, correctly, and make it the only fluvial model.

New files:
- `assets/shaders/procedural/wgsl/planet_stream_power.wgsl`
- `src/runtime/procedural/rendering/PlanetErosionPass.{hpp,cpp}`

Kernel split — this is the fix for the race, and it buys the README's claimed
optimisation as a side effect:

- `receiver_main` — computes each cell's D8 steepest receiver once and writes
  a `u32` direction index. One write per cell, by that cell only.
- `accumulate_main` — gathers drainage area from the 8 neighbours whose stored
  receiver points at me. Reads only; the eight redundant recomputations per cell
  in the reference disappear.
- `incise_main` — SPL + optional Laplacian + optional debris term, then
  `max(z, z_recv)`, then uplift.
- `gutter_main` (D1 = cube faces) — cross-face edge exchange.

Design points:

- `f32` storage buffers for `z`, `A`, `uplift`, and the receiver `u32` buffer.
  Never `rgba8unorm`.
- Iteration count derived from resolution, not a constant. Start at `2 × side`
  and cut it with the phase-0 slope–area plot: iterate until the channel-regime
  slope stops moving.
- Batch dispatches between fences the way
  `TerrainPreviewHydraulicErosionPass::submitBatch` already does
  ([PolygonTerrainPreviewPasses.cpp:748](samples/polygonTerrain/PolygonTerrainPreviewPasses.cpp#L748));
  reuse that submission structure and its `poll`/`progress` contract, which the
  sample UI already speaks.
- Ocean cells are the boundary condition: fixed `z = 0` and seeded `A`,
  replacing the reference's domain-edge clamp. On a sphere there is no edge, so
  the ocean mask *is* the outlet.
- Uplift comes from the existing `tectonics` map — that map is exactly the
  reference's `upliftBuffer`, already generated, already exported.

Removals: `erodeSphericalRasterMaps` and its call site
([:2686](src/runtime/procedural/SphericalTerrain.cpp#L2686)). Keep
`routeRainfallRunoff` for now — the river *mask* is still consumed by material
classification — but note that phase 3 makes it redundant, since `A` is a
strictly better discharge proxy than a re-routed mask.

Gates, all three required:
1. **Bit-exact across two runs of the same seed** (D3). Fails → fixed-point
   quantise per iteration and re-test.
2. Slope–area plot shows a clean hillslope/channel break; the phase-0 plot from
   the 10-iteration CPU pass does not, and that contrast is the proof the model
   is doing something.
3. Total generation time for the reference planet does not regress. If SPE at
   `2 × side` iterations is slower than the CPU pass at 10, say so plainly in
   the doc — the output is not comparable, and the honest framing is
   "converged relief now costs X", not "erosion got faster".

### Phase 2 — Lithology: spatially varying erodibility

The single highest-visual-return change in this plan, and it costs one buffer
read in the inner loop.

`k` becomes `k(x)` sampled from a rock-type field. Differential erosion is what
produces cliffs, structural benches, resistant caprock, mesas, waterfalls at
lithological contacts, and asymmetric valley cross-sections — none of which a
uniform-`k` field can produce at any iteration count.

- New enum `PlanetLithology` in `src/runtime/procedural/PlanetGeology.hpp`:
  start with `Sedimentary, Limestone, Sandstone, Shale, Granite, Basalt,
  Metamorphic, Volcanic`, each carrying `erodibility`, `talusAngle`,
  `regolithYield`, and an albedo/roughness pair.
- Generated from data the planet already has: plate type
  (`Plate::continental`) picks the basement suite; `tectonics` drives
  metamorphic and intrusive grades along convergent belts; volcanic follows
  divergent boundaries and hotspots; the `sphereFbm` machinery at
  [:125](src/runtime/procedural/SphericalTerrain.cpp#L125) supplies the
  stratigraphic banding. Bands should follow elevation contours in
  sedimentary provinces — that is what makes layered mesas read as layered.
- New raster channel `PlanetRasterChannel::Lithology` (R8UInt, `Mode` mip
  policy — same treatment `Material` already gets).
- Feed `erodibility` into `incise_main` as a per-cell multiplier on `k`, and
  `talusAngle` into the hillslope term.

Gate: side-by-side renders of the reference planet with uniform vs varying `k`.
If cliffs and benches are not obvious at a glance, the erodibility contrast
range is too narrow — real rock spans well over an order of magnitude in `k`.

### Phase 3 — Sediment and regolith: make mass conserve

SPE destroys mass. This phase adds the layer that receives it.

Two fields instead of one: `bedrock` and `regolith` (soil/sediment thickness),
with surface `z = bedrock + regolith`.

- **Production.** Bedrock weathers into regolith at a rate set by lithology
  (`regolithYield`), climate (existing `temperature`/`precipitation` maps), and
  an exponential decay in existing regolith depth — thick soil armours the rock
  beneath it, which is the standard soil-production function and the reason
  soil depth self-limits.
- **Transport.** Regolith moves by hillslope diffusion (the existing Laplacian
  term, but applied to regolith rather than bedrock) and by fluvial transport
  along the D8 receiver chain.
- **Deposition.** Where transport capacity falls below load, deposit. Use the
  paper's formulation, Eq. 4: `D = k_d · h_s / h_f` — deposition proportional to
  the ratio of sediment to water thickness. Their Appendix C gives
  `k_{d,f} = 0.05` as a starting coefficient.
- Incision now acts on regolith first and only reaches bedrock when regolith is
  exhausted — the transport-limited/detachment-limited distinction, and the
  thing that makes valley floors read as filled rather than scoured.

New channels: `RegolithDepth` (R16UInt, `Average`) and `SedimentFlux`
(R8UNorm, `Maximum`, for authoring feedback and river visuals).

Gate: alluvial fans at range fronts, valley fill upstream of constrictions, and
deltas where major channels meet the ocean mask. Total mass (bedrock lost vs
regolith gained vs ocean export) should balance to within a small tolerance —
instrument this, since a leak here is invisible in renders and lethal to
long runs.

### Phase 4 — Split `material` into a geology triple

The package and the voxel side stop pretending one byte describes the ground.

Replace the single `material` channel with three:

| channel | meaning | format |
|---|---|---|
| `SurfaceCover` | the existing 21 biome values, unchanged | R8UInt, Mode |
| `Lithology` | phase 2 rock type | R8UInt, Mode |
| `RegolithDepth` | phase 3 soil thickness in metres | R16UInt, Average |

Keep `PlanetRasterChannel::Material` emitting `SurfaceCover` so nothing breaks
on day one; the package reader already tolerates missing channels via
`findChannel`.

Voxel side, using the hooks from §2:

- `PlanetVoxelGeneratedMaterialField` gains `Regolith` and `Lithology`.
  `kVoxelMaterialFieldCount = 4` is already full at Grass/Dirt/Variation + 1 —
  decide here whether to raise it to 8 (touches `VoxelMaterialFieldResolverGPU`,
  currently `static_assert`-ed at 48 bytes, and the atlas RGBA8 packing) or to
  pack lithology into `Variation`'s spare bits. **Recommend raising it to 8**;
  the packing alternative will be regretted the first time a material wants both.
- Wire `VoxelMaterialContextField::DepthBelowSurface` to select the stratum:
  `0 → surface cover`, `< regolithDepth → soil`, `< regolithDepth + saprolite →
  weathered rock`, else `→ bedrock of the local lithology`. This is what makes
  digging reveal a soil profile instead of infinite grass.
- Extend `planetVoxelSurfaceMaterial`
  ([PlanetVoxelChunk.hpp:151](src/voxel/svraster/PlanetVoxelChunk.hpp#L151)) to
  take lithology and regolith depth alongside its existing `slope`/`mountain`
  arguments.

Gate: cut a vertical section through a hillside in the voxel viewer and see
cover → soil → saprolite → bedrock, with the bedrock colour changing across a
lithological contact.

### Phase 5 — Authoring and iteration speed

Once the model is right, make it usable. The WebGPU repo's real contribution
beyond the port is its interactive uplift painting, and it transfers directly:
`DrawPaint`/`DrawBrush` in `erosion.wgsl:227–284` edit the uplift field in place
between iterations, which is precisely how Schott et al. intend the system to
be authored.

- Uplift/rainfall/erodibility brushes in the planet sample, editing the field
  buffers between dispatch batches.
- Live preview at reduced resolution (512²/face) with a full-resolution bake on
  commit.
- Expose `k, k_h, k_d, m, n, dt` and the lithology table in the sample UI, and
  persist them into the package metadata so a planet records how it was made.

### Phase 6 — Stochastic transport (experimental, gated on phases 1–4)

Only after the above ships. Implement McDonald & Cordonnier's Algorithm 1 as an
alternative solver behind the same field interface: N particles per iteration,
each integrating `y ← y + Δx·v/‖v‖` and depositing `Ŝ·M` into the cells it
crosses, with `M ← M·e^(−Δx·R/‖v‖)`.

What it buys that phases 1–3 cannot: momentum conservation, and therefore
meanders, braided channels, and deltas as *emergent* features rather than
authored ones (their Fig. 5 ablation shows all three vanish when momentum is
removed). Also debris-flow scars via Eqs. 5–8.

What it costs: 51 ms/iteration at 1024² on an RTX 5070 Ti, and it does not reach
a steady state — they report a *dynamic* steady state where river trajectories
keep oscillating. That is a feature for a time-lapse and a problem for a bake
that must terminate.

Scope it to hero regions, not whole planets. Their §7.5 is candid about the
limitation that matters here: the quasi-static assumption fails when erosion is
fast relative to transport, and the method is inaccurate for glaciers.

---

## 5. Sequencing and risk

```text
phase 0  baseline            ── required, blocks everything
phase 1  GPU SPE             ── D1, D2, D3, D4 signed off first
phase 2  lithology           ── independent of 3, do it first: best ratio
phase 3  regolith/sediment   ── depends on 1
phase 4  package + voxel     ── depends on 2 and 3
phase 5  authoring           ── depends on 1
phase 6  stochastic transport── experimental, after 4
```

Phases 2 and 3 are independent of each other and both depend only on 1. If time
is short, **phase 2 alone changes the look of the planet more than any other
single item here**, because uniform erodibility is the reason the current
terrain reads as noise-driven rather than geology-driven.

Risks worth naming now:

- **Determinism (D3)** is the one that can force a redesign rather than a fix.
  Test it in phase 1, first thing, on the smallest possible domain.
- **Iteration count is the real cost.** SPE's O(N) convergence means a 4096-wide
  planet wants thousands of iterations. If that proves too slow, the escape
  hatch is the graph-reduction approach SPE was measured against (FastFlow,
  O(log n) routing) — but that is a different algorithm, not a tuning knob, so
  budget for it as such.
- **`kVoxelMaterialFieldCount = 4`** is a hard wall in phase 4 with a
  `static_assert` behind it. Do the arithmetic on the atlas footprint before
  committing to 8.
- **Two erosion models coexisting** is the current state and must not become the
  end state. If phase 1 lands without deleting `erodeSphericalRasterMaps`, the
  refactor has failed regardless of how good the new solver is.
