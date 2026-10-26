# Material-driven microvoxel carving plan

Status: proposed implementation plan.

## Outcome

Add exact, material-driven microvoxel carving to the sparse voxel brick
rasterizer. An occupied parent voxel remains the conservative raster and culling
volume, but its material may subtract a stable procedural microvolume from one
or more faces. The brick fragment traversal finds the first surviving
microvoxel, writes its exact depth and identity, and lets the existing material
G-buffer pass shade it from the material's top and bottom colours.

The first production target is a single upward-facing terrain surface:

- parent voxel size: any brick LOD size, 20 cm at the finest terrain level;
- subdivision: 2 through 8 initially, encoded for up to 15;
- `N = 5` produces 5 x 5 x 5 cells of 4 cm inside a 20 cm parent;
- maximum carve depth: 0 through `N` microvoxels inward from the selected face;
- stable procedural carve depth from material parameters and integer world
  coordinates;
- exact holes, vertical risers, normals, depth, motion, and temporal identity;
- primary/top and secondary/bottom colour shading through a selectable blend
  profile;
- no added geometry instances, draw calls, Tree64 nodes, or expanded bounds.

The architecture must later support snow crusts, grass-like pillars, and large
foliage volumes built by filling a coarse voxel envelope and carving it. It must
also preserve the zero-cost fast path for materials that do not use carving.

## Decisions

1. **Use discrete traversal, not conventional POM.** A one-face carve is a
   piecewise-constant heightfield and uses a bounded 2D column DDA. Multiple
   carved faces or arbitrary foliage use a bounded 3D microvoxel DDA. POM
   remains only a possible distant appearance fallback.
2. **Resolve microgeometry in the brick visibility pass.** The later full-screen
   material pass receives only the nearest resolved surface and cannot discover
   a parent voxel behind a carved hole.
3. **Continue the parent DDA after a micro miss.** `discard` is legal only after
   the ray exits the entire brick without finding a parent or microvoxel hit.
4. **Separate geometry and shading programs.** The geometry profile determines
   occupancy and therefore runs during depth/visibility. The shading profile
   only blends material properties after visibility is known.
5. **Use profile IDs, not runtime function pointers.** Core WGSL cannot carry an
   arbitrary callable function in material data. Bounded `switch` profiles are
   portable and testable. Compiled custom variants and material binning are a
   later optimization if profiling justifies them.
6. **Store descriptors per material, not per voxel.** Brick palette entries
   reference a deduplicated descriptor table. No microvolume is allocated for
   every terrain voxel.
7. **Carving is inward-only.** It can remove any part of the parent volume but
   cannot create geometry outside its AABB. Tall grass and large foliage must
   begin with an occupied parent-voxel envelope covering their maximum extent.
8. **Exact carving is a near-field representation.** Once a microcell is
   sub-pixel, geometry hands off to a stable coverage/colour representation.
   Coarse clipmap nodes do not independently invent a different fine pattern.

## Current renderer integration

The brick resolver already implements the correct outer architecture in
`sparse_voxel_brick_draw.wgsl`:

```text
brick AABB cull
    -> one indirect brick proxy draw
    -> conservative proxy coverage
    -> 4^3 coarse occupancy DDA
    -> 8^3 parent voxel DDA
    -> exact fragment depth + packed visibility
    -> full-screen material G-buffer
    -> HZB, GTAO, lighting, temporal reconstruction, screen traces
```

Microvoxel traversal becomes a nested part of the existing parent hit test:

```text
parent DDA reaches an occupied cell
    -> fetch packed material
    -> micro flag clear: accept normal parent hit
    -> micro flag set:
         fetch material descriptor
         test whether the entered face is enabled
         trace the procedural microvolume over this parent interval
             -> hit: accept exact micro hit
             -> miss: advance the parent DDA and keep searching
brick exit without a hit
    -> discard
```

The proxy still starts at the closest possible brick depth. A carved hit can
only move away from the camera, so the existing reverse-Z `DepthLess`
conservative-depth contract remains valid. The project currently injects that
SPIR-V execution mode after WGSL compilation; other backends must retain an
equivalent conservative-depth declaration or explicitly measure the loss of
early depth rejection.

## Material semantics

### Face coordinates

`packedLocalFace` already uses the axis in bits 0-1 and sign in bit 2. A
descriptor uses a six-bit face mask with this canonical mapping:

```text
bit 0: -X    bit 1: +X
bit 2: -Y    bit 3: +Y
bit 4: -Z    bit 5: +Z
```

Terrain grass and snow initially enable only `+Y`. A face-relative coordinate
system must have one documented orientation per face so procedural patterns do
not mirror unpredictably.

### Subdivision and carve depth

For subdivision `N`, the parent contains `N^3` cubic microcells and each cell
has size `parentVoxelSize / N`. A face profile returns an integer carve depth:

```text
carveDepth(face, u, v, parentKey, descriptor) in [0, maximumDepth]
maximumDepth in [0, N]
```

For a positive-axis face:

```text
depthFromFace = N - 1 - cell[axis]
```

For a negative-axis face:

```text
depthFromFace = cell[axis]
```

The face removes the cell when `depthFromFace < carveDepth`. A value of zero
keeps the full column; a value of `N` removes the column through the complete
parent. When several faces are enabled, carving is subtractive:

```text
occupied = baseParentOccupied && !carvedByAnyEnabledFace
```

That rule is deterministic at corners and makes multi-face traversal equivalent
to a real `N^3` occupancy volume.

### Stable procedural identity

No geometry decision may use frame number, pixel coordinate, floating camera
position, or temporal jitter. Hash inputs are integers:

```text
material seed
global parent cell key
local microcell coordinate
canonical face
geometry profile ID
```

Terrain clipmap pages must derive the parent key from their stable tile/cell
address or the brick's quantized world key, not by flooring a large floating
world position. Adjacent parent voxels use the same canonical world micro-grid
when a profile is intended to cross their boundary. Large foliage profiles use
a world-volume density field rather than independent per-parent noise, avoiding
20 cm seams.

### Initial geometry profiles

| Profile | Occupancy model | Intended use |
| --- | --- | --- |
| Disabled | Full parent cell | Legacy materials and fast path |
| Constant | Constant carve depth | Tests, layered snow |
| Hash columns | Independent bounded depth per face cell | Coarse grass and breakup |
| Clumped columns | Low-frequency cluster plus fine hash | Snow, soil, tufts |
| Strata | Quantized bands with material parameters | Snow/rock and layered dirt |
| Volume density | World-stable 3D threshold | Foliage envelopes; later phase |

The first implementation includes Disabled, Constant, and Hash columns. Clumped
columns lands only after the exact traversal and LOD handoff are stable. Volume
density requires the general 3D path.

### Shading profiles

The material pass receives the top colour, bottom colour, exact hit face,
normalized micro depth, stable random value, and descriptor parameters.

Initial profiles:

- linear top-to-bottom by normalized depth;
- hard crust threshold;
- noisy linear blend;
- top-face primary with exposed micro side walls using bottom colour;
- foliage tint using top/bottom hue and a stable cell variation.

Geometry and shading profile IDs are independent. Changing a colour profile
must not alter depth. Changing a geometry profile invalidates depth, HZB, and
temporal visibility history.

## GPU data model

### Source material extension

Keep `SparseVoxelMaterialGPU` at its existing 32-byte ABI. Its two reused words
already hold the secondary colour and face blend modes. Add a scene/material
extension table keyed by `materialId`:

```cpp
struct SparseVoxelMicroMaterialGPU
{
  uint32_t geometry = 0;
  uint32_t shading = 0;
};
```

Proposed packing:

```text
geometry bits  0..3   subdivision; 0 disables, initial valid range 2..8
geometry bits  4..7   maximum carve depth
geometry bits  8..13  six-face mask
geometry bits 14..18  geometry profile ID
geometry bits 19..23  shading profile ID
geometry bits 24..31  material seed

shading  bits  0..7   variation/coverage strength
shading  bits  8..15  profile parameter 0
shading  bits 16..23  profile parameter 1
shading  bits 24..31  LOD/handoff parameter
```

The encoding supports subdivisions through 15, but the initial shader accepts
only 2 through 8. Raising that limit requires a separate performance and loop
bound review.

Procedural terrain registers descriptors for its biome material IDs. Scene-file
materials gain an optional versioned extension chunk later; version-1 files
continue to load with descriptor zero.

### Brick palette extension

Use currently reserved bit 31 of `SparseVoxelBrickMaterialGPU::packed[1]` as
`micro enabled`. Add a descriptor-ID array parallel to the compact brick
palette. The draw shader checks the bit before touching the new buffer, giving
legacy pixels one predictable branch and no additional storage load.

Descriptor IDs are globally deduplicated 16-bit indices, packed two per `u32`
for WGSL storage access. ID zero is Disabled. Palette construction must treat
different geometry descriptor IDs as incompatible: nearest-colour palette
approximation may not merge shapes. Shading-only descriptor differences are
also exact initially; approximation can be evaluated later.

Every palette move, terrain slot update, serialization path, and GPU capacity
calculation must carry the parallel descriptor IDs with the same base and
stride as the colour palette.

### Visibility payload

Add one `R32Uint` micro-surface attachment, or promote the existing bottom
colour companion to an `RG32Uint` attachment after measuring format support and
bandwidth. The clear value is zero.

Proposed 32-bit payload:

```text
bits  0..15  micro material descriptor ID
bits 16..19  face-relative U cell
bits 20..23  face-relative V cell
bits 24..27  depth layer / hit microcell layer
bits 28..31  flags: valid, heightfield/volume, reserved
```

Subdivision comes from the descriptor; the exact hit face remains in the
existing surface record. The parent/micro surface identity stays 32 bits and is
hashed from the stable parent identity plus local microcell and hit face.

The material pass reconstructs normalized depth and stable shading randomness
from the descriptor, micro payload, and surface identity. It must not rerun an
expensive geometry search. A dedicated R32 target costs about 1.46 MiB at the
current 800 x 480 quality-MetalFX internal resolution; eliminating or packing
it is an optimization milestone, not a prerequisite for correctness.

## Traversal algorithms

### Single-face heightfield path

This is the primary terrain path. Intersect the ray with the parent AABB, map
the entry point and direction into face-relative coordinates, and walk crossed
`N x N` columns. For each column:

1. Evaluate its integer carve depth once.
2. Convert that depth to the surviving horizontal plane.
3. Compare the plane hit with the next U/V column boundary.
4. Return the horizontal face if it occurs first.
5. Otherwise step to the adjacent column and test the connecting vertical
   riser.
6. Return miss when the ray exits the parent interval.

The loop is statically bounded by `2 * N + 2`; it must not be a convergence
raymarch. It is the generalized form of the current connected 2 x 3 analytic
micro-surface, with discrete subdivision and full-column removal.

### Multi-face and volume path

Create the initial microcell from the parent entry point and perform standard
3D DDA. At each cell, evaluate `occupied(cell)` from all enabled face profiles
or the volume-density profile. Return the entering micro face and exact `t` for
the first occupied cell. The loop bound is `3 * N`; for `N = 5`, a ray crosses
at most approximately 13 cells.

### Parent continuation

Refactor the current parent DDA so an occupied parent candidate is not
irreversibly committed before material/micro evaluation. The candidate result
has three states:

```text
AcceptParentHit
AcceptMicroHit
ContinueParentTraversal
```

Only the first two terminate. The third advances from the current parent exit
boundary with the same tie-breaking rules as the existing DDA. This behavior
gets a CPU reference test before shader integration.

### Surface outputs

An accepted micro hit writes:

- projected exact hit depth;
- exact ray distance in the existing surface payload;
- micro hit face axis/sign;
- parent material and secondary colour;
- micro descriptor/payload;
- temporal identity including parent cell, microcell, face, and geometry
  descriptor revision.

The later analytic micro-depth effect must be disabled for a true micro hit to
avoid applying displacement twice.

## Pass architecture

### Required path

1. **Brick cull:** unchanged bounds and indirect draw topology. Optional node
   summaries may later bypass micro-capable shader work for all-legacy bricks.
2. **Brick visibility draw:** parent DDA, descriptor lookup, micro traversal,
   exact depth, packed visibility, and discard only on complete brick miss.
3. **Brick resolve:** propagate the micro payload and exact surface data into
   `ResolvedVoxelSurface` or its parallel screen texture.
4. **Material G-buffer compute:** fetch the descriptor, run the shading profile,
   and output albedo, material properties, emissive, and shading normal. The
   geometry normal remains the exact cubic hit normal.
5. **Existing consumers:** final HZB, GTAO, screen traces, motion, temporal
   rejection, and lighting consume the exact depth/normal without a special
   microgeometry branch.

This already realizes the proposed "depth first, shade second" visibility
architecture. Do not add a separate material cull pass before measuring the
unified material shader.

### Optional material classification/binning

If numerous heavy shading profiles cause measurable subgroup divergence, add:

```text
visibility buffer
    -> 8 x 8 tile material-profile classification
    -> compact active pixel IDs by shading profile
    -> one indirect compute dispatch per compiled profile
```

This only moves shading work. Geometry-profile evaluation remains in the brick
visibility pass because depth depends on it. The binning path is approved only
if profiling shows at least a 10% material-pass improvement after including its
classification, prefix/compaction, pixel-list bandwidth, and dispatch costs.

## Culling, LOD, and temporal behavior

### Culling

Carving only removes geometry, so parent and brick AABBs remain conservative.
No TLAS/BLAS expansion is required. Actual micro depth feeds the phase-one HZB,
so holes reduce occlusion naturally rather than being represented as solid
parent faces.

An optional one-word node summary can later record whether any palette entry in
the brick uses microgeometry and the maximum subdivision. It may select a
legacy pipeline or skip descriptor bindings for legacy-only draws, but it is
not necessary for correctness.

### Near/far policy

Compute projected microcell size after the parent material is known:

```text
microWorldSize = parentVoxelSize / subdivision
projectedPixels = microWorldSize * focalLength / surfaceDistance
```

Initial policy:

- exact micro traversal while `projectedPixels >= exactThreshold`;
- a broad handoff interval around one internal-resolution pixel;
- stable coverage and top/bottom colour LOD below the threshold;
- no exact carving on a coarse clipmap node when a finer resident cascade owns
  that world region.

Binary holes cannot simply alpha-fade while retaining opaque depth. The LOD
phase must evaluate two candidates: continuing exact traversal farther than
strict visibility requires, and a stable world-space coverage decision whose
expectation matches surviving microcell coverage. Temporal jitter must not
change world-space occupancy or identity. A representation switch may use
brick-level hysteresis, but not frame-random dithering.

### Clipmap consistency

The finest terrain level owns the canonical pattern. Coarser nodes either:

- aggregate that canonical occupancy conservatively;
- use a coverage/material mip derived from it; or
- disable geometric carving and retain only band-limited shading.

They must not evaluate `N` relative to their larger parent size and thereby
generate a different world pattern. Terrain ownership clipping remains atomic
with page publication.

### Temporal identity

History validation includes the microcell identity. A camera ray moving from a
top micro face to a carved side wall must reject the old surface history.
Descriptor geometry revisions invalidate affected pixels. Shading-only colour
changes use the existing material/reactive response without pretending the
surface moved.

## Procedural evaluation and optional mask cache

The initial profiles evaluate a small integer hash per crossed column/cell and
require no per-voxel storage. This is the lowest-memory solution, but a close
parent may cover many pixels and repeat the same profile evaluation.

Add an optional cache only after profiling:

- generate a 125-bit mask (four `u32`s) for a 5^3 parent once when a terrain
  page becomes resident;
- allocate masks only for surface parents whose descriptor requests caching;
- store a 512-bit `has cached microvolume` mask per 8^3 brick and use rank to
  address a sparse mask pool;
- key cached payloads by page revision and geometry-descriptor revision;
- discard them with the disposable terrain page.

Simple heightfield profiles should remain procedural. Cached full masks target
expensive foliage density functions or heavily magnified hero materials. A
worst-case mask on every parent is explicitly rejected because 16 bytes per
voxel would erase the compact brick representation's memory advantage.

## Tree64, probes, and shadows

The raster implementation immediately affects hardware depth, G-buffer
normals, final HZB, GTAO, and screen-space traces. Tree64 and other off-screen
ray paths still see the conservative parent voxel.

That is acceptable for the first milestones, with these semantics:

- screen-visible holes are exact;
- off-screen shadows and probe rays may over-occlude within one parent voxel;
- no ray path may under-occlude by assuming carved data it cannot evaluate.

Later, share the same descriptor and `occupied(microcell)` functions with the
Tree64 leaf-hit path. Invoke micro traversal only after Tree64 finds a
micro-enabled leaf. A micro miss continues Tree64 traversal from the parent exit
distance. Keep this phase separate because it affects shadow/probe budgets,
storage-dispatch WGSL variants, and conservative fallback rules.

## Implementation phases

### Phase 0 — reference, counters, and baselines

- Add a CPU reference for face carve depth, multi-face occupancy, one-face
  traversal, 3D DDA, and parent continuation.
- Record baseline brick visibility/material timings at 1200 x 720 quality
  MetalFX, plus the existing overdraw view.
- Add debug counters for micro-eligible parent hits, accepted micro hits,
  completely carved parents, parent continuations, final discards, heightfield
  steps, volume steps, and descriptor-cache loads.
- Add a debug view for descriptor ID, microcell coordinate, carve depth, and
  exact/fallback representation.

Exit criteria:

- exhaustive CPU rays agree with brute-force micro-AABB intersections for
  subdivisions 2 through 8 and all six face signs;
- the disabled reference profile is identical to a solid parent;
- baseline captures and counters are reproducible.

### Phase 1 — descriptor plumbing with disabled behavior

- Add the source material extension/library keyed by `materialId`.
- Add global descriptor deduplication, parallel palette descriptor IDs, bit-31
  enable metadata, capacity accounting, upload, and terrain slot updates.
- Extend transient terrain serialization and scene-file optional extensions.
- Bind the descriptor buffers to brick draw and material G-buffer passes.
- Keep every descriptor disabled by default.

Exit criteria:

- legacy scene and terrain output signatures are bit-identical;
- disabled GPU time changes by less than 1% over a stable capture;
- descriptor IDs survive palette packing, five-level clipmap combination,
  transient spill/restore, and incremental page replacement;
- version-1 scene files load with descriptor zero.

### Phase 2 — exact single-face visibility

- Refactor parent traversal to support ContinueParentTraversal.
- Implement Constant and Hash-column 2D DDA profiles in shared WGSL.
- Support subdivisions 2 through 8, maximum depth 0 through `N`, and one face
  selected from the face mask.
- Write exact micro depth, face, temporal identity, and R32 micro payload.
- Disable the old analytic micro displacement on true micro hits.
- Add ImGui controls for a selected terrain material: enabled, subdivision,
  maximum depth, face, geometry profile, seed, and debug view.

Exit criteria:

- a fully carved column reveals a later parent voxel in the same brick;
- a fully carved brick pixel discards and reveals another brick/background;
- depth and face normals match CPU reference intersections;
- HZB and GTAO contain the carved silhouette;
- static-camera output has no frame-dependent occupancy or blinking;
- `N = 5` produces measured 4 cm cells inside a 20 cm finest terrain voxel.

### Phase 3 — material shading profiles

- Propagate descriptor/micro payload through brick resolve.
- Implement linear, crust, noisy, and exposed-side blend profiles.
- Reconstruct stable shading random values from identity and microcell payload.
- Keep exact geometry normal separate from optional shading-normal treatment.
- Define material change classification: geometry changes invalidate visibility;
  shading changes invalidate material/lighting history only.

Exit criteria:

- top and bottom colours match profile reference values at every depth layer;
- newly exposed grass/snow side walls select the bottom colour as authored;
- depth remains bit-identical while switching only the shading profile;
- packed and source-material paths produce the same result.

### Phase 4 — geometric LOD and MetalFX stability

- Implement projected-microcell eligibility and a broad exact-to-coverage
  handoff.
- Add clipmap ownership rules and prevent coarse levels from regenerating a
  different pattern.
- Feed exact micro identity, depth, normal, and motion into temporal rejection.
- Tune at native, quality MetalFX, and frame-generation configurations.

Exit criteria:

- no visible distance ring where exact cells stop;
- no static-camera shimmer or camera-motion blinking;
- no clipmap-boundary pattern swap;
- unresolved microgeometry retains material/coverage variation at the horizon;
- screen-space traces and GTAO remain aligned with final depth.

### Phase 5 — multi-face and foliage volumes

- Implement subtractive six-face occupancy and bounded 3D micro DDA.
- Add Clumped-column and Volume-density geometry profiles.
- Ensure canonical face orientation and shared world-volume sampling.
- Build snow, grass-pillar, and filled-canopy foliage fixtures.

Exit criteria:

- multi-face DDA agrees with brute-force occupancy for all tested rays;
- adjacent carved parent voxels have no procedural seams;
- holes and disconnected surviving cells resolve the nearest actual surface;
- large foliage envelopes remain within conservative parent/brick bounds.

### Phase 6 — optimize from measurements

- Compare procedural reevaluation with sparse cached masks.
- Add node micro summaries or a legacy-only pipeline only if they save total
  frame time.
- Reduce or pack the R32 micro payload if bandwidth is material.
- Add optional tile material classification only if the measured 10% net
  improvement gate is met.
- Tune fixed loop bounds and split heightfield/volume pipeline variants if the
  unified geometry switch diverges materially.

Exit criteria:

- disabled materials retain the Phase-1 zero-cost target;
- no optimization changes depth/identity reference results;
- at 50% screen coverage, simple `N = 5` top-face carving adds no more than 15%
  to total G-buffer time on the target Apple GPU;
- full-screen stress coverage adds no more than 25%, or automatically selects a
  lower-cost LOD before exceeding the budget;
- descriptor and summary storage stay below 2 MiB for the reported five-level
  terrain hierarchy, excluding explicitly enabled sparse mask caches.

### Phase 7 — Tree64 and off-screen rays

- Carry descriptor identity to Tree64 leaves or a material lookup table.
- Invoke the shared micro traversal only after a micro-enabled parent hit.
- Continue Tree64 traversal after a completely carved parent.
- Cover primary, shadow, probe, mirrored-ancestor, dynamic, and storage-dispatch
  variants through shared includes rather than copied logic.

Exit criteria:

- raster, Tree64 primary rays, and probe/shadow rays agree on reference hits;
- carved holes no longer over-occlude off-screen lighting;
- legacy Tree64 traversal has no measurable regression;
- invalid/missing descriptor data falls back conservatively to the solid parent.

## Validation matrix

Required automated coverage:

| Area | Cases |
| --- | --- |
| Descriptor ABI | disabled, every valid `N`, `maximumDepth <= N`, invalid records rejected |
| Palette | exact descriptor preservation, secondary colour reference, combined clipmap palette bases |
| Geometry | six faces, signs, grazing rays, camera inside parent/brick, boundary ties, complete holes |
| Parent continuation | later cell in same coarse cell, later coarse cell, later brick, complete miss |
| Shading | all profiles, top/bottom endpoints, exposed side walls, stable random seed |
| Temporal | static jitter, yaw, dolly, page replacement, material geometry/shading revisions |
| LOD | native, MetalFX quality, frame generation, clipmap transitions, horizon |
| Consumers | hardware depth, HZB, GTAO, screen traces, motion, normal, temporal identity |
| Files/streaming | old scene file, extended scene file, transient memory/disk restore, corrupt descriptor |
| Performance | disabled, 25/50/100% coverage, N=2/5/8, heightfield versus volume, cache on/off |

Visual fixtures should use deliberately diagnostic colours before art tuning:
white snow over dark rock, bright green grass over brown dirt, and a filled
magenta foliage envelope carved into cyan interior surfaces. Each fixture gets
top, grazing, silhouette, and camera-inside captures.

## File-level work map

Core CPU/ABI:

- `src/voxel/svraster/SparseVoxelGPU.hpp`
- `src/voxel/svraster/SparseVoxelBrick.hpp`
- `src/voxel/svraster/SparseVoxelBrick.cpp`
- `src/voxel/svraster/PlanetVoxelChunk.cpp`
- `src/voxel/svraster/SparseVoxelSceneFileReader.cpp`

Rendering integration:

- `src/voxel/svraster/rendering/SparseVoxelBrickPasses.hpp`
- `src/voxel/svraster/rendering/SparseVoxelGBufferPass.hpp`
- `assets/shaders/voxel/svraster/wgsl/sparse_voxel_brick_draw.wgsl`
- `assets/shaders/voxel/svraster/wgsl/sparse_voxel_brick_resolve.wgsl`
- `assets/shaders/voxel/svraster/wgsl/sparse_voxel_gbuffer.wgsl`
- `assets/shaders/voxel/svraster/wgsl/sparse_voxel_material_gbuffer.wgsl`
- new shared `sparse_voxel_micro_material.wgsl`

Sample/UI and tests:

- `samples/sparseVoxelRenderer/SparseVoxelRendererSample.cpp`
- `tests/voxel/SparseVoxelPlanetChunkTests.cpp`
- `tests/voxel/SparseVoxelSceneFileTests.cpp`
- new CPU micro-traversal reference tests
- new raster depth/identity and temporal stability fixtures

Tree64 follow-up:

- `assets/shaders/voxel/svraster/wgsl/sparse_voxel_tree64_common.wgsl`
- the primary, shadow/probe, dynamic, mirrored-ancestor, and storage-dispatch
  Tree64 shader variants that include it.

## Completion definition

The feature is complete when a material can describe subdivision, maximum
inward depth, face mask, stable geometry profile, and top/bottom shading
profile; the brick renderer produces exact microvoxel silhouettes and depth;
the G-buffer and screen-space consumers agree with those surfaces; distance and
clipmap transitions are stable under MetalFX; legacy materials retain their
cost and output; and Tree64 has either gained matching traversal or continues to
use an explicitly documented conservative solid-parent fallback.
