# Sparse voxel planet navigation plan

## Outcome

Add a non-blocking navigation workflow that connects the local sparse-voxel
terrain renderer to a whole-planet overview:

```text
surface -> animated ascent -> planet orbit -> select destination
        -> background terrain generation -> animated descent -> surface
```

The user presses `M` or the equivalent UI button to enter planet view. In that
view the user can orbit and zoom around a complete globe, inspect mountain
ranges, forests, rivers, biomes, and oceans, and click a surface point to
teleport. Terrain generation and GPU publication continue in the background;
the render thread must not wait for them.

This is a navigation feature, not a request to extend local float coordinates
to planetary scale. Surface rendering remains in a camera-centred tangent frame,
while planet overview rendering uses normalized planet coordinates and a
double-precision geodetic camera state.

## User experience

### Controls

| Input | Surface mode | Planet mode |
| --- | --- | --- |
| `M` | Animate to planet overview | Return to the current surface location |
| Planet-view UI button | Same as `M` | Same as `M` |
| Left drag | Existing camera behavior | Orbit the globe |
| Mouse wheel | Existing behavior | Exponential orbit zoom |
| Left click | Existing behavior | Select and teleport to a planet point |
| `Esc` | Existing behavior | Cancel a pending teleport and remain in orbit |

A left-button interaction becomes a selection only when cursor movement stays
below a small drag threshold. The destination is marked immediately. Clicking a
new destination supersedes the previous pending request.

### Navigation states

```text
Surface
  | M
  v
Ascending ------------------------------+
  |                                     | cancel/return
  v                                     |
Orbit -- click --> LoadingTarget -------+
  ^                    |
  |                    | terrain resident
  |                    v
  +---------------- Descending
                         |
                         v
                      Surface
```

The state machine must be explicit. Input, rendering, automatic clipmap
streaming, effects, and request cancellation all depend on it; inferring mode
from camera altitude would make transitions fragile.

## Current constraints

The existing sparse voxel sample already has useful pieces:

- A local terrain clipmap and toroidal physical page slots.
- Asynchronous CPU terrain generation.
- Page-atomic, multi-frame GPU uploads.
- A local background planet cap and voxel/planet merge pass.
- Planet-package raster and graph channels.
- Stable cube-face direction and tile mapping.

It does not yet have a complete planet view:

- The background planet is an anchor-relative local cap, not a whole globe.
- The main camera is local `float` state and must not travel millions of metres.
- Automatic terrain streaming assumes that the main camera is in the local
  tangent frame.
- Terrain cache compatibility currently couples logical anchor identity to
  physical GPU slot-layout reuse.
- Background jobs cannot yet be promptly cancelled between terrain pages.
- Material-field publication still contains synchronous work that should join
  the staged GPU upload transaction.

## Coordinate model

### Geodetic navigation pose

Introduce `PlanetNavigationPose`, stored independently from the existing local
camera:

```cpp
struct PlanetNavigationPose
{
  procedural::SpherePoint direction; // double-precision unit vector
  double altitudeMetres;
  double orbitDistanceRadii;
  math::Quatd orientation;
};
```

The exact quaternion type can follow existing math-library conventions, but the
direction, altitude, and planet-radius calculations must use `double`.

The navigation controller also stores:

- Current navigation state.
- State-transition start and target poses.
- Saved local surface camera pose.
- Selected destination direction and tile.
- Active request generation ID.
- Animation start time and duration.
- Orbit drag and click-disambiguation state.

### Surface coordinates

Surface rendering continues in local tangent-space metres. Landing at a new
point changes the planet anchor and resets the camera near the local origin.
This keeps voxel, culling, depth, and material calculations in their current
precision range.

For a landing direction `up`, construct a stable tangent frame:

```text
up    = normalized planet direction
east  = normalized cross(reference axis, up)
north = cross(up, east)
```

Select a different reference axis near the poles. Preserve the orbit camera's
projected heading where possible so descent does not unexpectedly rotate the
view.

### Planet overview coordinates

Render the overview globe as a normalized unit sphere. Orbit distance is
expressed in planet radii and elevation displacement is normalized by the
source radius. The overview pass never receives a six-million-metre float
translation.

## Camera animation

Use one conceptual geodetic trajectory but derive two render cameras from it:

- Local tangent camera for voxels and the local background cap.
- Normalized overview camera for the globe.

Interpolate altitude logarithmically so both the surface departure and the
whole-planet reveal receive visible animation time. Interpolate orientation
with quaternion slerp and use an eased transition curve.

During ascent:

1. Keep rendering local voxels and the local planet cap.
2. Increase geodetic altitude.
3. Start rendering the whole globe.
4. Fade local geometry as its projected footprint becomes small.
5. Disable voxel rendering and voxel effects before local float precision is
   stressed.

Descent performs the inverse only after the destination terrain is fully
published. The final local camera height is the sampled destination surface
height plus the configured eye height.

Initial controls:

| Setting | Default |
| --- | ---: |
| Ascent duration | 2.0 s |
| Descent duration | 2.0 s |
| Minimum orbit distance | 1.12 planet radii |
| Maximum orbit distance | 6.0 planet radii |
| Click drag threshold | 4 px |
| Overview relief exaggeration | 8x |

All values should be exposed in the sample UI before being promoted to engine
configuration.

## Whole-planet overview renderer

### Separate rendering path

Add a dedicated overview renderer rather than expanding the existing local cap:

```text
SparseVoxelPlanetOverviewPass
  input: complete planet mesh, overview camera, sun/sky controls
  output: overview color and depth

SparseVoxelPlanetNavigationMergePass
  input: surface composite, overview color/depth, transition state
  output: scene color/depth before UI
```

The overview path uses rasterized triangles with simple planet-specific
lighting. It does not use Tree64, voxel ray tracing, voxel fog, GTAO, SVGF, or
voxel material resolve passes.

The overview geometry now defaults to the bounded GPU concurrent-binary-tree
path described in `concurrent-binary-tree-background-planet-plan.md`. Devices
without compute/graphics support, 256 compute invocations per workgroup, or the
9.005 MiB bounded allocation retain the immutable mesh automatically. The
sample also exposes `--static-planet-overview`, `--adaptive-planet-overview`,
and the Release qualification harness `--qualify-planet-cbt`.

### Mesh source

Prefer the package's reusable graph-native surface mesh:

- `PlanetGraphChannel::SurfaceDirections`
- `PlanetGraphChannel::SurfaceElevation`
- `PlanetGraphChannel::SurfaceIndices`
- `PlanetGraphChannel::SurfaceMaterial`
- `PlanetGraphChannel::SurfaceMountain`
- `PlanetGraphChannel::SurfaceTemperature`
- `PlanetGraphChannel::SurfacePrecipitation`
- `PlanetGraphChannel::SurfaceRiver`

Build and upload this mesh once when the planet package opens. If surface graph
blocks are absent, build a cube sphere and sample raster tiles at an appropriate
mip. The fallback must remain cube-direction based so it has no longitude seam
or polar topology dependency.

The first implementation can use one bounded mesh chosen at load time. If its
triangle count is too large, add overview mesh LOD using stable
`PlanetSurfaceBisectorAddress` patches rather than coupling overview LOD to
voxel clipmaps.

### Overview attributes and color

Each overview vertex needs at least:

```text
direction
normalized elevation
base terrain color
soil/rock color
mountain weight
forest weight
river weight
```

Base colors come from `planetVoxelSurfaceMaterial` so the overview and landed
voxel terrain share material identity and hue variation.

Forest coverage is not a dedicated package raster. Derive it using the same
inputs as the planet-generation sample:

```text
forest = precipitation response
       * habitable temperature response
       * non-ocean material mask
       * suppression on strong mountain relief
```

Use forest weight to shift suitable terrain toward darker, denser greens.
Mountain weight contributes rock/snow color, relief-normal shading, and a small
overview-only displacement exaggeration. Rivers and oceans retain their own
colors and can be rendered as either vertex attributes or a lightweight overlay.

### Lighting

Use a simple planet-only lighting model:

- Directional sun diffuse.
- Hemisphere/sky fill matching the surface scene mood.
- Relief normal mixed with radial normal.
- Optional ocean fresnel/specular.
- Subtle atmosphere rim.

The overview renderer should reuse the visual ideas in
`samples/polygonTerrain/PolygonTerrainSphere.wgsl`, but consume package data and
the shared voxel material palette rather than introducing a third terrain color
system.

## Orbit input and picking

### Orbit camera

Left drag rotates the camera around the planet. Clamp pitch only as needed to
avoid control singularities; do not prevent viewing the poles. Mouse-wheel zoom
uses an exponential scale so close and distant orbit both remain controllable.

The planet remains centred during ordinary orbit. Later, target-focused orbit
can offset the centre, but it is not required for the first implementation.

### Surface picking

For a click that was not a drag:

1. Convert the cursor to normalized device coordinates.
2. Unproject through the overview camera.
3. Intersect the ray with the normalized sphere.
4. Transform the hit into a planet-space unit direction.
5. Convert the direction to a cube face and tile using `planetCubeTile`.
6. Sample package elevation and material for the marker and landing pose.
7. Submit a new terrain request.

Sphere intersection is adequate for selection. Displaced relief is tiny
relative to the planet radius; exact terrain height is applied after selection.

Render a marker at the selected direction immediately. While loading, display
the destination tile, biome/material, and progress beside the marker or in the
navigation UI.

## Non-blocking terrain teleport

### Latest-request-wins job model

Replace the single opaque terrain future with a cancellable request record:

```cpp
struct PlanetTerrainRequest
{
  uint64_t generation;
  procedural::PlanetTileAddress anchor;
  std::stop_source stop;
  std::future<TerrainJobResult> future;
};
```

Submitting a new destination:

1. Requests cancellation of the previous job.
2. Increments the generation ID.
3. Starts the new job on the terrain worker pool.
4. Leaves the current surface terrain resident.

Terrain package reads, voxel page builds, packing, and Tree64 model work must
check the stop token between pages and major phases. A completed result whose
generation is no longer current is discarded without GPU publication.

The render thread may call `future.get()` only after a zero-time readiness poll
reports completion. It must never wait for cancellation or job completion.

### Decouple logical and physical compatibility

Changing the planet anchor invalidates logical chunk reuse, but it must not
invalidate the fixed GPU arena. Split the current cache decision into:

```text
logicalCacheCompatible   same anchor/settings; chunks may be reused directly
physicalLayoutCompatible same page count/settings/capacity contract; slots and
                         graph resources remain reusable
```

Physical toroidal slot capacities survive anchor changes. New planet pages are
assigned to the existing per-LOD slots and overwrite old contents through the
page-atomic upload path.

If a package can exceed the established capacity contract, reject it before
navigation starts or allocate its fixed arena at package load. Do not resize or
remap graph-owned resources during a teleport.

### GPU publication

Reuse the existing multi-frame terrain uploader:

1. Keep every page's occupancy, material, micro-surface, palette, and node data
   in one GPU dispatch.
2. Split a large planet teleport across bounded batches.
3. Keep the old Tree64 acceleration cut active while raster pages arrive.
4. Publish the new Tree64 snapshot with the final batch.
5. Atomically replace the active anchor, tangent frame, chunk records, and
   material-field mappings.
6. Begin descent.

Move material-field atlas and geometry-field-cache writes into the staged graph
upload. The teleport path must not call `waitIdle`, drain pending frames, rebuild
the render graph, synchronously overwrite the terrain arena, or issue a blocking
texture upload.

### Streaming behavior by navigation state

| State | Automatic local clipmap requests | Voxel rendering | Overview rendering |
| --- | --- | --- | --- |
| Surface | Enabled | Enabled | Horizon/local cap only |
| Ascending | Paused | Fading out | Fading in |
| Orbit | Disabled | Disabled | Enabled |
| LoadingTarget | Disabled | Disabled | Enabled |
| Descending | Paused until commit, then enabled | Fading in | Fading out |

Disabling automatic local requests in orbit mode is essential. The overview
camera is not expressed in local terrain coordinates and must never drive the
surface clipmap centre.

## State and ownership

Add a navigation controller owned by the sample initially:

```text
PlanetNavigationController
  navigation state and animations
  orbit input
  surface picking
  selected target
  request generation/cancellation
  saved surface pose
```

Keep rendering and terrain generation responsibilities separate:

```text
PlanetOverviewMesh              immutable package-derived globe data
SparseVoxelPlanetOverviewPass   globe rasterization
PlanetNavigationController      input, animation, and state transitions
PlanetTerrainStreamer           CPU requests and cancellation
Terrain upload path             bounded GPU installation and publication
```

The first integration may live in
`samples/sparseVoxelRenderer/SparseVoxelRendererSample.cpp`, but reusable math,
mesh loading, and navigation state should move into `src/voxel/svraster/` once
their interfaces stabilize.

Proposed files:

```text
src/voxel/svraster/PlanetNavigation.hpp
src/voxel/svraster/PlanetNavigation.cpp
src/voxel/svraster/PlanetOverviewMesh.hpp
src/voxel/svraster/PlanetOverviewMesh.cpp
src/voxel/svraster/rendering/SparseVoxelPlanetOverviewPass.hpp
src/voxel/svraster/rendering/SparseVoxelPlanetNavigationMergePass.hpp
assets/shaders/voxel/svraster/wgsl/sparse_voxel_planet_overview.wgsl
assets/shaders/voxel/svraster/wgsl/sparse_voxel_planet_navigation_merge.wgsl
```

## UI and diagnostics

Add a `Planet navigation` panel with:

- `Planet view (M)` / `Return to surface` button.
- Current navigation state.
- Orbit distance and altitude.
- Selected cube face, tile level, X, and Y.
- Selected surface material.
- CPU generation phase and progress.
- GPU upload batch and progress.
- Cancel teleport button.
- Ascent and descent durations.
- Overview relief exaggeration.
- Forest, mountain, river, ocean, and atmosphere strengths.

Log only state changes and bounded progress intervals:

```text
Planet navigation: Surface -> Ascending
Planet target: request=7 direction=(...) tile=(face,level,x,y)
Planet terrain build: request=7 ... 63%
Planet terrain upload: request=7 batch=2/4
Planet landing published: request=7 generation=...
Planet navigation: Descending -> Surface
```

Cancelled and stale jobs should be normal informational events, not errors.

## Implementation phases

### Phase 1: navigation math and state machine

- Add geodetic pose, tangent-frame construction, and state transitions.
- Add `M`, UI button, escape handling, and saved surface pose.
- Gate automatic terrain streaming by navigation state.
- Add deterministic unit tests for animation endpoints and tangent frames.

Exit condition: `M` runs a placeholder ascent/orbit/descent state sequence
without changing terrain residency or blocking a frame.

### Phase 2: whole-planet overview

- Load graph-native surface channels from the package.
- Add raster fallback mesh generation.
- Upload an immutable overview mesh.
- Add overview and navigation-merge render passes.
- Implement shared material colors, mountain relief, forest derivation, water,
  simple lighting, and atmosphere.

Exit condition: the complete planet is visible with recognizable mountain and
forest regions and remains visually related to landed voxel terrain.

### Phase 3: orbit controls and animated transition

- Add drag orbit and exponential wheel zoom.
- Connect the geodetic camera to both render paths.
- Blend local terrain and the overview globe during ascent/descent.
- Disable voxel-only effects in overview mode.

Exit condition: entering and leaving planet view is continuous, stable, and
free of large-coordinate precision artifacts.

### Phase 4: picking and destination selection

- Add cursor ray construction and ray-sphere intersection.
- Add direction-to-tile mapping and destination sampling.
- Add click/drag disambiguation and a globe marker.
- Add cube-face seam and pole tests.

Exit condition: clicking any visible planet point selects the correct package
tile and displays its material/elevation metadata.

### Phase 5: cancellable background teleport

- Add latest-request-wins generations and stop tokens.
- Add cancellation checks to package loading, voxel page workgroups, packing,
  and Tree64 preparation.
- Separate logical cache compatibility from physical arena compatibility.
- Reuse fixed toroidal slots across planet anchors.
- Connect final GPU publication to automatic descent.

Exit condition: repeated clicks while loading never stall rendering, stale work
never publishes, and the final selected destination lands correctly.

### Phase 6: remove remaining synchronous publication

- Stage material-field atlas updates in the graph upload.
- Stage geometry-field-cache changes with the final batch.
- Remove the teleport path's arena-remap and queue-drain fallbacks.
- Bound CPU upload packet memory if full-window replacements become excessive.

Exit condition: no planet navigation operation performs a render-thread wait,
graph rebuild, or synchronous terrain-resource overwrite.

### Phase 7: validation and polish

- Add progress UI and target labels.
- Tune overview relief and biome readability.
- Validate cached return trips and rapid target changes.
- Capture performance and visual baselines.
- Document controls in the sample usage string.

## Tests

### Unit tests

- Geodetic direction and tangent-frame orthonormality, including poles.
- Surface pose to orbit pose and back without drift.
- Ray-sphere hits at screen centre, off-centre, and near the limb.
- Planet direction to cube tile on every face and along seams.
- Click versus drag classification.
- Navigation state transitions, cancellation, and repeated `M` input.
- Request generations prevent stale publication.
- Physical clipmap layout remains stable when the anchor tile changes.
- Forest-weight derivation is deterministic and bounded.

### Integration tests

- Enter overview from a surface scene and return to the same pose.
- Select a known mountain tile and land with the expected anchor/material.
- Select a point on every cube face.
- Submit several destinations while the first is still generating.
- Teleport far enough that no terrain pages are reused.
- Return to a cached destination and verify transient-page restoration.
- Run with missing optional mountain/river graph channels and validate fallback.
- Verify Tree64 publishes only after the last raster-page batch.

### Render tests

- Full planet fits the camera at default orbit distance.
- Mountain belts and forest regions remain readable at whole-globe scale.
- Ocean, atmosphere, and terrain have no globe seam or polar star artifact.
- Ascent and descent do not reveal local-cap edges.
- No stale voxel terrain appears at the destination before publication.
- Surface and overview palettes remain recognizably consistent.

### Performance assertions

- No `waitIdle`, pending-frame drain, or graph rebuild during navigation.
- Render loop continues while CPU terrain work is active.
- GPU upload size per frame stays within the configured staging transaction.
- Cancellation latency is bounded by one terrain-page build phase.
- Overview rendering does not allocate or update Tree64 resources.

## Acceptance criteria

- Pressing `M` animates from the surface to a complete planet view.
- The planet shows recognizable mountain ranges, forests, rivers, biomes, and
  oceans using package-authored data.
- The user can rotate and zoom the globe smoothly.
- Clicking a visible point starts a background teleport to that location.
- The globe remains interactive and the project continues rendering while all
  required terrain chunks are generated and uploaded.
- Descent begins only after voxel terrain, material fields, and Tree64 are
  coherently published.
- Any new click safely supersedes an older pending destination.
- Planet navigation never rebuilds the render graph or blocks on GPU/CPU terrain
  work.
- Landing works across cube faces, seams, and poles without coordinate drift.
- Returning to cached locations reuses transient terrain pages.

## Deferred work

- Gameplay-scale travel while continuously descending through atmospheric
  flight rather than the bounded navigation animation.
- Multiplayer replication of overview selections and teleport requests.
- Persistent named bookmarks and a searchable region browser.
- Cloud layers, weather visualization, night lights, and seasonal overview
  coloring.
- Background prefetch around the cursor before a destination is clicked.

## Principal risks

| Risk | Mitigation |
| --- | --- |
| True-scale mountains are invisible from orbit | Use package mountain color/relief plus configurable overview-only displacement exaggeration. |
| A whole graph mesh is too large | Select a bounded package mesh or add stable-bisector overview LOD. |
| Orbit camera accidentally drives local streaming | Gate clipmap requests explicitly by navigation state. |
| Global anchor change causes an arena remap | Decouple physical slot compatibility from logical chunk reuse. |
| Rapid clicks publish stale terrain | Generation IDs, stop tokens, and final-generation validation. |
| Terrain cancellation remains slow | Check cancellation between pages and every major build phase. |
| Material atlas publication stalls the render thread | Move it into the staged graph upload and final publication batch. |
| Overview and voxel colors diverge | Generate both from shared material functions and package channels. |
