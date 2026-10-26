# Virtual geometry: unified visibility buffer and two-pass occlusion

## Status

- **Two-pass occlusion: done.** See `VirtualGeometryRendererPass::Settings::twoPhaseOcclusion`
  and the sample's `--vg-two-phase-occlusion` (default on).
- **Visibility-buffer unification: not started.** The design and the blockers
  found while surveying the code are below.

## What was measured before the two-pass change

The chain was `preCulling(previous HZB) -> depthPrePass(depth only,
prepassResolutionDivisor = 2) -> depthPyramid -> finalCulling(new HZB) ->
hardwareDraw(everything)`. There was no persistent per-cluster visibility state
anywhere in the VG path: phase one approximated "visible last frame" by culling
against last frame's pyramid rather than remembering what it drew, and its
raster work was discarded. Every surviving cluster was rasterized twice - once
for a half-resolution depth buffer, once for the G-buffer - and the half
resolution existed to make that second rasterization affordable.

## Two-pass occlusion, as implemented

`VirtualGeometryScene::clusterVisibilityBuffer` holds two bits per decoded
cluster slot, indexed by the global cluster index
(`pageTable[page].clusterOffset + localCluster`), sixteen clusters per word:

- bit 0 - drawn last frame. Phase one draws exactly this set.
- bit 1 - drawn this frame. Phase two skips it.

`advanceClusterVisibility` runs once per frame ahead of phase one and does
`(w >> 1) & 0x55555555`: for every lane at once, drawn becomes last-frame and
drawn is cleared. Clearing the drawn bit every frame is what makes a stale bit
harmless - it can only add a phase-one draw, never make phase two skip something
that nothing drew.

Phase one is a full `VirtualGeometryHardwareDrawPass` into the real target with
occlusion culling off; the pyramid comes from that depth; phase two is the
existing hardware draw with `LoadOp_Load` on colour and depth. Both write the
same attachments, so the frame is rasterized once and phase one's pixels are
kept. `prepassResolutionDivisor` does not apply in this mode.

## Remaining work: one payload for both rasterizers

`VirtualGeometryComputeRasterPass` with `Backend::NativeMetalAtomic64` already
runs: `canRouteSoftwareClusters` passes on Apple, the culler routes
small-triangle clusters into the SW queue, and the native encoder fills
`visibilityKeys` (one u64 per pixel) and `resolvedPayloads`. But
`duplicateSoftwareRasterClustersToHardware` keeps every routed cluster in the
hardware queue as well, and nothing outside that pass reads either buffer, so
the compute raster's output is discarded. Hardware is still the only G-buffer
producer.

Steps, in dependency order:

1. **Hardware path writes the same payload.** Its current output is six MRT
   attachments: `packedGeometryIdsLo`, `packedGeometryIdsHi`, `materialId`,
   `materialUV`, `geometryNormal`, `motionVectors`. The compute path's is a
   single `PixelPayload{u32 payload, u32 depthBits}`. A unified payload is
   either at least 64 bits of IDs, or narrow IDs plus a resolve that
   reconstructs the rest.
2. **Merge pass.** Combine the hardware depth/ID output with the compute path's
   `visibilityKeys` by depth compare, into one visibility buffer.
3. **G-buffer resolve pass.** Expand cluster + triangle + barycentrics back into
   the current attachment set.

### The payload encoding (done)

One encoding, defined in three places that must stay in step:

- `virtualgeometry::packVisibilityPayload` in `VirtualGeometryData.hpp`
- `packVisibilityPayload` in `virtualgeometrydata.wgsl`
- `packVisibilityPayload` in
  `assets/shaders/voxel/svraster/metal/sparse_voxel_terrain_compute_raster_atomic64.metal`

```
bits [6:0]    triangle index within its cluster   (ClusterSize = 128)
bits [25:7]   global decoded cluster index        (MAX_DECODED_CLUSTERS = 524288)
```

26 bits used, 6 spare, `static_assert`ed against `ClusterSize` and
`MAX_DECODED_CLUSTERS`. It goes in the low half of the same 64-bit key the
software rasterizer already writes, with depth in the high half.

The global cluster index is the right thing to carry rather than an index into a
per-frame visible list: the two rasterizers keep separate visible lists, so a
list index would not mean the same thing in both, while the global index does -
and a resolve reaches geometry straight through
`decodedClusterGeometry[globalCluster]`.

The instance index is deliberately not in the payload. A resolve recovers it
from the cluster's owning object, which is exact while one instance draws each
object (the terrain case). Geometry drawn by several instances at once needs the
spare bits or a wider key.

**The software rasterizer now emits this.** What it emitted before:

### The software payload could not round-trip (fixed)

`packDecodedVGPayload` in
`assets/shaders/voxel/svraster/metal/sparse_voxel_terrain_compute_raster_atomic64.metal`
packs the whole thing into the low 32 bits of the visibility key:

```
clusterSlot & 0xfff  << 20   (4096)
pageIndex   & 0xff   << 12   (256)
localCluster& 0x3f   <<  6   (64)
triangle    & 0x3f           (64)
```

Against the real ranges - `MAX_DECODED_CLUSTERS` 524288, `kMaxScenePages`
65536, `ClusterSize` 128 triangles - every field is truncated. It is a
bring-up encoding, not something a resolve pass could expand. So
`duplicateSoftwareRasterClustersToHardware = false` cannot be flipped on its
own: the routed clusters would simply be missing from the G-buffer.

### Remaining: the hardware path has to emit it too

Two routes were evaluated; both have real blast radius, so this is a decision
rather than a discovery.

**A. A new R32Uint colour attachment.** The vertex shader already computes
`globalClusterIndex(entry, localMeshletIdx)`, so the shader change is small: one
extra flat interpolant and one extra `FragmentOutput` location. The cost is the
variant matrix - shader variants are precompiled SPIR-V selected by filename
(`virtualgeometry-meshlet-fs-motion.spirv` and friends), so a payload variant
multiplies against `OUTPUT_MOTION_VECTORS` and `DRAW_INDIRECT_COUNT_DISABLED`,
and every `FrameTarget` construction site needs the new view.

**B. Repurpose `packedGeometryIdsHi` as the payload.** No new attachment, no new
variant. `packGeometryIdsLo/Hi` currently spread
(instance 30 | page 18 | group 6 | clusterInGroup 3 | triangle 7) across two
words; redefining them as `Lo = instanceIndex`, `Hi = visibility payload` is
lossless and strictly better packed. The cost is migrating every consumer that
walks page/group/clusterInGroup today, to go through
`decodedClusterGeometryInfos[globalCluster]` instead:

- `sparse_voxel_vg_gbuffer_adapter.wgsl`
- `virtualgeometry-taa-common.wgsl` (and the resolve/history passes on it)
- `virtualgeometry-id-readback-cs.wgsl`
- `renderToQuadSelectedDebug-fs.wgsl`
- `MaterialPass`, `SurfelScreenSpaceSpawnPass`

B is the better end state - it removes a second, redundant identity encoding
rather than adding a third - but it is a wider change and every one of those
consumers is a place the screen can go wrong quietly.

### Two-pass occlusion interaction

Phase one runs with `enableSoftwareRaster = false`, so it routes everything to
hardware and marks it drawn; phase two then skips those clusters. That is
correct while software raster is inert, but once it is authoritative a
small-triangle cluster visible last frame would be drawn by hardware in phase
one and never reach the software path. Phase one has to route the same way phase
two does, and the software raster and merge then have to run once per phase -
before the pyramid for phase one - or the pyramid is built from hardware depth
only.

### Blockers to plan around

- **Motion vectors and normals are rasterizer-interpolated today.** The resolve
  pass has to recompute them from the cluster/triangle and barycentrics. The
  scene already stores `previousModelMatrix` per instance, so the data exists;
  this is the bulk of step 3.
- **Every current consumer of `packedGeometryIds*` moves behind the resolve**,
  including the sparse-voxel G-buffer adapter and the material pass.
- Only phase one's attachments can be cleared; with two-pass occlusion the merge
  and resolve run once, after phase two.

### Trap worth remembering

A newly added child draw pass renders nothing until its per-frame uniforms are
forwarded. The sample updates `getDepthPrePass()` and `getHardwareDrawPass()`
uniforms by hand; the new phase-one pass needed `getPhaseOneDrawPass()` and an
`appendFrameOverrides` entry, and without them it rasterized with an identity
transform and produced no pixels. The symptom was erratic coverage, which reads
like a frames-in-flight race or a lost `LoadOp_Load`.

Separately, any buffer written with `renderGraph->bufferWrite` needs
`BufferUsage_Push`, or MoltenVK fails it with `VK_ERROR_MEMORY_MAP_FAILED` and
the initial contents are whatever the allocation held.
