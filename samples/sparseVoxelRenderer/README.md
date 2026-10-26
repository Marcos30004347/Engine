# Sparse voxel brick renderer sample

The G-buffer path now uses only 8x8x8 bricks. The former SVRaster
TLAS/LOD/projection/binning/resolve chain is not submitted. Legacy
`--gbuffer-resolver svraster|tree64` arguments remain temporary compatibility
aliases for `bricks` while existing benchmark commands are migrated.

Configure and build from the repository root:

```sh
cmake -S . -B build -DENGINE_BUILD_SAMPLES=ON
cmake --build build --target SparseVoxelRendererSample -j
```

Run from `build/tests` so the executable can find the generated shader assets:

```sh
cd build/tests
./SparseVoxelRendererSample
```

Normal interactive launches start at 1280x720. Use `--resolution WIDTHxHEIGHT`
to request another window and swapchain size, for example Full HD:

```sh
./SparseVoxelRendererSample --resolution 1920x1080
```

Startup logs report the requested, window, drawable, and final swapchain sizes
so macOS display scaling can be checked directly.

`--resolution` sets the output/drawable resolution. Temporal upscaling renders
internally at `--render-scale` (0.25 to 1.0). The fixed default is Performance
(50% per dimension); for example, Full HD uses a 960x540 input and a 1920x1080
output:

```sh
./SparseVoxelRendererSample --resolution 1920x1080
```

The same input scale, presets, and current input/output dimensions are
available under **Temporal AA / TSR > Upscaler** in the ImGui panel. Super
Performance selects a 480x270 input for a 1920x1080 output.

The portable frame-generation backend now follows the FidelityFX-style staged
data flow: previous/current log-luminance pyramids, coarse-to-fine optical
flow, a validity-weighted game-motion inpainting pyramid, two-sided
depth/disocclusion validation, and a color-inpainting pyramid. Select it under
**Frame generation**, or launch with `--frame-generation`. The generated frame
is presented before the real frame and ImGui is composed late on both.

On macOS, MetalFX temporal upscaling and frame generation are enabled by
default. The reusable RHI boundary lives in
`rendering/gpgpu/MetalFXPass.hpp`; the sample does not include Objective-C or
Apple framework code. Select the backends in **Temporal AA / TSR > Upscaler**
and **Frame generation**. To select the portable WGSL frame-generation path
from the command line, use:

```sh
./SparseVoxelRendererSample \
  --frame-generation
```

The temporal upscaler requires macOS 13 or later. Native frame interpolation
requires macOS 26 and a GPU accepted by MetalFX's runtime capability query.
Both paths reuse MoltenVK's Metal device, graphics command queue, and exported
textures; ImGui is composed afterward so it is not interpolated. The existing
WGSL FSR-class upscaler and WGSL frame interpolation remain the defaults on
other platforms. `--disable-frame-generation` keeps MetalFX upscaling but turns
off interpolation; `--disable-metalfx-upscaler` selects the WGSL upscaler, and
`--frame-generation` selects both portable WGSL temporal stages.
`--metalfx-frame-generation` enables TAA/TSR automatically, while
`--metalfx-upscaler` selects the native temporal accumulator. Frame-generation
launches select FIFO/vsync by default so generated and real images reach
consecutive presentation intervals; `--disable-vsync` remains available for
profiling and intentionally gives up reliable display pacing.

The terrain virtual-geometry route renders through one visibility buffer:
every rasterizer writes a 64-bit key per pixel (reverse-Z depth bits over
`packVisibilityPayload(cluster, triangle)`, Nanite's layout), a merge pass
folds the hardware draw's depth and payload attachments with the software
rasterizer's buffers, and a fullscreen resolve writes the whole G-buffer -
depth, ids, material, UV, normal, motion - from the key. Hardware-drawn and
software-drawn pixels are therefore indistinguishable downstream.
`--vg-software-raster` routes clusters whose average projected triangle edge is
under 32 px to the in-graph WGSL compute rasterizer (two atomic passes, depth
then payload, because MoltenVK exposes no 64-bit buffer atomics);
`--disable-vg-software-raster` is the explicit off and the default, so every
cluster takes the hardware meshlet draw. `--vg-metal-raster` enables the
dormant native Metal atomic64 encoder instead; it needs exportable buffers and
its hand-off from the graph is not ordered against two-phase occlusion, so it
is a bring-up tool, not a rendering mode.

Temporal upscaling and frame generation use the fixed **Performance** preset by
default: 50% output width and height, or one-quarter of the output pixels. The
**Super performance** preset uses 25% width and height, or one-sixteenth of the
output pixels. Select either under **Temporal AA / TSR > Upscaler**, or use
`--render-scale 0.5` and `--render-scale 0.25` respectively. Frame generation
does not dynamically change the selected scale. Super Performance uses the
portable WGSL temporal upscaler and frame interpolator because MetalFX does not
offer the required 4x temporal-upscale configuration.

The primary terrain source is now a planar `WorldGeneratorSample` export. Build
one with `--export`, then pass its directory to the renderer:

```sh
./build/samples/WorldGeneratorSample --headless \
  --output world-map.png --export world-data --export-resolution 1024
./build/tests/SparseVoxelRendererSample \
  --world-map world-data \
  --world-map-patch-km 16
```

The bridge combines refined and base elevation, uplift, relief/roughness,
erosion wear/flow/deposition, rivers, coasts, climate, biome mixtures,
placement potential, substrate, soil depth, and soil layers into the existing
voxel-brick and Tree64 renderer contracts. With no explicit center, startup
selects gentle dry ground with a visible mountain or cliff inside the initial
render distance and aims the camera at that skyline. `--world-map-center U,V`
pins the spawn to an exact normalized map point while still selecting a visible
relief target for the initial camera heading. `--world-map-patch-km` remains
accepted as a compatibility spelling for `--terrain-render-distance-km`;
generated worlds do not create a background mesh or planet surface. The finest
generated-world voxels are 10 cm by default. A base page is 128 x 128 cells
(12.8 m per horizontal edge); `--terrain-page-voxels N` overrides that size,
with `--planet-chunk-voxels N` retained as its compatibility spelling. The
projected-error selector builds one non-overlapping spatial quadtree leaf cut:
near pages retain the 10 cm scale, while farther pages cover larger footprints
at coarser voxel scales. The `Terrain LOD error pixels` control directly
rebuilds that cut; generated-world pages deliberately bypass the separate
brick-parent LOD control. Tree64 is built from the exact same committed page
cut as rasterization, and uncovered pixels resolve to the atmosphere/sky rather
than another terrain representation.

Generation emits the sampled top surface, exposed cliff cells, and configured
closure depth; hidden column interiors are omitted. Payload revisions include
the bundle, chunk address, internal LOD and generator revision, and recently evicted
payloads can be retained in the bounded CPU cache without changing their bytes.

As the camera moves, the renderer asynchronously builds missing chunks under
`--terrain-pages-per-update` and preserves stable physical slots for unchanged
owners. The generated map has finite bounds, so leaves beyond the export are
intentionally omitted. Admission prioritizes nearby chunks first, then chunks
inside the horizontal camera view, then off-screen chunks. The same rank is
retained by asynchronous Tree64 generation after pages are assigned stable
physical slots. The default update budget is 32 pages, reduced to 16 at 1440p
and above; use `--terrain-pages-per-update 0` only for an intentionally
unbounded transaction. Startup and metric changes obey the same budget as
camera movement. A retained worker pool produces one quadtree level at a time,
and each completed parent-to-four-children ownership group can publish without
waiting for unrelated chunks. The parent remains authoritative until all four
independent child instances are ready, so streaming cannot expose a temporary
hole.

The full-GPU builder keeps four immutable page lanes in one persistent render
graph. Columns, fine raster, optional raster hierarchy, Tree64 conversion,
renderer adaptation, and publication use the indirect dispatch `z` dimension as the lane address, so a
complete parent-to-four-children refinement group is generated without a
per-page CPU submission or readback bubble. Each lane has disjoint config,
scratch, result, and hidden output ranges. The four completed children remain
hidden until one generation-checked four-record ownership transaction commits
them together. Runtime admission is the minimum of urgent queue depth, four
ownership lanes, the generation-time budget divided by the page p95 estimate,
and transient memory divided by per-lane scratch plus hidden output. A one-page
floor preserves urgent progress under deliberately undersized diagnostic
budgets; more than four lanes do not improve publication latency until several
ownership groups may be in flight.

Generated-world chunks use stable physical GPU slots. Worker threads prepare
changed-slot packets and material/Tree64 data; the terrain upload pass installs
bounded packets while normal frames continue, then publishes raster and Tree64
as one generation after GPU completion. Camera movement therefore does not
reconstruct the render graph.

The previous complete slot remains authoritative until its brick,
material-field and Tree64 packet succeeds. Stale camera/generation results and
rejected capacity, memory or upload packets roll back on the CPU and leave the
resident GPU cut unchanged.

The **LOD / terrain streaming** panel exposes the progressive enable, split and
merge errors, CPU chunk-generation and GPU upload budgets, request/publication
freezes, the admitted and currently resident LOD ranges, and live CPU-cache,
GPU-page and Tree64 memory use. The Chunk LOD, Physical slot and Generation
buttons switch directly to the ownership debug views. Disabling progressive
streaming keeps the currently committed raster and Tree64 generation visible;
freezing only requests still permits camera-centred coarse spawn/despawn, while
freezing publication holds a completed transaction before GPU staging.

Generated-world benchmark JSON records `terrain_streaming_mode` as
`progressive-chunks`; OBJ and packaged-planet workloads record `none`.

`--terrain` no longer searches for a planet package. It requires an explicit
`--world-map <directory>`, so missing generated data fails clearly instead of
falling back to the old spherical terrain. The `M` planet-overview shortcut was
also removed.

Legacy planet generation and export belong to `PolygonTerrainSample`. Saving
there writes `polygon-terrain/planet.planetpkg`, `planet-mesh.ply`,
`planet-metadata.json`, and `terrain-config.json`; the sparse voxel renderer
never regenerates or rewrites those source files. Explicit
`--planet-package <file>` loading remains available for planet-specific tests.
The package stores the planet radius in metres and exposes derived kilometre
dimensions through its metadata API. The JSON companion records radius,
diameter, circumference, surface area, height encoding, elevation bounds, and
mesh counts. The binary PLY uses planet-centred XYZ kilometres and includes
stable region and source-triangle properties.

The package graph also contains the reusable mesh without requiring the PLY:
unit surface directions, encoded elevations, triangle indices, per-vertex
material/plate/climate fields, one stable Voronoi region per render face, one
source Delaunay triangle per render face, and the three stable region IDs of
every source triangle. Reconstruct a full-precision position in metres as
`direction * (planetRadiusMetres + decodedElevationMetres)`.

For a quick development package, generate a 64x32 planet with 4,096 graph
regions from the repository root:

```sh
./build/samples/PolygonTerrainSample \
  --headless --sphere \
  --output build/tests/polygon-terrain-small \
  --seed 0x5eed1234 \
  --resolution 64 \
  --tile-size 16 \
  --regions 4096 \
  --plates 16
```

```sh
./SparseVoxelRendererSample \
  --terrain-package /path/to/polygon-terrain/planet.planetpkg \
  --terrain-chunk-radius 3
```

To build and render one fine terrain page from a planet export:

```sh
./SparseVoxelRendererSample \
  --planet-package /path/to/planet.planetpkg \
  --planet-tile 0,14,8192,8192 \
  --terrain-chunk-radius 0
```

`--planet-tile` is optional. Without it, the sample decodes only the coarsest
height/material blocks, prefers gentle vegetated dry land with strong generated
relief in the outer part of the visible window, and resolves it to the runtime
page level.
After the resident pages are built, the camera is aimed at the most prominent
distant ridge while remaining over the selected land page. The default target
leaf size is 20 cm. Pages are refined
until one needs no more than 256 cells across, then its cell count is rounded up
to the next 8-cell brick boundary. With the current 1 km package contract this
selects level 19: a 31.25 m page with 160 cells, or 19.53125 cm per leaf. Use
`--terrain-voxel-size metres` to change that target. The camera is placed just
above the center terrain surface, never at the default ocean location. Terrain
preview uses the configured analytic sky, directional sunlight, ray-traced
Tree64 visibility, and GTAO by default.

`--terrain-lod-levels 8` sets the number of usable per-page resolution tiers;
the runtime clamps it to resolutions that remain brick aligned. With the
default 160-cell page this admits 160, 80, and 40 cells. These internal LODs
make distant pages cheaper, but do not change their physical footprint.

World coverage comes from a bounded grid of fixed-footprint chunk instances.
Each instance starts at its coarsest brick-aligned internal page and refines
toward the camera-selected hierarchy cut. Ownership does not depend on raster
or shadow-side clipping masks. Generated pages store a closed surface shell and
use their 10 cm parent cells directly, so horizontal sampling does not require
filling unreachable geology or carving sub-voxel detail into regular bands.

Generated terrain no longer enables exact microgeometry. Its raster pass uses
a separately compiled parent-voxel-only shader, and its Tree64 pages contain
occupancy without the material-geometry sidecar. This keeps raster visibility
and Tree64 ray visibility on the same 10 cm representation. The legacy exact
microgeometry shaders and qualification fixtures remain available only for
non-terrain compatibility tests.

For legacy exact-micro fixtures, `--brick-exact-micro-distance METRES` selects
the separately compiled distance-cutoff shader. It does not affect generated
terrain, whose material descriptors contain no sub-voxel geometry.

The R1/R2 brick micro-raster is **opt-in**, selected with
`--brick-micro-raster`. It is byte-identical to the incumbent full-brick draw
and measurably slower, so the default renderer keeps the incumbent path; see
"R1/R2 micro-raster measurement" below. After the Phase0
visible-brick cut, one 64-lane workgroup per visible brick compacts
guaranteed-exact heightfield parents into 16-byte records, writes the remaining
parents to a residual 512-bit occupancy mask, rebuilds its logical 64-bit coarse
mask, and prepares indirect draw arguments. `ResidualDraw` handles that legacy
occupancy, `PreparedMicroDraw` consumes the pre-resolved parent records through
an 18-vertex tight parent proxy without the full-brick occupancy/material walk,
and `PreparedTieReplayDraw` resolves sparse boundary-tie and proxy-edge
side-entry cases. All three complete before the Phase0 HZB is built.
Reservations are whole-brick: if the compact record buffer cannot hold
every accepted parent in a brick, that brick remains entirely in legacy
occupancy. Phase1 newcomers also remain on the full legacy path.

### R1/R2 micro-raster measurement

The split is retained as an experiment, not as the default raster. The initial
broad-extraction version was much slower (40.92 ms versus 24.19 ms p50).
Camera-facing extraction and the tight proxy reduce that gap, but two matched
built-in timestamp repeats, plus a post-fast-path R2 attribution capture, on
the default terrain package at 1200x720, LOD 0.75/1.5, camera-walk, 120 warmup
plus 360 measured frames still reject it:

| Arm | observed p50 range | observed p95 range |
| --- | ---: | ---: |
| `--brick-micro-raster` | 26.12--27.03 ms | 28.14--29.49 ms |
| default (incumbent) | 23.63--24.30 ms | 24.82--26.69 ms |

The output remains byte-identical in the synthetic acceptance matrix and the
separate full-resolution terrain capture. Per-pass p50s are useful attribution,
but their sum is not the percentile of a per-frame Phase0 sum. They also cannot
place a stall: Vulkan timestamps bracket Vulkan pass boundaries rather than
Metal encoders, so time lost inside MoltenVK is billed to whichever pass runs
next. Registering a second, identical depth-pyramid pass demonstrates it
directly - whichever of the two ran first absorbed about 4.5 ms and the other
cost under 0.15 ms, for the same work on the same texture. Read a per-pass cost
here as "this pass or the boundary before it":

| Pass | Incumbent | Split |
| --- | ---: | ---: |
| `BrickPhase0.Draw` | 5.80--5.83 ms | - |
| `MicroPrepareTransform` | - | 1.62--1.68 ms |
| `ResidualDraw` | - | 6.20--6.24 ms |
| `PreparedMicroDraw` | - | 0.006 ms |
| `PreparedTieReplayDraw` | - | 0.004--0.006 ms |
| Sum of pass p50s, excluding HZB | **5.82--5.84 ms** | **7.96--8.00 ms** |

Summing each frame before calculating percentiles, including the common Phase0
HZB, gives **6.16--6.20 ms p50 / 6.61--7.03 ms p95** for the incumbent and
**8.29--8.37 ms p50 / 8.66--9.13 ms p95** for R2. Schema 14 emits that exact
grouping as `brick_phase0_total_ms`; it selects the incumbent Draw or the full
R2 replacement chain from the passes actually present in each frame.

Camera-facing eligibility makes the direct and replay draws sparse at the
median terrain frame, but MicroPrepare still scans every visible brick and
`ResidualDraw` retains almost all incumbent work. The replay is load-bearing for
exact boundary ownership, so the current replacement chain is structurally
unable to win this workload. See stage R2 in
`docs/sparse-voxel-raster-ddgi-implementation-plan.md`. A future positive result
must be confirmed with the diagnostics-off Metal capture; this timestamp run is
negative attribution only.

#### The three-render-pass explanation, tested and refuted

The split runs three render passes over the same four-MRT brick target where
the incumbent runs one, which on a tile GPU costs two extra full-screen
attachment load/store cycles - about 69 MB per frame at 1200x720 - whatever the
extra draws do. `--brick-micro-merged-draw` records all three indirect draws
inside a *single* render pass instead. Output is byte-identical and the graph
provably collapses: it registers `MergedMicroDraw` and none of `ResidualDraw`,
`PreparedMicroDraw` or `PreparedTieReplayDraw`, both of which the R2 gate
asserts.

Three interleaved repeats per arm, `--benchmark-lighting` aggregates, GPU
timestamps off, same camera walk and thresholds as above:

| Arm | p50 (run spread) | p95 (run spread) |
| --- | ---: | ---: |
| default (incumbent) | 24.043 ms (0.017) | 25.642 ms (0.270) |
| `--brick-micro-raster` | 25.865 ms (0.038) | 27.687 ms (0.310) |
| ... `--brick-micro-merged-draw` | 25.940 ms (0.184) | 27.538 ms (0.058) |

Merged against split is +0.074 ms p50 and -0.149 ms p95 - inside the spreads and
opposite in sign, so neutral. Removing the two render passes is not measurable
here, and R2's gap is not attachment traffic. Benchmark JSON records the arm as
`brick_micro_raster_mode`, so a capture is identifiable without its file name.

`--disable-brick-micro-raster` names the default incumbent full-brick raster
explicitly and removes all MicroPrepare, residual, and compact-parent buffers
and passes from the graph. `--brick-micro-merged-draw` records the split's three
draws into one render pass rather than three; it requires `--brick-micro-raster`
and is measured neutral, retained as a structural option rather than a win. `--brick-micro-record-capacity N` overrides the
automatically sized compact-record capacity with a value in `1..262144`; it is
primarily a forced-overflow qualification control. The capacity override and an
explicit `--brick-micro-prepare-diagnostic` both require `--brick-micro-raster`,
because the incumbent graph builds no MicroPrepare passes to report on. The R0
umbrella `--brick-raster-diagnostics` is not in that set: it instruments the
incumbent draw as well, and only its MicroPrepare counters need the split.

`--test-micro-raster` selects a deterministic static exterior-shell fixture with
three source materials: a hash-column heightfield descriptor, a terrain
staircase-surface descriptor on the shell's top slab, and an unbound legacy id.
It exercises compact records, residual occupancy and R3's compact column cache
without an external terrain package.

The staircase material is confined to the top slab on purpose. Its carve face is
always +Y, so only a camera above a parent extracts it - which is why the R2
gate observes the column cache through `--test-camera-dolly` and asserts it
stays at zero under the default framing. Spread over the whole shell it would
also win every coarsened parent, because a surface context outranks an interior
one during brick coarsening, and extraction would collapse to nothing.

A focused headless run is:

```sh
./SparseVoxelRendererSample \
  --gbuffer-resolver bricks --no-terrain --test-micro-raster \
  --brick-micro-raster \
  --frames 3 --test-frame-signatures --resolution 320x180 \
  --disable-lighting --no-taa --disable-metalfx-upscaler \
  --disable-frame-generation --disable-vsync
```

`--brick-micro-prepare-diagnostic` adds the opt-in sizing scan and readbacks to
the enabled R1/R2 path. It scans immutable source occupancy once and reports
context counts, projected microcell bins, a farthest-AABB guaranteed-exact set,
compact heightfield record/quad estimates, residual parents, and overflow. The
diagnostic scan and readbacks perturb timing and are not renderer benchmarks;
without the flag, the shipping Reset/Transform and all three raster draws remain,
but the sizing scan, diagnostic buffers, and readbacks do not.

Use `--brick-raster-diagnostics` for the complete R0 raster counter set. It
selects the separately compiled instrumented fragment shader and enables the
occupied-box area, per-pixel overdraw, brick traversal, micro-context, and
MicroPrepare counters together. `--brick-measure-area` and
`--brick-micro-prepare-diagnostic` remain available for focused captures;
`--debug-mode 19` and `--debug-mode 20` show brick overdraw and micro surfaces.
All of these modes perturb the workload and must not supply performance
numbers. From `build/tests`, a canonical counter-only camera-walk capture is:

```sh
./SparseVoxelRendererSample \
  --terrain-package "$PWD/polygon-terrain-default/planet.planetpkg" \
  --terrain-lod-levels 5 \
  --lod-threshold 0.75 --tree64-lod-threshold 1.5 \
  --resolution 1200x720 --render-scale 1.0 \
  --no-taa --disable-metalfx-upscaler \
  --disable-frame-generation --disable-vsync \
  --test-camera-walk --frames 240 \
  --brick-raster-diagnostics --skip-output-signature
```

The output-signature skip keeps this counter-only run from adding the separate
full-frame correctness readback. Omit it when the exact-output gate is the
purpose of the run. Never compare either diagnostic run's frame time with the
diagnostics-off renderer.

`SparseVoxelRasterR0ExactEquivalence` is the reusable non-perturbation gate. It
runs the shipping and diagnostic shaders on the same deterministic fixture and
compares every frame's depth, resolved-surface and temporal identity, packed
material/bottom layer, micro payload, canonical sample address, geometry and
shading normals, aggregate G-buffer, exact record, and final presented-colour
hashes:

```sh
ctest -R '^SparseVoxelRasterR0ExactEquivalence$' --output-on-failure
```

`SparseVoxelMicroRasterR1ExactEquivalence` compares the enabled split against
`--disable-brick-micro-raster` for every exact field. It requires nonzero
compact and residual products, checks parent/byte conservation, forces
capacity-1 whole-brick overflow, requires every production draw including the
sparse tie replay, sweeps eight grazing camera angles, and covers the
camera-inside fallback:

```sh
ctest -R '^SparseVoxelMicroRasterR1ExactEquivalence$' --output-on-failure
```

`SparseVoxelMicroRasterR2ShaderContract` inspects all four compiled direct
prepared variants and rejects any fragment interface that retains occupancy,
material-index, palette-descriptor, or micro-surface buffers.
`SparseVoxelMicroRasterR2DirectEquivalence` compares all 13 exact fields for
static, grazing-yaw, camera-interior, capacity-overflow, and camera-dolly cases.
It also requires nonzero prepared, residual, direct, continuation, and fallback
work and checks the 16-byte record and render-graph contracts:

```sh
ctest -R '^SparseVoxelMicroRasterR2(ShaderContract|DirectEquivalence)$' \
  --output-on-failure
```

Visible detail is controlled by the projected LOD threshold. The current
cut sizes a bounded page arena from that projection and keeps headroom for a
complete replacement cut and deferred retirement. `--terrain-render-distance-km`
sets the actual voxel cut radius. LOD edits build the complete replacement on
the worker pool while the preceding cut remains visible, then publish raster
and Tree64 ownership together. A requested curve that cannot fit the fixed
page arena is clamped to the nearest feasible error and reported in the panel.
The 2 GiB terrain and Tree64 budgets remain hard guards.

Terrain lighting follows the same distance cut. Probe density drops to one
probe per 2x2 tiles at 35% of the terrain view radius and one per 4x4 tiles at
60%. Each Tree64 terrain page uses the same authored voxel lattice and instance
bounds as its raster page. The default 25 km view begins fading traced shadow
contrast at 3.75 km and reaches analytical sky/directional-sun lighting at
10 km. Finite/local lights fade out over that interval. Probes beyond the
cutoff reserve no scene rays, so compaction and Tree64 traversal only process
nearer probes; resolve still projects the sky and sun into probe SH. This
prevents coarse outer-page cells from becoming long dark bands while retaining
the same broad colour and cosine response. The physical solar
angular radius also rotates a sun-disk visibility sample through the existing
probe history, producing a temporally stable penumbra without adding another
ray per probe. Override the distance thresholds with `--probe-density-half`,
`--probe-density-quarter`, `--probe-local-light-cull-start`, and
`--probe-analytical-distance` (`--probe-sky-only-distance` is retained as an
alias). Distances are world metres; zero selects the automatic fraction of the
probe trace extent (70% for the cutoff), while omitting the option keeps the
terrain preset.
`--probe-max-density-stride 1` restores full probe density everywhere.

For example, this starts the shadow transition at 2 km and stops probe scene
traces at 5 km:

```sh
./SparseVoxelRendererSample \
  --probe-local-light-cull-start 2000 \
  --probe-analytical-distance 5000
```

Terrain also damps directional probe bands where adjacent G-buffer normals
disagree and uses the orientation-independent probe ambient term whenever a
pixel has to fall back to an incompatible screen-tile probe. This prevents a
perpendicular tiny voxel face from making the fallback reconstruct black for a
whole tile. Use `--disable-probe-normal-damping` for comparison;
`--probe-normal-damping` enables the same treatment in non-terrain scenes.

Outdoor terrain enables physical height fog inside a screen-scaled froxel
volume. Desktop quality uses one froxel per 12 x 12 internal pixels and 48
logarithmic depth slices; low quality uses 16 x 16 tiles and 32 slices. The
default ground extinction is
0.5/km with a 180 m exponential height falloff. Because it is a participating
medium rather than a depth-colour blend, sunlight is phase-scattered and the
optional terrain atmosphere map produces volumetric shadow shafts. The
terrain approximation is enabled by default and falls back to analytic planet
shadowing when local coverage is unavailable. Use `--disable-volumetric-fog`,
`--volumetric-fog-density PER_METRE`, `--volumetric-fog-height METRES`, or the
Atmosphere/Fog controls to tune it. The old distance-fog controls remain a
disabled compatibility fallback. Terrain-shadow depth comparisons use a
continuous four-texel tent filter in the low-resolution atmosphere LUTs; the
wider kernel hides shadow-map texel rectangles without paying a full-resolution
volumetric-shadow filter. A complete screen column now integrates ordered local
segments front-to-back, so each depth cell is evaluated once instead of
re-marching the camera-ray prefix for every endpoint. Stratified samples rotate
per cell and frame. A dedicated 3D temporal resolve reprojects each froxel by
the current and previous camera transforms, clips history to the six-neighbor
current-volume bounds, and rejects history from transmittance disagreement or
large screen motion. This produces smooth sun shafts without fixed slice bands,
camera trails, or the cost of a full-resolution volumetric march.

Following Hillaire's EGSR 2020 split, ground views expand the low-frequency,
nonlinearly horizon-parameterized Sky-View LUT with filtered sampling, while
planet overview views ray-march the atmosphere shell directly on screen. The
orbit march warps samples toward the ray's closest approach to the planet,
where a grazing ray's density is concentrated; this keeps the thin limb visible
without increasing the render target or introducing a high-dimensional LUT.
The default orbit budget is 24 samples and remains adjustable with
`--orbit-atmosphere-samples 4..32`.

Sub-third-pixel blades are rejected during instance generation.

Each page's height and material LZ4 blocks are decoded independently, and a
missing fine tile automatically samples its closest stored ancestor. Pages are
translated into a shared tangent frame and uploaded as one multi-root brick
hierarchy. Crossing a 31.25 m page boundary starts an asynchronous cache update
while the previous GPU window remains visible. A normal one-page move reuses
288 of the default 297 pages, builds the entering nine, and evicts the trailing
nine. The coarser rings change only when the camera crosses their 62.5 m,
125 m, and 250 m boundaries. Logical offsets are resolved through cube
directions, so the package address continues onto adjacent cube faces instead
of stopping at an edge.

Each logical page keeps a stable physical slot for its LOD. On an ordinary
move, only the slots whose contents changed are rewritten in the existing GPU
buffers; the renderer does not re-upload the full hierarchy or rebuild the
render graph. Slot capacity growth still falls back to a safe graph rebuild.

The exported default planet is intentionally compact, so its stored global
rasters are much coarser than a 20 cm runtime page. The voxel builder therefore
adds deterministic planet-space relief below the stored raster resolution. The
exported mountain and tectonic maps control its strength, and the package
content hash seeds it, so independently generated pages meet at identical cube
face boundaries and regenerate consistently. `--terrain-detail-strength 0`
disables this refinement; the default is `1`, and values up to `3` exaggerate
relief for inspection.

Surface biome IDs are expanded during voxelization into a compact procedural
terrain palette. Grass, forest, and scrub receive deterministic cool-to-warm
green variants, while slope, mountain weight, and broad planet-space noise
expose quantized dirt and soil tones. Rock, sand, snow, and the remaining
biomes receive restrained tonal variation as well. Because palette selection
uses the package hash and sphere direction rather than page-local coordinates,
adjacent pages reproduce the same colours at their boundaries without another
texture asset or streaming lookup.

Evicted generated pages enter a transient VSVO-like backing store instead of
being discarded immediately. The store keeps LZ4-compressed page payloads in an
LRU RAM tier and spills older pages to a unique runtime-owned temporary
procedural regeneration. The directory is deleted when the terrain session
ends and is never added to `planet.planetpkg` or treated as an authored save.
The defaults are 4 GiB of compressed RAM pages and 4 GiB of disposable disk
pages; override them with `--terrain-page-memory-mb N` and
`--terrain-page-disk-mb N`. These are upper bounds, not eager allocations. A
disk budget of zero disables spilling, in which case old pages are simply
regenerated if revisited.

Terrain brick data has a separate 2 GiB resident GPU budget, configurable with
`--terrain-gpu-memory-mb N`. The renderer allocates the current packed brick
hierarchy plus a 25% whole-arena guard. Tree64 capacity is reserved separately
for every stable physical clipmap page, using a complete per-page node slot;
it therefore follows the distance-derived ring count instead of assuming a
fixed 64-page window. An asynchronously built hierarchy is rejected before it
replaces the visible terrain window if it exceeds either contract. Normal
relief variation therefore uploads in place without rebuilding the render
graph when an entering page contains slightly more bricks.

The renderer submits each terrain update asynchronously. In the default
generated-world path the CPU task performs only streaming metadata work:
request coalescing, dependency/fallback resolution, page-slot reservation, and
tiny install-patch publication. Height/material sampling, voxel/page payload
generation, raster hierarchy construction, and Tree64/trace payload generation
belong to the GPU page-build lane. Use `--terrain-generation=cpu` only for the
CPU oracle/debug path.
`--terrain-voxel-workers 0` selects up to eight workers automatically while
reserving two hardware threads for rendering, input, and the asynchronous
coordinator; use one for serial profiling or an explicit value up to 64. Missing
GPU-requested chunk rewrites are held in a priority queue ordered by projected
screen-space importance and camera distance. `--terrain-pages-per-update N`
bounds how many independent slots one transaction generates or refines
(default 64). Cache restoration does not occupy
voxel workers. A completed update follows virtual geometry publication
semantics: the previous slot hierarchy remains renderable while its replacement
is built, then brick payloads, node records, and matching per-instance Tree64 data are
published together. The
shader binary-searches a bounded section table, so hundreds of disjoint page
ranges do not turn into a linear scan per uploaded word. Residency is committed
only after the frame containing that transaction completes. There is therefore
no invalidated or partially updated page between frames, and retained pages keep
their lighting history through stable world-space surface identities.

Brick residency uses fixed physical pages for nodes, child/root topology,
occupancy, material bytes, and palettes. Each page record carries separate
payload and translated-node revisions. Entering terrain installs a complete
brick page; retained coarse terrain whose cascade ownership changes rewrites
only its node page, while every unaffected page remains untouched. Material
pages are four-byte aligned so no upload range aliases a neighbouring page.

`--planet-chunk-voxels N` remains a manual compatibility override. Supplying it
keeps the package's nominal 1 km page extent and disables automatic 20 cm page
selection; for example, 64 cells produces 15.625 m leaves. Planet mode builds
one material-free Tree64 BLAS per resident terrain page, and its lattice is the
page's own voxel grid: one Tree64 cell per raster voxel, taken from the same
512 occupancy bits per brick the brick DDA consumes. The earlier builder always
sampled a page into 64 cells, which agreed with the raster only for a 64-voxel
page; automatic selection produces 160-voxel pages, so every shadow cell
covered 2.5 raster voxels and did not align to a voxel boundary. That is what
made slopes shadow as blocky terraces wherever a ray left the screen trace.
A 160-voxel page becomes a 256-cell BLAS - the cell cube extends past the page,
and the instance bounds keep traversal out of the empty margin.

`--terrain-tree64-side 0|4|16|64|256` requests a coarser lattice in cells per
page edge, OR-reducing aligned fine-cell groups into a conservative BLAS
without changing render-brick resolution. Zero - one cell per page voxel -
is the default; every occupied voxel remains contained by an occupied cell at
any reduction, so the coarser settings stay safe for visibility and are useful
only for profiling and distant-only consumers. Each page also has a fixed node
slot (8192 nodes); the worst page of the default planet package measures 3579,
and a page that would exceed the slot reduces one level instead of failing the
publication. `Terrain Tree64 build` logs `worstCellScale`, `worstPageNodes` and
`clipped` so a page that stopped matching the raster is visible in a run.
Every BLAS is an instance whose stable model and instance IDs are its physical
terrain page slot.
Each model owns a fixed node slot with bounded growth headroom, so installing
one page rewrites that BLAS and its affected directory ranges without repacking
or uploading later models. The rewrites are generated in the same asynchronous
terrain job and published with the brick page update, so directional shadows
and indirect visibility follow entering and evicted terrain pages without
duplicating material data.

The bounded streaming smoke test moves the camera across one runtime page and
then returns, verifying that the evicted row is restored rather than generated
again. Set the RAM tier to zero to force this round trip through disk:

```sh
./SparseVoxelRendererSample \
  --frames 4 \
  --terrain-package polygon-terrain-small/planet.planetpkg \
  --terrain-chunk-radius 1 \
  --terrain-page-memory-mb 0 \
  --test-terrain-streaming \
  --disable-lighting
```

The long-range stability test requires a real exported planet, validates a
multi-level footprint of at least 2 km, aims at a distant ridge, and applies
a slow sub-page camera drift. Frame signatures then cover the small distant
voxels, per-instance LOD rewrites, jitter, and traced lighting together:

```sh
./SparseVoxelRendererSample \
  --frames 96 \
  --terrain-package polygon-terrain-small/planet.planetpkg \
  --test-terrain-long-range

# Also exercise the non-temporal path where jitter remains visible directly.
./SparseVoxelRendererSample \
  --frames 96 \
  --terrain-package polygon-terrain-small/planet.planetpkg \
  --test-terrain-long-range \
  --no-taa
```

Controls:

- `W/A/S/D`: move the camera
- `Q/E`: move down/up
- Right- or middle-mouse drag: look
- `F8`: copy a replayable camera command to the clipboard
- `Shift+F8`: copy the command and save it to `camera-setup.txt`
- `Shift`: move four times faster
- ImGui: **Copy camera setup** and **Copy + save** provide the same capture commands; `--camera-setup-output file` changes the saved path. The command records the absolute world-data address, map center, terrain render distance, output resolution, render scale, position, quaternion, near/far planes, and debug view. Replay uses `--camera-position X,Y,Z` and `--camera-rotation X,Y,Z,W`.
- ImGui: open a `.vsvo`, selective/load-all residency, OBJ authoring and explicit save, camera speed, reverse-Z near plane, LOD threshold, BLAS frustum diagnostics, two-phase occlusion, TAA/TSR settings, GPU pass timeline, instance position, G-buffer channel, and linear-depth scale

The default LOD threshold is 8 pixels. Traversal keeps the current node when
its children would project below that size; otherwise it descends. Internal
nodes use appearance values averaged recursively from their resident children.

For a bounded integration smoke run, use:

```sh
./SparseVoxelRendererSample --frames 3
./SparseVoxelRendererSample --frames 4 --enable-occlusion --test-occlusion-disocclusion
```

The bounded mode moves the instance after the first frame to exercise temporal
velocity and fails if the final sparse-raster coverage buffer has no pixels.
TAA and TSR are enabled by default. The voxel raster runs at 0.75 display
resolution while TSR reconstructs display resolution; disabling TSR keeps TAA
at native resolution. The temporal history uses depth, normal, and motion-vector
validation and maintains one history slot per frame in flight.
The renderer uses previous-HZB TLAS pre-culling, temporal depth, a provisional
current HZB, and the complete current render when the TLAS has useful
multi-instance granularity. A single-instance scene automatically bypasses the
TLAS-only two-phase chain because it cannot reject a subset and otherwise
duplicates the full LOD/bin/sort/resolve path. Use
`--disable-occlusion` for the one-phase frustum-only baseline; `--enable-occlusion`
is retained for scripts written before the default changed. Add
`--enable-blas-occlusion` to force the experimental current-HZB BLAS rejection;
for a single instance the BLAS expansion owns HZB tests and the scene-sized
TLAS leaf is not tested redundantly.
The UI reports current and temporal visible instances/voxels, candidate work,
and any GPU capacity overflow. `--disable-blas-frustum` is a diagnostic baseline
for checking node-frustum output equivalence.

Per-pixel ReSTIR is the default direct-illumination path. Its canonical receiver
domain stays at native raster resolution, while the default 2x2 window traces
one blue-noise-selected mixed-light lane per window: 0.25 rays per pixel.
Direct shadows therefore keep
pixel-resolution contact detail without copying a resolved visibility result
between receivers.
`--di-mode probe` selects the screen-probe
path instead, where one probe per tile traces a whole hemisphere and arrives
already low-variance and the scheduler drops most of that cost again once tiles
settle - cheaper and smoother, but tile-resolution. The two paths are exclusive
and switching rebuilds the graph.

Screen probes default to a 12-output-pixel footprint. Each tile keeps one
primary probe plus up to two cached probes for secondary face clusters at
90-degree normal changes; any unused secondary lane remains available to the
depth/plane-error refinement. `--probe-tile 8` restores the denser high-quality
mode. A same-surface probe woken only by camera motion validates a
rotating 16-direction slice of its reprojected map, while a genuinely cold probe
still traces all 64 directions. This keeps disocclusions responsive without
making broad corridor walls pay the cold-start ray burst every moving frame.
The uniform cold-start budget is 64 / 12² = 0.44 rays per output pixel. A tile
showing two cube faces costs at most 0.89 while both probes are cold; the rare
three-face case has a 1.33 worst-case cold ceiling. These are capacity bounds,
not steady-state rates: planar tiles allocate no extra probe and cached face
probes move to the reduced scheduled updates after convergence.

The scene-ray lighting panel exposes point, sphere, planar, disk-sampled
directional sun, and analytic sky lights. Sky controls include world up, horizon, zenith,
and ground colors, intensity, and horizon curve. The production per-pixel path
uses two estimator strata: one cosine-sampled sky-visibility lane for ambient
occlusion, plus a ReSTIR-selected non-sky lane for analytical emitters. Extra
rays per pixel add non-sky samples within the latter stratum. The ray resolve
produces a
material-independent diffuse-light multiplier: RGB contains visible light
color/attenuation/cosine terms and alpha identifies a covered receiver.
Albedo, metallic, roughness, emissive, and uncovered-background composition are
deliberately left for a later final material pass. This light buffer is what
SVGF denoises.

SVGF is spatial-only and uses five edge-aware a-trous passes by default. The
current-frame ReSTIR resolve publishes only the traced receiver; it never
spreads binary visibility to neighbouring pixels before accumulation.
Larger ReSTIR sampling windows automatically extend the chain to three, four,
or five passes for 4x4, 8x8, and 16x16. Its bilateral gate is normal/depth-aware
and treats streamed terrain instance/voxel identities as confidence penalties,
so hierarchy boundaries do not become lighting edges.
High local variance retains a bounded filter response even after temporal
sample confidence fills, and viewport kernels discard off-screen taps instead
of repeating the border texel. Use `--svgf-atrous-iterations 0..5` (or **SVGF
a-trous iterations** in ImGui) to control the baseline cost. Zero disables the
spatial chain; a nonzero setting is raised as needed to span the selected
window. Changing the value rebuilds the graph, so unused passes and their
intermediate textures are not allocated or dispatched.

The full-screen emitter visualization is an editor overlay and is disabled by
default. Enable it with the `Draw light emitters` checkbox or
`--light-emitters`; disabling it removes the pass from the graph rather than
dispatching an early-out thread for every pixel.

Scene shadow-ray buffers and histories stay at full internal raster resolution.
Legacy light-field quality and `--lighting-scale` controls no longer shrink the
per-pixel receiver domain. Instead, each 2x2, 4x4, 8x8, or 16x16 screen-space
window traces one blue-noise-selected ReSTIR receiver per frame. A dedicated
lighting accumulator averages those current-frame estimates before the
stateless geometry-aware SVGF filter. It is independent of SVGF and display
TAA: disabling either does not disable lighting accumulation. History is used
while scene, Tree64 publication, and light signatures remain unchanged. Camera
motion reprojects it through the G-buffer velocity field, then validates the
2x2 history footprint by instance, voxel, geometric normal, and coverage;
disoccluded receivers reject only their own history. The default bounded
window is 32 frames. The default 2x2 sampling window traces
0.25 rays per raster pixel. The optional full-rate viewport guard band remains available
for diagnostics but defaults to zero so window budgets are exact. Demand-driven
scheduling and visible-voxel receiver sharing are optional diagnostics rather
than part of this baseline. The adaptive tile budget remains disabled unless a
nonzero value is selected explicitly in ImGui or with `--ray-tile-budget`;
loading world-map terrain does not change that setting. Select the window in
ImGui, or use
`--ray-resolution-divisor 1|2|4|8|16` for bounded profiling runs (`1` is the
full-rate reference).
Resolved visibility is receiver-local: ReSTIR can reuse proposals before the
shadow ray because it reconnects and retraces them at the current receiver.
Unscheduled pixels are reconstructed from current-frame compatible receivers;
an unresolved covered pixel may retain a surface-validated accumulated estimate.
Incomplete Tree64 queries are held as unknown rather
than accumulated as black occlusion, and sky directions use a per-pixel
low-discrepancy sequence to avoid persistent direction clusters.
Demand scheduling now treats convergence as statistical evidence rather than
four unchanged frames. By default a receiver needs at least 32 measured
samples, eight stable direct observations, and relative standard error at or
below 0.20 before it sleeps. Settled 16x16 screen tiles retain an expected one
rotating validation ray per frame. Point or directional visibility that flips
relative to the tile's previous observation resets the sentinel pixel and
wakes only that tile for four frames; this discovers moving occluders without
returning to full-screen tracing. Sphere, planar, and sky visibility instead
use one packed 16-observation distribution per tile/light. A tile wakes only
after multiple windows differ materially from its reference distribution, so
ordinary soft-shadow and hemisphere sampling noise does not become perpetual
work. A sparse subset of active rays prefills these distributions before the
tile sleeps. Motion is separated into camera and object components using the
current/previous camera matrices: camera-only motion reseeds a screen probe,
failed reprojection/disocclusion clears it and wakes the tile, and genuinely
moving geometry remains a change signal. Tune this with `--ray-min-converged-samples`,
`--ray-convergence-error`, `--ray-stable-observations`,
`--ray-stability-tile`, `--ray-refresh-interval`, and
`--ray-region-wake-frames`. The soft distribution controls are
`--ray-soft-change-threshold`, `--ray-soft-change-streak`, and
`--ray-soft-prefill-stride`. Setting the refresh interval to zero disables the
settled-region heartbeat, while disocclusion invalidation remains active.
Ray scheduling itself is a per-tile budget rather than a per-pixel decision.
One workgroup owns each 16x16 tile: every lane classifies a few receivers, the
workgroup reduces that into a single tile record, and the same workgroup elects
receivers up to the tile's quota. Ranking is a 64-bucket workgroup histogram
scanned from the highest priority down, so it costs O(pixels + 64) with no
global sort, no prefix scan, and no second full-screen pass. Priority order is
disocclusion, object motion, detected lighting or shadow change, measured
relative error, and plain lack of samples, with a small reserve kept for the
settled-tile heartbeat so a sleeping region never stops validating itself.
Quotas are redistributed against the previous frame's mean demand *per covered
receiver*, read from a frame-parity-double-buffered statistics header, which is
why redistribution needs no extra barrier. Normalising per receiver rather than
per tile matters: an absolute per-tile sum lets every fully covered tile
saturate the boost and the budget stops binding.
`--ray-tile-budget` is the share of a tile's covered receivers that a tile of
average demand may trace, expressed against the sparse-schedule rate. Zero, the
default, keeps the legacy schedule and only measures - the tile records and the
statistics bank are still produced, so the budget a scene would use is visible
before it is enforced. `--ray-tile-budget-boost` caps how far above its equal
share an urgent tile may be lifted, and `--ray-tile-budget-heartbeat` sets the
reserved validation slots per covered tile. On the shell scene at 1280x720 the
measured GPU totals are 15.04 ms at the default, 14.21 ms at `1.0`, and
13.85 ms at `0.5`, with SceneRayCreate falling from 3.37 ms to 2.32 ms; the
scheduler pass itself is 0.002 ms. Sizing the pool from covered receivers, not
from eligible ones, is what makes the total a budget: eligibility grows with
demand, so a fraction of it would let a noisy frame spend its way past the
unbudgeted baseline.
`--benchmark-sequence moving-occluder` measures invalidation latency directly.
It holds the caster still until `--occluder-settle-frames`, steps it once, and
records the per-frame tile budget from there. The report's
`occluder_invalidation` block carries frames-to-visibility-change,
frames-to-tile-wake, the sentinel change count that proves stale shadow history
was rejected, and the share of the frame budget the change claimed. This is the
one sequence that pays for a per-frame readback, so its GPU timings are not
comparable with the throughput sequences.
The optional 16x16 sky-direction guide follows the same policy: camera motion or
disocclusion marks its tile for reset, and the next resolve rebuilds the CDF
from a uniform prior plus current observations rather than reusing stale
screen-space directions.
Visible-voxel receiver sharing is the default lighting trace path: one
scheduled receiver is shared inside each compatible 2x2 pixel block. Identity,
face orientation, depth, and normal checks prevent sharing across voxel or
brick boundaries. This is the fast integrated-GPU mode and remains local, so it
adds no full-screen map pass. `--visible-voxel-rays` is retained as an explicit
no-op enable, and `--disable-visible-voxel-rays` restores the per-pixel receiver
path for A/B captures.
`--visible-voxel-five-rays` enables the experimental large-face variant. It
builds a generation-tagged screen-space face map and selects center, left,
right, top, and bottom representatives for reconstruction. On the current
Sponza Metal captures its map construction costs roughly 2.3-2.9 ms, more than
the saved settled-scene trace work, so this variant is deliberately opt-in
rather than the default.
Resolved sky rays also feed a 16x16-pixel screen-tile direction guide. Each
tile stores a 4x4 histogram in the squared-radius/azimuth parameters of cosine
hemisphere sampling, rather than discrete octahedral directions. The next
frame samples a cosine/guide mixture and records its exact solid-angle PDF, so
guiding remains unbiased while concentrating work on historically visible,
bright sky regions. It is disabled by default so the minimal path has no
second adaptive temporal state; use `--sky-guiding-strength N` to enable it.
Directional light is not sampled through that stochastic sky distribution.
Screen probes reserve an explicit next-event shadow sample for the sun and
accumulate it in emitter SH history. This prevents low-ray probe tiers from
missing a tiny sun cone for several updates and then injecting one
solid-angle-weighted bright sample—the periodic bright/dark terrain blotches
that otherwise appear with sky and directional lighting enabled together.
The full-resolution light field is the single lighting accumulator. The normal
render path evaluates current ray visibility directly and does not run a
second, 96-byte-per-pixel hit-history pass; that detailed hit history is
created only by the frame-signature diagnostic that reads its reuse verdicts.
Multi-light selection maintains a 4096-entry world-space reachability cache.
Entries are keyed by a four-trace-cell region and dominant receiver face, then
learn a packed visible/observed ratio per light from completed screen/Tree64
shadow rays. Candidate PDFs and temporal/spatial reservoir reuse are weighted
by that ratio. A 1/64 exploration floor keeps occluded lights eligible after
visibility changes without letting them consume most samples in a sky-only
region. Lights entirely behind the receiver hemisphere are rejected before
reservoir sampling because they cannot produce a visibility observation for
the cache. Sky directions are never reused temporally or spatially by ReSTIR;
the full-resolution light-field reconstruction exclusively owns their temporal
accumulation. This avoids retaining one binary sky occlusion sample in both
temporal estimators.
ReSTIR candidate count is independent from traced rays per pixel. The default
considers four Light-BVH proposals and selects one visibility ray; multiple
traced rays use independent reservoir lanes that partition the larger
candidate pool. Initial RIS is streamed during proposal generation, retaining
only the selected ray for each lane instead of an 80-byte record for every
proposal. Tune these with **ReSTIR candidates** and **Rays per pixel** in ImGui,
or `--restir-candidates-per-pixel 1..16` and `--rays-per-pixel 1..8`. The
sample keeps the production candidate count at least one above the traced-ray
count.
Screen-space traces are enabled by default for near-field visibility, with the
authoritative world-space Tree64 tracer handling misses and rays that leave the
screen. Use `--disable-screen-traces` for the world-only diagnostic baseline.
Adjacent voxel cells from the same instance are treated as one receiver sheet
when their geometric normals and planes agree within one world-pixel width;
this prevents coplanar cell boundaries and depth quantization from becoming
false occluders while retaining real foreground blockers. Spatial ReSTIR is likewise opt-in with
`--enable-spatial-restir`; the baseline uses only temporal normal/depth reuse.

Where the screen trace and the world trace both answer, they can disagree, and
the merge gives the screen hit priority unconditionally - so the overlap is
exactly the region whose lighting changes when an occluder scrolls off screen.
Two controls bound it. **Screen trace span (cells)**
(`--screen-trace-cell-span`, default 8) is how far in front of a receiver the
screen trace marches, in receiver cells; it used to be 32 because the world
trace could not be trusted at that scale, and is now sized to cover what the
world trace genuinely cannot see - its own normal-direction origin bias and the
microgeometry under it. **Screen slab (cells)**
(`--screen-trace-max-thickness-cells`, default 4) bounds how deep behind a
depth-buffer surface a ray still counts as occluded. A depth buffer has no back
face, so the leaf test assumes a finite slab; deriving that slab from the
traversed interval alone (**Screen slab steps**, `--screen-trace-thickness`,
now 1 rather than 2) makes it metres deep for a grazing ray and turns every
silhouette into a false near-field shadow that the world trace does not agree
with. Four cells is the depth terrain geometry actually has, its surface shell
being four voxels. The traversal also starts at the mip whose cell already
contains the whole screen segment rather than at the coarsest one: for a
near-field ray the levels above that cover most of the screen and can reject
nothing.
Enable **GPU pass timeline** (or use `--gpu-timings` for a bounded run) to
collect the same timestamp-resolve timeline style used by the virtual-geometry
renderer. Profiling is opt-in: timer resolve and CPU readback are disabled when
the timeline window is closed. The timeline reports both the sum of timed pass
bodies and the first-to-last GPU timestamp span. The span is the appropriate
frame-occupancy number: unlike the pass sum, it includes barriers and gaps
between queue submissions. A companion CPU timeline records GPU waits and
syncs, timer/visibility/page-request readbacks, SDL events, file I/O and
streaming uploads, UI/camera work, scene buffer uploads, RenderGraph submission,
present, and untracked frame time.
The CPU **Present** interval includes the swapchain wait. With FIFO presentation
it can therefore report tens of milliseconds while the scene GPU passes take
only a few milliseconds; that is frame pacing/vsync idle time, not another
30-ms render pass. Use `--disable-vsync` for throughput profiling. The renderer
prefers immediate presentation when the platform exposes it and logs the
selected present mode; it falls back to mailbox/FIFO when immediate mode is not
available.
On Apple/MoltenVK, RenderGraph compiles graphics, compute, and transfer work
onto the graphics-capable universal queue. This removes cross-family ownership
transfers and timeline waits that do not provide hardware overlap on the
unified GPU. Use `--separate-gpu-queues` only as an A/B diagnostic. Other
platforms retain their backend-selected queues.

TSR accumulation writes its color/depth/normal/identity/luma/lock histories
directly into the current frame slot. The remaining history-copy kernel copies
only source-resolution dilated motion; the former six full-resolution scratch
textures and copy traffic are gone.
The last command runs the deterministic two-instance test fixture: a previous
frame occluder moves aside and the current-HZB pass must recover the revealed
instance without a one-frame hole.

Tree64 is the default software shadow-ray backend. Instance scenes prebuild
model-space 4-, 16-, 64-, 256-, ... cell Tree64 BLAS LODs on a bounded set of
CPU workers. The ladder continues through the model's authored maximum octree
level (up to a 65536-cell side); odd binary levels use the next 4-way Tree64
level while preserving the original occupied-voxel boundaries. Coarse adaptive
or not-yet-streamed voxels use early terminal nodes, so a fine model side does
not expand them into a dense grid.

The immutable payloads are uploaded once; each instance then selects a payload
from camera distance and uploads only its current transform and directory
membership. `Tree64 LOD error pixels` is independent of the raster LOD
threshold, although both default to 8 pixels. Streamed residency changes are
copied into a CPU snapshot, coalesced for four stable frames, and rebuilt in the
background before a revision-tagged payload replacement.
Headless runs can override it with `--tree64-lod-threshold N`.

The per-frame GPU world voxelize/build/compact/publish chain is disabled for
these instance scenes. The older 64-cell, world-aligned camera-directory
builder remains available only for a Tree64 scene without runtime instances.

Static chunks and transformed model-local trees use the same stackless
region-skipping traversal. Each step descends only to the child containing the
current ray position, tests its 64-bit occupancy mask, and skips to the exit of
the largest known-empty region. An additional mask test skips empty 2-by-2-by-2
child groups. Compact 12-byte nodes remain in storage buffers; the node
allocation order has no useful XYZ mapping for a 3D texture.

The complete VoxelRT-style negative-octant/workgroup-ancestor specialization is
available for comparison with `--tree64-traversal mirrored-ancestor`. It is
correct but slower on the current shallow 4/16/64-cell trees, so
`--tree64-traversal region` remains the default.

The CPU remains the control plane: it selects BLAS LODs, updates transformed
instance memberships, and publishes revision-tagged model payloads. The GPU
only traverses the selected model-space tree for normal instance scenes.
The benchmark report exposes `tree64_planning_ms` and `tree64_upload_ms` so the
cost of this control plane remains visible.

Capture comparable Tree64 and packed-DDA reports with:

```sh
./SparseVoxelRendererSample \
  --shadow-backend tree64 \
  --benchmark-shadow tree64-static.json \
  --benchmark-sequence static \
  --benchmark-warmup 120 \
  --benchmark-frames 600 \
  --benchmark-resolution 1280x720

./SparseVoxelRendererSample \
  --shadow-backend packed-dda \
  --benchmark-shadow packed-dda-static.json \
  --benchmark-sequence static \
  --benchmark-warmup 120 \
  --benchmark-frames 600 \
  --benchmark-resolution 1280x720
```

`interior-static`, `roof-down`, `camera-walk`, `camera-yaw`, and `transform` are also
available benchmark sequences. `camera-walk` is the default. `interior-static` places the camera at the
model root center, which gives Sponza a representative central-hall workload
instead of an exterior framing shot. `roof-down` places the camera near the
roof and looks into the model, reproducing the high-detail one-pixel LOD and
tile-candidate-capacity stress case. `camera-walk` smoothly walks along the
initial view axis while yawing the camera. On terrain it stays inside the
resident center page so the result measures camera-dependent rendering instead
of asynchronous page turnover. Benchmark mode excludes warm-up frames and
omits the expensive full-frame G-buffer/output-signature readback. Use
`--benchmark-lighting lighting.json` to measure the normal scene-lighting
pipeline without diagnostic occupancy rays. Use `--benchmark-shadow` for the
backend comparison above; that mode additionally enables surface-to-light
occupancy rays and reports their final ray/result hashes.

The canonical R0 performance capture pins every baseline control and leaves
sparse-raster diagnostics, debug views, and built-in GPU timestamps disabled:

```sh
./SparseVoxelRendererSample \
  --terrain-package "$PWD/polygon-terrain-default/planet.planetpkg" \
  --terrain-lod-levels 5 \
  --lod-threshold 0.75 --tree64-lod-threshold 1.5 \
  --benchmark-lighting r0-native-camera-walk.json \
  --benchmark-sequence camera-walk --benchmark-warmup 120 \
  --benchmark-frames 600 --benchmark-resolution 1200x720 \
  --render-scale 1.0 --no-taa --disable-metalfx-upscaler \
  --disable-frame-generation --disable-vsync
```

Run the Release executable under Xcode Metal System Trace for authoritative
GPU occupancy and encoder intervals. Do not add `--gpu-timings`,
`--brick-raster-diagnostics`, `--brick-micro-prepare-diagnostic`,
`--brick-measure-area`, or a diagnostic `--debug-mode`. Benchmark startup
rejects sparse-raster diagnostic combinations instead of writing a
normal-looking contaminated report.

Benchmarks preserve the selected scene's raster LOD threshold. Use
`--lod-threshold N` for an explicit raster sweep; it does not also change the
independent Tree64 threshold in benchmark mode. Select that separately with
`--tree64-lod-threshold N`. The historical 64-pixel coarse cut remains limited
to bounded non-benchmark smoke runs.

### Sizing a pass: `--profile-duplicate-pass`

`--profile-duplicate-pass GROUP` registers a group of **idempotent** passes a
second time. The duplicate rewrites identical values, so the exact-output
signature is unchanged and the whole-frame delta is that group's true marginal
cost. This is the only trustworthy local sizing tool in this tree.

Groups: `brick-resolve` (a control with an independently known cost),
`svgf-copies`, `probe-copies`, `probe-interpolate`, `compose`, or `all`.

Two rules, both learned the hard way:

- **Neutrality-check first.** Run with `--test-frame-signatures` and confirm the
  hash matches the unduplicated run. The G-buffer fails this check - duplicating
  it changes output, so it is not idempotent and has no group here. A number
  from a non-idempotent duplicate is meaningless.
- **Never for draws.** Early-Z rejects the duplicate and underestimates.

Take min-of-N across runs and leave a cooldown between them: a three-repeat
sweep with no cooldown had every arm's third repeat inflated by thermal drift,
producing a 6 ms within-arm spread against the usual 0.06-0.27 ms.

Measured results are recorded in `docs/sparse-voxel-1080p-statistics.md`.

GPU pass timestamps are opt-in with `--gpu-timings`; otherwise their JSON fields
are `null`/empty. For MoltenVK on macOS, profile the Release executable with
Xcode Instruments' Metal System Trace and use the benchmark's CPU frame/wait
percentiles for pacing.

Both modes write every measured CPU/GPU sample plus aggregate percentiles,
device/build identifiers, packed and Tree64 memory, Tree64 CPU planning/upload
cost, the resolved raster and Tree64 LOD thresholds, the selected SVGF a-trous
iteration count, probe tile/adaptive/awake-ray settings, planet background
stride, terrain micro-material state, R2 micro-raster mode and record capacity,
micro record/residual/dispatch/draw-argument buffer bytes, the brick exact-micro
distance cutoff, and the four resolved probe distance thresholds in metres.
The report therefore records the effective cutoff even when the command line
selected an automatic value or inherited the terrain preset. Normal scene-ray
shading and presentation remain active.
The retained `output_hash` report field is therefore zero for performance
captures.
`--shadow-backend packed-dda` is the explicit stabilization fallback and
comparison baseline.

Open an existing scene directly, without generating or modifying it:

```sh
./SparseVoxelRendererSample --open-vsvo /path/to/scene.vsvo
./SparseVoxelRendererSample --depth-only --open-vsvo /path/to/scene.vsvo
```

The default is metadata-first selective residency. Add `--load-all-pages` for
the diagnostic fully resident cut. In the UI, **Open VSVO...** behaves the same
way: the current scene stays visible until all pinned roots validate, and a
failed replacement leaves it untouched.

## GPU terrain page generation

`--terrain-generation gpu-v2` builds terrain pages entirely on the GPU: the
column sampler, the fine raster and a virtual-geometry emit that writes decoded
clusters straight into the scene. Each page takes two transactions, a count
that sizes it per tile and an emit that fills the exact reservation the count
produced. The CPU keeps only the cut, the page set and the page table; it
meshes nothing.

Measured on the slope test (700 frames, 640x360, static camera): 566 pages
across LOD 10 down to LOD 3, each generated once, cut error 2.65 px against a
1.5 px budget with no oversized cluster, and no hole on any frame. The first
pages land around frame 8, so the opening frames are empty. Generation stops
when the scene's decoded arena fills, at roughly 430 resident pages.

Diagnostics: `SVR_TRACE_TERRAIN=1` prints one line per generated page with its
faces, clusters and the tile counters the finalize saw.
