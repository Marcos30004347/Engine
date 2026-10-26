# Hybrid virtual shadow cache status

**Latest integration update:** [GPU cuts, RT depth pages, screen-first handoff and VSM light BVH](hybrid-shadow-gpu-integration-status.md). This update supersedes the older open-gate entries below for those components.

September 9, 2026. Implementation is in progress. The integrated raster VSM
foundation and additional Metal query/build controls below are implemented.
**The complete hybrid shadow system is not implemented:** RT depth-page production,
AS-ready VG integration, area/sky receiver caches and temporal accumulation remain
required before exposing a replacement hardware-shadow mode.

## Integrated changes

- Global and per-light camera-to-receiver fade/cutoff policies. Bookkeeping skips
  shadow demand beyond the cutoff; final lighting skips shadow-page reads and
  preserves unshadowed analytical illumination. The layered penumbra pass also
  skips shadow-disabled receivers. Receiver distance never becomes a caster-ray
  length. The existing standalone screen-space shadow pass still has independent
  work; it is not yet driven by these tile lists.
- GPU 16×16 opaque-receiver tile light culling before demand and final lighting.
  Exact bitsets use `16 + tiles * ceil(maxLights/32) * 4` bytes. Directional lights
  remain global in effect; local spheres conservatively include spot cones and
  the existing point-light demand margin. Incompatible/disabled lists fall back
  to the complete light loop. This first version trades compact-list scans for
  bounded exact bitsets and has no per-tile list overflow.
- Stable GPU prefix scans replace racing request appends. Coarse work takes the
  shared `maxPageUpdatesPerFrame` quota first (default 64). Fine jobs may share the
  frame when every pending coarse job fits; otherwise fine work stays pending.
  Coarse physical allocations execute before fine allocations. Admission is
  repeatable for identical demand; physical slot assignment remains atomic.
- An explicit scheduled-page state controls raster work. Raster culling does not
  impose a second, racing quota or exempt fallback pages. Every admitted allocated
  page, including pages with no casters, enters the clear/completion list. This
  fixes empty coarse pages otherwise blocking cache warmup.
- Demand and resolve agree on the coarsest fallback level/face mip. Clipmap parent
  projection uses each actual matrix and wrapped grid, not a page-coordinate shift.
  `fallbackCascadeOffset == 0` still disables directional fallback; nonzero values
  now enable coarsest coverage rather than selecting a fixed offset.
- `Settings::matchDirectionalResolution` computes a first extent from FOV, shading
  height, reference depth and target texel footprint, then derives a power-of-two
  level ladder covering the shadow distance with page-snap margin. The shared
  directional selector uses the smaller light-space Jacobian singular value;
  missing/discontinuous depth derivatives request the finest covering level.
  Legacy radial selection remains available with `targetShadowTexelPixels == 0`.
- The resolution helper enables a quantized camera-relative directional depth
  origin and expands its half-range to include the configured receiver radius.
  Crossing a depth-origin band invalidates that light's pages. This finite
  projection is still a bounded caster domain; it does not cover infinite casters.
- Packed page ownership is checked against its 256-layer limit. Dynamic logical
  capacity resizing, per-light active directional level counts, FOV/resolution
  hysteresis, starvation prevention and unmet-footprint diagnostics remain open.

## Metal additions

`VirtualGeometryMetalRayTraceBackend::Frame::Query::ClosestHit` returns a 16-byte
`ClosestHitResult` containing ray parameter distance, state, instance and primitive.
It explicitly disables any-hit acceptance. A miss or invalid scene is distinct
from a nearest blocker; for normalized directions, the parameter is world distance.
The graph adapter exposes query selection. Existing any-hit outputs remain 4 bytes.

`maxBlasBuildsPerFrame` and `maxBlasBuildBytesPerFrame` bound native build admission
in stable installation order; defaults preserve previous unlimited admission.
Bytes measure AS output allocation, not execution time or scratch bandwidth.
Deferred handles are absent from the usable resource-ID table and produce invalid
scene results if referenced. Pending work remains queued, with a pending-build
counter. A job larger than the byte quota stays pending until the quota increases.
The internal masked sentinel is exempt from user-geometry build quotas.

The [page-install BLAS gate](page-install-blas-status.md) now provides versioned
encoded-page manifests and completion-before-publication for file/runtime installs.
It also adds native producer-event support and an install-managed TLAS/trace mode.
Generated-terrain manifests and reservation lifetime handling are now wired too,
with sample batch preflight and deferred cancellation. GPU scene-cut conversion,
complete replacement-group/parent cuts and camera-first streaming merging remain open.

## Configuration

Configure the immutable manager capacity before creating its passes:

```cpp
VirtualShadowMapManager::Settings shadows;
shadows.matchDirectionalResolution(1080, verticalFovRadians, 1.0f,
    ShadowDistancePolicy{800.0f, 1000.0f}, 16, 1.0f);
shadows.maxPageUpdatesPerFrame = 64;
VirtualShadowMapManager manager(&graph, shadows);
// After creating a light:
manager.setLightShadowDistance(lightId, {80.0f, 100.0f});
```

The helper chooses a **capacity at construction**, not an automatic per-frame
quality controller. Changing distance later does not grow that capacity. Keep
later cutoffs inside the configured coverage or recreate the manager. Point and
spot lights retain their bounded face/mip allocation rather than acquiring the
world directional level count. The page budget covers admitted depth page work;
separate RT ray, receiver-record and audit budgets are not yet connected.

## Validation and measurements

- `VirtualShadowPolicyTests`: fade endpoints/zero cutoff, first-extent calculation,
  coverage margins and invalid policy rejection.
- `VirtualShadowTileTests`: real GPU bookkeeping plus final-resolve bindings;
  34-light bit boundaries, conservative local rejection, identical page-state
  demand with/without tile culling, zero-distance suppression, empty depth,
  repeatable exact/zero budgets, shared footprint-selector execution and packed
  capacity rejection. Real empty-scene raster execution verifies clear/completion
  and cache warmup over repeated frames. Image parity for nonempty caster scenes
  and tile-culling performance sweeps remain acceptance gates.
- Native Metal tests additionally verify nearest-hit ordering/distance/masks,
  deferred BLAS admission and a zero-byte build quota. Existing graph handoff,
  offscreen culling, cache eviction and render-graph regressions are retained.
- [Ten interleaved release benchmarks](benchmarks/hybrid-shadow-foundation-m4-2026-09-09/README.md)
  compare any-hit and closest-hit on M4. Median per-run GPU p50 is 0.749/0.855 ms
  at 1,000 instances and one million rays; p95 is 4.828/5.376 ms. These are native
  microbenchmarks, not a complete shadow-frame performance claim.

## Next implementation gates

Continue H0/H2/H3 in the [implementation plan](hybrid-virtual-shadow-cache-plan.md),
then H4–H7: install/readiness/caster-demand integration, closest-hit depth-page
production with raster-compatible depth publication, surface-identified receiver
records, deterministic ray-range admission, temporal weighted irradiance moments
and convergence/audit policy, area emitters, sky clipmaps/sampling, material resolve,
and full-renderer quality/performance benchmarks. Native Vulkan AS remains deferred.
