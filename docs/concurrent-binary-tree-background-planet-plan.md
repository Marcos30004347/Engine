# Concurrent binary tree background planet rendering plan

## Outcome

Replace the fixed whole-planet overview mesh with a bounded, GPU-driven adaptive
triangulation based on Benyoub and Dupuy's *Concurrent Binary Trees for
Large-Scale Game Components*. The background/overview planet should refine
smoothly where it occupies screen space, decimate where detail is unnecessary,
and continue using the package-authored eroded height, material, mountain,
river, and roughness maps.

The first integration targets `SparseVoxelPlanetOverviewPass`. It does not
replace local sparse-voxel terrain, Tree64, or the existing near-surface horizon
cap. Those systems remain responsible for gameplay-scale geometry after the
navigation transition reaches the surface.

The intended render path is:

```text
coarse closed halfedge mesh
        |
        v
CBT-managed active bisector pool <--- incremental screen-space LOD update
        |
        v
procedural indirect triangle draw
        |
        +--- eroded height texture -> vertex displacement and relief normal
        +--- material maps --------> fragment shading
        v
existing overview color/depth and navigation merge
```

The result must be progressive and bounded: a newly opened planet is always
renderable from its coarse roots, refinement may take several frames, and pool
exhaustion must retain the last valid conforming mesh rather than corrupting
topology or stalling the render thread.

## Source and adaptation

Reference:

- Anis Benyoub and Jonathan Dupuy, *Concurrent Binary Trees for Large-Scale
  Game Components*, Proceedings of the ACM on Computer Graphics and
  Interactive Techniques 7(3), 2024, DOI `10.1145/3675371`.
- Local reference copy: `Concurrent Binary Trees for Large-Scale Game
  Components.pdf`.

The implementation should follow these ideas from the paper:

- One root bisector per halfedge of a coarse polygon mesh.
- Newest-vertex bisection with compatibility-chain propagation so local
  refinement remains conforming.
- Conservative triangle/quad decimation.
- A CBT used as a fixed-capacity memory-pool allocator and active-thread
  scheduler, independent from geometric subdivision depth.
- Incremental updates using reset, pointer-cache, command generation,
  reservation, block creation, neighbor repair, bitfield update, and
  sum-reduction stages.

Engine-specific adaptations:

- Use the existing `PlanetSurfaceHalfedgeMesh` and
  `PlanetSurfaceBisectorAddress` CPU model instead of introducing another
  hierarchy identity.
- Store the 64-bit path as two `u32` words in WGSL. The shader tree currently
  has no established native `u64` convention.
- Use normalized, camera-relative planet coordinates. The paper needs double
  precision for true-scale world-space vertices; the overview renderer avoids
  those large metre-valued translations by design.
- Render active bisectors directly with a procedural non-indexed indirect draw.
  A visibility buffer is not required for the first integration because the
  existing overview pass already shades a dedicated color/depth target.
- Retain the current immutable overview mesh behind a runtime switch until the
  adaptive path passes visual and performance gates.
- Independently implement the algorithms against the paper. Do not copy source
  code from the authors' repository without first reviewing its license and
  recording the dependency decision.

## Current foundation

The repository already provides:

- `PlanetSurfaceHalfedgeMesh`, including `next`, `previous`, and `twin`
  topology.
- Stable `PlanetSurfaceBisectorAddress {rootHalfedge, depth, path}` identities
  that do not depend on recycled pool slots.
- CPU reconstruction of a leaf triangle from its root and binary path.
- A normalized whole-planet camera and animated surface/orbit navigation.
- A high-resolution 3-by-2 cube atlas containing albedo/roughness and terrain
  features derived from the generated planet package.
- Eroded raster height data in the terrain-feature atlas and relief-normal
  shading in the overview shader.
- Independent ocean occupancy, zero-datum water height, and negative
  `OceanFloorHeight` bathymetry in the planet package. The visible overview
  follows the water surface; the solid seabed remains available for future
  underwater displacement and voxel generation.
- Render-graph storage buffers, indirect compute dispatch, indirect graphics
  draw, and explicit resource-access declarations.
- Release smoke coverage for whole-globe bounds and center coverage.

The current limitation is that `SparseVoxelPlanetOverviewPass` uploads and
draws one immutable vertex/index mesh. Its triangle count does not respond to
camera distance, viewport size, visibility, or projected terrain error.

## Scope and non-goals

In scope:

- A closed coarse planet mesh and GPU root-bisector initialization.
- Fixed-capacity CBT allocation and active/free slot lookup.
- Conforming split and merge updates on the GPU.
- Camera-driven incremental LOD with hysteresis.
- Procedural rendering of the active bisectors.
- Height displacement and existing high-resolution planet materials.
- Diagnostics, deterministic CPU validation, GPU conformance tests, visual
  tests, and performance budgets.

Not in the first implementation:

- Replacing local voxel terrain or its chunk streamer.
- Generating terrain or erosion on the GPU.
- Continuous centimetre-detail flight from orbit to the ground using only the
  CBT mesh.
- Ocean FFT simulation, atmosphere replacement, mesh shaders, or a visibility
  buffer.
- Streaming arbitrary planet-package texture pages. The current bounded atlas
  remains resident for overview rendering.

## Geometry model

### Root mesh

Use a regular icosahedron as the initial closed triangle mesh. Passing its 20
faces through `buildPlanetSurfaceHalfedgeMesh` produces 60 root bisectors, the
same root count as the paper's dodecahedron example while reusing the engine's
already-tested triangular halfedge builder.

Each root bisector is formed by:

```text
v0 = vertex(halfedge)
v1 = vertex(next(halfedge))
v2 = face centre
```

Subdivision operates on the coarse polyhedron. Vertices are projected to the
unit sphere only after the leaf triangle has been decoded. Terrain height is
then sampled in the vertex shader and normalized by the source planet radius.

The package's large graph-native surface remains useful for authoring and CPU
data validation, but it should not be the adaptive root mesh. Starting from it
would spend most of the CBT pool before view-dependent refinement begins.

### Stable identity versus physical storage

Stable identity remains:

```cpp
struct PlanetSurfaceBisectorAddress
{
  uint32_t rootHalfedge;
  uint32_t depth;
  uint64_t path;
};
```

The GPU pool slot is transient. Tests, diagnostics, cache keys, and comparisons
must use the stable address, never the slot number. This is particularly
important because atomic allocation order is not guaranteed to assign the same
physical slots across GPU implementations.

### GPU data layout

Start with an explicit, alignment-safe layout and optimize packing only after
correctness is established:

```text
PlanetCbtBisectorGPU (12 x u32 = 48 bytes)
  next, previous, twin, command
  rootHalfedge, depth, pathLow, pathHigh
  allocation[4]

PlanetCbtPointerGPU (2 x u32 = 8 bytes)
  activePoolSlot
  freePoolSlot

PlanetCbtCountersGPU
  activeCount, freeCount, requestedAllocations, overflowCount
  splitCount, mergeCount, unchangedCount, maximumDepth

PlanetCbtDrawIndirectGPU
  vertexCount, instanceCount, firstVertex, firstInstance
```

Use `0xffffffff` as the null slot. Add C++ `sizeof`, alignment, and field-offset
assertions matching WGSL declarations.

Initial production capacity:

| Setting | Default |
| --- | ---: |
| CBT depth | 17 |
| Bisector pool capacity | 131,072 |
| Root bisectors | 60 |
| Target projected triangle area | 1024 px² |
| Refine threshold | 1.25 x target |
| Merge threshold | 0.55 x target |
| Maximum overview subdivision depth | 24 |
| Update rounds per ordinary frame | 1 |
| Update rounds during initial reveal | Up to 2 |

At depth 17, the initial unpacked design uses approximately 6 MiB for the
bisector pool, 1 MiB for the CBT heap, and 1 MiB for cached active/free
pointers. Counters, indirect arguments, roots, and uniforms are small. The
overview CBT allocation must remain below 10 MiB unless a measured quality case
justifies increasing it.

## CBT memory manager

Allocate a CBT heap with `2^(D+1)` `u32` entries and a pool with `2^D` slots.
The leaf bit for a slot is one when allocated and zero when free. Internal nodes
contain the number of allocated leaves beneath them; the root is therefore the
active bisector count.

Implement and share these GPU/CPU reference operations:

- `oneToBitID(i)`: locate the pool slot of the `i`th allocated leaf in `O(D)`.
- `zeroToBitID(i)`: locate the pool slot of the `i`th free leaf in `O(D)`.
- Leaf bit set/clear.
- Bottom-up sum reduction.
- Active/free count and indirect argument generation.

The pointer-cache stage should cover `max(activeCount, freeCount)`, rounded to a
workgroup count. This avoids assuming that the number of free slots is no
larger than the number of active bisectors during early refinement.

No stage may allocate CPU memory, read a GPU counter back to the CPU, rebuild
the render graph, or use `waitIdle` during a frame.

## Incremental GPU update chain

Model the paper's update as a fixed render-graph chain. Each numbered stage is
a distinct compute pass, or otherwise has an explicit intra-chain barrier that
the render graph can prove. Do not rely on dispatch order alone for storage
visibility.

1. `ResetCounters`
   - Clear requested allocations and per-frame diagnostics.
   - Derive active/free counts and dispatch arguments from the CBT root.
2. `CachePointers`
   - Cache active slots with `oneToBitID`.
   - Cache free slots with `zeroToBitID`.
3. `ResetCommands`
   - Clear the command word for every active bisector.
4. `GenerateCommands`
   - Decode triangle vertices from the stable address.
   - Evaluate visibility and projected error.
   - Atomically OR split commands across compatibility chains.
   - Generate conservative triangle/quad merge commands.
   - Reserve worst-case capacity before accepting a split request.
5. `ReserveBlocks`
   - Give each accepted command a unique range of cached free slots.
   - Split wins if a bisector receives both split and merge commands.
   - A merge proceeds only when every bisector in its configuration agrees.
6. `FillNewBlocks`
   - Write child or parent stable addresses and inherited neighbor pointers.
7. `UpdateNeighbors`
   - Scatter repaired `next`, `previous`, and `twin` pointers.
8. `UpdateBitfield`
   - Free superseded source slots and activate completed destination slots.
9. `SumReduction`
   - Reduce the CBT bottom-up with one dispatch per tree level and barriers
     between levels.
   - Write next-frame compute dispatch and current-frame draw arguments.

Only one refinement or decimation level is applied to a bisector per update
round. Multiple rounds during the overview reveal are repeated executions of
the complete chain, not shortcuts within a stage.

## LOD policy

### Projected error

Begin with the paper's projected triangle area metric. Decode and project the
three displaced vertices, conservatively clip against the near plane, and
estimate pixel area. Refine above the refine threshold and merge below the
merge threshold.

Then incorporate terrain-aware error:

```text
lodError = max(projected triangle area,
               projected displacement range,
               projected silhouette curvature)
```

The displacement-range term should come from min/max height mip data for the
covered cube-atlas region. Until min/max mips exist, use a conservative global
height range so terrain cannot under-refine.

### Hysteresis and visibility

- Keep separate refine and merge thresholds to prevent alternating commands.
- Never merge a visible neighbor configuration unless the conservative merge
  rules are satisfied.
- Offscreen and back-facing regions may converge toward the root mesh, but
  topology changes still propagate through compatibility chains.
- Clamp refinement by pool availability, configured maximum depth, texture
  resolution, and floating-point precision.
- If the camera is stationary and all visible leaves fall inside the hysteresis
  band, topology must stop changing.

### Precision boundary

The adaptive background planet uses a unit sphere and camera-relative
projection. Cap initial GPU refinement at depth 24 and stop earlier when the
projected edge is subpixel or the height texture has no finer information.
Local sparse voxels take over during descent before background geometry needs
centimetric precision. Native GPU `f64` is therefore not a phase-one
requirement.

## Adaptive draw path

Render one procedural triangle per active bisector:

```text
triangleID = vertexIndex / 3
cornerID   = vertexIndex % 3
poolSlot   = activePointers[triangleID]
address    = bisectorPool[poolSlot]
direction  = decodeAddressVertex(address, cornerID)
```

The final reduction writes:

```text
vertexCount   = activeCount * 3
instanceCount = 1
firstVertex   = 0
firstInstance = 0
```

`SparseVoxelPlanetOverviewPass` then calls `cmdDrawIndirect`. The draw reads the
same active-pointer buffer used by compute and samples the current surface
textures. Duplicating vertices between triangles is acceptable for this pass;
it avoids a geometry-compaction stage, and conforming topology prevents cracks.

Keep the current static indexed path selectable with:

```text
planet overview geometry: Static | Adaptive CBT
```

The static path is the recovery path for unsupported devices and a visual
comparison oracle during rollout.

## Height and material integration

The CBT changes geometry density, not terrain authorship. Continue deriving
appearance from the generated planet package:

- Albedo and roughness from the high-resolution surface atlas.
- Material, mountain, and river features from the terrain-feature atlas.
- Eroded height for vertex displacement and relief normals.
- Existing shared voxel material colors for visual continuity after landing.

Before enabling displaced adaptive geometry, split height into a dedicated
`R16_UNORM` or `R16_SFLOAT` cube atlas with the existing metre scale/bias. An
8-bit packed feature channel is sufficient for distant normal variation but is
not the preferred geometry source.

Generate mip chains for albedo, features, and height. Add a parallel min/max
height hierarchy for conservative LOD error. Cube-atlas gutters or seam-aware
sampling must ensure identical displacement along edges belonging to different
faces. The vertex shader must sample a deterministic mip from bisector depth;
the fragment shader may use gradients for ordinary material filtering.

## Ownership and proposed files

Keep topology/data logic reusable and rendering orchestration separate:

```text
src/voxel/svraster/PlanetCbt.hpp
src/voxel/svraster/PlanetCbt.cpp
src/voxel/svraster/PlanetCbtReference.hpp
src/voxel/svraster/rendering/SparseVoxelPlanetCbtUpdateChain.hpp
src/voxel/svraster/rendering/SparseVoxelPlanetCbtOverviewPass.hpp

assets/shaders/voxel/svraster/wgsl/sparse_voxel_planet_cbt_reset.wgsl
assets/shaders/voxel/svraster/wgsl/sparse_voxel_planet_cbt_cache.wgsl
assets/shaders/voxel/svraster/wgsl/sparse_voxel_planet_cbt_commands.wgsl
assets/shaders/voxel/svraster/wgsl/sparse_voxel_planet_cbt_reserve.wgsl
assets/shaders/voxel/svraster/wgsl/sparse_voxel_planet_cbt_fill.wgsl
assets/shaders/voxel/svraster/wgsl/sparse_voxel_planet_cbt_neighbors.wgsl
assets/shaders/voxel/svraster/wgsl/sparse_voxel_planet_cbt_bitfield.wgsl
assets/shaders/voxel/svraster/wgsl/sparse_voxel_planet_cbt_reduce.wgsl
assets/shaders/voxel/svraster/wgsl/sparse_voxel_planet_cbt_overview.wgsl

tests/voxel/SparseVoxelPlanetCbtTests.cpp
tests/voxel/SparseVoxelPlanetCbtGpuTests.cpp
```

If the shader toolchain has no reliable source-include mechanism, generate a
small shared WGSL prelude at configure time or duplicate only the stable decode
helpers with a test that compares their signatures. Do not create subtly
different address decoders in each kernel.

## Diagnostics

Add a `Planet CBT` section to the existing planet-navigation UI:

- Static/adaptive geometry selector.
- Pool capacity and active/free count.
- Split, merge, and unchanged counts.
- Maximum active depth.
- Requested and rejected allocations.
- Update rounds per frame.
- Projected-area target and hysteresis thresholds.
- Wireframe, depth heat map, and pool-pressure overlays.
- Per-stage and total GPU timestamps.
- Freeze-updates control for inspecting a stable triangulation.

Log initialization, mode changes, and first overflow only. Per-frame counters
belong in the UI/profiler and must not flood the log.

## Implementation phases

### Phase 1: CPU contract and reference model

- Add the normalized icosahedron constructor and convert it through the
  existing halfedge builder.
- Define the C++/WGSL GPU layouts, command bits, null values, and indirect
  argument structures.
- Implement CPU reference `oneToBitID`, `zeroToBitID`, reduction, root
  initialization, split, conservative merge, and neighbor repair.
- Compare reference leaf decoding against `planetSurfaceBisectorTriangle`.
- Add a topology validator that reports the first invalid stable address and
  neighbor relation.

Tests:

- The icosahedron is closed, consistently wound, and produces exactly 60 root
  bisectors with reciprocal twins.
- Root triangles cover every base face without overlap or holes.
- Random addresses through depth 24 reconstruct the same vertices through both
  CPU decoders.
- Every split sequence preserves active-slot uniqueness, reciprocal twins,
  valid `next`/`previous`, and conforming shared edges.
- Reversing valid split sequences through conservative merges returns the same
  multiset of stable roots.
- Capacity rejection leaves the previous topology byte-for-byte unchanged.

Exit condition: a seeded CPU stress test can execute at least 100,000 mixed
split/merge requests under AddressSanitizer and UndefinedBehaviorSanitizer with
no invalid topology or allocation.

### Phase 2: GPU CBT allocator primitives

- Allocate and initialize the CBT heap, pool, pointer cache, counters, and
  indirect arguments once when the overview planet is registered.
- Implement active/free binary search and bottom-up sum reduction in WGSL.
- Add dispatch-argument preparation without CPU readback.
- Add guard/canary words around test buffers in GPU test builds.

Tests:

- For empty, full, alternating, sparse, and seeded-random bitfields, GPU root
  counts equal CPU popcount.
- `oneToBitID` enumerates every active slot exactly once.
- `zeroToBitID` enumerates every free slot exactly once.
- Pool capacities from 64 through 131,072 produce correct reductions.
- Indirect dispatch never exceeds the pool and becomes zero-safe for an empty
  work range.
- Canary values remain intact after every kernel.

Exit condition: release Vulkan tests pass validation layers with no out-of-bounds
access, CPU readback is used only by the test harness, and frame execution needs
no host-visible count.

### Phase 3: conforming GPU split and merge pipeline

- Implement command reset and projected-area command generation.
- Port compatibility-chain split propagation with atomic command OR.
- Implement conservative boundary-pair and interior-quad merge agreement.
- Implement block reservation, fill, neighbor scatter, and bitfield update.
- Run the complete fixed render-graph update chain one round per frame.

Tests:

- Scripted one-root, twin-pair, compatibility-chain, boundary, and quad-merge
  cases match the CPU reference by stable-address multiset.
- Randomized GPU update sequences preserve all topology invariants after every
  round.
- Physical pool-slot order may differ, but stable addresses and neighbor
  identities must match.
- Conflicting split/merge requests deterministically resolve in favor of split.
- Simultaneous reservations never reuse a destination slot.
- Pool exhaustion increments diagnostics and retains a valid conforming mesh.

Exit condition: 10,000 seeded camera/command rounds run on the GPU without a
topology violation, validation error, stale pointer, or allocator leak.

### Phase 4: procedural adaptive overview draw

- Add the procedural vertex decoder and indirect triangle draw.
- Feed the active pointer and bisector pools into the overview graphics pass.
- First render a radial sphere with existing lighting but no height
  displacement.
- Keep static and adaptive paths switchable without rebuilding the graph.
- Add wireframe/depth diagnostics.

Tests:

- At root depth, adaptive and CPU root meshes have matching projected bounds,
  winding, and center coverage.
- Fixed scripted subdivisions produce identical image coverage to an equivalent
  CPU-expanded triangle list.
- No black pixels or cracks appear along refined/unrefined edges at cube faces,
  the limb, or the poles.
- Disabled adaptive mode preserves the existing static overview signature.
- Changing the active count changes indirect geometry without a CPU draw-count
  update.

Exit condition: the complete planet renders through the CBT path in the release
Vulkan smoke test, with the static path retained as an immediate fallback.

### Phase 5: terrain-aware LOD and high-resolution displacement

- Add the dedicated higher-precision eroded-height cube atlas and mip chain.
- Preserve separate land, water-surface, and ocean-floor fields: render the
  ocean at `WaterHeight`, and sample `OceanFloorHeight` only when solid
  underwater geometry is explicitly requested.
- Add seam-safe height sampling and min/max height mips.
- Displace procedural vertices and reconstruct relief normals from the same
  height source.
- Add displacement and silhouette terms to the LOD metric.
- Select texture mip from bisector footprint and keep materials aligned with
  displaced geometry.

Tests:

- A synthetic constant-height planet remains perfectly spherical at the
  expected radius.
- Analytic ridge, crater, and step maps refine where projected displacement
  error exceeds the threshold and merge after zooming out.
- Shared vertices on all cube-face edges sample bit-identical heights.
- Eroded land/ocean boundaries do not produce cracks or material displacement
  disagreement.
- Ocean-floor elevation is negative after channel decoding, is zero outside
  the ocean mask, approaches the water datum continuously at coasts, and never
  replaces the flat visible ocean surface.
- Height, material, and marker directions remain aligned after arbitrary orbit
  rotations.
- High-frequency checker and impulse maps show mip convergence rather than
  shimmer or aliasing.

Exit condition: the adaptive planet preserves the current terrain/material
look, improves close-orbit silhouettes, and has no visible atlas seams during a
scripted 360-degree orbit.

### Phase 6: navigation lifecycle and temporal stability

- Enable bounded multi-round warm-up during ascent and initial planet reveal.
- Pause or reduce updates when overview contribution is zero.
- Apply hysteresis, offscreen decimation, depth limits, and overflow fallback.
- Reset cleanly when a different planet package is loaded.
- Preserve target marker and picking behavior independently of triangulation.

Tests:

- Surface, ascent, orbit, loading, descent, and return transitions never expose
  an uninitialized CBT or blank planet.
- Repeated `M`, rapid drag, wheel zoom, and destination clicks do not rebuild
  resources or invalidate active topology.
- A stationary camera converges and then produces zero split/merge churn.
- A repeatable orbit camera path produces bounded active counts and no one-frame
  triangle spikes.
- Package reload returns to 60 valid roots before refining the new planet.
- Forced tiny capacity remains interactive and visually coarse rather than
  failing.

Exit condition: the entire navigation smoke sequence runs with zero render-thread
waits, zero GPU resource creation after initialization, and stable topology
after the camera settles.

### Phase 7: performance qualification and rollout

- Capture static-versus-adaptive visual and GPU timing baselines.
- Tune pool capacity, workgroup sizes, thresholds, and warm-up rounds on the
  project reference GPU.
- Add compact packing only if measured bandwidth justifies its complexity.
- Make adaptive CBT the default after all release gates pass; retain a device
  capability fallback to the static path.
- Update sample usage text and the main planet-navigation plan.

Release gates:

- Active triangles remain at or below 128,000 under the standard orbit path.
- Total CBT storage remains at or below 10 MiB with default settings.
- Total incremental update GPU time is at most 0.50 ms p95 and 0.75 ms maximum
  on the project reference GPU at 1280x720. Record the exact device and driver
  with the baseline.
- Adaptive draw plus update is no more than 15% slower than the static overview
  at the same visible triangle count.
- There are no per-frame GPU allocations, CPU readbacks, queue-idle waits, or
  render-graph rebuilds.
- A stationary camera changes fewer than 0.1% of active leaves after
  convergence; the expected value is zero.
- At least 95% of visible leaves settle inside the configured projected-error
  band without exhausting the pool.

Exit condition: release build, focused CPU/GPU suites, validation-layer smoke,
scripted navigation smoke, visual baselines, and timing budgets all pass on the
recorded reference configuration.

#### Phase 7 qualification record (2026-08-11)

Reference configuration: Apple M4 through MoltenVK 1.2.11 (Vulkan driver
version `10211`), Release build, Vulkan, 1280x720, render scale 1, frame
generation and MetalFX upscaling disabled. The deterministic harness runs a
fixed-distance orbit followed by twelve stationary convergence frames and
discards its first twelve timestamp samples.

The tuned production settings are a depth-17/131,072-slot pool, 256-thread
workgroups, a 512-allocation per-round cap, 1024 px² projected-area target, one
ordinary update round, and two rounds during each of the two reveal warm-up
frames. Heap sums are maintained along changed ancestor paths, so production
does not run a full bottom-up reduction. The fixed stages share one explicitly
barriered compute node; no compact bisector packing was added because storage
and measured bandwidth already meet their gates.

Measured adaptive result over 60 frames:

```text
active=386 stable=386 split=0 merge=0 overflow=0 storage=9.005 MiB
update avg/p95/max = 0.2094 / 0.2730 / 0.2769 ms
draw   avg/p95/max = 0.0091 / 0.0241 / 0.0252 ms
```

Retained static-path baseline at the same camera and viewport:

```text
draw avg/p95/max = 3.2301 / 4.2074 / 4.3005 ms
```

The Phase 4 CPU-expanded coverage fixture remains the matched-tessellation
visual comparison; the numbers above compare the two shipping render paths.
The adaptive path is the default on capable devices. `--static-planet-overview`
forces the fallback, `--adaptive-planet-overview` forces the requested mode
subject to capability checks, and `--qualify-planet-cbt` runs the repeatable
orbit/convergence timing gate and reports the exact device, driver, topology,
storage, update, and draw results.

## Test matrix

### CPU unit tests

- Halfedge root construction, winding, and reciprocal topology.
- Stable address parent/child operations at depth boundaries.
- Two-word path shifts and bit extraction through depth 63.
- Subdivision matrix/triangle decoding parity.
- CBT popcount reduction and active/free rank lookup.
- Split compatibility-chain and conservative-merge configurations.
- Allocation exhaustion and command-conflict behavior.
- Deterministic topology signatures independent of physical pool slots.

### GPU conformance tests

- Each compute kernel against CPU reference fixtures.
- Full update rounds over scripted and seeded-random command sets.
- Storage barriers between every dependent stage and reduction level.
- Indirect dispatch/draw argument production.
- Canary protection and validation-layer cleanliness.
- Empty, root-only, half-full, nearly-full, and full pools.

### Integration tests

- Static/adaptive runtime switching.
- Graph-native and raster-fallback planet packages.
- Missing optional feature channels.
- Viewport resize and FOV changes.
- Orbit zoom from minimum to maximum distance.
- Navigation transition while topology is still converging.
- Destination marker and surface picking over adaptively refined geometry.
- Planet-package replacement and device fallback.

### Visual tests

- Whole globe fits the default camera and covers the viewport center.
- Wireframe has no T-junction cracks under asymmetric refinement.
- Cube seams, poles, limb, coastlines, mountain silhouettes, and atmosphere.
- Static/adaptive material parity at matched tessellation.
- Eroded height displacement remains aligned with albedo and biome maps.
- Temporal sequence tests for stationary camera, slow orbit, fast drag, and
  exponential zoom.

### Failure-injection tests

- Pool capacity smaller than requested refinement.
- Maximum depth reached.
- Invalid root topology rejected before GPU upload.
- Missing or invalid height atlas falls back to a radial sphere/static mesh.
- Shader or required device-feature failure selects the static renderer.
- Package changes while the old CBT is active.

## Acceptance criteria

- The background/overview planet uses a GPU-managed adaptive triangulation with
  one stable representation across all orbit zoom levels.
- Refinement and decimation are conforming; no cracks, T-junction holes, stale
  neighbors, or invalid pool references appear.
- The planet continues to use the generated high-resolution eroded height and
  material textures.
- LOD reacts to viewport, camera distance, terrain displacement, and silhouette
  error without visible oscillation.
- All geometry updates and draws are GPU-driven after initialization.
- Resource usage is fixed and bounded, and exhaustion degrades quality safely.
- Navigation, dragging, zooming, picking, and destination markers keep their
  existing behavior.
- Local sparse-voxel rendering remains unchanged and takes over before the CBT
  precision boundary becomes visible.
- Release correctness, GPU validation, visual, temporal, and performance gates
  pass before adaptive CBT becomes the default.

## Principal risks

| Risk | Mitigation |
| --- | --- |
| Atomic update order changes physical slot allocation | Compare and diagnose stable bisector addresses, never slot order. |
| Missing storage barriers produce intermittent topology corruption | Represent dependent kernels and reduction levels as explicit render-graph stages with declared read/write access. |
| Compatibility chains overrun the fixed pool | Reserve conservative worst-case capacity before issuing commands; skip refinement and increment overflow telemetry when unavailable. |
| Refinement and merge oscillate while orbiting | Separate thresholds, one-level updates, stationary convergence tests, and bounded warm-up rounds. |
| `u64` is unavailable or inconsistent in WGSL | Store path as two `u32` words and test shifts/bit extraction through the supported depth. |
| Float precision collapses deeply subdivided vertices | Use normalized camera-relative space, cap overview depth, and hand off to local voxels before sub-metre background detail is required. |
| Height sampling opens cube-face cracks | Dedicated higher-precision height atlas, seam-aware/gutter sampling, and bit-equality tests on every shared face edge. |
| Direct procedural draw duplicates vertex work | Measure first; active count is bounded and avoids an additional compaction/index-generation pass. Add visibility-buffer or mesh-shader paths only if profiling warrants them. |
| Nine update stages cost more than the static mesh | Timestamp every stage, update only while visible/needed, and retain the static fallback until performance gates pass. |
| Existing graph-native mesh and CBT root mesh diverge visually | Treat package textures as authoritative appearance; use graph data for authoring/validation, not as a second shading source. |

## Deferred extensions

- A visibility-buffer shading path like the paper's terrain example.
- Mesh-shader emission when the engine has a portable mesh-shader abstraction.
- Planet texture tile streaming driven by stable bisector ancestors.
- Water-specific displacement and FFT waves.
- Atmosphere and cloud LOD coupled to the same active surface hierarchy.
- Deep fixed-point or double-precision decoding for continuous orbital-to-ground
  flight without the existing voxel handoff.
