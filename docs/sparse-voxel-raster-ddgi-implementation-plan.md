# Sparse voxel raster and hybrid DDGI implementation plan

Status: active implementation sequence, 2026-08-14. R0 is landed. R1/R2 are
implemented and correctness-qualified as an opt-in experiment, but their
performance acceptance failed and their last structural explanation has now
been tested and refuted, so the incumbent raster remains the default and R2 is
rejected rather than pending. R3 is implemented, exact and now covered by a
gate, but inactive at the current extraction density. R4 onward and the DDGI
stages remain planned.

This plan covers two related goals:

1. reduce the sparse brick raster cost at the intended terrain quality; and
2. add a world-anchored DDGI cache that complements, rather than immediately
   replaces, the existing camera-adaptive surface probes.

The plan is specific to `SparseVoxelRendererSample`. It does not supersede
`full-gi-pipeline-plan.md`, whose engine-wide architecture deliberately avoids
depending on an older DDGI backend. The DDGI work below is an isolated,
feature-gated pilot with explicit comparison and removal criteria.

## Baseline and success criteria

All performance captures must use:

- output and internal resolution 1200 x 720;
- raster LOD threshold 0.75 px;
- Tree64 LOD threshold 1.5 px;
- TAA, MetalFX, frame generation, vsync, and built-in GPU timestamps disabled;
- the default terrain package and the same camera-walk sequence; and
- an Xcode Metal System Trace for authoritative GPU occupancy.

The current intended-quality checkpoints are:

| Metric | Current result |
| --- | ---: |
| Source cadence after the Interior shortcut | 36.432 ms p50 / 40.881 ms p95 |
| Median source FPS | 27.45 FPS |
| Phase-zero brick raster | 16.884 ms p50 |
| Brick setup through G-buffer | 21.455 ms p50 |
| GPU utilization | 99.34% of the traced frame span |

The primary performance target is less than 25 ms p50 and p95 without an
upscaler or frame generation. Quality acceptance requires stable camera motion,
no missing microgeometry, no new LOD seams, no probe-grid leaks, and no static
camera colour pulsing.

## Part I: remaining raster implementation

### R0. Freeze instrumentation and exact-output gates

Before changing representation, retain diagnostic-only counters for:

- visible bricks and projected occupied-box area;
- fragment invocations, covered pixels, and peak/average proxy overdraw;
- scale-4 groups tested and skipped;
- scale-2 cells visited and fine occupancy probes;
- first-hit exits, misses, and carved-parent continuations;
- micro traces by Interior, Flat, Slope, Wall, and general volume context; and
- MicroPrepare records, residual parents, overflow bricks, and output bytes.

The fragment counters perturb timing and therefore never supply performance
numbers. Performance comes from Metal counters and encoder intervals with all
diagnostics disabled.

For every exact-quality change, compare the full resolved surface and G-buffer:

- hardware depth;
- surface identity and temporal identity;
- packed material and bottom layer;
- micro payload;
- canonical material sample address;
- geometry and shading normal; and
- final output signature.

### R1. Connect MicroPrepare to residual brick rasterization

**Implemented behind `--brick-micro-raster`.** MicroPrepare transforms one visible
brick per workgroup, writes compact 16-byte micro-parent records, removes
accepted parents from a residual 512-bit occupancy mask, rebuilds the residual
4 x 4 x 4 mask, and uses an all-or-nothing reservation so overflow cannot
create holes. `--brick-micro-prepare-diagnostic` now adds only the sizing scan
and readbacks to that experimental path.

The opt-in Phase0 path is:

```text
Phase0 Cull
  -> MicroPrepare Reset
  -> MicroPrepare Transform
  -> Legacy Brick Draw over residual occupancy
  -> Direct prepared-parent draw over compact records
  -> Sparse prepared-parent tie replay
  -> Phase0 depth/HZB
```

Requirements:

- retain the incumbent path as the default and allow it to be named explicitly
  with `--disable-brick-micro-raster`;
- create no buffers or passes when the feature is disabled;
- make residual occupancy index by visible-list slot, never by reusable node
  residency slot;
- accept or reject every brick's extraction atomically;
- leave every rejected/overflow parent in legacy occupancy; and
- preserve phase-one semantics. Phase-one newcomers either need their own
  transform or must remain entirely on the legacy path for the first pilot.

The first acceptance metric is the complete prepare plus all three raster
passes, not the direct prepared draw in isolation.

### R2. Specialized heightfield parent raster

**Implemented.** The direct prepared-parent draw consumes the compact record;
a separate sparse tie-replay draw preserves incumbent boundary/side ownership
without returning the direct fragment to the full-brick dependency set.

Do not expand a parent into N cubed microvoxels. The initial specialized draw
uses one compact record per parent and an 18-vertex tight parent proxy. Its
fragment shader performs only the existing exact 2D column intersection:

- no outer brick slab intersection;
- no 4-cubed coarse traversal;
- no fine parent search;
- no dependent material-prefix lookup; and
- no general volume branch.

MicroPrepare resolves the node, parent index, material slot, palette address,
descriptor, context, and surface patch once. The specialized shader reconstructs
the same canonical parent key, microcell, hit face, distance, material payload,
and temporal identity as the legacy fragment.

Start with only parents that are guaranteed to select the exact path for every
entry point. Any stochastic handoff parent remains in residual occupancy until
the representation has an exact ownership rule.

Acceptance:

- byte-identical outputs to the legacy path;
- zero double ownership between specialized and residual paths;
- no missing pixels under forced output capacity exhaustion; and
- a net reduction in Phase0 after including MicroPrepare.

#### R2 progress, 2026-08-14 (later): residual passthrough, and where the cost is not

Extraction density is the fact that frames this stage. On the default terrain
package at 1200 x 720, one camera-walk frame has 13,499 visible bricks holding
**2,890,365** occupied parents, and camera-facing eligibility accepts
**61** of them. The direct and replay draws are not cheap because they are fast;
they are cheap because they draw 61 records.

Two changes were measured against that.

**Kept - residual passthrough.** A brick that extracts nothing has a residual
mask that is byte-for-byte its source occupancy, yet MicroPrepare still restated
all eighteen words per brick: about **19 MB of store traffic per frame** to
publish data the draw already had. `SparseVoxelMicroResidualGPU` now carries a
`passthrough` flag; non-extracting bricks write that single word, and the draw
reads the immutable node occupancy and the node's own coarse mask instead. The
fragment path costs nothing extra because `entry` is already loaded for the
material prefix. Whole-frame gap fell from about **+2.5 ms to +1.60 ms p50**
(spreads 0.13/0.27), output byte-identical across all gates.

**Reverted - whole-brick eligibility early-out.** A conservative per-brick bound
(most permissive handoff scale 0.25, minimum subdivision 2, distance to the
nearest point of the occupied box) correctly skipped the 512-parent scan without
dropping a single record - extraction stayed at exactly 61. It measured
**neutral**: +1.82 ms against +1.60 ms, both inside the run spreads. Removed
rather than kept, because unmeasured complexity on a conservative bound is a
silent-microgeometry-loss risk. The useful result is negative and worth keeping:
**MicroPrepare's cost is not the parent scan.** It sits behind the per-brick
occupancy load and the workgroup barrier, so a cheaper decision changes nothing.

That leaves one untested hypothesis for the remaining ~1.6 ms. The split runs
**three** draws over the same four-MRT brick target where the incumbent runs one.
On a tile GPU each additional render pass reloads and restores those attachments:
40 bytes per covered pixel over 864,000 pixels is roughly 34.5 MB per pass, so
two extra passes cost about 69 MB of traffic per frame - the right order of
magnitude for the gap, and independent of how little work the extra draws do.
Testing it requires merging the residual, direct, and replay draws into one
encoder rather than three passes. Do that before any further per-kernel tuning.

#### R2 render-pass hypothesis, 2026-08-14 (later): refuted, and R2 ends

The merge exists and is byte-exact. `SparseVoxelBrickDrawPass` now takes a list
of `Source`s and records all of them inside a **single** `cmdBeginRenderPass`;
`--brick-micro-merged-draw` selects it. The R2 gate asserts that the merged graph
registers `MergedMicroDraw` and registers *none* of `ResidualDraw`,
`PreparedMicroDraw`, `PreparedTieReplayDraw`, so the two render passes are
provably gone rather than renamed, and every exactness case runs a third merged
arm.

It changed nothing. Three interleaved repeats per arm, `--benchmark-lighting`
aggregates, GPU timestamps off, canonical 1200 x 720 camera walk, LOD 0.75/1.5:

| Arm | p50 (spread) | p95 (spread) |
| --- | ---: | ---: |
| incumbent | 24.043 ms (0.017) | 25.642 ms (0.270) |
| R2 split, three passes | 25.865 ms (0.038) | 27.687 ms (0.310) |
| R2 merged, one pass | 25.940 ms (0.184) | 27.538 ms (0.058) |

Merged versus split is **+0.074 ms p50 and -0.149 ms p95**, both inside the run
spreads and opposite in sign. Removing two full-screen four-MRT load/store cycles
per frame - the predicted ~69 MB - is not measurable in this frame. The tile
attachment-traffic explanation for the gap is wrong, and the residual +1.82 ms
against the incumbent reproduces at full size.

So the hypothesis that was holding R2 open is closed, and it closed negatively.
**R2 is rejected on cost, not deferred.** The remaining candidates were already
identified above and none of them is a render-pass property: MicroPrepare scans
every visible brick unconditionally, and `ResidualDraw` retains almost all of the
incumbent's traversal, so the split pays for the incumbent's work plus a
preparation pass and gets 61 extracted parents for it. Reopening R2 requires a
different extraction rule, not another encoder experiment.

The multi-source draw pass is retained. It is byte-exact, gated, and the
single-source path it now shares is the default renderer's own; R9's hybrid
visibility raster needs exactly this shape. `--brick-micro-merged-draw` is
recorded in benchmark JSON as `brick_micro_raster_mode`
`"r2-heightfield-parent-merged"`, because an arm identified only by its output
file name cannot be told apart afterwards.

#### R2 outcome, 2026-08-14: correct, rejected on cost, default off

The first three functional criteria are covered by
`SparseVoxelMicroRasterR1ExactEquivalence`,
`SparseVoxelMicroRasterR2DirectEquivalence`, and
`SparseVoxelMicroRasterR2ShaderContract`. The registered tests use the
deterministic synthetic fixture; a separate 1200 x 720 terrain capture also
matched all exact-output fields.

The fourth criterion is still missed. The initial broad-extraction experiment
cost 40.92 ms p50 against 24.19 ms for the incumbent. Camera-facing extraction,
the 18-vertex tight proxy, and complementary boundary replay reduced that gap
substantially. Two matched 120-warmup/360-measured built-in timestamp repeats,
plus a post-fast-path R2 attribution capture, on the canonical 1200 x 720
camera walk, LOD 0.75/1.5, now report:

| Arm | observed p50 range | observed p95 range |
| --- | ---: | ---: |
| `--brick-micro-raster` | 26.12--27.03 ms | 28.14--29.49 ms |
| incumbent | 23.63--24.30 ms | 24.82--26.69 ms |

The sum of independently aggregated per-pass p50 values is attribution only,
not the percentile of a per-frame Phase0 sum. It grows from 5.82--5.84 ms for
incumbent Reset, Cull, DrawArgs, and Draw to about 8.00 ms for the corresponding
R2 chain. The largest added costs are MicroPrepare Transform (1.62--1.68 ms) and
ResidualDraw (6.20--6.24 ms); the camera-facing direct and tie passes are nearly
empty at the median terrain frame.

Re-summing each captured frame before calculating percentiles, including the
common Phase0 HZB, gives 8.29--8.37 ms p50 / 8.66--9.13 ms p95 for R2 versus
6.16--6.20 ms p50 / 6.61--7.03 ms p95 for the incumbent. Benchmark schema 14
now emits this exact grouping as `brick_phase0_total_ms`.

This exposes the remaining structural problem:

- MicroPrepare still scans every visible brick even when its face eligibility
  rule extracts very few parents;
- `ResidualDraw` then retains almost all incumbent traversal work; and
- any broader extraction again increases direct/replay proxy coverage.

The replay remains load-bearing for exact edge/side ownership. Its filter is
the complement of the direct pass's discard rule, and candidate extraction is
limited to the descriptor face seen from the parent centre. These changes make
replay sparse, but they do not recover the unconditional preparation scan.
Built-in timestamps are useful for this negative attribution; an authoritative
positive acceptance would still require the diagnostics-off Metal trace defined
above.

Therefore the split is **off by default** and selected with
`--brick-micro-raster`. `brickMicroRasterEnabled` defaults to false.

Retaining it requires avoiding the unconditional scan/residual duplication and
ultimately resolving boundary ownership without a legacy replay. Until that
exists, R3-R5 should not assume a compact-record raster is the destination,
because they were sequenced behind an R2 win that has not happened. The one
structural explanation that remained for the gap has since been tested and
refuted; see the render-pass entry above.

### R3. Precompute compact heightfield columns

For the default subdivision N=5, one terrain parent has 25 column depths. Four
bits per depth require 100 bits, rounded to a 16-byte record. Generate these
once in MicroPrepare from the existing 24-bit four-corner patch and context.

This removes repeated fragment-side interpolation, quantization, staircase
classification, and carve-depth evaluation. It also supplies a stable macro
surface derivative without adding a normal to every source brick material.

If a normal is useful to the specialized path, append an octahedral 16-bit
macro normal to the generated micro-parent record. Do not add it to the current
8-byte compact material or another full-resolution brick MRT: both choices add
bandwidth to the hottest pass. The G-buffer already carries geometry and
shading normals and current direct illumination already evaluates a Lambertian
clamped-cosine lobe.

#### R3 status, 2026-08-14: plumbed and exact, generating but barely exercised

Implemented: `SparseVoxelMicroBrickRecordGPU` grew from 16 to 32 bytes with a
`columns[4]` cache, four bits per surface column, so the default subdivision of 5
stores all 25 depths in 100 bits and bit 31 of the last word marks it usable.
MicroPrepare generates it once per extracted parent; the prepared vertex stage
carries it in a flat varying; `sparseVoxelExactMicroCarveDepth` reads it instead
of re-deriving the bilinear patch, quantization and staircase classification per
visited column, and falls back to live evaluation whenever the valid bit is
clear.

Bit-identity is structural, not reviewed: the quantizer moved into
`sparse_voxel_micro_columns.wgsl`, which both MicroPrepare and the raster
fragment include, so the cached and live values come from one function. That
header deliberately depends only on the packed surface patch, keeping the
geometry-IR bindings out of MicroPrepare. All exactness gates pass.

**The gates do not cover the cache, and the workload barely exercises it.** A
mutation test - deliberately storing depth + 1 - changed no output on the
synthetic fixture or on terrain. The new `cachedRecords` counter explains why,
and the two numbers say different things:

| Workload | extracted records | cachedRecords |
| --- | ---: | ---: |
| `--test-micro-raster` fixture | 536 | **0** |
| default terrain, 1200 x 720 | 61 | **11** |

The fixture extracts plenty of parents but none are terrain staircase surfaces,
so the cache never generates there. That is the whole reason the mutation left
every gate green: **the exactness suite cannot see this feature at all.** Terrain
does generate it, for 11 parents - too little screen area for a one-cell depth
change to move a frame hash, which is why that comparison was also silent.

So the code path works; the coverage does not exist. Two things follow, in order:

1. **Give the fixture a staircase-surface material** so `cachedRecords > 0` there
   and the R1/R2 gates actually constrain the cache. Until then no test protects
   it, and the mutation test above is the only evidence it behaves at all. Assert
   `cachedRecords >= 1` in the R2 counter block so this cannot silently regress.
2. **Recognize that R3's payoff is bounded by R2's extraction density**, currently
   61 parents out of 2,890,365. A per-column cache cannot matter until far more
   parents take the prepared path. R3 is correctly built and correctly inactive;
   it is waiting on R2's eligibility rule, not on its own implementation.

#### R3 coverage, 2026-08-14 (later): closed, and the camera was the obstacle

Item 1 above is done. `SparseVoxelMicroRasterR2DirectEquivalence` now runs a
`COLUMNS` case and requires `cachedRecords >= 1`; the same mutation that used to
leave every gate green - storing `depth + 1` in `microPrepareColumnCache` - now
fails it on frame 0. That is the evidence the previous entry could not supply.

Two facts made this harder than adding a material.

**A staircase surface always carves through +Y, so the default framing can never
see one.** The default camera sits at the root centre roughly 2.2 model radii
along +Z, so for every parent the dominant camera-to-centre axis is Z and
`centreEntryFace` is +Z. `microPrepareTransformCandidate` rejects any parent
whose centre entry face is not the carve face, so no staircase parent extracts
under that camera no matter how many exist. The incumbent fixture descriptor is
HashColumns with a single +Z face, which is exactly why it extracted well there.
The registered top-down `--test-camera-dolly` camera is the only case that can
observe the cache; the new `COLUMNS` case is that camera with diagnostics on and
three frames. The gate also asserts the default framing still caches **zero**, so
this stays a deliberate observation rather than an accident.

**A surface context outranks an interior one during brick coarsening.** The first
attempt gave every third source material the staircase descriptor. `contextRank`
in `buildSparseVoxelBrickHierarchy` promotes a surface context over an interior
one when OR-reducing children into a coarser cell, so at the eight-brick level
this fixture actually renders, *all* 1,352 parents became staircase: contexts
went `interior=0 flat=0 slope=1352`, and extraction collapsed from 536 records to
**zero**. Confining the staircase material to the shell's top 15% of Y fixes both
halves at once - those are precisely the parents a top-down camera enters through
+Y, and the rest of the shell keeps its hash-column/legacy split.

The fixture now assigns three source material ids: hash-column micro, staircase
micro on the top slab, and an unbound legacy id. Because the scene ABI pins
`SparseVoxelMaterialGPU::flags` to zero - `validateBootstrapSparseVoxelModel`
rejects anything else, and `SparseVoxelBootstrapTests` locks that - the per-voxel
context and four-corner patch are stamped on `lodHierarchy.materials`, which is
the array `buildSparseVoxelBrickHierarchy` consumes. That is the same place
`PlanetVoxelChunk` publishes terrain's own context, so this is not a fixture-only
back door.

Resulting counters, three frames, 320 x 180, diagnostics on:

| Case | records | cachedRecords |
| --- | ---: | ---: |
| default framing | 410 | 0 |
| `--test-camera-dolly` | 68 | **68** |
| `--test-camera-interior` | 349 | 0 |

R1's cases are static, yaw and interior only, so R1 still does not constrain the
cache; R2 does. Item 2 above is unchanged: 68 cached records on a synthetic
fixture is coverage, not payoff, and the payoff still waits on extraction
density.

### R4. Evaluate generated heightfield geometry

Only after R2/R3 win, A/B explicit top and riser geometry:

1. generate exposed strips from the N x N column-depth tile;
2. greedily merge adjacent coplanar strips;
3. append vertices/indices into bounded per-frame buffers;
4. emit indirect arguments; and
5. rasterize the same integer payload and depth contract.

The current conservative upper bound is about 1.23 million unmerged quads.
That can be slower than parent proxies on Apple's tiler, so generated geometry
is retained only if prepare plus vertex plus fragment time improves and output
is exact. Porous/general volume materials never use this path initially.

### R5. Complete microgeometry specializations

Keep the retained Interior shortcut. Add, separately:

- a FlatSurface direct first-column path using exactly the incumbent WGSL f32
  divisions and tie ordering;
- a slope-only 2D heightfield kernel;
- a one-face lateral kernel for staircase wall contexts; and
- a small, explicit general-volume fallback.

Compile specialized pipelines rather than adding more divergent branches to
the current 140 KiB fragment SPIR-V. MicroPrepare classification selects the
pipeline and builds each indirect count.

### R6. Cost-aware raster LOD

Raw brick count is not a useful objective: the measured 6 px cut has fewer
bricks and is slower than 0.75 px because coarse occupied boxes cover more
pixels and perform more traversal.

Augment the cull decision with a conservative cost estimate:

```text
estimated cost = projected occupied face area
               * expected overdraw
               * material traversal weight
```

Compare a parent's estimate with the sum of its available children while still
enforcing the 0.75 px geometric error bound and crack-free parent/child cut.
Use slowly varying quantized cost bins and the existing temporal hysteresis so
the decision does not introduce a new source of LOD blinking.

### R7. Anisotropic power-of-two brick shapes

Add non-cubic brick shapes, but keep every axis a power of two and keep the
logical cell capacity at 512. This preserves shift/mask indexing and the
existing 16-word exact occupancy allocation while allowing the brick to follow
the local surface.

Initial shape presets are:

| Shape | Intended use | Coarse 2-cell grid |
| --- | --- | --- |
| 8 x 8 x 8 | General fallback | 4 x 4 x 4 |
| 16 x 16 x 2 | Nearly horizontal terrain | 8 x 8 x 1 |
| 16 x 2 x 16 | Thin horizontal layer under the engine's axis convention | 8 x 1 x 8 |
| 2 x 16 x 16 | Thin wall/slab | 1 x 8 x 8 |
| permutations of 16 x 8 x 4 | Slopes and elongated features | permutations of 8 x 4 x 2 |

The user's example `4 x 8 x 2` is useful as a compact 64-cell microbrick, but
it does not reduce source-brick count relative to an 8-cubed brick because it
covers only one eighth of the volume. For flat terrain, `16 x 16 x 2` retains
the existing 512-cell maximum, covers four times the horizontal footprint, and
can reduce both brick count and vertical proxy/DDA extent.

#### Node encoding

Replace the current six 3-bit occupied-bound fields with six 4-bit fields.
That supports cell coordinates 0..15. Use another six bits for the three
2-bit log2 axis extents:

```text
bits  0.. 3  occupied minimum X
bits  4.. 7  occupied minimum Y
bits  8..11  occupied minimum Z
bits 12..15  occupied maximum X
bits 16..19  occupied maximum Y
bits 20..23  occupied maximum Z
bits 24..25  log2(extent X) - 1       // 2,4,8,16
bits 26..27  log2(extent Y) - 1
bits 28..29  log2(extent Z) - 1
bits 30..31  encoding version/reserved
```

Keep `SparseVoxelBrickNodeGPU` at 96 bytes. Introduce CPU and WGSL helpers for:

- shape validation and cell count;
- variable-stride cell flatten/unflatten;
- occupied bounds;
- coarse dimensions and coarse-bit flattening;
- world extent and proxy bounds; and
- canonical parent coordinate reconstruction.

Land the encoding, helpers, and CPU/GPU parity tests together. Do not write new
metadata that old shaders interpret as the legacy 3-bit layout.

#### Hierarchy construction

Keep coarse hierarchy levels as cubic 8-cubed nodes. Retile only the finest
children inside each parent-sized 16-cubed world region. Every 512-cell preset
partitions that region into exactly eight children:

- eight 8 x 8 x 8 octants;
- eight 16 x 16 x 2 slabs; or
- eight bricks for any 16 x 8 x 4 permutation.

Choose one shape for the entire sibling group so children form a disjoint,
crack-free partition and retain one ordinary cubic parent. This keeps the
existing parent-versus-children LOD decision: a coarse parent is selected, or
all nonempty anisotropic children that replace it are selected.

The terrain page builder selects a shape per finest parent region. Generic
meshes initially retain 8-cubed leaves. Do not merge across terrain pages,
palette ownership, region mappings, or canonical clipmap ownership boundaries.

#### Shape selection objective

Do not minimize brick count alone. Evaluate every preset with a build-time cost
estimate over its eight candidate children:

```text
shape cost = nonempty brick setup cost
           + tight occupied-box surface area
           + empty coarse-cell traversal estimate
           + material/microgeometry dispersion penalty
```

For terrain, bias the estimate using the stored macro surface normal or height
range. A nearly horizontal 16-cubed parent should normally select 16 x 16 x 2;
a cliff should rotate the thin axis; a noisy/volumetric region should retain
8 x 8 x 8.

Shape choice is built data and must not change with the camera. Runtime
cost-aware LOD still chooses parent versus children, but never changes the
children's shape, preventing a new temporal blinking source.

#### Shader traversal

The vertex and cull shaders use `extent = voxelSize * vec3(shape)`. The
fragment shader intersects the tight occupied box exactly as today, then walks
the variable coarse grid. Every allowed preset has at most 64 coarse cells, so
the existing logical 64-bit two-word mask remains valid.

Fine occupancy remains a fixed 512-bit allocation with variable strides. The
material prefix is still the popcount before the flattened bit, so material
storage and palette records do not change. Specialize the common presets at
pipeline compile time if dynamic shape arithmetic increases register pressure;
the shape code then selects an indirect batch.

Validation must include axis-parallel and boundary rays for every shape and
rotation, camera-inside proxies, negative canonical coordinates, parent/child
LOD transitions, page seams, micro payload identity, and output hashes.

Measure:

- nonempty bricks per shape and hierarchy level;
- occupied cells and tight-box volume per brick;
- submitted proxy area and fragment invocations;
- coarse/fine DDA iterations;
- Phase0 and complete prepare-to-G-buffer time; and
- memory used by nodes, occupancy, materials, and per-shape batches.

Retain anisotropic bricks only if the complete frame improves. A lower brick
count with wider screen-space proxies is not sufficient.

### R8. DDA changes only from counter evidence

The brick fragment already uses a logical 64-bit 4-cubed mask, an empty aligned
4-cubed octant skip, and lazy exact occupancy loads in the selected 2-cubed
region. An eager fine-mask prefetch regressed by about 1 ms and was removed.

Pending experiments are conditional:

- store the scale-4 mask only if mask construction is measurable and the
  derived sibling test is proven useful;
- batch prefix/material fetches only for a coherent occupied group;
- keep fine occupancy lazy in hit-first populations; and
- never replace the shallow side-8 brick with the deeper Tree64 traversal.

### R9. Nanite-inspired hybrid hardware/compute visibility raster

Use the existing brick hierarchy as a GPU-driven cluster hierarchy. The design
borrows Nanite's broad principles—hierarchical error selection, compact work
queues, representation-dependent rasterization, and deferred material resolve—
without attempting to reproduce Nanite's triangle-cluster format.

The hierarchy selects visible work once and routes it into two raster paths:

```text
resident terrain/page hierarchy
  -> hierarchical frustum/HZB/error cull
  -> selected raster work items
       ├─ hardware queue: large, coherent, low-overlap brick proxies
       └─ compute queue: high-overlap anisotropic/micro-heightfield work
  -> deterministic nearest-depth merge
  -> compact visibility buffer
  -> one material/G-buffer resolve per covered pixel
```

The objective is not to replace hardware rasterization everywhere. Apple's
tile rasterizer remains valuable for large coherent coverage and early depth.
Compute is used where proxy overlap and repeated fragment-side DDA make the
hardware path inefficient.

#### Unified raster work item

Add a compact immutable table produced by culling/MicroPrepare:

```text
SparseVoxelRasterWorkItemGPU
  source node or micro-parent record
  representation and shape code
  world/tight-bound reference
  material/palette base
  hierarchy error and depth range
  canonical mapping/generation
```

Representation values initially include:

- cubic or anisotropic legacy brick;
- residual brick after micro extraction;
- exact terrain heightfield parent;
- generated heightfield strips; and
- general-volume fallback.

Work item identity is stable for the frame and never aliases a streamed page
generation. It is an address into immutable frame resources, not a temporal
surface identity.

#### Hierarchical selection

Extend the current parent/child cut with a screen-space error and estimated
raster cost. A node is accepted when its error is below the quality threshold
and its estimated work is no worse than descending, or when children are not
resident. Otherwise enqueue resident children.

The selection must preserve:

- a crack-free parent-or-complete-children cut;
- one owner across terrain clipmap overlaps;
- residency fallback when a finer page is missing;
- temporal hysteresis around both error and cost thresholds; and
- immutable anisotropic shape choice inside the selected leaf group.

This is the voxel analogue of selecting a cluster cut. It remains a tree, not
a general dependency graph, for the first implementation.

#### Routing heuristic

Route each selected work item using a quantized cost estimate:

```text
hardware cost = projected proxy pixels
              * expected early-depth survival
              * fragment traversal weight

compute cost  = tile references
              + covered tile pixels * expected candidate tests
              + output stores
```

Hardware candidates are normally large, dense, hit-first and low-overlap.
Compute candidates are normally extracted heightfields, thin anisotropic
bricks, high-overlap proxies, or expensive microgeometry repeated across many
fragments.

The decision uses stable bins and hysteresis. It may not oscillate every frame
with subpixel camera movement. Add a forced `hardware`, `compute`, and `hybrid`
mode for controlled comparisons.

#### Compact visibility contract

Both paths publish the same logical hit record rather than four material MRTs:

```text
SparseVoxelVisibilityGPU
  workItemIndex
  packed parent cell / face / microcell / hit flags
  linear ray distance
  depth or validity metadata
```

The record references enough immutable work-item/node data for resolve to
reconstruct:

- packed material and bottom palette;
- descriptor/context and exact micro payload;
- canonical material sample address;
- stable parent/micro surface identity; and
- geometry normal and hardware depth.

Target 12–16 bytes per covered pixel before depth, rather than the current 40
bytes of integer colour attachments. Validate that recomputing linear/hardware
depth preserves the exact reverse-Z and temporal identity contracts.

Do not put a raw reusable node slot into temporal history. Resolve must produce
the existing world/canonical identity hash.

#### Hardware visibility path

The hardware path keeps conservative proxy rasterization and reverse-Z, but its
fragment shader emits only the compact visibility record. It still performs
the minimum DDA needed to identify the exact parent/micro hit. Material shading
remains deferred.

Large brick proxies continue to benefit from fixed-function clipping, coverage,
early depth, and Apple tile memory. Per-shape pipeline specialization avoids one
large dynamically divergent shader for cubic, anisotropic, heightfield, and
general-volume work.

#### Tiled compute path

Build screen-tile candidate lists in count/prefix/scatter stages:

1. project each compute work item's tight occupied bounds;
2. count touched 16 x 16 tiles;
3. exclusive-scan tile counts;
4. scatter work-item IDs into bounded tile ranges; and
5. dispatch one 16 x 16 workgroup per nonempty tile.

The tile kernel processes candidate chunks through threadgroup memory. One
thread owns one pixel, carries its nearest `bestT`, rejects candidate AABBs
behind that hit, and executes representation-specific intersection only when
necessary. It writes one final visibility record, not one record per candidate.

Overflow is exact: an overflow tile or work item is routed to the hardware
fallback for that frame. Never truncate the list and create holes.

#### Deterministic hardware/compute merge

The first implementation uses hardware visibility as the initial nearest hit:

```text
hardware visibility/depth
  -> compact hardware resolve or readable depth publication
  -> tiled compute starts with hardware bestT
  -> compute tests only candidates that can be nearer
  -> unified visibility output
```

This avoids depth atomics and gives every pixel one writer in the compute
stage. When no compute candidate touches a tile, alias or copy the hardware
result without launching a full tile kernel.

Tie rules must match the incumbent raster: nearest ray distance first, followed
by stable representation/identity ordering for equal boundaries. Test exact
face, edge, corner, coplanar, parent/child, page-seam, and camera-inside cases.

A later optimization may partition tiles so hardware-only tiles bypass the
merge entirely, but only after dependency and output equivalence are proven.

#### Deferred material and normal resolve

Resolve the unified visibility buffer once per covered pixel. This stage
reconstructs the current resolved-surface ABI and writes material/G-buffer
outputs. It is also the correct place to:

- decode optional oct16 normals from generated micro-parent records;
- calculate terrain macro normals from cached heightfield columns;
- run material programs once rather than once per overlapping fragment; and
- publish separated directional visibility for exact Lambertian composition.

The visibility path never evaluates BRDF/material appearance.

#### Streaming and memory

Raster work items are page-local and generated only for resident data. Page
installation publishes immutable work-item ranges and hierarchy references;
eviction advances a generation before memory is reused. Frame queues contain
generation-validated references.

Track memory for work items, tile counts/ranges, candidate IDs, compact
visibility, residual occupancy, and generated micro records. Avoid permanently
allocating worst-case candidate buffers for diagnostic-only modes.

#### Hybrid implementation stages

1. **H0, measurement-only binning:** construct tile lists and record candidate
   distributions without changing rendering.
2. **H1, shadow compute result:** run compute visibility into a comparison
   buffer; require exact hit/depth/identity parity with hardware.
3. **H2, compute-only reference:** render all eligible work through compute for
   a complete timing ceiling and correctness reference.
4. **H3, hardware-first merge:** hardware initializes best depth; compute
   resolves routed candidates into unified visibility.
5. **H4, compact hardware visibility:** replace auxiliary material MRTs and
   reconstruct them in deferred resolve.
6. **H5, adaptive routing:** enable stable per-item routing with forced-mode
   A/B controls.
7. **H6, fused visibility/material resolve:** combine compatible resolve and
   G-buffer storage work.

#### Acceptance and counters

Compare complete pipelines:

```text
legacy = cull + brick raster + BrickResolve + material + G-buffer
hybrid = cull + route/bin + hardware visibility + compute merge + deferred resolve
```

Capture:

- selected work items by representation and route;
- projected proxy pixels and hardware fragment invocations;
- tiles, candidate references, candidates per tile, and overflow;
- AABB tests, DDA steps, bestT prunes, and compute SIMD utilization;
- visibility/depth bytes and tile/device memory traffic;
- encoder boundaries and dependency waits; and
- p50/p95 for every stage and the complete frame.

Require exact default-quality output before enabling adaptive routing. Keep the
hybrid only if it beats both forced hardware and forced compute paths on the
complete frame. A faster compute kernel that loses after binning/merge is
rejected.

### R10. Fuse BrickResolve and material G-buffer work

After reducing Phase0, merge the approximately 2.39 ms BrickResolve/material
compute with compatible material texture writes. Load each integer brick
target once, publish resolved buffers, and write base colour, properties,
emissive, and shading normal in the same dispatch.

Retain a minimal fixed-function G-buffer pass for hardware depth and only the
attachments that downstream passes demonstrably consume. Then remove or pack
auxiliary brick MRTs made redundant by the fused kernel.

Do not move material shading into the overdrawn brick fragment shader.

#### R10 sizing, 2026-08-14: deprioritized, the fused pass cannot pay for itself

`BrickResolve` is not a 2.39 ms pass. Its **true marginal cost is 0.467 ms p50 /
0.692 ms p95**, against a largest within-arm spread of 0.124 ms over three
interleaved repeats per arm.

Measured without built-in GPU timestamps, by registering the resolver **twice**.
It is a pure per-pixel copy, so the second dispatch rewrites identical values:
the exact-output signature is unchanged (verified), every downstream pass sees
identical inputs, and the whole-frame wall-clock delta is therefore the pass's
own marginal cost. The technique generalizes to any idempotent pass and is the
only trustworthy local sizing tool in this tree - see the caveat below.

The earlier 2.2-2.39 ms figure came from `--gpu-timings`, which overstates this
pass by 4.7x. Vulkan timestamps bracket Vulkan pass boundaries rather than Metal
encoders, so a MoltenVK stall is billed to whichever pass runs next; a second,
identical `DepthPyramidPass` showed whichever ran first absorbing ~4.5 ms while
the other cost under 0.15 ms for the same work. Do not size a pass from that
report, and do not rank levers by it.

The consequence for this stage: fusing the resolver into the G-buffer fullscreen
pass can recover at most that 0.467 ms plus one encoder boundary, roughly 2% of
a 24.2 ms frame, because the fused kernel still performs the same four texture
reads and five buffer writes. Against that it requires a second G-buffer shader
variant and compile target, eight added bindings across a dual graphics/compute
layout, `resolvedSurfaces` promoted to read-write on the graphics layout, and a
conditional wiring that keeps the Tree64/SVRaster resolver paths working - all
on the byte-exact G-buffer contract. That is a poor risk-adjusted return, so R10
is **deprioritized rather than implemented**. Reopen it only if a Metal capture
shows the resolve/G-buffer boundary itself is expensive, which the pass timeline
cannot establish.

Attachment reduction, the other half of this stage, is already partly done: the
G-buffer render pass binds seven colour attachments, and the four material
targets are storage images written by the compute half of the same pass, not
extra attachments.

## Part II: hybrid DDGI implementation

### Why hybrid

The current surface probes are efficient in a sparse, planet-scale scene
because they exist only near visible receivers. Their weaknesses are view
dependence, cache churn during camera motion, weak offscreen persistence, and
limited multi-bounce lighting.

DDGI provides a stable world-anchored low-frequency cache. It should initially
provide indirect diffuse lighting and fallback continuity while surface probes
retain detailed local visibility, direct-light shadows, and near-camera
response. This avoids double counting sky and sun.

### D0. Public mode and settings

Add:

```text
--gi-mode surface|ddgi|hybrid
--ddgi-cascades 1..4
--ddgi-grid 8|12|16
--ddgi-rays-per-probe 8..64
--ddgi-update-fraction 0.01..1.0
--ddgi-hysteresis 0..0.999
--ddgi-relocation|--disable-ddgi-relocation
--ddgi-multibounce|--disable-ddgi-multibounce
--ddgi-debug off|cascade|validity|irradiance|distance
```

Default mode remains `surface` until hybrid passes correctness and performance
acceptance. Serialize all resolved DDGI settings in benchmark JSON.

### D1. Volume layout

Use camera-centred, world-axis-aligned cascades with origins snapped to probe
spacing. Start with four 12-cubed cascades. Suggested terrain spacings are 8,
32, 128, and 512 metres, configurable from the finest terrain voxel size and
view distance.

Logical probe coordinates map to a toroidal physical array. Scrolling clears
only newly exposed slabs and increments a generation value; unchanged physical
probes retain history. A teleport or incompatible scene revision invalidates
the complete volume.

Core CPU/GPU types:

```text
DDGIVolumeGPU
  originSpacing.xyz + spacing
  dimensions.xyz + cascade
  scrollOffset.xyz + generation
  probeBase + flags + updateCursor + padding

DDGIProbeStateGPU
  relocation.xyz + confidence
  generation + classification + sampleCount + flags
```

Keep layout helpers in a standalone `WorldGIDDGI.hpp` and mirror every mapping
function in WGSL and CPU tests.

### D2. Storage

Reuse the renderer's nine-coefficient diffuse SH representation:

- 9 x `vec4<f16>` irradiance coefficients per probe;
- an 8 x 8 octahedral visibility map storing first and second hit-distance
  moments as `RG16Float`;
- probe state and relocation buffer; and
- double-buffered irradiance/visibility history.

Four 12-cubed cascades contain 6,912 probes. Approximate double-buffered memory
is intentionally kept in the single-digit MiB range; record actual
`currentAllocatedSize` and physical residency rather than relying on this
estimate.

### D3. Scheduling and scrolling

Update a deterministic rotating fraction of every cascade each frame. Newly
exposed, invalid, or lighting-revision probes take priority. The starting
quality profile is:

- 1/8 of probes updated per frame;
- 32 rays per updated probe;
- full-sphere low-discrepancy directions rotated per frame; and
- a heartbeat update for long-lived probes.

The update list produces indirect dispatch arguments and exact active-ray
counts. Do not dispatch every capacity slot and return from the shader.

### D4. Ray generation and Tree64 trace reuse

Generate the existing canonical scene-ray representation from DDGI probe
positions. Feed the same compaction, Tree64 dynamic trace, and hit merge path
used by surface probes. Add a DDGI ray-source tag rather than duplicating the
terrain tracer.

The ray direction set is full-sphere because volume probes are not attached to
a receiver normal. Apply per-probe Cranley-Patterson or quaternion rotation so
temporal accumulation covers the sphere without fixed directional bands.

### D5. Probe update

For every traced ray:

- miss radiance is the current analytical sky;
- direct hit radiance evaluates emissive plus direct sun/local lighting at the
  hit with Tree64 visibility already represented by the ray;
- optional multibounce samples the previous DDGI history at the hit and
  multiplies by diffuse albedo divided by pi; and
- hit distance updates octahedral mean and second moment.

Accumulate into SH with finite checks, firefly clamping, and separate history
validity. Blend hysteresis by sample confidence. Reduce or reset history when a
probe scrolls, relocates, changes classification, or observes a large lighting
revision.

Keep direct sun and sky out of the DDGI output initially when `gi-mode=hybrid`:
store only bounce/emissive irradiance, or subtract the known direct terms before
composition. Surface probes remain the authoritative direct-light path.

### D6. Classification and relocation

Use visibility moments to classify probes:

- active: useful open-space probe;
- inactive: surrounded by backfaces or geometry;
- newly exposed: no valid history; and
- scrolling/invalid: generation mismatch.

Relocate probes away from geometry using the shortest hit directions, bounded
to less than half a grid cell. Relocation must be stable, quantized, and reset
on scroll generation. Never move a probe across a cell boundary.

### D7. Sampling and leak prevention

Sample the eight neighbouring probes with trilinear weights multiplied by:

- cascade-edge blend;
- probe validity/confidence;
- wrap-safe generation match;
- backface/receiver bias;
- Chebyshev visibility from distance moments; and
- a crush threshold for probes behind the receiver surface.

Normalize only after all weights. If no valid weight remains, fall back to the
current surface-probe ambient or analytical sky rather than black.

Cascade selection uses the finest volume containing the receiver, with a
one-cell overlap blend into the next cascade. Sampling must be continuous while
the snapped volume origin scrolls.

### D8. Integration with surface probes

Integration order:

```text
Tree64 scene commit
  -> DDGI update list/ray generation
  -> shared Tree64 trace
  -> DDGI resolve into next history
  -> surface-probe resolve/interpolate
  -> hybrid composition
```

The first hybrid implementation uses DDGI only when surface-probe interpolation
falls back or beyond a configurable distance. The second implementation blends
DDGI low-frequency indirect into every receiver while surface probes provide
direct light and local visibility.

Avoid a new unconditional full-resolution DDGI pass. Query DDGI inside the
existing `ProbeInterpolate` path or at a lower-resolution lighting stage so the
new cache replaces work rather than simply adding to the current 4 ms
interpolation cost.

### D9. Direct Lambertian lighting

Do not add a normal to every source brick. The G-buffer already supplies a
stable geometry normal and a material/detail shading normal, and the current SH
evaluation already applies the standard Lambertian clamped-cosine convolution.

For sharper direct lighting, split directional visibility from indirect
irradiance. Store a compact directional visibility/confidence channel and
evaluate in composition:

```text
direct = sunColour * sunIntensity
       * visibility
       * max(dot(shadingNormal, directionToSun), 0)
```

Surface probes or dedicated shadow probes own this channel. DDGI owns diffuse
indirect irradiance. This produces a more directional result without counting
the sun in both caches.

### D10. Debugging and validation

CPU tests:

- logical/physical grid mapping and negative-coordinate modulo;
- snapped-origin scroll and newly exposed slab enumeration;
- generation invalidation and teleport reset;
- cascade choice/blend continuity;
- octahedral direction round-trip;
- SH constant and directional-function reconstruction;
- moment visibility bounds; and
- relocation staying inside its cell.

GPU/reference tests:

- empty sky volume converges to the analytical environment;
- enclosed box remains dark without leaks;
- thin wall blocks irradiance across the wall;
- coloured emissive wall produces the correct first bounce;
- camera scroll preserves unchanged probes exactly;
- invalid/NaN history cannot poison neighbouring probes; and
- hybrid mode never doubles sky or directional sun.

Visual sequences:

- static camera for at least 1,000 frames;
- slow translation across one probe spacing;
- fast camera walk and teleport;
- terrain page streaming and raster LOD transition;
- sunrise/sunset lighting revision; and
- movement from voxel surface view into planet overview.

Track per cascade: active/inactive/new probes, probes and rays updated, scroll
slabs cleared, relocation count, history age, mean irradiance, rejected sample
weights, fallback pixels, and leak-test error.

### D11. Performance budgets and retention criteria

Initial budgets at 1200 x 720 native:

| Stage | p50 GPU budget |
| --- | ---: |
| Update-list construction and scrolling | 0.20 ms |
| DDGI ray generation | 0.30 ms |
| Incremental Tree64 trace contribution | 1.50 ms |
| Probe update/resolve | 0.60 ms |
| Added receiver sampling | 0.60 ms |
| Total incremental hybrid cost | no more than 3.20 ms |

Hybrid is retained only if it replaces enough surface-probe work to remain
neutral or faster in the complete frame, or if its stability/multibounce gain
is explicitly accepted as a quality tier. It must not silently move the 40 FPS
target farther away.

## Delivery order

1. R0 counters and exact-output harness. **Landed.**
2. R1 residual occupancy wired to the existing brick draw under a feature flag.
   **Implemented; opt-in, performance-rejected.**
3. R2 tight-parent heightfield raster with overflow fallback.
   **Implemented and correctness-qualified; opt-in, performance-rejected. Its
   last open explanation - three render passes instead of one - was merged,
   measured neutral, and closed.**
4. R3 compact column cache and optional generated-record oct normal.
   **Implemented, exact, and now covered: the R2 gate's top-down `COLUMNS` case
   requires `cachedRecords >= 1`. Payoff still bounded by R2's extraction
   density.**
5. R5 Flat/Slope/Wall pipeline specialization.
6. R7 anisotropic-brick encoding, parity tests, terrain leaf retiling, and
   per-shape indirect batches.
7. R6 cost-aware runtime parent/child LOD over the immutable built shapes.
8. R9 H0/H1 tile binning and shadow compute-visibility parity.
9. R9 H2/H3 forced compute and hardware-first hybrid merge.
10. R9 H4-H6 compact visibility, adaptive routing, and deferred resolve.
11. R10 remaining material/G-buffer fusion and attachment removal.
12. D0-D3 DDGI types, resources, scrolling, update list, and debug view.
13. D4-D5 shared Tree64 tracing and single-bounce probe update.
14. D6-D7 relocation, classification, visibility, and cascade sampling.
15. D8-D9 hybrid integration and separated Lambertian direct lighting.
16. D10-D11 long-sequence validation and authoritative Metal captures.

Rebaseline after every retained stage. Do not combine unmeasured changes when
assigning a performance gain, and do not report generated/displayed FPS as
source-renderer throughput.
