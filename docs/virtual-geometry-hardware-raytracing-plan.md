# Virtual geometry hardware ray traced shadows: feasibility investigation

Status: design and standalone CPU reference tests, September 9, 2026. No hardware
RT backend, GPU shadow traversal, or renderer option is implemented by this change.

The follow-up [implementation plan](virtual-geometry-hardware-raytracing-implementation-plan.md)
specifies the native Metal indirect TLAS path, one-time cached BLAS builds, phased
delivery, and hardware performance benchmarks. It also distinguishes GPU-executed
BLAS builds from GPU-authored build commands supported by neither checked descriptor path.

## Recommendation

Add an optional direct-shadow path with a separate `RayTraceCull` traversal, a
persistent cluster BLAS cache, a frame shadow-cut TLAS, and a compute ray-query
pass. Share hierarchy traversal, camera error, page addressing, and replacement
rules with raster culling; keep output queues, visibility history, and demand
channels separate. The three proposed passes are a good frame-level interface,
but BLAS maintenance and receiver-volume preparation are additional work.

Interpret "primary rays from lights" as direct-light visibility: reconstruct a
visible surface from the camera depth/G-buffer and trace from that surface toward
a sampled light, terminating at the first accepted blocker. No bounce traversal is
needed. Launching rays outward from lights is not required to answer those queries.
This cut is suitable for those shadow rays; it is not a general reflection/GI scene.

In usual terminology, geometry goes into **BLAS** (Metal primitive acceleration
structures); instances of selected geometry go into **TLAS/IAS**. A persistent
IAS containing every resident cluster is unnecessary. Keep reusable BLAS available
for resident clusters, and put only a valid selected cut into the frame TLAS.

## What already exists

| Area | Current implementation | Consequence |
| --- | --- | --- |
| Runtime visibility flags | `src/runtime/virtualgeometry/rendering/VirtualGeometryCullingMultipleDispatchesPass.hpp`, `Settings` and `updateUniforms` | Frustum and HZB can already be disabled independently. |
| Camera-based LOD | `assets/shaders/virtualgeometry/wgsl/culling_multipass.wgsl`, `calculateLodDistance` / `calculateProjectedError` | Uses radial camera distance minus scaled sphere radius, clamped by near distance. Works outside/behind the frustum. |
| Other camera rejection | Same shader, hierarchy/cluster processing | Projected-footprint rejection and camera cone rejection remain after disabling frustum/HZB. Two-phase visibility history must also be bypassed. |
| Streaming demand | Same shader, `initSync` / `computePriority` | Culling initialization clears shared priorities. Normal demand uses addition; speculation uses priority 1. A second enabled traversal would erase earlier demand. |
| Resident fallback | Same shader, first missing page and streaming-leaf / enabled masks | Traversal stops at missing pages; cut masks supply fallback. AS readiness must become an additional replacement constraint. |
| Decoded geometry | `src/runtime/virtualgeometry/VirtualGeometryScene.hpp`, `DecodedClusterGeometryInfo`, `DecodedVertexData`, `VisibleClusterInfo` | Cluster vertex/index ranges and instance IDs already exist. Position is three floats at offset zero in a 48-byte vertex. AS build usage/address/alignment still need validation. |
| Frame scheduling | `src/runtime/virtualgeometry/rendering/VirtualGeometryRendererPass.hpp` | Decode precedes camera culling; streaming selection currently follows final culling. Selection must move after merged camera/shadow demand. |
| Hardware RT abstraction | `src/runtime/rendering/gpu/{Types.hpp,RHI.hpp}` and `backend/vulkan/` | No AS handle/binding/build command, RT feature bit, AS synchronization stages, or ray-query shader path found. This is substantive backend work. |

Existing `SoftwareRayTraceScene` and voxel software TLAS implementations are not
hardware AS support. The native Metal compute raster bridge and `docs/metal-rhi-plan.md`
are useful precedents, but do not provide a ready ray tracing backend.

## Frame data flow

```text
page upload/decode ──> build dirty/new cluster BLAS ───────────────┐
                                                               │
camera traversal + depth/visibility                             │
             │                                                 │
             ├──> conservative receiver/light volumes           │
             │                  │                              │
             │           RayTraceCull(full scene roots)         │
             │                  ├──> shadow demand              │
             │                  └──> ready shadow cut ──────────┤
             │                                                 v
             │                                  TopAccelerationStructureBuild
             │                                                 │
             └──> G-buffer + lights ─────────────────> RayTraceShadow
                                                               │
                                                         lighting resolve

camera demand + shadow demand -> merge -> streaming page selection
                                      -> future page installs/BLAS work
```

`RayTraceCull` starts from all relevant scene roots. The camera-visible cluster
list is input for **receiver bounds**, not the only source of caster candidates.
Otherwise an offscreen object whose root camera traversal rejected is undiscoverable.
With two-phase occlusion and HW/SW raster, receivers must include all contributing
phases/routes, or come from the completed depth/visibility target. Final phase-two
output alone omits phase-one geometry.

For the first prototype, construct receiver bounds from conservative world bounds
of the camera cut. This can run without a depth reduction and admits extra work.
Later use occupied depth tiles plus light lists to make tighter volumes. A global
receiver AABB is safe but can include enormous empty regions. If volume storage
overflows, merge bounds or use a broader fallback; never silently discard receivers
or lights. Use current-frame bounds, or explicitly conservatively expand temporal
bounds for camera/object/light motion.

## Light-aware culling

For finite lights, every potential blocker lies somewhere on a segment joining
a receiver to a light sample. An AABB around receiver bounds and emitter bounds
contains all these segments. Reject a hierarchy bound only when it misses every
applicable volume. Intersection establishes possible contribution, not actual
occlusion; the eventual ray query decides visibility.

- Point light: start with the AABB union of receivers and light position. Tighten
  later to the convex hull/pyramid of receiver bounds and the point light.
- Spot light: additionally intersect with conservative light range/cone bounds.
- Area light: include the whole emitter, not only its center. Otherwise penumbra
  blockers can disappear.
- Directional light: sweep receiver bounds toward the light over the shadow ray
  distance. Use a finite scene bound or an explicit maximum shadow distance. A cap
  changes which distant shadows are represented; use the same cap in cull and trace.

Use light/receiver tile lists or a hierarchy of these volumes to avoid testing
every node against every light. Union their demands; do not independently concatenate
one selected LOD cut per light. Selection of incompatible levels for the same region
must resolve into one valid replacement cut at the finest required error.

## LOD and coverage rules

Start with the existing radial camera-error metric and a configurable shadow error
threshold. It is stable outside the view and easy to compare to the raster cut.
It is a heuristic for shadow quality: camera distance to a caster does not bound
shadow displacement on a receiver. For a point light and parallel receiver/blocker
planes, lateral blocker error is magnified approximately by light-to-receiver
distance divided by light-to-blocker distance. Grazing receivers can amplify it
further. A near-light caster can therefore need refinement even when far from camera.

Later evaluate error over receiver/light pairs, including receiver pixel footprint,
light geometry, and conservative shadow projection. Take the maximum requirement
across affected pairs and camera requirements where shared. Add hysteresis and a
budget; don't claim camera-error-only LOD guarantees pixel-accurate shadows.

Keep the production DAG/group replacement semantics. A cluster is not necessarily
independently replaceable by an arbitrary subset of descendants. Refinement must
preserve coverage across the complete replacement group. Parent and children must
not both enter the TLAS: overlapping approximations cause duplicate intersections
and false self-shadows. A missing child keeps the appropriate complete parent
fallback. Keep it until children are decoded **and all replacement BLAS are ready**.

Existing enabled masks and terrain publication can remove parents as soon as raster
replacement is ready. Merely checking `blasReady` in a new culler is insufficient
after that parent is retired. Choose either shared publication gated on AS readiness
or a separate RT cut that retains and pins its fallback representations. The latter
preserves raster progress, but requires explicit retention and per-consumer cut state.
Global masks can also limit how independently the shadow cut chooses coarser LOD.

Do not union raw camera and shadow lists if their thresholds differ. Build a cut
from reconciled per-region demand. For initial opaque tests use matching LOD near
visible receivers; mismatched raster and ray geometry also causes self-intersection
and light leaks even when the RT cut itself has no duplicates.

## Flags and streaming priorities

A shadow traversal policy must bypass camera frustum, camera HZB, camera backface
cone, camera projected-size, and camera two-phase-history rejection. It needs its
own output emission, without raster HW/SW routing or camera visibility-bit writes.
Retain LOD tests, valid page/cluster tests, enabled topology, conservative transforms,
and the new receiver/light-volume test. Share helper code through a shader variant
or policy, with separate pass resources. Adding more flags to the raster dispatch
alone does not make its output a correct shadow cut.

Use separate camera and shadow demand buffers, clear once per frame, then merge
before streaming selection. Proposed ordering is visible camera demand > required
shadow-only demand > speculative prefetch > no demand. Keep an urgency score within
each class. A packed key can implement this, but saturate within each class before
packing; never let repeated light visits add across class bits. An explicit class
plus score is easier to audit. Touch selected resident RT pages for retention too.

The current raw priority values also feed runtime refinement and eviction logic;
do not silently reinterpret them as packed classes. Integrate the scheduler and
all consumers, or preserve current camera metrics in a separate channel. An
offscreen shadow page can serve a large visible shadow, so allow quality-controlled
promotion/budgeting later. Absolute camera priority can starve shadows under load.

## Acceleration structures and backend scope

Cache static object-space BLAS by geometry identity including page generation and
local cluster, with shared use by repeated instances. Never key only by a reusable
page slot. Transform-only motion changes TLAS instance transforms. Deformation
requires updated vertex positions and BLAS build/refit; existing raster skinning is
not automatically applied by an AS builder. Begin with static opaque triangles.

Build BLAS on page decode/install and geometry change, not every frame. All resident
clusters can eventually have BLAS, but prioritize selected clusters plus coarse
fallbacks and budget AS bytes, scratch, and builds. Streaming residency and AS
residency are distinct readiness states. Pin referenced geometry/BLAS until all
in-flight TLAS/ray work retires; eviction or slot reuse must not invalidate them.

One BLAS per cluster gives exact arbitrary cluster selection and is useful for
bring-up, but may have severe allocation/build overhead and TLAS instance counts.
Measure before committing. A BLAS per page includes multiple LOD alternatives when
pages mix levels, so it is not automatically equivalent. Candidate rejection with
cluster metadata can filter inactive triangles but adds traversal/any-hit work;
grouping geometry by stable replacement sets or assembling a cut BLAS trades that
cost for rebuild cost. Do not assume nested IAS is a portable third level.

Vulkan first needs capability queries/enabling, AS resource and descriptor types,
device-address/build-input buffers, scratch management, build commands, and graph
dependencies for decode -> BLAS -> TLAS -> compute query. Ray queries can perform
shadows within ordinary shader stages without a dedicated ray-tracing pipeline or
shader binding table. See the [Khronos ray-query sample](https://github.com/KhronosGroup/Vulkan-Samples/blob/main/samples/extensions/ray_queries/README.adoc).
The current WGSL toolchain needs a verified RT extension path or a small native
GLSL/SPIR-V shader path; no existing ray-query shader was found.

A GPU-written cut count is another capability boundary. Vulkan's indirect AS build
command requires `accelerationStructureIndirectBuild`. Do not infer it from ray-query
support. If unavailable, an initial path can rebuild a fixed-capacity TLAS with valid
masked dummy instances in unused slots, or use an explicit readback prototype. The
former costs build capacity; the latter stalls. Changing topology/count/readiness
generally calls for rebuild; only refit under the API's update constraints. See
[Vulkan indirect AS build requirements](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBuildAccelerationStructuresIndirectKHR.html).

For Apple bring-up, evaluate a native Metal AS/query bridge or the planned Metal RHI,
with explicit shared-buffer and completion synchronization. Query device support;
do not assume MoltenVK exposes the required Vulkan RT extensions, or that API ray
tracing support means dedicated hardware acceleration. Apple documents the native
route in [Accelerating ray tracing using Metal](https://developer.apple.com/documentation/metal/accelerating-ray-tracing-using-metal).

## Tests now and implementation sequence

`VirtualGeometryShadowCutReferenceTests` is a standalone CPU experiment using the
existing `AABB` type. It checks:

1. A blocker outside the camera frustum survives a receiver-to-light volume;
   unrelated and beyond-light bounds are rejected; multiple lights and no lights.
2. Area-light and directional bounds retain 4,224 sampled segment points, including
   emitter extent and negative sweep components.
3. Camera error remains radial behind/offscreen and finite inside the bounds.
4. Required shadow demand cannot overwrite/outscore visible demand or fall below
   speculation, regardless of the number of light visits.
5. Missing pages and decoded-but-unbuilt children retain the parent; ready children
   replace it without overlapping levels; nested groups retain a local fallback;
   an unready root emits no invalid reference.

These are reference-policy tests, not validation of the production GPU shader,
real DAG coverage, actual triangle hits, AS lifetime, or GPU performance. The tree
fixture assumes each node is a complete replacement group. Volume tests establish
conservative candidate selection, not that every admitted candidate blocks a ray.

Build/run from the repository root:

```sh
cmake -S . -B build
cmake --build build --target VirtualGeometryShadowCutReferenceTests -j 4
ctest --test-dir build -R '^VirtualGeometryShadowCutReferenceTests$' --output-on-failure
```

Validation on this checkout: configuration/build succeeded and CTest passed 1/1
on September 9, 2026. Hardware RT capability and timings have not been measured.

Next milestones:

1. Hardware capability probe and static triangle BLAS/TLAS compute-query smoke test:
   known hit/miss, transform, empty scene, and device unsupported fallback.
2. Small fully resident VG scene, all-direction camera-error cut, static opaque
   geometry. Compare shadow results with brute-force triangles from the same cut.
3. Separate GPU shadow traversal/demand channel and light volumes. A visible receiver,
   offscreen blocker and movable light must agree with the unculled shadow baseline.
4. Stream out a needed child and delay its BLAS: retain fallback, verify no invalid
   references, then refine. Exercise page-generation reuse and frames in flight.
5. Add tighter receiver tiles, AS budgets, shadow-aware LOD, alpha tests, deformation,
   and optional soft-shadow accumulation after correctness. Record culled nodes,
   requested pages, cut clusters, BLAS/TLAS bytes/build time and query time separately.
