# Terrain primary visibility via a mesh proxy - KILLED, 2026-08-15

**Verdict: +3.25 ms slower, measured whole-frame. Do not revive.**

```text
                         whole frame   fragments    overdraw
terrain via bricks         38.790 ms   18,096,470    15.163x
terrain via the mesh       42.044 ms    1,795,418     1.524x
                                                     1.197x back-face culled
repeat spreads: 0.208 / 0.138 ms, arms do not overlap
```

The design did what it promised - a **10x reduction in fragment invocations**,
strict containment, a one-voxel march bound - and the frame got slower anyway,
because the overdraw was never the cost. This is the same wall the compute
raster hit (`-0.03 ms` for the same 15x -> 1x substitution), and it is now
measured twice by two unrelated implementations. The evidence against
"overdraw is the problem" should be considered conclusive.

Beware the trap this run laid: `bench-mesh-1` had a contaminated warmup repeat
(min 34.27, max 51.83). **Min-of-N would have reported the mesh 0.8 ms
faster.** Medians said +3.25 ms.

What survives and is worth keeping:

- `PlanetVoxelChunk::surfaceHeights`, the per-column height raster with its
  neighbour halo, GPU-resident and **proven bit-exact across every page seam**
  (92 pairs, 44,160 samples, 0 mismatches) and against the CPU (22,784 GPU
  samples, 0 mismatches). This independently unblocks the heightmap microvoxel
  plan, section 6a of the optimizations doc.
- The containment analysis: a mesh through a height field leaves **100% of
  columns' top voxels outside it**, and only a max-dilated envelope restores
  containment. Any future scheme that carves voxels out of a proxy surface has
  to solve this, and the plan's original "containment is definitional" claim was
  simply wrong.
- The instruments: `--terrain-mesh-probe`, its containment/seam/GPU-parity
  reports, and `--terrain-mesh-only`.

The unspent stages below (T3 carve, T4 grazing bound, T5 fade, T6 default,
T7 reclaim) are retained for the record only.

---

# Original plan: terrain primary visibility via a CBT mesh proxy

Plan, 2026-08-15. Replaces the terrain brick culling and brick raster with a
CBT-mesh-proxy pass that carves the voxel surface in the fragment. Tree64 stays
and keeps being fed by terrain voxelization.

Every number quoted here is measured; see `sparse-voxel-optimizations.md` for the
runs behind them.

## The trade, in one place

```text
today, terrain primary visibility:
  brick cull (two phase) + brick raster + brick resolve
  18,096,470 fragments for 1,193,491 covered pixels   = 15.163x overdraw
  raster+cull at shipping default                     = 14.57 ms
  of a 51.3 ms frame

proposed:
  rasterize the CBT mesh (1-2 fragments per covered pixel)
  carve the voxel surface in the fragment with a short march
  no brick proxies, no brick culling, no brick binning for terrain
```

The prize is the 15.163x multiplier. Everything else in this plan exists to make
that substitution correct.

**The whole design rests on one property**: if the voxel surface is the
voxelization *of the mesh surface*, then the mesh is a strict superset of the
voxel silhouette and the march from the mesh surface to the first occupied cell
is ~1 step. Both of the objections that killed earlier variants - protrusion
holes, and unbounded grazing marches - are definitional rather than tuned away.
That property must be preserved by every stage below; if it is ever violated the
scheme degrades to holes in the terrain, not to a soft artifact.

## What is replaced, kept, and explicitly not saved

**Replaced (terrain only):**

- `SparseVoxelBrickCullPass` phases for terrain bricks
- `SparseVoxelBrickDrawPass` proxy draws for terrain bricks
- The terrain share of `SparseVoxelBrickResolvePass`
- Exact micro carving for terrain (`microSurfaceData`, descriptors, handoff) -
  the voxel surface comes from the height field directly

**Kept, unchanged:**

- **Tree64.** `tree64TerrainScene` / `terrainTreePageCapacity` mean terrain
  voxels still feed the Tree64 used for scene rays, shadows and lighting.
  Terrain voxelization and streaming continue exactly as now.
- The brick path in full, for **non-terrain** content (instanced meshes, OBJ
  scenes, Tree64 dynamic layer). This is an added terrain path, not a removal of
  the brick renderer.
- Everything downstream of the resolve contract: material G-buffer, probes,
  SVGF, TAA/TSR, shadows.

**Explicitly NOT saved** - state this up front so the plan is not oversold:

| | why it stays |
| --- | --- |
| terrain voxelization + streaming | Tree64 still needs the voxels |
| brick memory for terrain | same |
| material G-buffer (1.27 ms) | downstream, untouched |
| probe / lighting block (12.2 ms) | downstream, untouched |

So the addressable envelope is the **14.57 ms** of raster+cull, not the frame.
A realistic target is recovering most of it and paying back 2-4 ms in mesh
rasterization, i.e. **~10 ms of a 51.3 ms frame**. If a stage measures worse
than that trajectory, stop - see kill criteria.

## The contract the new pass must satisfy

The new pass is a drop-in producer of what `SparseVoxelBrickResolvePass`
publishes. Getting this exactly right is what keeps every downstream consumer
unmodified:

```text
resolvedSurfaces[pixel] = ResolvedVoxelSurface(
    surfaceT,            // linear ray distance to the cube face
    0u,                  // instanceIndex: bricks are one implicit instance
    identity,            // voxelIndex is NOT an index - it is a stable
                         //   per-voxel identity; temporal reuse validates
                         //   history against it
    packedLocalFace,     // axis | (bit 2 set when entered through +face)
    hitFlags,            // SURFACE_HIT | PACKED_MATERIAL
    packedMaterialX,     // colour/roughness
    packedMaterialY,     // metadata, incl. EXACT_MICRO_MATERIAL bit
    bottomAlbedo | (terrainContext << 24))
coverage[pixel]                 = 1
linearDepth[pixel]              = surfaceT
microPayloads[pixel]            = payload or 0
materialSampleAddresses[pixel]  = canonical micro coordinate + regionMappingId
```

Plus `@builtin(frag_depth)`, declared **DepthLess** via
`spirv_add_execution_mode.py` (the toolchain already does this for
`sparse_voxel_brick_draw`; naga cannot emit conservative depth, and without it
writing frag_depth forces late-Z).

**Identity is the subtle one.** `brickSurfaceIdentity` hashes the brick world
minimum, voxel size and cell index - deliberately not a residency slot, so a
streamed page keeps its history. The mesh path needs an equivalent that is
stable across CBT re-tessellation: hash the **world-space voxel cell** (grid
origin, voxel size, integer cell), which is tessellation-independent by
construction. If identity flickers with tessellation, temporal reuse throws away
history every time the CBT subdivides and the image will boil under motion.

## The crux: mesh and voxels must share one height source

This is the prerequisite that has blocked every variant of this idea, and it is
the first real work.

```text
CBT mesh displacement  <- surfaceAtlas_.height   (R16Float, faceResolution 1024)
                          = 9.77 km per texel on a 6,371 km planet
voxel surface          <- haloHeights            (PlanetVoxelChunk.cpp:2203)
                          = authored package raster + proceduralDetailStrength
```

Against a 3.9 cm microvoxel the overview atlas is ~250,000x too coarse, and
`proceduralDetailStrength` adds metre-scale relief the mesh does not carry.
Today the two surfaces disagree by metres. Under this scheme that is not a seam:
**a voxel outside the mesh is a hole**, because the mesh fails to rasterize a
pixel where geometry is visible.

**Resolution: upload `haloHeights` and drive both from it.** The dense
per-column height array with a neighbour halo already exists at chunk build
time, is the final surface (authored raster + procedural detail), and is never
uploaded. Making it GPU-resident:

- gives the fragment `height(worldXZ)` at parent-voxel spacing,
- gives the CBT the same displacement source, so the two agree by construction,
- is quality-neutral (same values the voxels were built from), so no exactness
  rebaseline,
- and independently unblocks the heightmap microvoxel plan (section 6a of the
  optimizations doc).

Addressing must be clipmap-consistent with the existing terrain residency so a
page swap does not move the sampling grid.

## Pass design

```text
  CBT mesh (adaptive tessellation)
        |
        v
  vertex: displace by height(worldXZ) from the shared raster
        |
        v
  fragment:
        P            = interpolated world position on the mesh
        ro, rd       = camera ray
        cell         = floor((P - gridOrigin) / voxelSize)
        march inward, at most N steps:
            occupied(cell) = cellTop <= height(cellCentreXZ)   // heightfield
            if occupied: slab-intersect that cell -> face, exact t
        if no hit: discard        // carves the stair-stepped silhouette
        |
        v
  write the resolve contract + frag_depth (DepthLess)
```

Four things worth stating explicitly:

1. **Occupancy is a comparison, not stored data.** Terrain is a height field
   (`PlanetVoxelChunk.hpp:59`), so `occupied(cell)` is one height fetch. No brick
   occupancy, no `microSurfaceData`, no per-voxel storage in this path at all.
2. **Erode inward.** Use the cube-circumradius test so a cell counts as occupied
   only when fully inside: `height >= cellTop`, equivalently
   `sdf(centre) < -½·sqrt(3)·s`. This is what makes containment strict and the
   discard valid.
3. **Discard is what produces the silhouette.** Not a smooth outline with blocky
   shading - the mesh covers a superset of pixels and the discards carve it back
   to the true voxel outline.
4. **The face comes from the slab test**, so normals and depth are real
   ray/cube intersections, not quantized approximations.

## Staged plan, each stage with a gate

Modelled on the H0/H1 staging that worked: every stage is measurable on its own
and has an explicit stop condition.

### T0 - Measure the numerator. GATE. **PASSED, 2026-08-15**

Measured with `--terrain-mesh-probe STRIDE`, which rasterizes the resident
terrain height field as a mesh under the scene camera and counts fragments into
the *same* per-pixel counter the brick overdraw view uses, with the brick
raster's own counting switched off. One instrument, one numerator, one log line.

```text
                       fragments   coveredPixels   overdraw   triangles
brick proxy (incumbent) 18,096,470     1,193,491     15.163x       n/a
mesh, stride 1  (0.20 m) 1,795,418     1,178,229      1.524x   4,556,800
mesh, stride 2  (0.39 m) 1,776,348     1,178,207      1.508x   1,139,200
mesh, stride 4  (0.78 m) 1,747,586     1,178,141      1.483x     284,800
mesh, stride 8  (1.56 m) 1,707,164     1,177,959      1.449x      71,200
mesh, stride 16 (3.13 m) 1,641,568     1,177,556      1.394x      17,800
```

**10.1x fewer fragment invocations**, against a gate that asked for better than
2x. `coveredPixels` agrees with the brick path to 1.3%, which is the check that
the two are rasterizing the same picture rather than two different framings.

Three findings that change the plan:

1. **Overdraw is nearly flat in tessellation** - 1.52x at 0.20 m quads against
   1.39x at 3.13 m. The feared micro-triangle regime never arrives, because the
   resident set is already a clipmap: the 0.195 m pages only exist within 47 m
   of the camera, where their quads are several pixels across. Tessellation
   should therefore be chosen by containment and vertex cost, not by overdraw.
2. **Comparability took two culls, and without them the number is meaningless.**
   The first attempt covered all 2,073,600 pixels. Coarse clipmap pages extend
   *under* the finer levels (the brick packer clips every coarse brick against
   the finer-owned rectangle), and terrain is resident to 48 km while frustum
   culling removes everything past the 25 km far plane before the brick raster
   sees it. Drawing either is charging the mesh for geometry the incumbent never
   rasterizes.
3. **The CBT is not needed for this.** The resident chunk set is already a
   clipmap of 160x160 column pages, each with its own voxel size and a
   neighbour halo. A per-page grid over that raster is LOD'd by construction,
   seams on shared halo values, and needs no mesh build at all - the vertex
   stage pulls straight from the height buffer. See "Mesh source" below.

**Containment, measured over 2,278,400 columns:**

```text
mesh displaced by            columns outside the mesh   worst
the exact height, stride 1            0.0000%           0 voxels
the exact height, stride 4           40.5443%       0.649 voxels
voxel tops vs that mesh, stride 1   100.0000%       1.000 voxels
voxel tops vs max-envelope mesh       0.0000%           0 voxels
```

The plan's claim that containment is definitional is **false as written**. A
voxel column's top *face* sits above its exact height by up to a full voxel by
construction, so a mesh through the height field leaves every single column's
top voxel outside it - 100% of columns, p50 0.476 voxels. Voxelization of a
surface produces cells that straddle it; it does not produce cells below it.

The fix is cheap and exact: displace each mesh corner by the **maximum cell top
of the columns in the quads touching that corner** (a dilation by one stride).
Bilinear interpolation is never below the minimum of its four corners, so every
enclosed column's top is then at or below the mesh. Measured: 0 columns outside
at any stride. What it costs is march length, and that is bounded:

```text
march from the envelope down to the voxel top
  stride 1:  p50 1.000  p99 1.000  max 1.000 voxels
  stride 4:  p50 1.000  p99 1.500  max 2.500 voxels
```

So the design's central claim - "the march from the mesh surface is ~1 step" -
holds, and at stride 1 it is exactly one voxel everywhere rather than
approximately one. This is the construction T2/T3 must displace by; a mesh
through the raw height field would be holes, not seams.

### T0 - original text

The entire trade rests on a number I have only estimated: CBT mesh fragments at
the tessellation depth this needs. Also measure containment.

- Render the CBT mesh at candidate `renderSubdivisionDepth`, count fragments and
  covered pixels at the standard camera-walk framing.
- Measure signed mesh-vs-voxel disagreement: how often, and by how much, the
  voxel surface lies outside the mesh.

**Gate:** mesh overdraw must be comfortably under ~8x (against today's 15.163x)
for the substitution to have margin. **Kill if** overdraw is within ~2x of the
brick path - there is then no prize to spend on the march.

### T1 - Upload the shared height raster. **DONE, 2026-08-15**

`haloHeights` is now published from the chunk build as
`PlanetVoxelChunk::surfaceHeights` (float, 166x166 per page: 160 columns plus
the 3-column halo the build already samples), concatenated across the resident
set into `PlanetVoxelChunkSet::surfaceHeights` in record order, and uploaded as
one storage buffer. It is part of the transient page payload, so a spilled page
restores with its raster; the payload magic is bumped accordingly.

Addressing is clipmap-consistent without a world-space lookup: a page's slice
is chosen by the drawing instance and the halo covers the seam, so a page can be
meshed knowing nothing about its neighbours. 89 pages x 166^2 x 4 B = **9.8 MB**,
matching the estimate.

**Gate: PASSED, both halves.**

```text
seam consistency   pairs=92  samples=44,160  mismatches=0  max 0.000000000 m
GPU parity      compared=22,784  mismatches=0  max 0.000000000 m
```

The seam half was the one at risk and is worth stating: a page samples its halo
through the cube-tile mapping while its neighbour samples the same world
position through its own local patch parameters. Two different code paths
reaching the same texel is an assumption, and any disagreement is a crack at
every page boundary. Measured bit-exact over all 92 adjacent same-LOD pairs.

The GPU half compares what a shader invocation *read* against what the CPU
uploaded, through the same `terrainMeshProbeHeight()` the mesh is built from.
Two implementation notes worth keeping:

- `vertexPipelineStoresAndAtomics` is not enabled on this device, so a vertex
  stage cannot publish anything. The sampler is a compute entry in the same
  shader, sharing the addressing helper so the gate tests the real code path.
- The per-frame uniform block rebuilds its struct from scratch, so a field set
  once at graph build reads as zero from the second frame on. That silently
  disabled the sampler and made the gate compare against a buffer nobody wrote.
  The brick draw carries a comment about exactly this; it cost a run here too.

### T1 - original text

### T2 - Terrain visibility pass, smooth. **LANDED, gate partly closed**

`--terrain-mesh-visibility` draws the same mesh into the brick raster's own four
attachments and depth, with `LoadOp_Load`, after the brick phases and before the
resolver. Nothing downstream changed - not one line - because the existing
`SparseVoxelBrickResolvePass` copies those attachments into the resolver
contract exactly as it does for bricks, and the two paths compose through the
shared depth buffer, which is what keeps non-terrain brick geometry working.

Published per fragment: `packedMaterialX/Y` from a per-column material raster
exported by the chunk build (the appearance the column's top voxel carries),
`surfaceT` from a **ray/plane intersection against the triangle's plane** rather
than the interpolated position - surfaceT has to be measured along the ray the
G-buffer reconstructs for that pixel, jitter included - `frag_depth` from that
world point, and `bottomAlbedo | context << 24`. `microPayload` is zero, which
the resolver already reads as a legacy parent hit.

**Identity** hashes the world voxel cell (`floor(world / voxelSize)` at the
page's own voxel size) and nothing else. Not the triangle, not the quad, not the
page slot: anything tessellation-derived throws away history whenever the mesh
changes and the image boils under motion.

The mesh is displaced by the **envelope**, not the height field, for two
reasons: it is the surface T3 carves from, and it is the only displacement that
wins the depth test against the terrain bricks that are still being drawn at
this stage. A mesh through the height field sits below every voxel top and would
be invisible behind the incumbent.

**Gate:**

```text
coveredPixels   baseline 1,254,989   mesh 1,254,989   identical
depthHash              40da0e80...   aa8005b2...      changed
resolvedSurfaceHash    b866f8f8...   917d291e...      changed
surfaceIdentityHash    bdd7cede...   16f0fe04...      changed
packedMaterialBottom   85570b24...   c7fffff2...      changed
```

Coverage identical to the pixel while every geometry and appearance hash moves
is the signature wanted: the mesh replaced the terrain surface across exactly
the pixels the brick path covered, with no holes and no extra pixels.

**Temporal clause: closed, against the correct control.** `--test-camera-dolly`
is useless here - it renders a frame-frozen image *identical in both arms*, so
it does not exercise terrain at all. The walk camera plus the existing per-probe
flicker metric does, and the first reading looked bad: mean flicker doubled
while median and p90 *improved*, which is the shape the metric's own comment
warns about - a small population losing history entirely rather than everything
getting noisier.

```text
                                mean   median     p90   severeFraction
bricks + exact micro (shipping) 0.028   0.0077  0.045       0.013
mesh, no micro (T2)             0.049   0.0050  0.023       0.040
bricks, micro disabled          0.079   0.0085  0.147       0.070
```

The third row is the control that settles it. T2 deliberately publishes no
micro geometry, so the comparable brick configuration is the one with micro
disabled - and against that, the mesh is **better on every statistic**, roughly
halving both mean and severe fraction. The entire gap to the shipping path is
exact micro geometry, which is what T3's carve restores. The identity scheme is
not implicated.

Two hypotheses died on the way, both worth recording because each cost a run:

- *"The two publishers tie and alternate."* Plausible - the envelope equals a
  column's own voxel top on flat ground, and the two paths publish different
  identities. Refuted directly: `--terrain-mesh-only`, which suppresses the
  terrain brick roots outright, produces a **byte-identical output hash** to
  leaving them on. The bricks were already contributing nothing; the mesh wins
  every terrain pixel deterministically because it is drawn later and is at or
  above the voxel tops under `GreaterOrEqual`.
- *"Lifting the mesh fixes it."* A lift did reduce severe flicker monotonically
  (0.040 -> 0.025 at half a voxel, 0.010 at two voxels), which looked like
  confirmation of the tie hypothesis. With the tie refuted, that improvement has
  to be a geometry change - a raised surface moves probe placement - and is not
  evidence about identity. A measurement that moves in the expected direction is
  not a diagnosis.

**Bonus, and it is T6's gate arriving early:** the identical output hash under
`--terrain-mesh-only` proves the terrain brick draws are *already fully
redundant* once the mesh publishes. Routing terrain off the brick path is a pure
removal with no image change, which is the hard half of T6 demonstrated before
the pass that needs it.

### Silhouettes, depth, and which fragments can be skipped

The question T3 turns on: when the fragment carves voxels out of the mesh, what
happens at a silhouette, where the voxel that should be visible sits *behind*
the triangle that covered the pixel?

**1. Screen coverage is safe, and that is what the envelope buys.** With
`mesh(x,z) >= voxelTop(x,z)` everywhere - measured, 0 of 2,278,400 columns
outside - the voxel solid is a subset of the mesh solid. A ray that reaches any
voxel must first cross the mesh surface, the mesh is a continuous height field
so that crossing lands on a triangle, and therefore a fragment exists at that
pixel. There is no silhouette hole to fix; there would be one the moment the
mesh stopped being an envelope.

**2. Depth must be the marched hit, and that is why DepthLess is correct.** The
carved hit is always at or beyond the entry crossing, so writing it only ever
pushes depth away from the eye. That is exactly the conservative-depth
declaration `DepthLess` promises, so early-Z survives a fragment shader that
writes `frag_depth`; the contract shader already declares it. A near triangle
whose march lands far away simply loses the depth test to whichever fragment
found a nearer voxel, which is the brick path's model unchanged.

**3. The march bound is the correctness rule, not the step cap.** Each fragment
must march from *its own* entry until it hits or leaves the mesh-solid interval
it entered. A fixed step cap that `discard`s on exhaustion can drop a voxel no
other fragment covers; a cap that *accepts* the deepest tested cell fails
conservatively instead. This is the constraint T4's empty-space skipping has to
respect.

**4. Back-face culling is the cheap fragment saving, and it is not free.**
Measured with the T0 instrument at stride 1:

```text
cull      fragments   coveredPixels   overdraw   peak
none      1,795,418      1,178,229     1.524x     38
back      1,410,626      1,178,177     1.197x     19
front       384,792        164,837     2.334x     19
```

Culling back faces removes **21.4% of fragments** and halves peak depth
complexity, taking the mesh to **1.197x against the brick path's 15.163x**. It
costs 52 covered pixels of 1.18 M - and those 52 are precisely the case in
question: pixels where only a back face covers the pixel, i.e. the camera seeing
the inside of the envelope. Under a carve they become holes. The brick raster
has the same case and solves it by emitting a full-screen triangle when the
camera is inside the box; the mesh path needs the equivalent before back-face
culling can be turned on by default.

### T2 - original text

Rasterize the CBT mesh displaced by the shared raster; publish the full resolve
contract with the smooth surface. Voxel carving deliberately absent.

This isolates *integration* from *voxel math* - if lighting, probes, SVGF, TAA
or shadows break, it is the contract, not the march.

**Gate:** coverage matches the brick path within the terrain region; downstream
passes run unmodified; no temporal boiling under the dolly camera (validates the
tessellation-independent identity).

### T3 - Carve the voxel surface.

Add the inward march, slab-intersect for face and exact `t`, discard on miss,
declare DepthLess.

**Gate:** parity against the brick path's `geometryNormal` and face
distribution; silhouettes visibly stair-stepped; `coveredPixels` slightly *lower*
than the brick path (erosion recedes the outline) and never higher.

### T4 - Bound the grazing march.

Near-tangent rays are the unbounded case. Add conservative empty-space skipping
from a min/max height pyramid - `SparseVoxelPlanetSurfaceAtlas` already carries
`heightMinMax` (RG16Float, mipped) with CPU-built mips; the structure and
sampling path exist, only the resolution is wrong.

**Gate:** 99th-percentile march length bounded; no visible artifacts from the
step cap at grazing angles.

### T5 - Distance fade.

Hard cell quantization below the sampling rate shimmers, and this renderer has a
long flicker history. Fade the carve out where voxels go sub-pixel, mirroring
the exact-micro handoff that already ships and was worth 5.23 ms.

**Gate:** judged on the **moving** camera sequences, not a static frame - the
same discipline the probe-tile note calls for.

### T6 - Route terrain off the brick path, make it default.

Terrain bricks stop being culled and drawn; the brick path keeps serving
non-terrain content; Tree64 keeps being fed.

**Gate:** the measurement that justifies the whole plan - whole-frame min-of-N
with cooldown against the 51.3 ms baseline, quoting spread. Voxel test suite
shows only the 7 known pre-existing failures.

### T7 - Reclaim what is now dead.

Only after T6 holds: `microSurfaceData` (108 MB) and the terrain descriptor
system become unreferenced for terrain. Removing them is a separate change with
its own exactness rebaseline.

## Risks and kill criteria

| risk | signal | response |
| --- | --- | --- |
| CBT overdraw close to 15x | T0 | **kill** - no prize |
| voxels outside the mesh | T0 containment | fix the height source first; holes are unacceptable |
| grazing marches unbounded | T4 99th percentile | min/max pyramid; if still unbounded, cap and accept silhouette artifacts only beyond the fade distance |
| identity flickers with tessellation | boiling under the dolly camera at T2 | identity must hash the world voxel cell, never anything tessellation-derived |
| shimmer when sub-pixel | moving-camera review at T5 | earlier fade distance |
| non-terrain regressions | voxel suite | the brick path must remain untouched for non-terrain |

**Two traps this project has already paid for, restated because they apply
directly here:**

- `--skip-output-signature` disables the coverage readback, so `coveredPixels`
  reads 0 and looks like broken geometry. Check a known-good arm too.
- min-of-N over benchmark repeats **selects** a contaminated run. One repeat this
  session sat entirely below the others because terrain had not finished
  streaming, and min-of-N would have reported a fake -12.65 ms win. Use medians
  and inspect per-repeat min/max.

## What this reuses rather than invents

Worth listing, because it is most of the risk retired:

- **Conservative depth**: `spirv_add_execution_mode.py` + `EXECUTION_MODES
  DepthLess`, already applied to the brick draw for exactly this reason.
- **The resolve contract**: unchanged, so every downstream consumer is untouched.
- **`heightMinMax` pyramid**: already built, mipped and sampled by the CBT chain.
- **CBT adaptive tessellation with LOD**: already the mechanism that would supply
  near-field detail.
- **`haloHeights`**: the final height field, already computed per chunk with a
  neighbour halo, currently discarded after voxelization.
- **The handoff-fade pattern**: already shipping for exact micro, already tuned.
