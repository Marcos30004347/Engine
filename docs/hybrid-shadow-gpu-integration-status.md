# Hybrid shadow GPU integration

September 9, 2026. This update adds working renderer building blocks and an opt-in
hard-depth producer. The complete moving-light/area/sky receiver cache is still
unfinished. Native Vulkan AS remains deferred.

## VSM light BVH

VSM and WorldGI share `LightBVHBuilder`, a deterministic spatial median builder
with preorder escape indices. VSM retains its own stable light indices and its
own frame-local buffers; it does not depend on a GI grid or GI light lifetime.
Finite influence bounds include the existing point-light demand margin. Infinite
lights occupy a separate prefix. Color/camera-only updates reuse the tree;
changed influence bounds rebuild it, and each frame slot receives new revisions
before use.

`VirtualShadowMapBookkeepingPass::Settings::enableLightBVH` and
`lightBVHMinLocalLights` choose stackless conservative enumeration or the parallel
full-light baseline. Both produce the same existing tile bitsets, consumed by
page demand and lighting resolve. Malformed traversal metadata falls back to a
complete light loop. Enumeration has no fixed stack or compact-list overflow.
Traversal splits disjoint subtrees across 64 tile lanes. The current M4 sweep
still favors the parallel full-light baseline, so BVH enumeration is opt-in
(`enableLightBVH=false` by default), pending a workload showing a benefit.
The [M4 measurements](benchmarks/vsm-light-bvh-m4-2026-09-09/README.md) are
0.274 ms for BVH versus 0.229 ms for the full loop in the 202-local-light fixture.

## Installed handles and scene cut

Successful encoded-file, runtime and generated-page publication now uploads
`uint2(handle, globalPageTableIndex)` records indexed by global cluster slot.
GPU handle publication precedes page visibility and rewrites. BLAS work still
belongs to installation; the render passes do not rebuild warm geometry.

`VirtualGeometryRayTraceCutPass` consumes the independent VG scene-cut output,
page table, handle table, instance transforms and cluster bounds. It writes native
packed instances, GPU count and transformed caster bounds with GPU indirect
conversion. It rejects missing/stale handles, uninstalled pages, unsupported
mesh-part deformation, capacity overflow, unfinished hierarchy traversal and
software-routed geometry. Native rays then return `InvalidScene`, preserving
shadow fallback instead of accepting a partial cut as an empty/visible scene.
Culling now guards hierarchy/cluster queue writes and reports overflow explicitly.

The cull has independent `sceneCutClusterCapacity` (default 524,288 entries) and
`sceneCutErrorPixels`. The latter can be updated with the culling uniforms without
changing raster error; capacity changes require graph recreation. Match native
`maxInstances` and adapter capacity. These controls are API settings; an automatic
hardware-shadow mode and the corresponding sample UI sliders are still open.
Complete replacement groups across pages, offscreen receiver/emitter traversal
and camera-first streaming-demand merging remain correctness/integration gates.

## Hard RT depth pages

`VirtualShadowMapRayTraceDepthPass` consumes the existing scheduled coarse/fine
requests and an AS-ready instance/count pair. GPU scheduling admits whole pages
under its ray budget, coarse first. It generates light-projection rays and runs
native closest-hit queries. Hit positions are projected into the same depth
convention as raster VSM; misses publish far depth. The pass validates a whole
page before publication and leaves invalid scenes dirty for retry/fallback.

The existing atlas is a depth attachment, which compute cannot write directly.
A depth-only page blit publishes the computed values. It draws six vertices per
admitted page, with no geometry/caster rasterization, raster culling or HPB work.
Page-table validity is committed afterward, with physical ownership rechecked.
Changing `setRayBudget` preserves pending pages; budgets smaller than one page
perform no partial page production.

`VirtualShadowMapPass::Settings::hardwareDepthBackend`,
`hardwareDepthInstances`, `hardwareDepthInstanceCount` and `hardwareDepthSettings`
select this producer in the wrapper. The wrapper verifies that the backend is
also the scene's Metal installation backend. This opt-in currently replaces the
whole hard-depth producer; per-light mixed raster/RT dispatch is still open.
Callers must maintain the independent complete scene cut and invalidation policy.

## Screen-first receiver shadows

`VirtualGeometryScreenFirstShadowPass` accepts the exact ray/hit buffers used by
`SparseVoxelSceneScreenTracePass`, plus a GPU source count and native scene cut.
Confirmed screen blockers must match sample seed, light ID, scene generation and
ray range. Inactive rays remain invalid/pending; every other active shadow ray
enters a deterministic prefix-scan compaction. Native any-hit traces only that
queue, over the original world segment, and scatters visibility to original ray
indices. Source overflow remains explicitly invalid. There is no count readback.

This is the hardware handoff for the existing screen tracer. Connecting its final
visibility to the new VSM receiver records depends on that cache being implemented;
it does not make the missing receiver/light sampling and accumulation passes exist.
Closest-hit depth-page rays cannot terminate at an arbitrary screen hit: an
offscreen blocker may be closer. Such hits can eventually bound the world query.

## Validation

Native graph tests cover scene-cut conversion followed by actual intersections,
stale ownership, uninstalled pages, overflow and unfinished traversal. Hard page
tests sample the actual published depth atlas, including directional/spot/point
projection, both depth conventions, misses, whole-page budgets and invalid-scene
retry. Screen-first tests span multiple scan blocks, confirmed/missing/stale hits,
inactive rays, repeated frames and overflow. They test the screen-result handoff
with seeded screen records; full scene-image screen/world parity remains open.
Out-of-segment screen hits (before the ray minimum or beyond the light) also
fall through to world tracing.
Terrain generation checks GPU handle records against completed install manifests.
VSM tests compare BVH/full-loop tile masks and page demand, including moving lights.

Validation completed on Apple M4: all 12 selected regression suites passed;
`SparseVoxelRendererSample` and `VirtualGeometryRenderer` release targets built,
and the release `SparseVoxelRendererSampleSmoke` passed. After the final
screen-range and direction-normalization changes, the native graph suite passed
again and the release screen-first shader rebuilt. These checks validate the
individual integrations described above, not the unfinished full receiver cache.

## Remaining complete-system work

- Complete caster cuts and camera/shadow streaming demand scheduling.
- Surface-identified sparse receiver records and coarse compatible receiver fallback.
- Area-emitter and sky sample generation, material resolve and cache memory budgets.
- Moving-light transform/radiance history, motion-aware weighted accumulation,
  effective sample counts, bounded temporal lag, discontinuity resets and audits.
  The user-approved policy is recorded in the implementation plan; it is not yet
  an implemented receiver accumulation path.
- Per-light mixed raster/RT routing, automatic capability/user controls and live UI.
- Full-renderer image comparisons, long motion/streaming replays and GPU budgets.
