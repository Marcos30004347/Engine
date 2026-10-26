# Sparse voxel temporal rendering and scene-ray plan

Status: active — two-phase culling and TAA/TSR integration complete; the Tree64 sample path now runs Light-BVH candidate creation → temporal DI reservoir selection → spatial DI reuse → current-HZB screen trace → Tree64 fallback → canonical hit merge → temporal hit accumulation → visibility-aware shading. GPU compaction is live. The light set supports directional, point, sphere, planar rectangle, and analytic sky records. Temporal reuse uses motion-vector reprojection plus normal/depth/identity validation, and spatial reuse applies the same receiver validation.

Date: 2026-07-25

Scope: `src/voxel/svraster`, the sparse-voxel renderer sample, shared temporal rendering utilities, and the reusable WorldGI light BVH.

## Decision

The sparse-voxel renderer will use a single hit-only ray pipeline. Rays are created from visible voxel surfaces and a selected light, first traced against the current screen depth hierarchy, and only unresolved rays fall back to the camera-centered Tree64 world volume. The merged hit result is temporally accumulated before shading. TAA/TSR then temporally resolves the final shaded color.

The target sequence is:

```text
two-phase HZB culling
  -> jittered voxel G-buffer
  -> current depth pyramid
  -> create N weighted light candidates
  -> temporal DI reservoir selection
  -> spatial DI reservoir reuse (one ray/pixel)
  -> screen ray trace
  -> Tree64 world ray trace for unresolved rays
  -> merge hit results
  -> temporal hit accumulation
  -> shading
  -> TAA / TSR
  -> present
```

Occlusion culling and the two-phase HZB path are enabled by default. The explicit `--disable-occlusion` baseline remains available for validation and benchmarking. BLAS-node HZB rejection remains a separately controlled experimental optimization until it has the same equivalence and performance evidence as the TLAS path.

## Existing integration points

- `SparseVoxelGBufferPass` already supplies depth, geometry normal, material data, motion vectors, and stable instance/voxel identity.
- `TemporalSuperResolutionPass` is the reusable six-stage temporal resolve. It accepts color, normal, depth, motion, a disocclusion mask, and ping-pong history targets.
- `ScreenSpaceRayTracePass` already contains the fast HZB screen marching kernel and emits screen hits plus a compact unresolved-ray list. Its ray generation must be separated from tracing.
- `SparseVoxelTree64TracePass` already has a hit-only world traversal ABI. A preparation/merge layer will adapt its results to the common ray ABI rather than adding material logic to Tree64.
- The sample already implements a previous-HZB temporal pass, a provisional current HZB, and a final recovery pass.
- `WorldGILightBVHBuildPass` builds directional, point, sphere, and planar rectangle records. Directional emitters are queried outside the finite tree; finite records carry explicit shape parameters.

## Phase A — temporal raster foundation

1. Make two-phase occlusion the default in the sample and retain an explicit `--disable-occlusion` baseline mode.
2. Add voxel renderer temporal settings: TAA enabled, TSR enabled, render scale, jitter sequence/scale, accumulation count, and validation thresholds.
3. Split display and internal render dimensions. Raster buffers, G-buffer, HZB, ray buffers, and shading are internal-resolution; temporal histories and output are display-resolution.
4. Reuse the generic `TemporalSuperResolutionPass` after voxel shading. Use a per-frame history ring when multiple frames are in flight.
5. Use jittered projection and matching raster principal point for G-buffer/ray reconstruction. Keep TLAS/BLAS culling unjittered so jitter does not cause culling instability.
6. Invalidate history on resize, render-scale changes, camera cuts, scene/topology replacement, temporal settings changes, and invalid previous transforms.

## Phase B — common ray and hit ABI

Introduce shared GPU records equivalent to:

```cpp
struct SceneRayGPU {
  float originTMin[4];
  float directionTMax[4];
  uint32_t pixelRayLight[4]; // pixel x/y, ray slot, stable light ID
  uint32_t seedFlags[4];     // sample seed, ray type, flags, scene generation
};

struct SceneRayHitGPU {
  float hitPositionT[4];
  int32_t hitCellStatus[4];
  uint32_t identity[4];      // ray ID, light ID, backend, generation
};
```

The ray contract contains only geometric/visibility data. Shading data remains in the G-buffer and light buffers.

Canonical statuses are `Hit`, `MissTMax`, `EscapedScene`, `EscapedTreeVolume`, `UnknownResidency`, `QueryLimit`, and `Invalid`.

`EscapedTreeVolume` must not be treated as a true scene miss. Tree64 is camera-centered coverage, while `EscapedScene` means the ray has exited the authoritative scene AABB. An unknown or unavailable Tree64 region is initially conservative for shadowing and carries low confidence for temporal handling.

## Phase C — tracing and merge

1. `SceneRayCreatePass` reads G-buffer receivers and creates dense, stable ray slots. It writes the selected light ID and deterministic sample seed. **Implemented** as `SparseVoxelSceneRayCreatePass`.
2. Screen tracing consumes those rays and writes dense hit records indexed by ray ID. It distinguishes a screen escape from a finite-distance miss. **Implemented** as `SparseVoxelSceneScreenTracePass`; unresolved IDs are compacted for Tree64.

   The sparse-voxel path uses a `1e-5` normalized-depth confirmation interval. This intentionally thin surface depth avoids false screen occluders; uncertain cases fall through to Tree64.
3. `SceneRayTree64PreparePass` converts unresolved rays into Tree64 input rays and preserves their canonical dense slot. **Implemented** as `SparseVoxelSceneRayCompactionPass`, which writes compact rays plus canonical/compact ID maps.
4. Extend Tree64 tracing so the fallback count is GPU-driven. Do not add a CPU readback merely to obtain an unresolved-ray count. **Implemented:** `SparseVoxelTree64TracePass` consumes a GPU-authored storage dispatch record and indirect arguments.
5. `SceneRayHitMergePass` writes exactly one canonical result per input ray, combining screen results, Tree64 results, and explicit escape/unknown classifications. **Implemented** as `SparseVoxelSceneHitMergePass`, including Tree64-volume escape classification.

## Phase D — temporal hit accumulation and shading

`SceneRayTemporalPass` accumulates visibility rather than enum values or voxel-cell IDs. It stores visibility moments, history length/confidence, representative current hit distance, receiver origin, light identity, and generation information. The dense-slot implementation is `SparseVoxelSceneHitTemporalPass`: it rejects changed light/generation, receiver displacement, invalid states, and materially different hit distances while allowing real hit/miss visibility transitions to converge. History reprojection with motion vectors and depth/normal/identity disocclusion rejection is the next refinement.

The sample now consumes stabilized visibility in its default scene-ray shading output. It creates N Light-BVH-selected candidates per pixel (two by default), reservoir-samples one candidate, and traces only that selected shadow ray through screen and Tree64 backends. The shading path applies the reservoir RIS correction before evaluating the selected light direction, color/intensity, G-buffer albedo/normal, and emissive data. Point, sphere, planar, and directional emitters have independent ImGui controls; the default is an elevated point light at the scene center. Tree64 coverage escapes, unknown residency, and query limits use a conservative partial-visibility fallback rather than a hard black shadow. A scene-ray status output exposes every final classification for validation. Light edits advance an explicit generation so stale ray history is rejected. Visibility transitions and uncertain trace classifications feed a reactive mask into TAA/TSR so final-color history does not preserve stale shadowing.

## Phase E — light BVH

Generalize the existing WorldGI light records and CPU BVH builder rather than creating a competing structure.

- Supported light types: point, sphere, planar rectangle, directional.
- Give every light a stable handle and revision.
- Store finite lights in a BVH using conservative influence AABBs.
- Keep directional lights in a separate infinite-light list; do not use huge AABBs in the finite tree.
- Build/refit only when light data changes. A CPU SAH build is appropriate initially; GPU LBVH/refit is a later measured optimization.
- Query the BVH during ray creation, sample the selected emitter with the ray seed, and set finite-light `tMax` to the sample distance. Directional rays use the scene-exit distance.

Current implementation: `WorldGILightGPU` stores sphere radius and planar half-extents in `shapeParams`; planar records use `directionType.xyz` as their emitting normal. Sky records use the otherwise position-like fields for horizon, zenith, and ground colors plus world up and horizon exponent. The CPU builder creates conservative, shape-aware finite influence AABBs while directional and sky records remain outside the finite tree. The scene-ray create pass performs power/distance-weighted stochastic selection, records selection/emitter PDFs, and cosine-samples sky directions. `SparseVoxelDIReservoirPass` consumes N candidates per pixel and motion-reprojects the prior selected ray/reservoir. It accepts history only when generation, stable instance/voxel identity, geometry normal, and relative linear depth validate. `SparseVoxelDISpatialReservoirPass` applies the same receiver tests to four immediate neighbors plus a world-space receiver-radius gate. Finite samples reconnect their emitter point; directional and sky samples retain their infinite direction. Stable handles are fixed per light slot and revisions are explicit settings fields, so frame progression alone does not invalidate temporal state.

## Validation gates

1. Default smoke runs report two-phase occlusion enabled; disabled baselines remain executable.
2. Existing occlusion equivalence, disocclusion, frame-stability, and UI-toggle tests remain green.
3. New GPU fixtures cover screen hit, Tree64 hit, finite miss, scene escape, Tree64 escape, unavailable residency, and query limit.
4. Temporal tests cover static convergence, camera movement, moving occluders, light changes, camera cuts, and Tree64 directory scrolling.
5. TAA/TSR tests cover native TAA and reduced-resolution TSR against static and moving scenes.
6. Timings record screen-hit rate, fallback count, Tree64 query count, unknown/escape counts, temporal rejection rate, and GPU time per stage.

### Debug observability

The sample now writes three independent `RGBA16F` scene-ray diagnostic
textures every frame: raw screen-trace classification, final world/Tree64
classification, and the full reservoir-corrected lighting accumulation. The
last three scene-ray entries in the G-buffer output selector expose the same
views interactively. A compact GPU statistics buffer is atomically populated
alongside those textures and asynchronously read back for ImGui: active rays,
screen/world hit and miss counts, scene/tree escapes, unresolved results,
reservoir candidate count (M), temporal reuse, spatial reuse, and invalid
rays. This keeps debugging off the full-resolution ray-buffer readback path.

### Test ownership

Every phase lands with both a local contract test and an integration test.
`SparseVoxelSceneRayABIContractTests` locks the cross-backend memory layout and
status values. `SparseVoxelSceneRayGPUValidationTests` now validates
motion-reprojected temporal reuse, independent identity/normal/depth rejection,
spatial receiver gating, and analytic-sky ray/PDF generation from readback. The
next fixture will add screen-hit, screen-escape, Tree64-hit,
Tree64-volume-escape, and merge precedence. Temporal hit accumulation still
needs static-convergence, camera-cut, and Tree64-scroll rejection cases; light
BVH work still needs one builder-output fixture per finite light type plus
mixed-light traversal.

## Implementation order

1. Default-on two-phase culling and temporal renderer configuration.
2. TAA/TSR integration at the voxel renderer boundary.
3. Shared ray/hit ABI and separated ray generation.
4. Screen trace to Tree64 fallback and explicit bounds classification.
5. Temporal hit accumulation and shaded HDR output.
6. Generalized point/sphere/planar/directional light BVH.
7. Weighted Light-BVH candidate selection and current-frame DI reservoir selection.
8. Temporal then spatial ReSTIR DI reuse, followed by dedicated trace/lighting debug textures and counters.
