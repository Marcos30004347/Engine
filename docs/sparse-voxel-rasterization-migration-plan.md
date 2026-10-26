# Sparse voxel rasterization migration plan

## Outcome

Replace the voxel sample's brick-shell and ray-tracing render paths with an Engine-native implementation of the core SVRaster algorithm:

1. Refactor the renderer around a minimal SVRaster-style resident model and a visibility-output contract, initially containing exactly one hand-authored instance.
2. Render a deterministic 128 x 128 x 128 level-7 volume containing only its exterior boundary voxels, without reading a scene file.
3. Complete projection, duplication, scan, 64-bit sort, tile ranges, solid nearest-surface resolve, full Engine G-buffer commit, and debug presentation while a bootstrap visibility pass emits the single instance directly.
4. At the end of milestone 1, replace that bootstrap producer with the real one-instance scene/TLAS frustum-culling passes without changing any downstream raster pass or buffer ABI.
5. Only after the synthetic renderer and every pass test are correct, add the versioned scene-file generator/deserializer, dependency-safe spatial page streaming, and an ImGui OBJ authoring workflow that measures source geometry in meters before voxelization, previews the result, and saves `.vsvo` only when explicitly requested.
6. After file-backed multi-instance rendering is stable, add a reusable depth-only path, the repository's fast SPD HZB, and conservative two-phase TLAS occlusion culling.
7. Finally, derive a camera-centered real-time coarse occupancy volume from the resident scene. Store one occupancy bit per logical volume voxel in a packed `R32Uint` 3D texture, scroll it toroidally so camera motion regenerates only newly exposed sections, and support interior dirty regions for transformed, added, removed, or residency-changed instances.
8. After the single-resolution volume is complete, derive a globally anchored packed occupancy pyramid without shifting retained bits, then use it for conservative hierarchical empty-space skipping in a sparse DDA consumer.

No voxel asset is regenerated automatically or required to be checked into the repository. The editor opens ordinary OBJ sources, keeps preview builds transient, and writes only the `.vsvo` files the user explicitly chooses through Save. Everything specific to EVXL—including the old `.evxl` asset, readers/writers, cache path, tests, documentation, data structures, voxel ray-tracing implementations, and serialized acceleration data—is deleted as soon as the synthetic renderer cutover is green. There is no compatibility fallback, conversion adapter, dual-format probe, migration tool, or shared runtime ABI.

This is a renderer migration, not an integration of SVRaster's PyTorch training stack.

The work is divided into five required milestones:

1. **Synthetic renderer:** first render one generated 128-cubed exterior shell through a hand-authored visible-instance producer; then add the real one-instance scene/TLAS culling producer at the end. No file I/O participates in this milestone.
2. **Scene files and selective residency:** generate the new multi-model format, open metadata and root pages first, stream spatially prioritized dependency-closed fine pages into bounded physical storage, and feed the unchanged preprocess-through-G-buffer contracts.
3. **Depth-only and occlusion:** expose the material-free depth renderer, build a fast hierarchical depth buffer, then add conservative previous/current-HZB TLAS occlusion without changing the BLAS representation or VSVO format.
4. **Camera-centered occupancy volume:** voxel-splat an approximate, material-free bit volume from the resident TLAS/BLAS scene, expose exact camera-cell/sub-cell addressing, and incrementally refresh the union of newly exposed toroidal slabs and explicitly invalidated interior world-space regions.
5. **Hierarchical occupancy and sparse DDA:** OR-reduce the committed base volume into independently toroidal, globally anchored packed levels, propagate only touched outgoing/incoming regions upward, and add a conservative hierarchical DDA that uses the pyramid for empty-space skipping.

The implementation order is intentionally renderer-first. File-format work must not block or obscure GPU pass bring-up, and renderer correctness must not initially depend on the current importer, virtual-scene builder, or legacy `.evxl` reader.

“SVRaster” in these milestones means its sparse adaptive octree representation, conservative projection, tile duplication, direction-dependent local Morton ordering, indirect sort/range construction, and per-pixel analytic voxel-box test. The reference volumetric density integration is intentionally not part of the first renderer: the final tile pass resolves an opaque solid voxel surface.

## Compatibility and fidelity rules

- Treat this as a replacement subsystem. New code must not include or translate `VoxelSceneCPU`, `VoxelCullingScene`, brick/BVH8/CWBVH records, `VoxelVirtualScene`, `EVXL`, or `EVX2` runtime types.
- Use the new `.vsvo` (sparse voxel octree) extension and the Engine-specific `VSVO` magic. Delete the old checked-in `.evxl` and every EVXL implementation/test/documentation path at the milestone-1 synthetic cutover; do not wait for the new asset and do not teach the new loader how to identify, reject specially, upgrade, or partially read old payloads. The loader must trust the magic and schema, not the generic extension, and must not claim compatibility with unrelated `.vsvo` formats.
- Match SVRaster's resident voxel inputs and pass boundaries wherever the TLAS and solid-output requirements do not force a difference: parallel path/center/length/appearance arrays, 16 x 16 tiles, packed pixel bounds, camera quadrant bitsets, duplicate counts, exclusive scan, 64-bit tile/Morton keys, stable radix sort, tile ranges, and per-pixel candidate filtering/box intersection.
- The only intentional algorithm extensions are the instance TLAS/transforms, an instance ID in candidate payloads, conservative near-plane handling required by the Engine camera convention, and opaque nearest-surface/G-buffer resolution instead of density integration.
- Milestone 4 is a downstream derived-data feature, not an SVRaster algorithm change. Enabling or disabling its update graph must not alter visibility, candidate ordering, resolved surfaces, depth, or G-buffer output.
- Milestone 5 is an optional consumer hierarchy over milestone 4. It must preserve the same renderer isolation, must never introduce a false-empty ancestor, and must not reinterpret conservative coarse occupancy as exact source-voxel topology.
- No voxel CWBVH is built, serialized, uploaded, bound, or retained for debugging. The new TLAS is a compact culling hierarchy only; it has no ray traversal representation and no dependency on `softwarebvh`.

## Reference checkout and constraints

The reference implementation is cloned at:

```text
ignore/svraster
remote: https://github.com/NVlabs/svraster.git
commit: 3503131b5558f5e5eaaab01881129a5f3bd79ad3
```

The checkout is intentionally under the git-ignored `ignore/` directory. It is an algorithm and validation reference, not a dependency of the Engine build.

Relevant reference files are:

- `cuda/src/preprocess.cu`: projected bounds, camera-quadrant coverage, and tile duplication counts.
- `cuda/src/auxiliary.h`: direction-dependent Morton rank and packed tile/order keys.
- `cuda/src/forward.cu`: tile range construction and per-pixel voxel intersection/depth behavior. Its density integration and alpha compositing are future references, not version-1 renderer requirements.
- `src/sparse_voxel_gears/io.py`: checkpoint fields and quantization behavior.
- `src/utils/octree_utils.py`: 48-bit octree paths, levels, centers, extents, and shared corner-grid indexing.

Milestone-3 Engine implementation references are:

- `assets/shaders/hzb/wgsl/depth_pyramid_spd_source_depth.wgsl` and `depth_pyramid_spd.wgsl`: the fast chunked SPD depth reduction, including reverse-Z `min` versus forward-Z `max` conservative reduction.
- `src/runtime/virtualgeometry/rendering/VirtualGeometryDepthPyramidPass.hpp`: native-depth input, same-size mip-0 skip convention, 4-to-8-mip shader variants, multi-dispatch continuation, counter reset, and RenderGraph barriers.
- `assets/shaders/virtualgeometry/wgsl/culling_multipass.wgsl`: conservative projected bounds, near-plane fallback, HZB mip choice/bias, 2 x 2 sampling, depth conversion, and occlusion debug reasons.
- `tests/virtualgeometry/VirtualGeometryCullingMultipleDispatchesPassTests.cpp` and its CMake file: executable HZB/culling fixtures and shader variants.
- The current voxel renderer is a behavioral reference only for temporal/current HZB cull ordering and invalid-history handling. Its depth-prepass rendering structure, brick/BVH records, and old shaders are not carried forward: G13 deletes them, and milestone 3 uses full normal rendering after each accepted cull batch.

Milestone-4 Engine implementation references are:

- `src/runtime/gpusigneddistancefields/GPUSignedDistanceFieldsManager.cpp`, `assets/shaders/gpusigneddistancefields/wgsl/gpusigneddistancefields_common.wgsl`, and `gpusigneddistancefields-build.wgsl`: quantized camera-centered grids, toroidal logical-to-physical offsets, and newly exposed slab invalidation.
- `tests/virtualgeometry/GPUSignedDistanceFieldsBuildPassKernelTests.cpp`: scroll, wrap, initialization, large-jump, resize, and stale-slot fixtures that can be adapted to the new bit-packed occupancy contract.
- The old `gpusparsevoxel` cache tests may be consulted only as behavioral evidence before G13 removes that subsystem. Milestone 4 must live on the new sparse-voxel scene contract and must not restore a dependency on legacy cascade, brick, trace, or occupancy types.

Do not copy the CUDA or PyTorch source into Engine. The reference license limits the work and derivatives to non-commercial research/evaluation use. Implement the described algorithm independently in C++/WGSL, retain a design attribution in the renderer documentation, and have the repository owner confirm the project's licensing posture before any distribution. If direct source reuse is later approved, vendor the license and notices explicitly; do not silently move the ignored checkout into `thirdparty/`.

## Current state

The current checked-in asset is `assets/meshes/obj/sponza.evxl`:

- 28,463,462 bytes.
- `EVXL` magic, file version 2.
- 8 x 8 x 8 bricks.
- 731 instance records.
- Instance payloads contain brick transforms, local octrees, and dense `VoxelBrickVoxel` arrays.
- The culling payload also stores ray-specific CWBVH nodes, leaf references, brick micro-trees, macro-grid words, and trace LODs.

At runtime, `samples/voxelRenderer/VoxelRenderer.cpp` can read both the legacy `EVXL` file and the newer `EVX2` virtual file. A legacy asset is expanded into `VoxelSceneCPU`, converted to a virtual scene, flattened for the GPU, culled to visible bricks, and rendered through a mixture of:

- Rasterized brick shells whose fragment shader performs a camera-ray DDA through 4 x 4 x 4 cells.
- A CWBVH primary-ray path.
- A GPU sparse-cascade primary-ray path.
- CWBVH, sparse-cascade, or random-sky GI paths.
- Global-octree update/free/splat scaffolding for screen and fallback tracing.

The migration should not extend either current file layout. A clean format boundary makes stale assets fail immediately and lets all ray acceleration data disappear from the authoring and loading paths.

Baseline captured before this migration: `VoxelCullingTests` and `VoxelVirtualSceneFileTests` pass. `VoxelVirtualBuilderTests` already fails the replay assertion at `tests/voxel/VoxelVirtualBuilderTests.cpp:495` after a rebuild; track that separately so it is not misattributed to the SVRaster changes, and do not weaken or delete it merely to make the migration green.

## Scope decisions

### Milestone 1 bootstrap limit

- Exactly one BLAS/model and one instance. The instance starts as a hand-authored identity transform; a fixed non-identity transform may be used only after the identity reference-parity path is green.
- The main interactive dataset is a uniform 128 x 128 x 128 grid represented by level-7 octree paths.
- Emit only boundary cells: a cell `(x, y, z)` is occupied when any coordinate is `0` or `127`. The exact count is `128^3 - 126^3 = 96,776` solid voxels; no interior voxel is emitted.
- Give every emitted voxel deterministic per-voxel material data derived from `(octreePath, materialSeed)`: albedo, roughness, metallic, and emissive radiance. Use an explicitly specified integer hash, not `std::hash`, implementation-defined RNG distributions, or generation order. All paths, levels, centers/lengths, and material values are generated directly in the new parallel-array format.
- Small single-voxel, tile-edge, quadrant, and depth-stack fixtures remain available to focused pass tests, but the sample does not need multiple models, multiple instances, adaptive mixed levels, a file, or a real TLAS to bring up the main raster passes.
- A bootstrap visibility compute pass writes the same visible-instance record, visible-voxel count, and indirect preprocess arguments that the final culling path will produce. Raster passes cannot inspect a `bootstrap` flag or bind a different scene layout.
- Implement the real `SparseVoxelScene`, one-instance TLAS, and frustum culling only after preprocess/bin/sort/range/resolve/G-buffer/debug-present are working. Replacing the bootstrap producer must leave every downstream pipeline and test fixture unchanged.

### Required for the milestone-1/2 renderer and file cutover

- A deterministic in-memory random/synthetic sparse-voxel scene generator used by the sample and tests during renderer bring-up.
- A file-independent `SparseVoxelScene` runtime contract shared by generated and deserialized scenes.
- Any static OBJ-authored scene selected by the user.
- One local-space, axis-aligned root cube per unique BLAS/model.
- One scene TLAS whose leaves reference model instances. Each instance stores a BLAS index and a different invertible model transform; many instances may share one BLAS.
- Conservative world-space instance and TLAS-node AABBs derived from the transformed BLAS root cube.
- GPU TLAS frustum culling and compaction before any BLAS leaf raster work is emitted.
- GPU-generated indirect dispatch for visible instances and their BLAS leaves; there is no CPU visible-instance readback.
- Adaptive sparse leaves at levels 1 through 16.
- No ancestor/descendant overlap in the active render set.
- Solid source occupancy: every emitted VSVO leaf is an opaque, filled local-space box for milestones 1-3. Milestone 4 derives a deliberately coarser binary approximation from those boxes; it stores no material or exact source-voxel identity.
- Per-leaf material appearance for generated and imported OBJ assets: sRGB8 albedo, UNORM8 roughness/metallic, linear HDR emissive RGB, and supported material flags. Alpha is not a material input in VSVO version 1; a hit is fully covered.
- LZ4-compressed, chunked, little-endian runtime files.
- Metadata-first loading, pinned root pages, and bounded GPU residency. Finer pages may be requested in spatial-priority order, but a page becomes visible only after all declared dependencies are resident.
- Full milestone-1 Engine G-buffer output: base color, geometry normal, shading normal, material properties, emissive radiance, motion vectors, hardware depth, stable voxel/instance identity, and binary coverage/debug linear depth.
- Vulkan/WGSL compute raster pipeline plus a fullscreen graphics G-buffer/depth commit integrated with `RenderGraph`.
- An editor authoring workflow that never writes until the user explicitly chooses a `.vsvo` destination.

### Deferred, with file-format room reserved

- Learned spherical-harmonic appearance.
- Importing SVRaster `.pt` checkpoints.
- Differentiable rendering and backward passes.
- Runtime editing of voxel topology.
- Volumetric density fields, translucent voxels, trilinear extinction integration, and transmittance output. Reserve format chunk/type IDs for them, but do not include density payloads in VSVO version 1.
- Incremental TLAS refit/rebuild for frequently moving instances. Late milestone 1 builds its one-instance TLAS once for the fixed transform; its GPU layout must not require a renderer rewrite to add refit later.
- TLAS occlusion culling and the depth-only/HZB schedule are intentionally deferred to milestone 3. Milestones 1 and 2 enable frustum culling only, but their TLAS traversal layout, culling flags, current/previous transforms, and G-buffer depth convention reserve the required hook.
- The camera-centered packed occupancy texture is intentionally deferred to milestone 4. It is derived runtime state, is never serialized into VSVO, and is not a prerequisite for renderer, file, depth-only, HZB, or occlusion correctness.
- Median-depth and volumetric-depth statistics beyond the required nearest hardware/linear surface depth.

The TLAS is a scene hierarchy, not one TLAS per model. A TLAS leaf identifies a model instance; the instance points to its model's sparse-voxel BLAS and supplies `modelToWorld`/`worldToModel`. This interpretation permits repeated models with different transforms without duplicating BLAS leaves.

Direction-dependent Morton ordering remains valid inside each local, axis-aligned BLAS after the camera ray is transformed into that BLAS's local space. It is not a complete global order across independently transformed instances. To stay close to SVRaster, keep its exact 64-bit tile/local-Morton key and place the instance ID in the payload. The solid resolver scans the full tile range and chooses the nearest exact forward transformed-box surface, so it remains correct even though candidates from different instances are not globally ordered. A single identity-instance test must match SVRaster's order exactly.

### Meaning of “remove ray tracing”

Remove all scene traversal backends, user-selectable ray modes, ray queues, BVHs built for tracing, GI rays, screen-ray passes, global-octree fallback tracing, trace statistics, and trace-only serialization.

The solid resolve pass still reconstructs a camera direction for each pixel and computes an analytic segment through each already binned voxel. That local ray/AABB segment is part of raster coverage/depth resolution; it is not a scene ray-tracing method and does not traverse a BVH, octree, or cascade. Name it `voxelSegmentForPixel` in the new shader rather than exposing another “trace” path.

## Target data model

Add a distinct model under `src/voxel/svraster/` so old brick types cannot leak into the new runtime accidentally.

```cpp
struct SparseVoxelBLAS {
  float rootCenter[3];
  float rootExtent;
  uint32_t maxLevel;
  uint32_t flags;
  // SVRaster-style structure-of-arrays; every array has voxelCount entries.
  std::vector<uint64_t> octreePaths; // low 48 bits used
  std::vector<uint8_t> octreeLevels;
  std::vector<uint32_t> albedosRGBA8;    // per-voxel sRGB reflectance, canonical A=255
  std::vector<uint8_t> roughness;
  std::vector<uint8_t> metallic;
  std::vector<uint32_t> emissiveRG16F;   // two linear half-floats
  std::vector<uint32_t> emissiveB16F;    // B linear half-float; upper 16 bits zero
  std::vector<uint32_t> materialIds;
  std::vector<uint32_t> materialFlags;
  std::vector<uint32_t> sourceIds;   // authoring/debug only; not uploaded for rendering
};

struct SparseVoxelInstance {
  uint32_t blasIndex;
  uint32_t flags;
  glm::mat4 modelToWorld;
  glm::mat4 worldToModel;
  glm::mat4 previousModelToWorld; // runtime temporal state; not serialized
  AABB worldBounds;
};

struct SparseVoxelTLASNode {
  AABB worldBounds;
  uint32_t firstChild;
  uint32_t childCount;
  uint32_t firstInstance;
  uint32_t instanceCount;
};

struct SparseVoxelScene {
  std::vector<SparseVoxelBLAS> models;
  std::vector<SparseVoxelInstance> instances;
  std::vector<uint32_t> tlasInstanceIndices;
  std::vector<SparseVoxelTLASNode> tlasNodes;
  uint32_t tlasRootNode;
  uint32_t tlasMaxDepth;
};
```

`SparseVoxelScene` is the only renderer input. The synthetic generator and the future deserializer both produce it, and both call `validateSparseVoxelScene` before upload. `worldToModel` is validated as the inverse of a finite, non-singular affine `modelToWorld`; the initial implementation supports translation, rotation, non-uniform scale, and reflection. Shear may be accepted if its CPU bounds and shader segment tests pass the same validation suite.

The BLAS intentionally uses parallel arrays like SVRaster's `octree_paths`, `vox_centers`, `vox_lengths`, and appearance inputs instead of preserving Engine leaf/brick records. CPU construction decodes local center and side length from `(rootCenter, rootExtent, octreePath, level)` once. Upload produces resident parallel GPU arrays for path/level, `vec4(center, length)`, and material. These are direct inputs to preprocess/resolve; there is no legacy flattening step, attribute indirection, or per-frame CPU expansion.

### Milestone-1 voxel material and G-buffer contract

Use the Engine's existing `MaterialGBufferOutput` meaning and frame-target attachments rather than a voxel-only color buffer. Treat `assets/shaders/material/wgsl/material-gbuffer-types.wgsl`, `material_store_gbuffer_outputs` in `assets/shaders/virtualgeometry/wgsl/material.wgsl`, and `VirtualGeometryHardwareDrawPass::FrameTarget` as the current channel/format source of truth; add shared helpers if needed so the definitions cannot drift. Albedo is the only stored base-color payload:

```text
baseColor = srgbToLinear(albedo.rgb)
opacity   = 1
```

Do not add a second generic color/tint field to the CPU model, GPU ABI, or VSVO chunks. If an authoring source has vertex color or a base-color factor in addition to texture/material albedo, combine them in linear space during offline voxelization and encode the resulting value once as voxel albedo. The synthetic shell's requested random per-voxel color is therefore generated directly as random albedo.

Each solid voxel also stores its own roughness, metallic value, linear HDR emissive RGB, material ID, and supported flags. Roughness and metallic decode from UNORM8. Emissive uses three IEEE-754 half-floats packed into two words in memory and on disk; the unused high half of the second word is zero. Reject NaN, Inf, negative emission, non-opaque albedo alpha, nonzero reserved bits, and unsupported flags. VSVO version 1 has no texture references, alpha mask, translucency, density, or normal map.

The exact box intersection returns the first forward surface, not only a boolean and distance:

```text
outside ray: surfaceT = tEnter, face = entering slab
inside ray:  surfaceT = tExit,  face = exiting slab
```

The selected slab supplies a local axis-aligned outward geometric normal. Transform it with the inverse-transpose of `modelToWorld` and normalize it; this is both geometry and shading normal in version 1. This rule is required for rotated, reflected, and non-uniformly scaled instances and gives camera-inside hits a defined normal. Normal maps and smoothed normals are deferred.

`ResolveSolidVoxels` writes a compact `ResolvedVoxelSurface` image/buffer containing hit validity, instance/voxel IDs, world-ray surface distance, and the selected local box-face axis/sign. It does not duplicate resident material into a large per-pixel buffer and does not write a prelit final color. `WriteVoxelGBuffer`, a fullscreen graphics pass, discards misses, reconstructs the world position, transforms the face normal, fetches/decodes the winning voxel's material, and writes the existing Engine frame target:

- `baseColorTexture`: decoded linear albedo and opacity 1;
- `geometryNormalTexture`: normalized world box-face normal;
- `normalTexture`: the same world normal plus roughness in alpha, matching the existing material contract;
- `materialPropertiesTexture`: roughness, metallic, opacity 1, and flags;
- `emissiveTexture`: linear HDR emissive RGB;
- `motionVectorTexture`: current minus previous UV from the resolved world/local point, current/previous camera matrices, and current/previous instance transform; zero only when previous-frame state is invalid;
- `depthTexture`: device depth derived from the selected world surface and written through fragment depth using the Engine depth convention;
- identity/material targets: stable instance and global voxel IDs plus the per-voxel material ID; material UV is defined as `(0, 0)` because version 1 has no textures;
- debug targets: binary coverage and optional linear world-space surface distance.

`DebugPresent` may visualize any G-buffer attachment but is not the material result. The milestone-1 renderer is complete only when downstream consumers can use the voxel G-buffer without a voxel-specific lighting path.

### TLAS construction

Build a flattened, breadth-first TLAS over instance `worldBounds`, inspired by the existing virtual-geometry hierarchy and its queue discipline:

- Use `assets/shaders/virtualgeometry/wgsl/culling_multipass.wgsl` as the Engine behavior reference for root setup, prepare-indirect, queue ping-pong/local spill, frustum conventions, optional HZB ordering, and debug rejection reasons. The TLAS traversal itself does not inherit virtual-geometry page, projected-error, mesh-part, or cluster selection data; milestone 2's separate BLAS residency subsystem adopts only the explicitly documented request/rewrite lifecycle.
- Use a new compact TLAS record rather than `VirtualGeometryHierarchy` from `src/runtime/virtualgeometry/VirtualGeometryData.hpp`; matching the traversal pattern does not require coupling the voxel renderer to virtual-geometry asset data.
- Internal nodes have a contiguous child range; leaf nodes have a contiguous range in `tlasInstanceIndices`.
- Use 4- or 8-way nodes, chosen once after measuring build quality and traversal occupancy. Do not reuse `VirtualGeometryHierarchy` directly because its LOD/page fields are irrelevant.
- A leaf may contain a small number of instances. When a leaf is visible, test each instance AABB before setting its visibility flag.
- Late milestone 1 builds the trivial one-instance TLAS on the CPU and uploads it only after raster bring-up. Static scene files use the same builder after load; multi-instance rebuild/refit policy is generalized later unless profiling justifies serialized TLAS nodes.
- Record `tlasMaxDepth`. RenderGraph records a bounded prepare/traverse pair for every depth, allowing indirect zero-work dispatches without CPU readback.
- Reserve `cullingFlags`, previous-HZB bindings, and debug rejection reasons compatible with the current virtual-geometry culling style. Frustum rejection is enabled in milestones 1/2; occlusion rejection remains disabled until milestone-3 gates G19-G22 are implemented and tested.

`SparseVoxelSceneManager` is the runtime owner of this derived state. Authored or deserialized `SparseVoxelScene` data remains immutable input; the manager owns stable instance IDs, current/previous transforms, inverse transforms, transformed BLAS-root bounds, scene/topology revisions, GPU instance records, the breadth-first TLAS, and its instance-index indirection. Topology changes rebuild the TLAS. Transform-only changes initially rebuild deterministically; a bounds-only bottom-up refit may replace that path after profiling without changing the GPU ABI or visible-instance ordering.

Frustum rejection is hierarchical in milestones 1/2:

1. TLAS internal-node world AABBs provide the broad phase.
2. Every accepted TLAS leaf tests each referenced instance AABB before setting its visibility bit.
3. The resident BLAS traversal tests each octree node's transformed cube before projected-error evaluation, refinement requests, child enqueue, or selected-node emission.

All three tests use the same conservative clip-space helper and near-plane uncertainty policy. Visibility is materialized as flags, then prefix-scanned and scattered in ascending stable instance ID; atomic append order is not an externally visible ordering contract. BLAS-node rejection may reduce LOD queues, page requests, projected voxels, and candidates, but disabling it must produce byte-identical final resolved surfaces and G-buffer output.

The same three-level bounds contract is reserved for milestone-3 occlusion. Previous-HZB pre-culling initially descends TLAS internal nodes unless validated previous bounds exist, tests leaf instances with previous transforms, and projects BLAS nodes with the previous instance transform. Current-HZB recovery may reject current TLAS internal nodes, instances, and BLAS nodes. Because an accepted instance can contain both submitted and previously occluded BLAS branches, recovery tracks submitted selected-node/logical-node IDs in addition to instance IDs; instance-only submission flags are insufficient.

### Initial GPU ABI

Mirror every record in C++ and WGSL and add `sizeof`, `alignof`, member-offset, and shader-reflection tests. Prefer explicit 16-byte groups over implicit packing.

| Record | Size | Fields |
| --- | ---: | --- |
| `SparseVoxelBLASGPU` | 32 B | `vec4<f32> rootCenterExtent`; root-page ID, page count, maximum level, and flags. Milestone 1 represents its generated hierarchy as one always-resident synthetic page; no dense global voxel base is required by milestone 2. |
| `SparseVoxelLODNodeGPU` | 16 B | Physical `firstChild`, resident `childCount`, physical voxel/material index, and level. A nonresident refinement always leaves `childCount = 0`. |
| `SparseVoxelResidentNodeMetadataGPU` | 16 B | Stable logical node ID, refinement-page ID or invalid, owning resident-page ID, and flags. Added in milestone 2 and indexed by physical node slot. |
| `SparseVoxelPageMetadataGPU` | 48 B | Conservative local AABB plus logical node range, physical base or invalid, level, and residency flags. Added in milestone 2; compressed payload offsets remain CPU-only. |
| `SparseVoxelPageRequestGPU` | 16 B | Page ID, quantized projected-error priority, requesting instance ID used for deterministic reduction/debug, and flags. Added in milestone 2. |
| `SparseVoxelPathLevelGPU` | 16 B | `pathLo`, `pathHi`, `level`, `flags`; only the low 48 path bits are valid. |
| `SparseVoxelCenterLengthGPU` | 16 B | local `vec3<f32> center` and full side `length`, matching SVRaster preprocess inputs. |
| `SparseVoxelMaterialGPU` | 32 B | Two explicit 16-byte groups: `albedoRGBA8`, packed UNORM8 roughness/metallic, `emissiveRG16F`, `emissiveB16F`; then `materialId`, supported flags, and two zeroed reserved words. A voxel is implicitly solid/opaque in version 1. |
| `SparseVoxelInstanceGPU` | 240 B | `mat4x4<f32> modelToWorld`; `mat4x4<f32> worldToModel`; `mat4x4<f32> previousModelToWorld`; `vec4<f32> worldBoundsMin`; `vec4<f32> worldBoundsMax`; `blasIndex`, `flags`, two reserved words. |
| `SparseVoxelTLASNodeGPU` | 48 B | `vec4<f32> worldBoundsMin`; `vec4<f32> worldBoundsMax`; `firstChild`, `childCount`, `firstInstance`, `instanceCount`. |
| `TLASQueueElementGPU` | 16 B | `nodeIndex`, `depth`, and two reserved words. |
| `VisibleInstanceGPU` | 16 B | Instance index, flags, and two reserved words. Resident LOD selection, rather than a dense BLAS range, produces selected-node offsets and preprocess work. |
| `ProjectedVoxelGPU` | 32 B | SVRaster-style packed inclusive pixel bounds (`vec2<u32>`), quadrant bitset, duplicate count, instance index, stable logical voxel ID, physical node/material index, and one reserved word. |
| `TileCandidateGPU` | 32 B | 64-bit `(tileId << 48) | directionDependentMortonRank` key; `instanceIndex`, global voxel index, quadrant, projected-record index, and two reserved words. |
| `ResolvedVoxelSurfaceGPU` | 32 B | `surfaceT`, instance index, stable logical voxel ID, packed local face axis/sign, hit flags, physical node/material index, and two reserved words. Miss records use invalid IDs and zero reserved words. |
| `IndirectDispatchArgsGPU` | 16 B | `x`, `y`, `z`, reserved. Use separate slots for TLAS, preprocess, duplicate, and sort/range stages so one pass cannot overwrite another pass's arguments. |

The first ABI deliberately mirrors SVRaster's parallel geometry arrays and 64-bit key. Profile instance matrix packing separately; do not change voxel layout merely to resemble the removed Engine brick formats.

### Data invariants

Authoring invariants:

- A scene may be completely empty. Otherwise every BLAS has at least one voxel and any nonzero instance set has a valid TLAS root.
- Every instance references a valid BLAS, has finite inverse transforms, and has conservative world bounds enclosing all eight transformed BLAS-root corners.
- Every TLAS child/range is in bounds, the graph is acyclic, every instance index occurs exactly once, and parent bounds contain child/instance bounds.
- `1 <= level <= 16`.
- Path bits below the voxel level are zero.
- `(path, level)` pairs are unique.
- Every authored hierarchy `(path, level)` pair is unique. Ancestor/descendant pairs are required across LOD levels, but each dependency-closed resident page set and every renderer-selected cut contains exactly one active node along a root-to-leaf path.
- Every BLAS render array (`octreePaths`, `octreeLevels`, albedo, roughness, metallic, emissive, material ID, and `materialFlags`) has the same length; `sourceIds` is either empty or has that length.
- Each BLAS root cube contains every decoded voxel belonging to that BLAS.
- Every version-1 albedo has opaque alpha; roughness/metallic are canonical UNORM8; emissive is finite, non-negative and canonically packed; reserved bits are zero; every material flag is supported.
- Voxels are stored in canonical `(level, path)` order within each BLAS, and BLASes/instances are stored in canonical source order for deterministic files.

## Renderer-first bootstrap data

Do not begin milestone 1 by building the full scene or TLAS. First create a `BootstrapSparseVoxelModel` containing one BLAS descriptor, the parallel voxel arrays, and one `SparseVoxelInstanceGPU`. It uses the same resident GPU records later produced from `SparseVoxelScene`, but it is constructed directly and has no scene hierarchy.

Add a deterministic `generateExteriorShell128` helper under `src/voxel/svraster/testing/` or `tests/voxel/support/`, with a small reusable core callable by the sample. Enumerate boundary coordinates in the 128-cubed integer grid, encode each as a level-7 path, derive all material channels from fixed-width hashes of `(path XOR materialSeed)`, then sort every parallel array by path. Emit a voxel only when `x`, `y`, or `z` is on the boundary. The result must contain exactly 96,776 path/level/material entries and their decoded center/full-length upload records.

Use a fixed default seed checked into the generator and independent SplitMix64 lanes for albedo, scalar material properties/ID, and emission. Map albedo hash bytes to sRGB while reserving a small lower bound such as 32 and force alpha to 255. Map roughness to a documented nonzero range such as `[0.08, 1]`, metallic to `[0, 1]`, derive a bounded material ID, and make a documented fixed fraction of voxels emissive with finite non-negative half-float RGB so both emissive and non-emissive paths are exercised. The same path/seed must always produce the same complete material regardless of enumeration order, standard library, platform, or build configuration. A development CLI option such as `--generated-material-seed` may override the seed without changing geometry.

Specify the hash and byte mapping as part of the test contract, for example:

```text
hAlbedo  = splitmix64(path XOR materialSeed)
hProps   = splitmix64(hAlbedo)
hEmission = splitmix64(hProps)
channel(byte) = 32 + ((byte * 224) >> 8)
albedoRGBA8    = rgb8(hAlbedo, channel) | 255 << 24
roughness8     = map_unorm8_to_documented_range(hProps[7:0])
metallic8      = hProps[15:8]
materialId     = hProps[31:16]
emissiveRGB16F = canonical_nonnegative_emission(hEmission)
```

Define `splitmix64(v)` with the exact add/xor/shift/multiply sequence and constants `0x9E3779B97F4A7C15`, `0xBF58476D1CE4E5B9`, and `0x94D049BB133111EB`; all integer operations use unsigned 64-bit wraparound. Specify the emission enable bit, range, half-float rounding, and byte/bit selection beside the implementation. Record golden path/seed/material vectors so a later cleanup cannot silently change the synthetic G-buffer.

Required milestone-1 bootstrap and focused fixtures:

- **Exterior shell 128:** the default interactive model and capacity/performance fixture; one identity instance and no interior entries.
- **Single voxel:** one centered solid voxel for projection, coverage, exact surface depth/face normal, and complete material resolution.
- **Depth stack:** two or three projected solid voxels with distinct materials and different surface depths; only the nearest material must populate the G-buffer.
- **Eight octants:** one voxel in every root octant to exercise all camera-direction signs.
- **Tile boundaries:** voxels whose conservative rectangles touch one tile, multiple tiles, and image edges.
- **Stress/overflow:** a controlled projected shell/camera that exceeds duplicate capacity without allocating unbounded CPU memory.

Defer random adaptive sets, mixed levels, shared BLAS instances, overlapping transformed instances, and multi-level TLAS fixtures until the generalized scene/file milestone. They remain required before multi-instance support is declared complete, but they must not block the first renderer passes.

The default interactive sample always uses the exterior shell during initial bring-up. Useful CLI controls are `--generated-shell`, `--generated-material-seed`, root extent, and a fixed model transform; model/instance counts are intentionally not configurable in milestone 1. `validateSparseVoxelBLASArrays` is shared by bootstrap generation and the later scene validator. At the end of milestone 1, wrap the same arrays in a one-model/one-instance `SparseVoxelScene`, build its trivial TLAS, and prove that rendering is unchanged.

## New `.vsvo` file format

Use `.vsvo` as the public extension for Virtual Sparse Voxel Octree assets and `VSVO` as its four-byte magic. A VSVO stores the authored coarse-to-fine render hierarchy and the data needed to publish resident hierarchy cuts; it is not a leaf-only SVO dump. The old `.evxl` file and format implementation are already gone before this milestone begins. The writer and reader are new code over `SparseVoxelScene`; they do not share headers, enums, chunk records, or dispatch logic with `VoxelSceneFile`/`VoxelVirtualSceneFile`.

### Fixed header

Use magic `VSVO`, version 1, and a fixed-size little-endian header containing:

- Magic and version.
- Endian marker.
- Header size and chunk-record size.
- Chunk count and chunk-table offset.
- Total uncompressed payload size.
- Source-content hash and build-settings hash.
- Reserved fields initialized to zero.

### Chunks

| Chunk | Required | Contents |
| --- | --- | --- |
| `Scene` | yes | Model/instance/node/page counts, solid-voxel appearance encoding, paged-hierarchy flags, and TLAS build settings. |
| `Models` | yes | Per-model root cube, maximum level, stable global node range, root-node range, root page, and page count. |
| `Instances` | yes | BLAS index, flags, and canonical affine `modelToWorld` records. `worldToModel` and world bounds are recomputed and validated on load. |
| `Pages` | yes | Per-page model/level/consecutive-node range, conservative model-local AABB, dependency and rewrite ranges, independently compressed payload location/sizes, flags, and checksum. |
| `PageDependencies` | yes | Parent/coarser page IDs required before each finer page may become resident. |
| `PageInstallUpdates` | yes | Stable-node rewrites that publish an installed sibling group by restoring its parent's global `firstChild` and authored `childCount`. |
| `PageUninstallUpdates` | yes | Reverse rewrites that set the affected parent's resident `childCount` to zero, making the parent a renderable leaf again. |
| `PagePayloads` | yes | Independently compressed, 16-byte-aligned page blocks containing GPU-layout hierarchy nodes, path/level records, decoded center/length records, and complete averaged material records. |
| `SourceInfo` | yes | Source-relative path, importer version, source hash, and normalized build options. |
| `SourceIds` | no | One authoring/debug source ID per hierarchy node. Never uploaded by the runtime renderer. |
| `DebugNames` | no | Model/material/shape names indexed by model and optional source ID. |

Each chunk-table entry contains type, flags, file offset, stored size, uncompressed size, record size, record count, and a payload checksum. Page payloads are compressed independently with LZ4 and concatenated in the `PagePayloads` chunk so one page can be read and decompressed without touching neighboring pages. Metadata, dependency lists, and rewrites remain directly readable without decompressing voxel data.

Page order is breadth-first by octree level, then model and Morton order. Root pages are always first and contain the coarsest renderable nodes. Each page belongs to one model and exactly one level, and its `[firstNode, firstNode + nodeCount)` range is the next consecutive range in that model's canonical breadth-first node stream. Complete sibling groups remain in one page. The writer expands the `Pages` record from 96 to 128 bytes before G16 so each page also carries a conservative finite model-local AABB; the final eight bytes remain reserved and zero. This metadata lets the scheduler rank or reject an unloaded page without reading its compressed voxel payload.

The page AABB is the componentwise union of the full solid cubes of every node stored in that page, computed from canonical path/level data in deterministic `float` operations and conservatively rounded outward. It must be contained by the model root cube, have finite ordered bounds, and be recomputed and compared by the exhaustive loader/compiler validator. It describes only spatial coverage for scheduling; it does not replace per-node bounds or imply that every point inside the page AABB is occupied.

Every finer page depends on the page or pages holding its parents. A page payload initializes all newly resident internal nodes with `childCount = 0`; applying that page's install rewrites exposes the complete child ranges from the prior resident cut. Any dependency-closed resident page set—not only a file-order prefix—therefore produces a disjoint, hole-free render cut. File order remains the deterministic sequential fallback and loads all pages of level `L` before level `L + 1`, but spatial streaming may select nonadjacent pages at the same or different levels after recursively admitting their dependencies.

Stable file-global node IDs identify authored nodes and deterministic ties, but they are not GPU addresses. The streaming runtime allocates each resident page into a bounded contiguous physical-node range. At install time it translates every logical `firstChild` rewrite to `physicalPageBase + (logicalFirstChild - page.firstNode)`. GPU material/path/center arrays use the physical node index; a resident sidecar stores the stable logical node ID and that node's refinement-page ID. This avoids allocating full-scene GPU arrays merely to preserve file-global indices.

Reserve chunk/type IDs for a future optional density field, but reject them in the version-1 loader until volumetric rendering has a separate accepted milestone. Do not serialize the TLAS in version 1. Rebuild it deterministically from instances after load so stale bounds cannot disagree with transforms. Also do not serialize any render-temporary data: visible-instance lists, projected rectangles, duplicate counts, scans, sort keys, tile ranges, coverage, or depth. Do not serialize BVH/CWBVH nodes, micro-trees, macro grids, ray LODs, or occupancy atlases.

### Loader behavior

Implement `readVirtualSparseVoxelSceneMetadata`, `readVirtualSparseVoxelPage`, and `readVirtualSparseVoxelScene` with checked integer arithmetic before allocation. Validate chunk uniqueness, alignment, non-overlap, file bounds, record sizes/counts, compression sizes, checksums, every BLAS/page range and AABB, every instance transform/reference, and all model invariants. Metadata loading reads and validates the fixed tables but does not read, allocate, decompress, or checksum the complete `PagePayloads` byte range; each page's checksum is verified when that page is read. The exhaustive whole-scene loader reads every page and additionally validates the aggregate payload checksum. Recompute inverses and world bounds and build the TLAS from model/instance metadata, independently of fine-page residency.

The voxel sample opens `.vsvo`, reads metadata, builds the instance TLAS, synchronously installs every model's root page, and then hands the file/page source to the streaming runtime. `readVirtualSparseVoxelScene` remains the exhaustive compiler/test/debug path and must reconstruct the same authored hierarchy. Delete legacy `EVXL`/`EVX2` probing and conversion rather than preserving a rejection branch. A wrong magic/version, including an unrelated `.vsvo` schema, receives the normal new-loader error; it is not inspected to offer legacy upgrade behavior. Tests and tools using old voxel runtime classes are rewritten against the new scene or deleted when they cover removed behavior.

### Dependency-safe spatial page residency

Root pages are pinned for the lifetime of an open scene. All other pages move through explicit states: `Unloaded`, `Requested`, `Reading`, `Decoded`, `UploadQueued`, `Resident`, and `EvictPending`. State transitions carry a scene generation so completion from a closed/reloaded scene cannot publish stale data. IO, decoded CPU bytes, physical GPU nodes, and uploads each have independent budgets; reaching a budget preserves the current coarse cut rather than blocking or exposing an incomplete page.

The resident LOD traversal drives demand. For each visible instance/node:

1. If the node's projected size is at or below the LOD threshold, select the resident node.
2. If its resident `childCount` is nonzero, traverse its installed children.
3. If finer authored children exist but are not resident, select the parent for this frame and append its refinement-page ID to a bounded GPU request buffer.
4. Deduplicate requests by page ID and reduce priority to the maximum score across every visible instance sharing that BLAS.

The deterministic priority tuple is: visible/frustum-intersecting first, descending projected page error in pixels, ascending camera distance, coarser level first, then ascending page ID. Projected error uses the page's metadata AABB transformed by each referencing instance and the same conservative near-plane rules as voxel projection. Pixel error and distance are conservatively clamped and quantized to documented unsigned fixed-point keys shared by CPU/WGSL; raw floating-point ordering is never used as a cross-platform tie-break. Equal inputs therefore produce byte-identical request order. The UI exposes request/upload budgets, resident pages/nodes/bytes, pending IO, evictions, request overflow, and the first rejected dependency.

G16 uses one 32-bit atomic priority word per file page: bit 31 is visible, bits 12-30 are projected error rounded to 1/16 pixel and clamped to 19 bits, and bits 0-11 are inverse camera distance rounded to 1/16 world unit and clamped to 12 bits. The GPU sets a bounded `ceil(pageCount / 32)` request bitset and reduces duplicate requests with `atomicMax`; the CPU then uses coarser level and ascending page ID as the remaining stable ties. The bitset and priority arrays are cleared explicitly every traversal, so request memory is bounded by metadata page count rather than visible-node count.

The CPU scheduler recursively expands every selected request to its transitive dependencies, orders the resulting admission set by level and page ID, and uploads a page only when all direct dependencies are already resident or occur earlier in the same accepted upload batch. A decoded page is copied into one contiguous physical allocation, its resident sidecars are written, and only then are translated install rewrites dispatched. The residency bit becomes visible after the upload and rewrite barriers complete. Until then traversal keeps rendering the coarser parent.

Eviction is the exact reverse transaction. Roots cannot be evicted. A page with a resident dependent cannot be evicted; the scheduler first evicts descendants in descending level/page order. It applies uninstall rewrites and waits for their barrier before releasing physical storage. LRU age is considered only after pages outside the current request set and spatial prefetch margin are identified, with finer and farther pages evicted first. Allocation, IO, decode, upload, request-capacity, or rewrite failure leaves the last published dependency-closed cut intact.

The initial G16 implementation uses a small asynchronous CPU read/decompress queue and GPU request readback with at least two-frame tolerance; correctness cannot depend on same-frame page arrival. Sequential all-page loading remains a diagnostic mode, while selective residency is the sample default. Future direct-storage or fully GPU-driven IO may replace transport without changing page states, priority, dependencies, or install/uninstall semantics.

The physical upload transaction is represented by `SparseVoxelPageInstallBatch`: page payload arrays and resident-node sidecars are copied first, then `ApplyPhysicalPageRewrites` publishes translated physical child ranges. Eviction dispatches the same pass with zero child counts before the allocator reclaims the range. `SparseVoxelAsyncPageReader` bounds concurrent independent page reads and tags every completion with the scene generation; stale completions are emptied and can never reach `commitInstall`. The LOD frontier stores both instance and physical node IDs, and optional prefix-addressed `SparseVoxelLODSeedGPU` records allow multiple transformed instances, including shared-BLAS instances, to contribute requests and selected resident voxels without duplicating BLAS payloads.

## Interactive OBJ authoring and explicit file generation

OBJ coordinates are unitless. The sparse-voxel subsystem therefore uses meters as its canonical internal length unit, but it must never guess that an arbitrary OBJ unit is a meter. Every import has an explicit `metersPerSourceUnit` setting. The UI defaults to `1.0` with a visible “OBJ has no declared units” warning and offers meter, centimeter, millimeter, inch, foot, and custom presets. The chosen scale, axis conversion, voxel size, and all other build settings participate in the build-settings hash. Model-local centers, lengths, root extents, instance translations, camera placement helpers, and displayed statistics are in meters after import.

Within the sparse-voxel renderer, one world unit means one meter. Label instance translation and bounds in `m`, voxel size and near/far planes in `m`, camera speed in `m/s`, and acceleration in `m/s^2`. This is a scoped renderer/asset contract rather than an unreviewed conversion of unrelated Engine subsystems. Imported geometry is scaled exactly once at the source-to-`SparseVoxelScene` boundary; render, serialization, reload and instance transforms must not apply the source scale again.

Add an ImGui authoring panel in the sparse-voxel sample for load/analyze, settings, asynchronous voxelization, preview, and optional save. Tests call the same analysis/settings/builder/serializer APIs directly; the panel cannot maintain a second voxelizer. There is no startup conversion, background cache generation, milestone-driven asset build, or mandatory checked-in `.vsvo` output.

Loading an OBJ from ImGui is an explicit user authoring action, not automatic startup conversion or a legacy sibling-cache path. The panel follows this state machine:

1. **No source:** `Open OBJ...` invokes the platform file picker. No output path is invented and no file is written.
2. **Analyzing:** parse geometry/material references on a cancellable worker job. Keep the current rendered scene alive until analysis succeeds.
3. **Measured:** before enabling `Voxelize`, show source-space AABB min/max, center, XYZ extents and diagonal; vertex, triangle, object/group and material counts; degenerate/non-finite diagnostics; selected unit preset and `metersPerSourceUnit`; and the resulting meter-space measurements. Draw the measured AABB and axes in the preview view.
4. **Configured:** make target leaf voxel size in meters the primary resolution control. Derive and display cells per axis, octree root cube, maximum level, effective leaf size, conservative work/memory/page estimates, and any device/format-limit violation before work is launched. Advanced controls expose coalescing/material tolerances, compression, page target, axis conversion, and model split policy.
5. **Voxelizing:** run the common builder asynchronously with progress, cancellation, elapsed time, current stage and generation. Editing units or build settings cancels/invalidates only the pending result; it never mutates the currently displayed scene.
6. **Preview ready:** atomically replace the preview with the in-memory `SparseVoxelScene`, render it through the same TLAS, selective-residency, LOD, raster and full G-buffer path used by loaded files, and show occupied nodes, level histogram, material ranges, page estimates, build time and memory. The source mesh AABB can remain as an overlay.
7. **Optional save:** expose `Save VSVO...` only for a valid preview. Saving is a separate explicit action, uses the normal atomic VSVO writer, validates a read-back before reporting success, and never happens merely because an OBJ was opened or voxelized. Overwriting an existing file requires confirmation. The user may continue rendering an unsaved preview indefinitely.

The root-grid derivation is deterministic. Given requested voxel size `s` meters and measured maximum extent `e`, choose the smallest level `L <= 16` and root side `s * 2^L` that conservatively contains the meter-space bounds under the documented snapped-origin rule. Do not silently change `metersPerSourceUnit` or the requested voxel size. If no valid level/device budget exists, disable `Voxelize`, show the limiting calculation, and suggest the minimum valid voxel size. The panel must distinguish requested voxel size from effective voxel size if a future builder mode explicitly permits adjustment.

Editor builder stages:

1. Parse and validate the OBJ into an immutable analysis result without voxelizing. Compute raw source-space measurements, apply the explicit unit/axis transform, and compute canonical meter-space measurements using checked deterministic `float` operations.
2. Validate the requested meter-space voxel size and derive the snapped local root cube, maximum level and resource estimates before allocating voxel output.
3. Reuse only format-neutral OBJ material sampling and conservative surface coverage logic from `VoxelOBJImporter`; retain no legacy voxel scene or cache type.
4. Build each unique source model in model-local meter space. Bake only transforms internal to that model; preserve scene placement in meter-valued `SparseVoxelInstance` records.
5. Convert each occupied cell to a level/path pair in that model's local root octree.
6. Deduplicate coincident cells deterministically and combine albedo in linear RGB, roughness/metallic, and emissive radiance using documented coverage-weighted rules. Fold source vertex color/base-color factor into albedo here; do not serialize it separately and do not average sRGB bytes directly.
7. Treat every occupied cell as a completely filled solid leaf. Partial source coverage affects coverage-weighted authoring attributes, not runtime opacity.
8. Coalesce eight siblings bottom-up only when all eight children are occupied and every albedo/roughness/metallic/emissive/material-flag error threshold passes. This preserves the exact solid union; never replace a partially occupied sibling group with a filled parent and never emit a parent while any descendant remains active.
9. Materialize one complete opaque material entry per voxel in arrays parallel to path and level; canonicalize albedo alpha to 255, emission half-floats, flags, and reserved fields.
10. Canonically sort every BLAS's parallel arrays plus model/instance records and build/validate the runtime TLAS. Return the in-memory result for preview. Only the explicit CLI output argument or ImGui `Save VSVO...` action serializes, read-validates, and atomically replaces a destination.

The CLI and ImGui result panel must report source hash, settings hash, raw and meter-space scene bounds, meters per source unit, requested/effective voxel size, root cube, level histogram, voxel/material count, material-channel ranges, raw/stored bytes, compression ratio, and final file hash when saved. The same inputs and compiler version must produce byte-identical in-memory scenes and, when saved, byte-identical output.

For the first checked-in rebuild, record the exact compiler command and resulting hashes in `docs/voxel-assets.md`. The renderer's `--rebuild-voxel-cache` option should be removed; rebuilding is an explicit authoring operation.

## GPU raster pipeline

Add `assets/shaders/voxel/svraster/` and a small C++ runtime/pass wrapper under `src/voxel/svraster/`.

During initial bring-up, resident BLAS/instance buffers are populated from `BootstrapSparseVoxelModel`, and a bootstrap visibility producer emits instance zero. The one-instance `SparseVoxelScene`/TLAS added late in milestone 1 must produce the same visibility contract. Milestone 2 replaces resident-data construction and generalizes the LOD-selection seam to physical pages plus refinement requests; preprocess through G-buffer commit retain their accepted layouts, ordering, and pass responsibilities.

### SVRaster parity boundary

| Stage | Match the reference | Required Engine difference |
| --- | --- | --- |
| Resident voxel geometry | Parallel 48-bit octree path, decoded center, and full side-length arrays. | Arrays are grouped by BLAS; per-voxel albedo/PBR/emissive inputs replace learned density/SH inputs. |
| Visibility | SVRaster receives its active voxel set directly. | Initial bring-up directly emits instance zero; late milestone 1 swaps in TLAS frustum culling. Both produce the same visible-instance/indirect-work contract. No CWBVH or per-voxel ray hierarchy is added. |
| Preprocess | Eight-corner projection, packed `uint2` pixel bounds, local camera quadrant bitset, `tilesTouched * popcount(quadrants)` duplicate count, 16 x 16 tiles. | Corners are transformed by the instance model matrix; camera/quadrants are evaluated in BLAS-local space; near-plane-crossing boxes use an explicitly tested conservative fallback. |
| Scan/duplicate | Exclusive duplicate scan and one candidate per `(tile, quadrant)` exactly follow the reference data flow. | Candidate payload also stores `instanceIndex`. |
| Key/sort | Exact 64-bit `(tileId << 48) | (octreePath XOR repeatedQuadrantBits)` key and stable radix order. | Equal local keys from different instances remain stable but are not claimed to be globally depth ordered. |
| Tile ranges | Same first/end range construction from sorted tile IDs. | None. |
| Per-pixel pass | Same tile candidate filtering, quadrant check, and analytic local voxel-box segment. | Scan the full general instanced range, select the nearest forward box surface, retain its face/material/IDs, and commit the Engine G-buffer; do not integrate density/transmittance. |

Any further deviation requires a measured reason, a plan update, and a reference-parity test. Do not add Engine legacy abstractions merely because they already exist.

The frame data flow is:

```text
scene metadata/TLAS + resident physical BLAS pages
    -> visibility producer
         initial: emit hand-authored instance zero
         late M1: TLAS seed/traverse + compact visible instances
    -> visible-instance records
    -> resident LOD traversal
         select a disjoint resident-node cut
         M2: emit/deduplicate missing-refinement page requests
    -> selected-node records + preprocess indirect args
    -> indirect selected-node preprocess
    -> duplicate-count scan
    -> indirect tile-candidate emission
    -> reference-compatible 64-bit tile/local-Morton radix sort
    -> tile ranges
    -> per-tile exact nearest solid-voxel surface/material resolve
    -> fullscreen Engine G-buffer/depth commit
    -> optional debug present
```

No pass reads visible instances or selected nodes on the CPU. The bootstrap producer is still a GPU pass; it writes fixed values rather than having the CPU dispatch the later passes manually. Milestone 2 reads back only the bounded deduplicated page-request queue and priority keys; this feedback changes future residency, never the current frame's accepted visible-node cut.

### Bind-group layout

Use the same semantic grouping in every shader even when RenderGraph creates pass-specific bind groups:

| Group | Role | Bindings |
| --- | --- | --- |
| 0 | Frame | camera/view/projection/frustum uniforms; viewport/tile counts; culling flags; capacity limits; background/debug settings. |
| 1 | Resident model/scene, read-only | instance/BLAS descriptors, physical node/path/center/material arrays and resident sidecars; TLAS nodes/index array is added for the late milestone-1 producer; page metadata/residency is added in milestone 2. |
| 2 | Visibility/transient | counters/debug reasons; TLAS and LOD queue ping-pong; visible instances; selected resident nodes; page request bitset/list/priorities; projected voxels; duplicate counts/offsets; indirect-argument slots. |
| 3 | Raster/transient | candidate key/payload ping-pong; tile ranges; radix histograms/scans; overflow diagnostics. |
| 4 | Images | resolved-surface/coverage/linear-depth intermediates; Engine base-color, geometry-normal, shading-normal, material-properties, emissive, motion-vector, identity and depth targets; milestone-3 provisional/final/previous HZBs. |
| 5 | Coarse occupancy, milestone 4 only | volume state and dense GPU dirty masks; scene instances, BLAS roots/nodes and traversal queues; atomic packed-word mirror; writable and sampled `R32Uint` 3D occupancy texture. |

Bindings that a pass does not use should be omitted from its concrete layout, but names and record ABIs remain consistent. Keep the previous HZB out of active milestone layouts unless the backend requires a fixed slot; a white fallback texture must never accidentally enable occlusion.

### Shader and runtime source layout

```text
assets/shaders/voxel/svraster/wgsl/
  sparse_voxel_types.wgsl          shared ABI and key helpers
  sparse_voxel_projection.wgsl     transformed-box projection and quadrant helpers
  sparse_voxel_clear.wgsl          ClearFrame
  sparse_voxel_bootstrap.wgsl      EmitBootstrapVisibleInstance; milestone-1 bring-up only
  sparse_voxel_tlas.wgsl           late M1: SeedTLASRoot, PrepareTLASDispatch, TraverseTLAS
  sparse_voxel_visibility.wgsl     flag compaction and visible-instance preparation
  sparse_voxel_lod.wgsl            resident LOD traversal, selected nodes, and milestone-2 page requests
  sparse_voxel_page_updates.wgsl   milestone-2 translated install/uninstall rewrite application
  sparse_voxel_preprocess.wgsl     PreprocessVisibleVoxels
  sparse_voxel_emit.wgsl           EmitTileCandidates
  sparse_voxel_radix.wgsl          stable 64-bit histogram/scan/scatter entry points
  sparse_voxel_ranges.wgsl         BuildTileRanges
  sparse_voxel_resolve.wgsl        voxelSegmentForPixel and ResolveSolidVoxels
  sparse_voxel_depth.wgsl          milestone-3 WriteVoxelDepth fullscreen fragment/depth commit
  sparse_voxel_gbuffer.wgsl        WriteVoxelGBuffer fullscreen fragment/depth commit
  sparse_voxel_present.wgsl        optional G-buffer/debug visualization
  sparse_voxel_occupancy.wgsl      milestone-4 dirty-word, coarse splat, apply, and texture-sync passes
  sparse_voxel_occupancy_dirty.wgsl GPU sparse dirty-word marking/list/indirect publication
  sparse_voxel_occupancy_scene.wgsl milestone-4 dirty-volume scene/BLAS traversal and atomic leaf splat
  sparse_voxel_occupancy_common.wgsl milestone-4 camera-relative and toroidal addressing helpers
  sparse_voxel_occupancy_mips.wgsl milestone-5 globally anchored packed OR pyramid
  sparse_voxel_occupancy_dda.wgsl  milestone-5 hierarchical sparse-DDA query
  sparse_voxel_occupancy_shadow_rays.wgsl milestone-5 camera/shadow debug rays

src/voxel/svraster/
  SparseVoxelGenerator.hpp/.cpp    first: exterior-shell bootstrap data
  SparseVoxelGPU.hpp               mirrored GPU ABI records
  SparseVoxelRasterRuntime.hpp/.cpp resources, capacities, and RenderGraph passes
  SparseVoxelScene.hpp/.cpp        late M1: logical one-instance scene and validation
  SparseVoxelTLAS.hpp/.cpp         late M1: bounds and hierarchy build
  SparseVoxelSceneFile.hpp/.cpp    milestone-2 VSVO writer/reader
  SparseVoxelStreaming.hpp/.cpp    milestone-2 page states, priority, IO, physical allocation, and rewrite scheduling
  SparseVoxelOBJAnalysis.hpp/.cpp  source parsing, raw/meter bounds, unit conversion, and build estimates
  SparseVoxelOBJBuilder.hpp/.cpp   milestone-2 offline BLAS construction
  SparseVoxelOBJBuildController.hpp/.cpp cancellable analysis/build generations shared by UI and tests
  SparseVoxelOccupancyVolume.hpp/.cpp milestone-4 persistent packed volume and RenderGraph scheduling

src/runtime/rendering/gpgpu/
  DepthPyramidPass.hpp/.cpp        milestone-3 genericized/reused fast SPD HZB wrapper

tests/voxel/reference/
  SparseVoxelRasterReference.hpp/.cpp scalar CPU oracle for pass tests only
  SparseVoxelReferenceFixtures.*      immutable SVRaster-derived and analytic fixtures
```

Use the existing generic prefix-scan implementation only after its dynamic-count and indirect-dispatch behavior passes the pass tests below. Otherwise add a narrowly scoped scan implementation under the same shader directory. Avoid making the renderer depend on current brick, virtual-scene, or ray-acceleration types.

### Per-frame buffers

- Resident instance zero, one BLAS descriptor, and parallel path/level, local center/length, and albedo/roughness/metallic/emissive/material-ID/flags arrays. Current and previous instance transforms are available for G-buffer motion vectors; TLAS nodes/index ranges are added at the late milestone-1 culling step.
- Bootstrap visibility output initially; TLAS read/write queues, queue counters, per-depth indirect arguments, and optional debug rejection reasons only after the renderer passes are green.
- Visible-instance records, total visible voxel count, and preprocess indirect arguments are present from the bootstrap. Per-instance visibility flags and compaction/scan scratch are added only with the late milestone-1 TLAS producer; compaction is in ascending instance ID order.
- Resident SVRaster-style local center/full-length records derived once at scene upload.
- Projected pixel rectangles and quadrant masks.
- Per-visible-voxel duplicate counts and exclusive offsets.
- Unsorted and sorted key/payload ping-pong buffers.
- Tile start/end ranges.
- Resolved winning-surface records, binary coverage, and optional linear surface-distance diagnostics.
- The existing Engine G-buffer frame targets: base color, geometry normal, shading normal, material properties, emissive, motion vectors, depth, and stable identity outputs.
- Counters/overflow flags and dedicated indirect dispatch arguments for TLAS traversal, visible-instance preprocess, candidate emission, and sort/range work.
- Milestone 2 adds page metadata/AABBs, resident-page state, logical-to-physical node/page mappings, per-resident-node logical/refinement sidecars, missing-refinement request bitset/list/priorities, request readback, upload/uninstall rewrite batches, allocator state, scene generation, and streaming diagnostics. These are bounded independently from raster candidate buffers.
- Milestone 3 adds temporal/final raster scratch, native temporal/final depth targets, provisional current HZB, per-frame-slot final HZB history, first-valid-mip metadata, history validity, and occlusion debug counters/reasons.
- Milestone 4 adds persistent committed/pending volume metadata, a packed `R32Uint` 3D texture, a same-word-layout `array<atomic<u32>>` mutation mirror, dense GPU dirty masks, frame-local scene/BLAS traversal queues and indirect arguments, generation/validity state, and update diagnostics. Production reads the renderer's existing instance, BLAS-root, hierarchy-node, and center/length buffers directly; it neither creates nor uploads a CPU source-leaf list. The scalar reference retains compact dirty-word lists for independent equivalence tests. These resources are owned by the occupancy subsystem rather than duplicated per swapchain frame.

Capacity is based on measured visible-instance, selected-node, resident-page, request, and duplicate budgets, not `instanceCount * authoredNodeCount * tileCount`. Any queue/list overflow atomically records the first failing stage, required count, and capacity; downstream indirect arguments become zero and the frame displays a diagnostic instead of writing out of bounds. Add optional development readback showing TLAS nodes tested, frustum rejections, visible instances, selected resident nodes, coarse fallbacks, page requests, duplicate count, maximum tile list, and exact voxel tests.

### Pass sequence

| # | Pass | Dispatch | Inputs | Outputs |
| ---: | --- | --- | --- | --- |
| 0 | `ClearFrame` | direct | capacities, viewport | zeroed counters/ranges/args/resolved-surface/debug images and `overflowStage = none`; G-buffer attachments receive their defined clear values at commit |
| 1A | `EmitBootstrapVisibleInstance` | direct, one invocation; initial producer | instance zero and BLAS zero | `VisibleInstanceGPU[0]`, visible counts, total 96,776 voxels, preprocess indirect args |
| 1B | `SeedTLASRoot` + `PrepareTLASDispatch[d]` + `TraverseTLAS[d]` + `PrepareVisibleInstances` | direct/indirect; late-M1 replacement producer | one-instance scene/TLAS and frustum | the exact same visible-instance/count/indirect contract as 1A |
| 1C | `SelectResidentLODAndRequestPages` | indirect hierarchy traversal | visible instances, resident physical hierarchy/sidecars, camera, pixel threshold; page requests enabled in M2 | disjoint selected resident-node records, preprocess arguments, and deduplicated missing-refinement requests/priorities |
| 2 | `PreprocessVisibleVoxels` | indirect, one invocation per selected resident node | selected instance/node records, transform, physical center/length/path arrays, camera | SVRaster-style projected-voxel records and duplicate counts |
| 3 | `ScanDuplicateCounts` | indirect-capable scan | duplicate counts, total visible voxels | duplicate offsets, total candidates, candidate-emission args |
| 4 | `EmitTileCandidates` | indirect, one invocation per visible voxel | projected voxels, duplicate offsets, instance/local path data | unsorted 64-bit keys and instanced payloads |
| 5 | `SortTileCandidates` | indirect radix subpasses | unsorted candidates and total count | stable lexicographically sorted candidates |
| 6 | `BuildTileRanges` | indirect | sorted keys | first/end range per tile |
| 7 | `ResolveSolidVoxels` | direct, one 16 x 16 group per tile | tile ranges, candidates, current transforms, center/length arrays | winning surface distance/local face/instance and voxel IDs, binary coverage, exact-test diagnostics |
| 8 | `WriteVoxelGBuffer` | fullscreen graphics | resolved surface, resident per-voxel materials, current/previous camera and instance transforms | Engine base-color, geometry-normal, shading-normal, material-properties, emissive, motion-vector, stable-ID, coverage and hardware-depth targets |
| 9 | `DebugPresent` | fullscreen graphics or compute copy; optional | selected G-buffer/debug attachment | swapchain target |

During raster bring-up, record path 1A and do not allocate or schedule TLAS queues. At the late milestone-1 culling step, replace 1A with path 1B. Record `PrepareTLASDispatch`/`TraverseTLAS` pairs up to the one-instance TLAS depth; empty queues produce zero indirect work. `SelectResidentLODAndRequestPages` initially traverses a fully resident generated hierarchy with page requests disabled. Milestone 2 changes only its hierarchy addressing to physical resident pages and enables missing-refinement feedback; milestone 3 inserts its HZB occlusion decision immediately after the TLAS frustum test.

`EmitBootstrapVisibleInstance` writes instance index 0, dense hierarchy metadata, and visible-instance count 1. `PrepareVisibleInstances` in the real TLAS path must generate byte-identical visibility input when the instance is in the frustum and zero selection work when it is outside. `SelectResidentLODAndRequestPages` produces the exact selected-node count and preprocess dispatch. `PreprocessVisibleVoxels` uses one invocation per selected node, matching SVRaster's parallelism over its active set, and resolves the owning visible-instance/node range through a shared helper even though milestone 1 always returns record zero.

### Milestone-3 depth-only and two-phase occlusion schedule

The milestone-1/2 single visibility/raster path remains the fallback and the correctness baseline. Milestone 3 exposes the already material-independent passes 0-7 as a reusable depth-only renderer and adds `WriteVoxelDepth`, a fullscreen fragment-depth commit that consumes `ResolvedVoxelSurfaceGPU` without binding or reading voxel materials or color G-buffer attachments. Its depth must be bit-identical to the depth written by `WriteVoxelGBuffer` for the same resolved surface.

For occlusion-enabled frames, use a conservative depth phase followed by a complete actual-scene phase. Phase 1 culls TLAS leaf instances with the previous final HZB and renders only material-free depth. Phase 2 builds a provisional current HZB from that depth, traverses the current TLAS and BLAS, and rerenders the complete currently visible scene from scratch. This full second render is deliberately simpler than a pre/late merge: stale temporal rejection leaves clear depth in the provisional HZB, so the missing object is recovered by the current traversal, while every final pixel still comes from one ordinary resolve/G-buffer invocation.

1. **Temporal depth phase:** frustum-cull and conservatively occlusion-cull TLAS instances against valid previous completed history, then run SVRaster passes 2-7 and the material-free `WriteVoxelDepth` pass.
2. **Actual-scene phase:** build the provisional current HZB, cull the full current TLAS and resident BLAS against it, rerun the complete normal voxel raster path for the accepted current set, write the full G-buffer or requested depth-only output, and rebuild final history.

```text
clear temporal depth work
    -> PreCullTLAS: current frustum + previous-final-HZB leaf-instance test when valid
    -> stable temporal visible instances
    -> normal SVRaster passes 2-7 -> temporal resolved surfaces
    -> material-free WriteVoxelDepth -> temporalDepth
    -> BuildProvisionalHZB(temporalDepth) with shared fast SPD
    -> clear actual-scene raster work
    -> CurrentCullTLAS: current frustum + provisional current HZB
    -> CurrentCullBLAS: current resident octree nodes + provisional current HZB
    -> complete normal SVRaster passes 2-7 -> final resolved surfaces
    -> WriteVoxelGBuffer or WriteVoxelDepth from the complete final result
    -> BuildFinalHZB(complete final depth) for the next frame
    -> deferred lighting / optional DebugPresent
```

Milestone-3 shader pseudocode:

```wgsl
// Material-free fullscreen commit used by the standalone depth-only renderer
// in either of its visibility phases. Full frames use WriteVoxelGBuffer instead.
@fragment
fn writeVoxelDepth(fragmentPosition) -> @builtin(frag_depth) f32 {
  let surface = resolvedSurfaces[vec2<u32>(fragmentPosition.xy)];
  if (!surface.hit) { discard; }
  let rayWorld = camera_ray(vec2<u32>(fragmentPosition.xy));
  let worldPosition = rayWorld.origin + surface.t * rayWorld.direction;
  return device_depth_from_world_position(worldPosition, frame.viewProjection);
}

fn visibleAgainstHzb(worldBounds: AABB, hzb: texture_2d<f32>, hzbFrame: HzbFrameMetadata) -> bool {
  let projection = project_conservative_bounds(worldBounds, hzbFrame.view, hzbFrame.projection);
  if (projection.uncertain || projection.crossesNearPlane || projection.escapesViewport) {
    return true;
  }
  let level = choose_hzb_level_relative_to_first_valid_mip(
    projection.pixelBounds, hzbMetadata.firstValidMip, hzbMetadata.mipCount, frame.mipBias);
  let occluderDepth = sample_conservative_2x2(hzb, projection.centerUv, level);
  let objectDepth = nearest_bound_device_depth_with_bias(projection, frame.depthBias);
  return depth_convention_says_visible(objectDepth, occluderDepth);
}

fn traverseTLASNode(nodeIndex: u32) {
  let node = tlasNodes[nodeIndex];
  if (!frustumIntersectsAABB(frame.frustum, node.worldBounds)) {
    record_reason(CulledFrustum); return;
  }
  // Previous-frame pre-cull tests leaf instances with previous bounds; current-frame
  // final cull may also reject internal nodes using their current bounds.
  if (phase_allows_node_occlusion(cullPhase) && occlusionEnabled &&
      !visibleAgainstHzb(node.worldBounds, activeHzb, activeHzbFrame)) {
    record_reason(CulledOcclusion); return;
  }
  enqueue_children_or_test_leaf_instances(node);
}
```

The two raster phases may safely recycle candidate/range/resolved scratch because temporal depth is committed before the actual-scene phase begins. The final phase clears and recomputes its result; it never merges temporal material or identity data. Centralize the world-position-to-device-depth helper so depth-only and G-buffer shaders cannot diverge.

`PreCullTLAS` may use the previous completed final HZB only when its camera/history metadata is valid. It descends current TLAS internal nodes without temporal occlusion, tests leaf instances, and forces moving transforms visible; it does not treat current internal bounds as previous-frame bounds. `CurrentCullTLAS` always traverses the full current frustum-visible TLAS against provisional current depth and may reject current internal nodes and instances. Current resident BLAS nodes use the same provisional HZB; previous-history BLAS rejection is intentionally disabled. On the first frame, resize, camera/projection change, scene reload, or invalid history, pre-cull disables occlusion and emits every frustum-visible instance.

Standalone depth-only mode uses `WriteVoxelDepth` for both the temporal and final commits and rebuilds final history. Full mode uses material-free depth only for the temporal phase, then writes the complete G-buffer in the actual-scene phase. With occlusion disabled either mode runs one frustum-visible raster prefix and its normal terminal pass. The VSVO format and resident BLAS data do not change for milestone 3.

Extract or genericize the fast SPD wrapper so the voxel runtime depends on a shared rendering HZB pass, not on virtual-geometry scene types. Reuse the existing HZB shaders and their tested conventions rather than cloning them into the voxel shader directory. Same-resolution native depth leaves HZB mip 0 unused and starts at reduced mip 1 when that is the selected shared convention; culling must query the wrapper's first valid mip instead of hard-coding it.

### Milestone-4 packed camera-centered occupancy volume

Milestone 4 derives a transient coarse occupancy approximation from the already resident `SparseVoxelScene`. It is not a new BLAS, a rendering acceleration structure, a replacement for the source voxels, or part of `.vsvo`; consumers may use it for approximate spatial queries only. A set bit means at least one transformed solid source-voxel box conservatively overlaps the coarse cell. A clear bit means no source box overlapped that cell under the accepted build generation. The texture contains no albedo, material, source ID, density, or distance.

Keep world size and storage resolution separate:

```text
worldExtent       = configured vec3<f32> size of the represented box
logicalResolution = configured vec3<u32> coarse-voxel counts (Nx, Ny, Nz)
cellSize          = worldExtent / vec3<f32>(logicalResolution)
wordResolution    = (ceilDiv(Nx, 32), Ny, Nz)
wordCount         = ceilDiv(Nx, 32) * Ny * Nz
residentBytes     = wordCount * 4 for the texture
```

Pack X because this preserves a true 3D texture: texel `(wordX, y, z)` is one `u32`, and bit `x & 31` represents physical voxel X coordinate `x`. Require nonzero dimensions, checked multiplication, device-limit validation, and a configurable memory ceiling. An X resolution divisible by 32 avoids padding and is the recommended configuration; otherwise the final word's unused high bits are permanently zero. For a cubic `N x N x N` logical volume, the texture contains `ceilDiv(N, 32) * N * N` integers. The atomic mutation mirror has the same number of words, so total persistent occupancy storage is twice `residentBytes` before small metadata/scratch allocations.

Use a public sampled/storage `R32Uint` 3D texture plus an internal same-layout `array<atomic<u32>>` buffer. Portable WGSL has atomic OR for storage-buffer words but not portable storage-texture bit atomics; splat passes therefore update the atomic mirror, then copy only dirty words into the 3D texture before any consumer runs. The texture is the public volume representation and the mirror is an implementation detail. If the Engine later guarantees storage-image atomics on every supported backend, removing the mirror is a separately tested optimization rather than a milestone-4 requirement.

The volume state distinguishes three coordinates that must not be conflated:

```cpp
struct SparseVoxelOccupancyVolumeGPU {
  uint3 logicalResolution; uint32_t wordsX;
  uint3 wordResolution;    uint32_t flags;
  int3  cameraCell;        uint32_t contentGeneration;
  int3  originCell;        uint32_t sceneRevision;
  int3  scrollDelta;       uint32_t dirtyWordCount;
  uint3 wrapOffset;        uint32_t valid;
  float3 cameraInnerCell;  uint32_t fullRebuild;
  float3 cellSize;         uint32_t reserved0;
  float3 worldExtent;      uint32_t overflowStage;
};
```

- `cameraCell = floor(cameraWorld / cellSize)` is the signed integer world-grid cell containing the camera. Compute it on the CPU with checked 64-bit intermediates, validate the uploaded signed-32-bit range, and define negative-coordinate floor behavior explicitly.
- `cameraInnerCell = cameraWorld / cellSize - cameraCell` is component-wise in `[0, 1)`. Motion inside the same cell updates this value but does not rebuild occupancy.
- `originCell = cameraCell - floor(logicalResolution / 2)` anchors logical coordinate zero. The volume is centered on the camera's containing cell; `cameraInnerCell` preserves the exact sub-cell camera position for camera-relative calculations.
- `wrapOffset` maps logical cells to persistent physical slots without moving retained texture data. It is unrelated to `cameraCell` even though both change when a cell boundary is crossed.

For logical cell `l` and physical cell `p`:

```text
p             = positiveModulo(l + wrapOffset, logicalResolution)
textureCoord  = (p.x >> 5, p.y, p.z)
bitMask       = 1u << (p.x & 31u)
globalCell    = originCell + l
relativeCenterToCamera =
    (float3(globalCell - cameraCell) + 0.5 - cameraInnerCell) * cellSize
```

For a world-space lookup, compute its signed `globalCell`, subtract `originCell` to get `l`, reject coordinates outside `[0, logicalResolution)`, then use the same toroidal mapping and integer `textureLoad`. Centralize these helpers in CPU and WGSL equivalents. Never sample the packed texture with normalized coordinates or filtering.

When the camera crosses coarse cells, let `delta = newOriginCell - oldOriginCell`. If `abs(delta.axis) >= logicalResolution.axis` on any axis, or the resource was created/resized, settings changed, or history is invalid, perform a full rebuild. A scene revision without a complete explicit invalidation set also forces a full rebuild; a revision accompanied by complete old/new dirty bounds may remain incremental.

```text
newWrapOffset = positiveModulo(oldWrapOffset + delta, logicalResolution)

delta.x > 0 -> newly exposed logical X range [Nx - delta.x, Nx)
delta.x < 0 -> newly exposed logical X range [0, -delta.x)
```

Apply the same rule to Y and Z and take the union of the three slabs. OR this camera-scroll union with any explicitly invalidated interior world-space AABBs, convert the result into exact physical word/bit masks under the pending mapping, and clear/resplat only those bits. Retained logical cells resolve to their existing physical slots under `newWrapOffset`; newly exposed logical cells resolve to the slots freed on the opposite side. A one-cell movement therefore regenerates one X column, Y row, Z slice, or their nonduplicated union rather than the complete volume.

Because an X slab can cover only some bits of a physical word, never clear an entire word merely because one bit became dirty. Production converts each entering or explicitly supplied cell-aligned box directly into toroidally mapped physical X-word intervals. One invocation handles up to 32 logical X cells analytically and emits at most the wrapped/word-boundary fragments; it does not loop over those cells. Atomic OR into the dense mask returns whether that physical word was previously clean, allowing exactly one invocation to append the word to a compact GPU dirty-word list. Clear and texture publication consume that list through indirect dispatch and reset only its masks afterward. Full initialization is deliberately separate: it directly visits every valid word without contending on a multi-million-entry append counter. Y/Z slabs naturally produce full masks except for X-tail padding; multi-axis overlaps are represented once by idempotent atomic OR.

The compact dirty list is the base of a VSM-inspired three-dimensional hierarchical dirty structure. Do not allocate an unpacked `R32Uint` texel per logical voxel: a 512-cubed texture would cost 512 MiB before mips. Use the same packed-X representation as occupancy. At 512 cubed, the packed dirty base is 16 MiB and its complete packed OR hierarchy is about 2.29 MiB. It may be exposed as a second mipmapped `R32Uint` 3D texture for culling/debug sampling, while atomic marking remains in same-layout buffers. Each dirty hierarchy bit means that at least one base cell below it needs regeneration.

The update pass graph is independent from the camera frustum/HZB renderer
graph. Production volume generation is GPU-only; the CPU computes and uploads
only the pending coordinate metadata plus at most three conservative
world-space dirty-query boxes:

```text
CPU UpdatePendingVolumeState(camera, settings, sceneRevision)
    -> pending cameraCell/cameraInnerCell/originCell/wrapOffset/delta/fullRebuild
    -> upload up to three camera-slab/interior-invalidation boxes
 GPU MarkOccupancyDirtyRegions
    -> mark only entering camera slabs and explicitly injected dirty regions
    -> atomically build exact packed masks and a unique dirty-word list
    -> OR-propagate touched cells into the packed 3D dirty HPB
    -> prepare indirect clear/traversal/publication arguments
    -> ClearSparseDirtyWords in the occupancy atomic mirror
    -> TraverseOccupancyDirtyTLAS: indirect hierarchy traversal
         -> query the dirty HPB at a level selected from each TLAS-node extent
         -> reject nodes whose world bounds contain no dirty cells
         -> append dirty-overlapping instance BLAS roots
    -> TraverseOccupancyScene[level]: indirect breadth-first BLAS traversal
         -> query the dirty HPB and reject clean node bounds
         -> append children, or conservatively splat a reached leaf
         -> test exact dense mask and atomicOr each touched toroidal base bit
    -> PublishSparseDirtyWords: indirect, one invocation per dirty word
    -> propagate only dirty parent words through the occupancy pyramid
    -> CommitOccupancyVolumeState and increment contentGeneration
    -> optional occupancy slice/box/ray-march debug view
```

Do not use view-frustum or camera HZB visibility to choose occupancy sources: the volume surrounds the camera and must include geometry behind it. Occupancy uses its own dirty HPB as the traversal predicate. TLAS and BLAS AABBs are mapped into pending global occupancy cells, a dirty-hierarchy level is selected from their cell extent, and all overlapped bits at that level are tested. A clear result rejects the subtree; an occupied result only permits descent. This query must handle signed global cells, active-range clipping and every toroidal split without false negatives. Traversal queue capacity must cover the maximum current resident hierarchy; malformed hierarchies that exceed the configured depth splat their reached internal node conservatively rather than creating a false-empty region.

For each resident source leaf, compute its world center and exact AABB extent as `abs(modelToWorld.linear) * localCubeExtent`, intersect that AABB with the pending coarse grid, and visit its covered dirty logical cells. Convert each cell through the pending toroidal mapping and `atomicOr` its bit directly. Multiple leaves, instances, and invocations may target the same bit; OR is idempotent, so no deduplication, per-source count, prefix scan, atomic source compaction, transformed-AABB scratch list, or intermediate word-splat record buffer is required. Define half-open cell ownership, outward floating-point bias, clamping, and non-finite rejection in the CPU oracle. Express exact-boundary ownership in cell coordinates rather than relying on subnormal `nextafter(0)`, because supported GPUs may flush subnormals. This conservative box-to-cell rule is the milestone-4 approximation contract; matching source octree resolution or preserving source topology is explicitly unnecessary.

Validate the fixed source and dense-mask capacities before scheduling destructive work. Keep committed and pending volume metadata separate until the texture copy succeeds. Pending/committed resource publication in G26 handles allocation or command-admission failure without adding a variable splat-record capacity to the direct path. A submitted GPU-device loss invalidates the resource rather than claiming transactionality. A camera teleport may use the full-rebuild path, but it may not silently leave wrapped old bits under new metadata.

Scene transforms, topology, and residency may change incrementally when the caller supplies a complete invalidation set. A moved instance contributes both its old and new conservative world bounds; removal or eviction contributes the old bounds; addition or newly resident geometry contributes the new bounds. Clear that union first, then resplat the complete current source set through the same masks so overlapping unchanged geometry is restored. If the caller cannot prove the invalidation set complete for a changed scene revision, fall back to a full rebuild.

Milestone-4 shader pseudocode:

```wgsl
fn physicalCell(logical: vec3<u32>, state: OccupancyVolume) -> vec3<u32> {
  return positive_mod_u32(logical + state.wrapOffset, state.logicalResolution);
}

fn occupancyAddress(logical: vec3<u32>, state: OccupancyVolume) -> OccupancyAddress {
  let p = physicalCell(logical, state);
  return OccupancyAddress(
    flatten3d(vec3<u32>(p.x >> 5u, p.y, p.z), state.wordResolution),
    1u << (p.x & 31u));
}

fn logicalCellFromPhysical(physical: vec3<u32>, state: OccupancyVolume) -> vec3<u32> {
  return vec3<u32>(positive_mod_i32(
    vec3<i32>(physical) - vec3<i32>(state.wrapOffset),
    vec3<i32>(state.logicalResolution)));
}

fn newlyExposed1D(c: u32, delta: i32, extent: u32) -> bool {
  if (delta > 0) { return c >= extent - u32(delta); }
  if (delta < 0) { return c < u32(-delta); }
  return false;
}

fn dirtyLogicalCell(c: vec3<u32>, state: OccupancyVolume) -> bool {
  return state.fullRebuild != 0u ||
    newlyExposed1D(c.x, state.scrollDelta.x, state.logicalResolution.x) ||
    newlyExposed1D(c.y, state.scrollDelta.y, state.logicalResolution.y) ||
    newlyExposed1D(c.z, state.scrollDelta.z, state.logicalResolution.z);
}

fn dirtyAddress(address: OccupancyAddress) -> bool {
  return (denseDirtyMasks[address.wordIndex] & address.bitMask) != 0u;
}

@compute @workgroup_size(256)
fn splatOccupancySources(@builtin(global_invocation_id) id: vec3<u32>) {
  if (id.x >= occupancySourceLeafCount) { return; }
  let bounds = transform_leaf_aabb(source_leaf(id.x), instances);
  if (!intersects_dirty_query(bounds)) { return; }
  let coverage = conservative_coarse_coverage(bounds, pendingState);
  for (var globalCell = coverage.minimum; globalCell <= coverage.maximum;
       globalCell = next_cell(globalCell, coverage)) {
    let logical = globalCell - pendingState.originCell;
    let address = occupancyAddress(vec3<u32>(logical), pendingState);
    if (!dirtyAddress(address)) { continue; }
    atomicOr(&occupancyAtomicWords[address.wordIndex], address.bitMask);
  }
}

@compute @workgroup_size(64)
fn clearDirtyWord(@builtin(global_invocation_id) id: vec3<u32>) {
  if (id.x >= wordCount) { return; }
  atomicAnd(&occupancyAtomicWords[id.x], ~denseDirtyMasks[id.x]);
}

@compute @workgroup_size(64)
fn copyDirtyWordToTexture(@builtin(global_invocation_id) id: vec3<u32>) {
  if (id.x >= wordCount || denseDirtyMasks[id.x] == 0u) { return; }
  let wordCoord = unflatten3d(id.x, pendingState.wordResolution);
  textureStore(occupancyTextureWrite, vec3<i32>(wordCoord),
               vec4<u32>(atomicLoad(&occupancyAtomicWords[id.x]), 0u, 0u, 0u));
}

fn occupancyAtWorldPosition(worldPosition: vec3<f32>) -> bool {
  let globalCell = floor_to_i32(worldPosition / occupancyState.cellSize);
  let logical = globalCell - occupancyState.originCell;
  if (any(logical < vec3<i32>(0)) ||
      any(logical >= vec3<i32>(occupancyState.logicalResolution))) { return false; }
  let address = occupancyAddress(vec3<u32>(logical), occupancyState);
  let coord = unflatten3d(address.wordIndex, occupancyState.wordResolution);
  return (textureLoad(occupancyTextureRead, vec3<i32>(coord), 0).r & address.bitMask) != 0u;
}
```

The shader reflection test, not this illustrative declaration, is authoritative for final byte offsets.

### Candidate key and transformed-instance ordering

Keep SVRaster's 64-bit key in both single- and multi-instance paths:

```text
orderRank = octreePath48 XOR repeat3Bits(localCameraQuadrant, 16)
key       = (uint64(tileId) << 48) | orderRank
payload   = { instanceIndex, globalVoxelIndex, quadrant, projectedVoxelIndex }
```

This supports at most 65,536 tiles; validate that limit during resize instead of silently truncating the tile ID. Candidate emission starts from ascending visible instance IDs and canonical BLAS voxel order, and the 64-bit radix sort is stable, so equal keys are deterministic. The payload is wider than SVRaster's packed 32-bit `(quadrant, voxelId)` only because an instance ID is required.

The local Morton rank is front-to-back for one axis-aligned BLAS/local quadrant, as in SVRaster, but it does not globally depth-order different transforms. Solid resolution therefore scans every candidate in the tile range that covers the pixel, transforms the normalized world ray into the candidate's BLAS-local space without renormalizing its parameterization, computes exact `tEnter/tExit` plus entering/exiting face IDs, and retains the smallest first forward surface (`tEnter` when outside, `tExit` when inside). It must not early-terminate from key order in the general instanced path. Exact-depth ties use ascending instance ID then global voxel ID. A one-identity-instance reference test may take the first valid outside hit to prove that the direct port has the same order as SVRaster, but this is a validation/optimization specialization rather than the general correctness rule.

### Shader pseudocode

The pseudocode fixes pass responsibilities and buffer ownership; implementation names may follow Engine conventions.

```wgsl
// Pass 0: ClearFrame
@compute @workgroup_size(256)
fn clearFrame(globalId) {
  for_each_owned_counter_or_arg(globalId.x) { value = 0u; }
  for_each_owned_tile(globalId.x) { tileRanges[i] = EMPTY_RANGE; }
  for_each_owned_pixel(globalId.x) {
    resolvedSurfaces[p] = make_miss_surface(); coverageOut[p] = 0u; linearDepthOut[p] = 0.0;
  }
  if (globalId.x == 0u) { overflow = NO_OVERFLOW; }
}

// Pass 1A: initial visibility producer; removed when the real TLAS producer is enabled
@compute @workgroup_size(1)
fn emitBootstrapVisibleInstance() {
  visibleInstances[0] = VisibleInstance(0u, 0u, 0u, 0u);
  counters.visibleInstanceCount = 1u;
  lodSelectionArgs = dispatch_for(1u, LOD_WORKGROUP_SIZE);
}

// Pass 1B, late milestone 1: SeedTLASRoot
@compute @workgroup_size(1)
fn seedTLASRoot() {
  if (scene.instanceCount != 0u) {
    queueA[0] = TLASQueueElement(scene.tlasRoot, 0u, 0u, 0u);
    counters.queueCount = 1u;
  }
}

// Pass 1B: PrepareTLASDispatch[d]
@compute @workgroup_size(1)
fn prepareTLASDispatch() {
  counters.readCount = atomicLoad(counters.queueCount);
  tlasArgs[currentDepth] = dispatch_for(counters.readCount, TLAS_WORKGROUP_SIZE);
  atomicStore(counters.queueCount, 0u); // next queue count
}

// Pass 1B: TraverseTLAS[d]
@compute @workgroup_size(256)
fn traverseTLAS(globalId) {
  if (globalId.x >= counters.readCount) { return; }
  let node = tlasNodes[readQueue[globalId.x].nodeIndex];
  if (!frustumIntersectsAABB(frame.frustum, node.worldBounds)) { debug_frustum_reject(); return; }
  // Milestone 3: if (OCCLUSION && hzbOccluded(node.worldBounds, activeHzb)) return;
  if (node.childCount != 0u) {
    append_checked(nextQueue, node.firstChild, node.childCount);
  } else {
    for (var i = 0u; i < node.instanceCount; i++) {
      let id = tlasInstanceIndices[node.firstInstance + i];
      if (frustumIntersectsAABB(frame.frustum, instances[id].worldBounds)) {
        visibleFlags[id] = 1u;
      }
    }
  }
}

// Pass 1B: PrepareVisibleInstances (logical stable compaction)
fn compactVisibleFlag(instanceId: u32, compactIndex: u32) {
  if (visibleFlags[instanceId] != 0u) { visibleInstanceIds[compactIndex] = instanceId; }
}
fn finishVisibleCompaction(total: u32) {
  counters.visibleInstanceCount = total;
  lodSelectionArgs = dispatch_for(total, LOD_WORKGROUP_SIZE);
}

// Pass 1C: logical operation implemented by bounded level/queue dispatches.
fn visitResidentLODNode(instanceId: u32, physicalNodeIndex: u32) {
  let node = lodNodes[physicalNodeIndex];
  let metadata = residentNodeMetadata[physicalNodeIndex];
  let projectedPixels = conservative_projected_size(instanceId, physicalNodeIndex);
  let wantsChildren = projectedPixels > frame.lodPixelThreshold;
  if (wantsChildren && node.childCount != 0u) {
    append_checked(lodNextQueue, instanceId, node.firstChild, node.childCount);
    return;
  }
  if (wantsChildren && metadata.refinementPageId != INVALID_PAGE) {
    let priority = quantized_page_priority(metadata.refinementPageId, instanceId);
    atomicMax(pageRequestPriorities[metadata.refinementPageId], priority);
    atomicSet(pageRequestBits, metadata.refinementPageId);
  }
  append_checked(selectedNodes, SelectedNode(instanceId, physicalNodeIndex, metadata.logicalNodeId));
}
fn finishLODSelection(total: u32) {
  counters.selectedNodeCount = total;
  preprocessArgs = dispatch_for(total, PREPROCESS_WORKGROUP_SIZE);
}

// Pass 2: one invocation per selected resident node
@compute @workgroup_size(PREPROCESS_WORKGROUP_SIZE)
fn preprocessVisibleVoxels(globalId) {
  let selectedId = globalId.x;
  if (selectedId >= counters.selectedNodeCount) { return; }
  let selected = selectedNodes[selectedId];
  let instance = instances[selected.instanceIndex];
  let cameraLocal = transform_point(instance.worldToModel, frame.cameraWorld);
  let centerLength = voxelCentersLengths[selected.physicalNodeIndex];
  let projected = project_transformed_local_box(instance.modelToWorld, centerLength);
  if (projected_is_conservatively_visible(projected)) {
    projectedVoxels[selectedId] = make_projected_record(
        projected, selected.instanceIndex, selected.logicalNodeId, selected.physicalNodeIndex);
    duplicateCounts[selectedId] = touched_tiles(projected) * touched_local_quadrants(cameraLocal, centerLength);
  } else { duplicateCounts[selectedId] = 0u; }
}

// Pass 3: exclusive scan duplicateCounts[0..selectedNodeCount)
fn finishDuplicateScan(total: u32) {
  counters.candidateCount = checked_capacity(total, candidateCapacity, OVERFLOW_CANDIDATES);
  emitArgs = dispatch_for(counters.selectedNodeCount, 256u);
}

// Pass 4: EmitTileCandidates
@compute @workgroup_size(256)
fn emitTileCandidates(globalId) {
  let selectedId = globalId.x;
  if (selectedId >= counters.selectedNodeCount || duplicateCounts[selectedId] == 0u) { return; }
  let projection = projectedVoxels[selectedId];
  var out = duplicateOffsets[selectedId];
  for (var quadrant = 0u; quadrant < 8u; quadrant++) {
    if ((projection.quadrantBitset & (1u << quadrant)) == 0u) { continue; }
    let localRank = direction_dependent_morton(voxelPathsLevels[projection.physicalNodeIndex], quadrant);
    for tile in touched_tiles_in_reference_order(projection.packedBounds) {
      candidates[out] = make_candidate(
        encode_reference_key(tile, localRank),
        projection.instanceIndex, projection.logicalVoxelId, quadrant, selectedId);
      out += 1u;
    }
  }
  assert_debug(out == duplicateOffsets[selectedId] + duplicateCounts[selectedId]);
}

// Pass 5: compose the repository's stable 32-bit RadixSort2Pass.
fn sortTileCandidates() {
  extract_low_word_and_original_indices();
  radix_sort_32(keys, originalIndices);
  extract_high_word_through_sorted_indices();
  radix_sort_32(keys, originalIndices);
  gather_candidate_payload_once(originalIndices);
}

// Pass 6: BuildTileRanges
@compute @workgroup_size(256)
fn buildTileRanges(globalId) {
  if (globalId.x >= counters.candidateCount) { return; }
  let tile = candidates[globalId.x].keyTile;
  if (globalId.x == 0u || tile != candidates[globalId.x - 1u].keyTile) { tileRanges[tile].first = globalId.x; }
  if (globalId.x + 1u == counters.candidateCount || tile != candidates[globalId.x + 1u].keyTile) { tileRanges[tile].end = globalId.x + 1u; }
}

// Pass 7: one 16x16 workgroup per tile; one lane per pixel
@compute @workgroup_size(16, 16, 1)
fn resolveSolidVoxels(workgroupId, localId) {
  let pixel = workgroupId.xy * 16u + localId.xy;
  if (outside_viewport(pixel)) { return; }
  let rayWorld = camera_ray(pixel);
  var bestT = INF;
  var bestInstance = INVALID;
  var bestVoxel = INVALID;
  var bestLocalNormal = vec3<f32>(0.0);
  for candidate in tile_range(workgroupId.xy) {
    if (!candidate_covers_pixel_and_quadrant(candidate, pixel)) { continue; }
    let instance = instances[candidate.instanceIndex];
    let localRay = transform_ray_keep_world_t(instance.worldToModel, rayWorld);
    let segment = voxelSegmentForPixel(localRay, voxelCentersLengths[candidate.voxelIndex]);
    let surface = first_forward_box_surface(segment); // entering face outside, exiting face inside
    if (surface.hit && exact_hit_precedes(surface.t, candidate, bestT, bestInstance, bestVoxel)) {
      bestT = surface.t;
      bestInstance = candidate.instanceIndex;
      bestVoxel = candidate.voxelIndex;
      bestLocalNormal = surface.localOutwardNormal;
    }
  }
  if (bestVoxel != INVALID) {
    resolvedSurfaces[pixel] = make_resolved_surface(
      bestT, pack_box_face(bestLocalNormal), bestInstance, bestVoxel);
    coverageOut[pixel] = 1u;
    linearDepthOut[pixel] = bestT;
  }
}

// Pass 8: fullscreen graphics G-buffer/depth commit
@fragment
fn writeVoxelGBuffer(fragmentPosition) -> VoxelGBufferFragmentOutput {
  let pixel = vec2<u32>(fragmentPosition.xy);
  let surface = resolvedSurfaces[pixel];
  if (!surface.hit) { discard; }
  let instance = instances[surface.instanceIndex];
  let voxelMaterial = decode_voxel_material(voxelMaterials[surface.voxelIndex]);
  let rayWorld = camera_ray(pixel);
  let worldPosition = rayWorld.origin + surface.t * rayWorld.direction;
  let localNormal = unpack_box_face_normal(surface.packedFace);
  let worldNormal = normalize(transform_normal_inverse_transpose(instance.worldToModel, localNormal));
  let gbufferMaterial = material_gbuffer_make(
    vec4(srgb_to_linear(voxelMaterial.albedo), 1.0),
    worldNormal, voxelMaterial.roughness, voxelMaterial.metallic, voxelMaterial.emissive, voxelMaterial.flags);
  var out = store_engine_gbuffer_outputs(gbufferMaterial);
  out.geometryNormal = vec4(worldNormal, 1.0);
  out.motionVector = motion_from_current_previous_surface(worldPosition, surface, frame, instance);
  out.instanceVoxelIds = vec2(surface.instanceIndex, surface.voxelIndex);
  out.materialId = voxelMaterial.materialId;
  out.materialUv = vec2<f32>(0.0);
  out.depth = device_depth_from_world_position(worldPosition, frame.viewProjection);
  return out;
}

// Pass 9: optional debug presentation
fn debugPresent(fragmentPosition) -> vec4<f32> {
  return visualize_selected_gbuffer_or_debug_output(fragmentPosition.xy);
}
```

Use the existing WebGPU-portable `RadixSort2Pass<uint32_t, uint32_t>` and `radixsort2.wgsl`; do not fork its count/reduce/scan/scan-add/scatter kernels into the voxel renderer. It uses WGSL workgroup memory, atomics, barriers, storage buffers, and indirect dispatch without subgroup extensions or OneSweep progress guarantees. The 32-bit mode performs one stable sort. The 64-bit mode performs a stable low-word sort followed by a stable high-word sort while carrying original candidate indices. Gather the 32-byte candidate payload only once after the final sort.

Use the reference constants only as starting points, not ABI:

- Tile size: 16 x 16.
- Maximum octree level: 16.
- Solid coverage/opacity: exactly 1 for a hit and 0 for a miss.
- Surface distance: nearest first forward boundary in world-ray units (`tEnter` outside, `tExit` inside); zero on a miss. Hardware depth is computed from the winning world position with the Engine depth convention.
- Surface normal: outward normal of the selected box slab transformed by the inverse transpose; geometry and shading normals are identical in version 1.
- Exact-depth tie epsilon: centralized CPU/GPU constant, with deterministic instance/voxel tie-breaking.

`RadixSort2Pass` remains a 32-bit primitive despite its C++ template arguments. The voxel renderer obtains stable 64-bit ordering through the two-sort composition above; widening the C++ key template alone would not widen the shader. G6 must continue to prove that the composed output exactly matches a CPU stable sort of SVRaster's `(tileId << 48) | orderRank` key.

### Initial renderer UI

Keep only settings relevant to solid voxel rasterization:

- Final lit/deferred output plus base color, geometry normal, shading normal, roughness, metallic, emissive, motion-vector, hardware-depth, linear-depth, coverage, instance-ID, or voxel-ID visualization.
- Supersampling factor, if added.
- Duplicate-buffer capacity/overflow diagnostics.
- Timing windows.

Milestone 2 adds selective/load-all mode, LOD pixel threshold, resident page/node/byte budgets, read/decode/upload budgets, prefetch margin and eviction hysteresis, plus requested/reading/decoded/resident/evicted page counts, coarse-fallback count, request overflow, residency hit rate, scene generation, and page-AABB/residency debug views. Budget changes are staged; lowering a budget triggers dependency-safe descendant-first eviction rather than immediate allocator reuse.

Milestone 3 adds depth-only/full mode, occlusion enable, HZB visualization, conservative mip bias, depth bias, history validity, and temporal/current/HZB counters. Do not expose a switch that bypasses current-frame recovery while temporal pre-culling remains enabled.

Milestone 4 adds occupancy enable, logical resolution, world extent, derived cell size/word dimensions/bytes, validity/generation/scene revision, camera cell, inner-cell coordinate, wrap offset, dirty-word/splat counts, full-versus-incremental reason, update timings, and slice/bit/box debug views. Configuration changes are staged and force a validated full rebuild; the UI must not reinterpret an existing allocation with new dimensions. The sample now initializes the occupancy cell size from the active models' smallest finest-level voxel, starts with a 512-voxel extent per axis so the framed bootstrap scene intersects the camera-centered volume, exposes editable voxel size and XYZ extent in voxels, previews the resulting world extent and base-plus-pyramid memory budget, warns when the staged volume clips or entirely misses scene bounds, and applies valid staged values only through an explicit occupancy-graph recompile button.

Remove CWBVH, sparse-primary, GI backend, trace LOD, trace heatmap, ray count, ray bias, trace distance, screen trace, global fallback, and global-octree controls.

## Code migration and deletion matrix

Build the generated-scene renderer as a new replacement subsystem, not a refactor layered on the old runtime. The old sample path may remain temporarily available only as untouched code while the new synthetic path is brought up on the branch; there is no data adapter or shared pass. Delete the entire old voxel runtime path at the end of milestone 1. Milestone 2's compiler starts from OBJ source and writes the new parallel arrays directly, so no legacy voxel conversion code is retained.

### Replace in the sample

- Rewrite `samples/voxelRenderer/VoxelRenderer.cpp` around `SparseVoxelScene` and `SparseVoxelRasterRuntime`; do not add another mode to the current monolith.
- Delete `prepareVoxelAsset` and populate the new runtime directly from `generateExteriorShell128` plus the single hand-authored instance in milestone 1.
- In milestone 2, add one strict metadata-first `.vsvo`/`VSVO` open path while preserving `--generated-scene` and `--load-all-pages` for diagnostics; do not restore format probing.
- Replace brick/occupancy/CWBVH uploads with TLAS nodes, current/previous instance transforms, BLAS descriptors, bounded physical page storage, resident sidecars, and streamed SVRaster-style parallel path/level, center/length, and complete material uploads.
- First replace the current frame graph with clear, bootstrap-visible-instance, indirect preprocess, duplicate scan/emission, sort, tile-range, solid nearest-surface resolve, full G-buffer commit, and optional debug-present passes. Late in milestone 1, replace only the bootstrap producer with TLAS seed/prepare/traverse plus visibility compaction/scan.
- Remove OBJ import at application startup and all legacy-format probing.

### Delete voxel ray rendering

- Delete `assets/shaders/voxel/wgsl/voxel-gi.wgsl`.
- Delete the DDA ray functions from `voxel-debug-renderer.wgsl`; retire the shader entirely once the new resolver and G-buffer commit produce validated material/normal/depth outputs.
- Delete `assets/shaders/gpusparsevoxel/wgsl/gpusparsevoxel-trace-common.wgsl`.
- Delete `gpusparsevoxel-trace-debug.wgsl`, `gpusparsevoxel-test-trace.wgsl`, and `gpusparsevoxel-gi.wgsl`.
- Delete `GPUSparseVoxelPrimaryPass`, CWBVH primary, noisy/sorted GI, trace tests, trace readbacks, and their buffers/pipelines/bindings from the sample.
- Delete screen-ray generation/tracing, global-octree ray tracing, and hybrid trace resolve scaffolding.
- Delete global-octree free/splat/update paths if no remaining non-ray consumer exists after the raster cutover.

### Delete ray acceleration data

- Delete all voxel CWBVH node/leaf/micro-tree/macro-grid/trace-LOD types, builders, upload buffers, bindings, serialization, debug views, and tests. Do not retain empty shells or renamed equivalents.
- Remove every voxel dependency on `softwarebvh/CWBVHBuilder.hpp`; the separate non-voxel software-BVH subsystem remains outside this migration.
- Delete trace rebuild calls and trace-only uniforms/WGSL layouts rather than mapping them into the new runtime.

### Delete legacy voxel runtime and format structures

- Delete `EVXL`/`EVX2` readers, writers, magic probes, version enums, conversion functions, and checked-in legacy assets during the milestone-1 synthetic-renderer cutover. Their removal is not conditional on an VSVO writer or replacement Sponza asset existing.
- Delete or fully detach `VoxelSceneCPU`, `VoxelCullingScene`, `VoxelCullingInstanceGPU`, `VoxelCullingBrickGPU`, voxel `VoxelBVH8NodeGPU`, brick occupancy/runtime atlas records, and virtual-scene runtime records when their final old-renderer consumers are removed.
- Rewrite still-relevant OBJ material/coverage helpers behind the new offline builder API. Do not make `SparseVoxelScene` consume a legacy scene as an intermediate.
- Replace legacy voxel scene/culling/file tests with the new test binaries. Preserve tests only for genuinely shared importer helpers.
- After deletion, a repository search over the voxel sample/runtime must find no CWBVH, legacy magic, legacy asset extension, or old brick/BVH structure name outside migration history/documentation.

Delete the current brick-oriented BVH8/CWBVH voxel culling data and replace it with the new model-instance TLAS. Reuse only proven frustum math and the virtual-geometry-inspired multipass queue pattern, not any legacy node/brick/LOD record. The offline compiler may have a private temporary occupancy grid, but it must emit SVRaster path/level arrays directly and must not include runtime brick or BVH types.

### Delete EVXL completely at the milestone-1 cutover

Once G12 proves the synthetic TLAS renderer, G13 removes EVXL in the same change series. There is intentionally a period where the sample renders only generated data and no checked-in voxel scene file exists. Do not retain EVXL until milestone 2 as a fallback.

The current-tree deletion inventory includes:

- Delete `assets/meshes/obj/sponza.evxl` immediately at G13; do not rename it, convert it in place, or use it as input to the VSVO compiler.
- Delete `src/voxel/core/VoxelSceneFile.hpp` and `src/voxel/core/VoxelSceneFile.cpp`, including `VoxelSceneFileCompression`, metadata records, `readVoxelSceneFile*`, `writeVoxelSceneFile*`, `EVXL`/payload magic, version constants, compression code, and CWBVH/culling payload serialization.
- Delete the coupled legacy `src/voxel/core/VoxelVirtualSceneFile.cpp`/EVX2 path rather than splitting the shared header merely to preserve another obsolete file implementation.
- Remove `VoxelSceneFile.hpp` dependencies from `VoxelOBJImporter` and any other surviving importer helper. Shared OBJ parsing/material/coverage code may survive only behind format-neutral APIs.
- Remove `.evxl` extension handling, sibling-cache naming, OBJ-at-runtime conversion, `--rebuild-voxel-cache`, help text, error text, and legacy load/probe branches from `samples/voxelRenderer/VoxelRenderer.cpp`.
- Delete EVXL-specific test cases and temporary filenames from `VoxelCullingTests`, `VoxelVirtualSceneFileTests`, `VoxelVirtualRuntimeTests`, `GPUSparseVoxelSponzaBuildTests`, and `GPUSparseVoxelSponzaGpuTests`; delete entire test targets and their CMake registrations when they test only the removed runtime. Re-express only format-independent math/OBJ coverage assertions in the new gated tests.
- Delete the obsolete `docs/voxel-virtual-geometry-streaming-plan.md` rather than leaving instructions that describe generating or loading `.evxl` files. Update any other active documentation, scripts, presets, packaging manifests, or asset lists discovered by the final search.
- Remove now-unused source files, headers, includes, CMake source lists, install rules, shader dependencies, CLI options, and test registrations. Do not leave deprecated stubs, typedef aliases, magic constants, commented code, fixtures, or compatibility error messages.

G13 runs a case-insensitive repository-wide source/asset-name audit for `EVXL` and `.evxl`, excluding only VCS metadata, build output, the ignored pinned reference checkout, and this migration plan's deletion record. Any other match fails the gate. It also searches for the concrete `VoxelSceneFile` APIs above so an EVXL implementation cannot survive under a file without the extension in its name. The clean build and all G1-G12 tests must pass after these removals.

### Retire the sparse-cascade subsystem if unreferenced

After the sample switches, check all remaining call sites. If no non-voxel-renderer consumer remains, delete:

- `src/runtime/gpusparsevoxel/`.
- The remaining `assets/shaders/gpusparsevoxel/` build/probe shaders.
- `GPUSparseVoxelCascadeCacheTests`, `GPUSparseVoxelSponzaBuildTests`, and `GPUSparseVoxelSponzaGpuTests` plus their CMake entries.
- The `GPUSparseVoxelShaders` target and sample dependency.

Do not remove the general `softwarebvh` subsystem or its tests; it is shared by the separate virtual-geometry/software-raytrace renderer and is outside this voxel-renderer migration.

### Simplify pass declarations and build wiring

- Replace `VoxelPassKind` ray entries with the raster pass names, or remove the enum if the new runtime owns pass naming directly.
- Remove ray fields from `VoxelPassSettings`.
- Add the new WGSL compile targets and remove the voxel GI/sparse-trace targets from `src/runtime/CMakeLists.txt`.
- In milestone 3 add `sparse_voxel_depth`/`sparse_voxel_merge` targets and consume the existing shared SPD HZB shader variants; do not create voxel-prefixed copies of the HZB shaders.
- Move/generalize the depth-pyramid C++ target without breaking `VirtualGeometryCullingMultipleDispatchesPassTests`; both virtual geometry and sparse voxels must exercise the same implementation.
- In milestone 4 add the occupancy common/update/debug shaders and runtime target without depending on the deleted `gpusparsevoxel` target. Reuse generic scan/compaction utilities only through their maintained shared interfaces.
- Add the offline voxel compiler to `src/cli/CMakeLists.txt`.
- Update `samples/voxelRenderer/VoxelRendererSample.cmake` to depend only on the new shaders.

## Test-gated development policy

Treat every numbered implementation step as a promotion gate, not merely a task list. A later pass may be scaffolded so the project compiles, but it must not be enabled in the sample, used to hide an earlier failure, or considered started for acceptance until the preceding gate is green. Each gate requires:

1. CPU unit tests for the data transformation or an independent analytic expected result.
2. A GPU pass test that uploads a small fixture, dispatches only the pass under test, reads back every output and counter, and compares them with the CPU result.
3. Boundary and malformed-input tests, including zero work, one item, workgroup-boundary counts, non-workgroup-aligned counts, maximum valid values, and deliberate capacity failure where applicable.
4. Determinism tests over repeated runs and different input enumeration orders whenever ordering is part of the contract.
5. A chained-prefix test that runs all already-accepted passes through the new pass and verifies every intermediate buffer, not only the final image.
6. A Vulkan validation run with no out-of-bounds access, uninitialized read, invalid indirect dispatch, validation error, NaN/Inf output, or unexpected overflow.

The stage-specific test label must pass locally and in the required headless Vulkan CI job before the next stage is promoted. A skipped GPU test does not satisfy a gate; platforms without the test GPU may skip it, but at least one required CI GPU configuration must execute it. Record the fixture seed, camera, viewport, capacities, expected hashes, and first differing element on failure so results are reproducible.

### CPU reference implementation

Build a deliberately simple, scalar, test-only implementation under `tests/voxel/reference/`. It must operate on the public CPU/GPU ABI records but must not be linked into the sample or asset compiler. Keep it structurally independent from the WGSL implementation: clarity and direct loops are preferred over mirroring GPU workgroup/radix mechanics. It provides:

- octree path decode, center/length reconstruction, local camera quadrant, conservative transformed-box projection, packed pixel bounds, and tile coverage;
- exclusive duplicate scan and checked total/capacity calculation;
- tile/quadrant candidate emission with the exact SVRaster 64-bit key and payload contract;
- stable comparison sort by the full 64-bit key, used as the oracle for the GPU radix implementation;
- tile range construction;
- scalar world-ray-to-local-box intersection with entering/exiting face selection and exhaustive nearest-solid-surface resolver;
- CPU G-buffer oracle for base color, geometry/shading normal, material properties, emissive, stable IDs, device depth, and current/previous-frame motion vectors;
- frustum/AABB classification, stable visible-instance compaction, visible-voxel offsets, and indirect-dispatch calculation;
- milestone-2 page-AABB projection, deterministic request priority/deduplication, recursive dependency admission, logical-to-physical rewrite translation, resident-cut traversal, and descendant-first budget eviction;
- milestone-3 forward/reverse-Z HZB reduction, valid-mip tracking, projected TLAS bounds, conservative sampling/depth comparison, and pre/final submitted-set composition;
- milestone-4 signed camera-cell/sub-cell decomposition, snapped origin, toroidal logical/physical mapping, dirty-slab/word masks, transformed-box-to-coarse-cell coverage, direct atomic-OR occupancy, texture state, and full/incremental frame composition;
- a tiny full-frame renderer that composes the reference stages for fixtures small enough to debug record by record.

Integer and discrete results must match exactly: counts, offsets, bitsets, packed bounds after the defined rounding rules, keys, payload order, tile ranges, instance lists, packed material words, IDs, coverage, and indirect arguments. Floating-point projection, decoded material values, normals, motion, and depth comparisons use explicitly documented absolute/relative tolerances and must also satisfy conservative/unit-vector/range invariants; the CPU oracle uses explicit `float` operations and Engine matrix/depth conventions so an accidental double-precision-only success cannot mask a GPU boundary error. Exact analytic fixtures cover plane, edge, corner, camera-inside, reflection, non-uniform scale, emissive range, and temporal camera/instance motion cases.

The CPU oracle is checked first against immutable tiny fixtures captured from the pinned SVRaster checkout for the identity-instance parity path, plus hand-computed analytic fixtures for Engine transforms and solid resolution. GPU results are then checked against both the oracle and those fixtures where applicable. Never regenerate expected files automatically during a test run.

Use fixed-seed property tests for canonical path/material sets, scans, keys, stable sorting, ranges, transforms, face normals, G-buffer packing, and nearest-surface selection. Persist any failing seed and minimized fixture. Random tests supplement explicit edge cases; they do not replace them.

### Ordered promotion gates

| Gate | Implementation admitted by the gate | Required proof before continuing |
| --- | --- | --- |
| G1 | GPU ABI and `generateExteriorShell128` | Compile-time/reflection layout equality; exact 96,776 canonical boundary voxels; deterministic path/albedo/roughness/metallic/emissive/material-ID goldens; invalid-array/material rejection. |
| G2 | `ClearFrame` and bootstrap visibility | Poison-before-clear tests; exact visible record/counts/indirect args; zero/stale-state cases; chained G1-G2 readback. |
| G3 | `PreprocessVisibleVoxels` | CPU/reference equality for packed bounds, masks and counts across analytic clip/near-plane/quadrant fixtures and fixed shell cameras; no false-negative conservative projection. |
| G4 | `ScanDuplicateCounts` | Exact CPU exclusive scan, total and indirect args for 0/1/workgroup-edge/random counts; overflow zeros downstream work. |
| G5 | `EmitTileCandidates` | Exact emitted count, 64-bit keys, payloads and enumeration order for every touched tile/quadrant; full 48-bit path adversaries. |
| G6 | `SortTileCandidates` | Stable full-width order equals CPU stable comparison sort and pinned reference fixtures for equal keys, both 32-bit words and multi-workgroup sizes. |
| G7 | `BuildTileRanges` | Exact CPU ranges for empty/sparse/full/repeated tiles and consecutive frames with shrinking work; chained G1-G7 equality. |
| G8 | `ResolveSolidVoxels` | CPU/analytic nearest forward surface distance, local face, IDs and coverage for misses, overlap, ties, adjacency, camera-inside and transformed boxes; exhaustive full tile scan and compact-record validation. |
| G9 | `WriteVoxelGBuffer`, `DebugPresent`, and bootstrap end-to-end | Every Engine G-buffer attachment, hardware depth, motion, clears, format/color-space and resize match CPU/analytic expectations; deterministic fixed-camera shell metrics/hash policy; all intermediate buffers still match the CPU pipeline. |
| G10 | `SparseVoxelSceneManager` and CPU TLAS builder | Stable scene/instance IDs and revisions; derived inverse/previous transforms and world bounds; deterministic breadth-first BVH8 construction; containment and exactly-once instance membership. |
| G11 | GPU TLAS traversal/compaction plus BLAS-node frustum culling | Per-plane CPU frustum parity at TLAS nodes, leaf instances, and resident octree nodes; queue/overflow/indirect checks; deterministic visible order; no page request or raster work outside the frustum. |
| G12 | Bootstrap-to-TLAS producer swap | Byte-identical visibility contract, identical G3-G9 intermediates and equivalent final frame for the visible identity instance. |
| G13 | EVXL/legacy/ray-path deletion | `sponza.evxl`, all EVXL APIs/files/tests/docs/cache options, and other scoped legacy/ray paths are gone; repository-wide EVXL audit, clean build, and cumulative renderer tests pass after each deletion batch. |
| G14 | VSVO paged-hierarchy writer | Golden 128-byte page records including exact conservative page AABBs; complete same-level consecutive LOD/material ranges; arbitrary dependency-closed set and prefix replay; install/uninstall rewrite replay; deterministic bytes, per-page checksums, None/LZ4 coverage and write-write identity. |
| G15 | VSVO reader | Metadata-without-payload reads, individual-page reads, and whole-file round trip; page AABB/consecutive-order/dependency validation; truncation/corruption/overflow/schema rejection; no allocation before checked sizes; direct/read hierarchy/page/scene equality. |
| G16 | Selective page residency plus multi-model/instance generalization | CPU priority/dependency/eviction oracle and GPU request/install/uninstall parity; bounded physical page allocation; coarse fallback during delayed/failed IO; deterministic spatial selection for shared BLAS instances, transformed overlap, mixed levels, adversarial file/physical orders, and arbitrary dependency-closed resident sets; full G-buffer equality with the equivalent authored cut. |
| G17 | Measured OBJ authoring, sparse-BLAS builder, preview, and optional save | Explicit source-unit-to-meter conversion; raw/meter bounds and deterministic grid estimates before voxelization; analytic mesh/material coverage; cancellable generation-safe build; in-memory full-G-buffer preview; no write before explicit editor save; saved-file round trip. |
| G18 | User-selected file load and final cutover | Any explicitly saved VSVO opens metadata-first, validates/round-trips, renders at fixed cameras, and passes capacity/performance, clean-suite, and removal audits; no particular generated asset is required. |
| G19 | Reusable depth-only rendering | Material-free bindings and `WriteVoxelDepth`; exact depth/coverage/IDs match the G-buffer path for generated, transformed, loaded, empty, resize and overflow fixtures. |
| G20 | Shared fast SPD HZB | CPU pyramid equality for forward/reverse Z, odd/tiny dimensions and multi-dispatch mip counts; barriers/counter reset/history rotation are validation-clean. |
| G21 | Isolated hierarchical HZB occlusion | CPU/GPU projected bounds, mip/sample/depth/bias decisions and debug reasons match for TLAS nodes, instances, and resident octree nodes; uncertain cases remain visible and false occlusion is forbidden. |
| G22 | Two-phase hierarchical occlusion integration | Previous-HZB TLAS pre-cull plus material-free temporal depth, provisional current HZB, and a complete current-HZB TLAS/BLAS actual-scene rerender are depth/G-buffer equivalent to frustum-only rendering across motion/disocclusion while measured final selected-node work decreases on occluded fixtures. |
| G23 | Occupancy layout and coordinate contract | Checked `R32Uint` dimensions/bytes and reflected ABI; CPU/GPU camera cell, inner coordinate, origin, wrap, address, tail-bit, negative-world and camera-relative reconstruction fixtures match exactly. |
| G24 | Full occupancy build | GPU dirty-volume instance culling and indirect BLAS traversal produce resident transformed-leaf conservative coverage; direct toroidally addressed atomic OR and dirty texture copy match the scalar CPU volume bit-for-bit for synthetic and loaded scenes; material changes do not affect occupancy. |
| G25 | Incremental dirty-region rebuild | Positive/negative and multi-axis camera-cell moves plus moved/added/removed/residency-changed instance bounds preserve retained bits, clear/rebuild exactly the union dirty masks, reuse freed physical slots, handle wrap and sub-cell-only motion, and match a from-scratch rebuild after every step. |
| G26 | Occupancy integration and resilience | Sponza camera paths, teleports, resize/settings/scene revision, empty scene and injected failures publish only complete generations; consumers/debug views read the 3D texture correctly and measured one-cell updates touch substantially less work than full rebuilds. |
| G27 | Full globally anchored occupancy pyramid | Exact checked packed level ranges, CPU/WGSL descriptors and bottom-up OR reduction match an independent scalar pyramid for arbitrary dimensions/origins/wraps; every occupied base bit has occupied ancestors and no false-empty ancestor is possible. |
| G28 | Incremental toroidal pyramid propagation | Outgoing/incoming base cells project to unique dirty parent masks; every level scrolls independently without shifting retained bits, clears padding, publishes atomically, and matches a full pyramid rebuild across fixed-seed camera walks. |
| G29 | Conservative sparse DDA | CPU/GPU hierarchical traversal matches scalar mip-0 hit/miss and first occupied-cell results, guarantees forward progress at boundaries/ties/zero axes, never skips occupancy across toroidal seams, and reduces traversal steps on sparse fixtures. |

Implementation status (2026-07-22): G10-G22 are implemented and locally green. G10-G12 provide the derived scene/TLAS, indirect GPU frustum traversal, stable compaction, BLAS-node rejection, and production producer swap. G13 removes the legacy voxel renderer, file/import path, voxel ray shaders, sparse-cascade subsystem, obsolete assets, and build/test wiring, with a repository audit enforcing the cutover. G14-G15 provide the deterministic dependency-aware VSVO writer and checked metadata/page/exhaustive readers. G16 supplies bounded dependency-aware residency, physical rewrites, GPU page requests, priority reduction, cancellation, and coarse fallback; G18 connects those contracts to the interactive renderer through metadata-first `Open VSVO...`, pinned-root publication, asynchronous selected-page reads, GPU request readback, incremental physical-buffer installs, load-all diagnostics, multi-model TLAS construction, and failure-safe scene replacement. G17 OBJ measurement, transient preview, and explicit-save authoring remain separate: opening or previewing never writes a file. Focused G18 tests cover metadata-only opening, no implicit writes, corrupt replacement preservation, multi-model roots, selective GPU-requested refinement, full residency, and file-backed rendering. LOD queue storage is sized from the file's declared maximum level through the supported level-16 limit rather than retaining the original eight-level sample assumption. G19 adds the standalone material-free `WriteVoxelDepth` path. G20 exposes the existing scene-independent SPD implementation through the shared rendering namespace and uses it for provisional/final reverse-Z HZBs. G21 adds conservative TLAS/BLAS HZB rejection, first-valid-mip metadata, bias, fail-open edge handling and debug reasons. G22 schedules previous-history temporal depth followed by a complete current-HZB actual-scene render; paired GPU tests hash every final G-buffer/depth pixel against the frustum-only baseline and require reduced selected-node work. The interactive default remains the faster one-phase frustum renderer; two-phase TLAS occlusion is opt-in, current-HZB BLAS rejection is separately experimental, and frustum-only mode allocates only a 1x1 HZB binding fallback. Per-phase visible instance/voxel, candidate, duplicate, and overflow metrics are exposed in ImGui. A Stanford bunny 0.01 m regression compares the complete output with BLAS node frustum culling enabled/disabled at four camera-orbit angles. The virtual-geometry-style pass timeline is also available on demand with 180-frame timestamp history, separate compute/graphics graphs, selected-frame pass costs, and voxel phase totals; its timer resolve/readback is disabled while the window is closed.

If a gate fails, fix the first mismatching stage before proceeding. Do not bless a new image golden, widen a tolerance, or increase a capacity until the mismatch is explained and the CPU/reference expectation is reviewed. Golden-image comparisons are an integration backstop; pass-level structured-buffer comparisons remain the primary correctness proof.

## Milestones

### Milestone 1: Render one synthetic exterior-shell model

This milestone proves the raster renderer with exactly one model and one instance, without any dependency on a voxel file, OBJ importer, legacy scene converter, or current runtime voxel structures. Scene/TLAS work is intentionally last, after the raster pipeline resolves the bootstrap shell and commits its complete G-buffer.

#### M1.1: Rewrite the sample around the bootstrap model contract

- Add the new parallel BLAS path/level, center/length, complete per-voxel material, instance, visible-instance, projected-voxel, key/payload, resolved-surface, range, and indirect-argument GPU types without including legacy voxel headers.
- Implement `generateExteriorShell128`: 96,776 unique level-7 boundary paths from a 128-cubed grid, deterministic pseudo-random albedo/roughness/metallic/emissive/material-ID values per path, and no interior entries.
- Lock version-1 solid semantics: each path/level voxel is a filled opaque box, albedo alpha is canonicalized to one, the visible surface is the first forward box boundary, its normal comes from that boundary face, and depth ties use instance/voxel IDs.
- Rewrite the sample around the new resident buffers and delete/bypass old frame setup; do not implement `SparseVoxelScene` or TLAS traversal yet.
- Implement `ClearFrame` and `EmitBootstrapVisibleInstance`. The latter emits instance zero, 96,776 visible voxels, and the preprocess indirect arguments entirely on the GPU.

Promotion requires G1 and then G2. Do not begin preprocess acceptance until the exterior generator/ABI gate is green, then the clear/bootstrap GPU gate is green. The exterior generator produces exactly `128^3 - 126^3` canonical entries; every decoded grid coordinate satisfies the boundary predicate; the bootstrap visibility record/indirect arguments match the documented contract; no scene, TLAS, file, CWBVH, or legacy renderer data is needed to reach the first preprocess dispatch.

#### M1.2: Bring up SVRaster preprocess, binning, and ordering

- Implement and promote exactly one pass at a time in this order: G3 `PreprocessVisibleVoxels`, G4 duplicate scan, G5 reference-compatible 64-bit candidate emission, G6 stable two-word radix sort, and G7 tile ranges. The next pass must initially consume a checked fixture until its predecessor's real GPU output is accepted.
- Give every pass an isolated intermediate readback/CPU reference test and a chained-prefix test before implementing resolved material or G-buffer output.
- Validate packed rectangles, quadrant masks, counts, offsets, keys, stable order, and ranges first on small fixtures, then on the 96,776-voxel shell.
- Size capacities from the shell's measured projections and add deterministic overflow handling.

Promotion requires G3, G4, G5, G6, and G7 in order. Every intermediate buffer matches the CPU/reference result for the focused fixtures and fixed shell cameras; single-instance key/payload order matches SVRaster; the shell reaches valid tile ranges without CPU visibility readback or a real culling hierarchy.

#### M1.3: Resolve solid surfaces and write the full G-buffer

- Implement local-box segment/face calculation, exact nearest forward surface selection, deterministic depth ties, inverse-transpose world face normals, per-voxel material decode, resolved-surface records, and binary coverage.
- Implement `WriteVoxelGBuffer` against the existing Engine frame target: base color, geometry/shading normal, roughness/metallic/opacity/flags, emissive, motion vector, stable IDs, and hardware depth. Keep `DebugPresent` only as an attachment visualizer.
- Use single-voxel, depth-stack, face-normal, material-channel, camera-inside, transformed-normal, and temporal-motion fixtures for analytic validation and the exterior shell for the first interactive image.
- Exercise camera motion around and inside the shell, local quadrant/face changes, previous-frame camera/instance transforms, resize, empty output, and every G-buffer debug mode.

Promotion requires G8 before G-buffer commit work is accepted, then G9. The one-model/one-instance 128-cubed exterior shell renders interactively with finite deterministic base color, normals, material properties, emissive, motion, coverage, and depth in the real Engine G-buffer; all renderer passes are tested and no scene/TLAS/file code participates.

#### M1.4: Replace bootstrap visibility with the one-instance scene/TLAS

- Add `SparseVoxelSceneManager`, one BLAS, one instance, validation, derived inverse/previous transforms and transformed bounds, stable IDs/revisions, and the compact breadth-first BVH8 TLAS types/builder. Wrap the already-tested shell arrays rather than regenerating or adapting them.
- Implement TLAS root seed, depth-bounded indirect traversal, node/leaf-instance frustum tests, stable visibility flag scan/compaction, LOD seeds, and downstream indirect arguments.
- Add the same conservative frustum predicate to resident octree traversal before LOD evaluation, refinement requests, child enqueue, or selected-node emission. Keep frustum and future occlusion reason bits distinct.
- Replace `EmitBootstrapVisibleInstance` in the graph with the TLAS visibility producer. Do not modify preprocess, duplicate, sort, range, resolve, G-buffer commit, debug present, or their bindings.
- Test TLAS nodes, instances, and octree nodes inside, outside, and intersecting each frustum plane. Keep occlusion disabled and reserve its hook only.
- Compare the bootstrap and TLAS-producer visible records, indirect arguments, every downstream intermediate buffer, and final image for an in-frustum identity instance; they must be equivalent.

Promotion requires G10 for the scene manager and CPU TLAS before any GPU traversal is enabled, G11 for isolated GPU TLAS traversal/stable compaction and BLAS-node frustum traversal, and G12 for the producer swap. The real one-instance scene/TLAS producer can replace the bootstrap pass without any downstream raster code or ABI change; in-frustum output is equivalent, and an out-of-frustum instance or octree branch produces zero downstream work. Although G14-G17 were brought up against the bootstrap producer, G10-G12 are the next execution gates and must pass before G18 file-load cutover.

#### M1.5: Synthetic-renderer cutover and ray-path removal

- Apply the deletion matrix once the generated renderer is green.
- Remove `EmitBootstrapVisibleInstance` from the production graph and shader targets after the TLAS equivalence gate; retain its expected record as a small test fixture for the visibility contract.
- Delete the checked-in `sponza.evxl` and all EVXL readers, writers, tests, cache flags, documentation, and build wiring now. The generated shell remains the default sample input until the user previews an OBJ or opens a `.vsvo`.
- Remove legacy format probing from frame startup, all selectable ray/GI paths, trace-only acceleration builders and payloads, obsolete shader targets, UI, timings, and tests.
- Keep only genuinely shared OBJ parsing/material-sampling helpers behind the new offline builder API; retain no legacy voxel conversion code.
- Delete in small dependency-ordered batches. After each batch, run the affected stage tests plus the generated-render integration test so removal cannot silently alter output.
- Run the repository-wide removal audit and a clean build/test pass.

Promotion requires G13. Milestone 1 exits only after G1-G13 remain green together: Engine renders one generated 96,776-voxel exterior-shell BLAS through one TLAS instance using frustum culling and GPU indirect work; it writes the complete existing Engine G-buffer from per-voxel material data and exact box-face hits; the raster passes were proven independently through the bootstrap producer before TLAS integration; every active pass has a focused test; the voxel target has no scene ray-tracing path, CWBVH/BVH8/brick runtime type, legacy reader, or `softwarebvh` dependency. The only per-pixel ray/box operation is the local segment calculation inside the solid raster resolver.

### Milestone 2: Generate, deserialize, and render scene files

This milestone preserves the accepted preprocess, duplicate, sort, range, solid resolve, and G-buffer contracts from milestone 1. It may generalize the resident-hierarchy/LOD seam from dense model-local arrays to bounded physical page storage and add missing-refinement feedback before preprocess; it must not change SVRaster candidate ordering, surface resolution, material meaning, or G-buffer outputs.

#### M2.1: Versioned multi-model file writer and reader

- Generalize the already-defined scene/TLAS contract from the milestone-1 one-model/one-instance case to multiple BLASes, shared BLAS instances, transformed overlap, mixed levels, and deeper TLAS fixtures without changing raster pass order or buffer ABI.
- Finalize `VSVO` constants, 128-byte page records with conservative local AABBs, paged hierarchy/payload chunks, dependency and install/uninstall rewrite tables, writer, metadata reader, independent-page reader, exhaustive reader, checked allocation/error handling, instance transform records, and deterministic metadata-only TLAS construction. Re-run G14 and G15 after this pre-release schema rewrite; no compatibility reader for the earlier 96-byte development record is retained.
- Prove arbitrary dependency-closed page sets, not only file prefixes. Generate fixed and randomized resident subsets; replay installs in multiple valid orders and uninstalls in reverse dependency order; every published cut must contain exactly one renderable node on each occupied root-to-leaf path.
- Add golden-layout/AABB, independent-page None/LZ4, metadata-without-page-payload IO, install/uninstall cut validity, deterministic write, write-read-write, corruption, truncation, overflow, mismatched hierarchy arrays, invalid node/child/page ranges/AABBs, dependency cycles/forward edges, invalid transform/reference, invalid level/albedo/roughness/metallic/emissive/flags, non-opaque-alpha, and unsupported-density-chunk tests.
- Serialize every deterministic generated preset and feed the deserialized runtime scene into the milestone 1 renderer harness.

Then implement selective residency before accepting general file-backed rendering:

- Add a bounded physical page allocator and per-resident-node sidecar containing stable logical node ID and refinement-page ID. Translate logical file rewrites to physical child addresses only after the target page upload completes.
- Add GPU missing-refinement request emission to LOD traversal, page-ID deduplication, max-priority reduction across shared-BLAS instances, bounded request readback, and deterministic CPU scoring from page AABBs.
- Add the page-state machine, generation cancellation, asynchronous read/LZ4 decode queue, recursive dependency admission, upload batching, install barriers, resident publication, descendant-first uninstall, root pinning, and budget-driven eviction.
- Keep selecting the coarser resident parent while a request is pending, delayed, rejected, over budget, corrupt, or cancelled. No failure may create a hole, expose uninitialized children, invalidate an indirect dispatch, or discard the prior published cut.
- Retain `--load-all-pages` as a diagnostic comparison mode. At the same camera and LOD threshold, its selected logical-node set and every raster/G-buffer output must equal selective mode once all pages requested by that view have converged.

Promotion requires the revised G14 writer/AABB contract, then the revised G15 metadata/page/exhaustive reader contract, then G16 selective residency and multi-model/instance integration. Direct-generated and write/read-generated scenes have identical logical BLAS/instance data and deterministic metadata-built TLASes. For every tested dependency-closed resident set, CPU and GPU residency state, logical-to-physical translations, selected nodes, intermediate renderer buffers, and final GPU output are equivalent. Camera motion demonstrably changes requested spatial pages without loading unrelated fine pages, and measured resident memory stays within the configured budget.

#### M2.2: Analyze OBJ geometry and establish meter-space settings

- Add `SparseVoxelOBJAnalysis` as a non-voxelizing first phase. Parse geometry/material references, reject non-finite or structurally invalid input, retain explicit degenerate diagnostics, and calculate deterministic source-space AABB/center/extents/diagonal and mesh statistics.
- Add explicit unit presets and custom `metersPerSourceUnit`; convert measurements and later geometry to canonical meters. Do not infer units from filename, bounds, material names, or Sponza-specific knowledge.
- Add a pure settings-derivation function taking analysis, unit/axis conversion, requested voxel size in meters, maximum level and budgets. Return the snapped root cube, cells per axis, effective leaf size, conservative memory/work/page estimates, and actionable limit errors without allocating voxel output.
- Add the ImGui `Open OBJ...` and measurement/settings panels. Keep `Voxelize` disabled until analysis and derivation are valid. Preserve the old preview scene across open, parse, unit, and settings errors.

G17A promotion requires exact raw/meter measurements and derived-grid results for analytic OBJs under meter, centimeter, millimeter, inch, foot and custom scales; unit changes scale lengths but do not alter source measurements. Invalid scale, zero/negative/NaN voxel size, overflow, level-16 excess, huge coordinates, empty geometry and cancelled/stale analysis must produce diagnostics without voxel allocation or file writes.

#### M2.3: Voxelize asynchronously, preview, and optionally save

- Add the OBJ-to-BLAS builder over the accepted analysis/settings contract, plus model-local path encode/decode, solid occupancy, material conversion, deduplication, full-eight-sibling coalescing, hierarchy material averaging and VSVO page construction.
- Run analysis and voxelization through a bounded cancellable build controller. New source/settings generations make older completions stale. Publish a complete `SparseVoxelScene` atomically and keep the previous preview on cancellation or failure.
- Feed the in-memory build directly to the normal multi-model TLAS/selective-residency renderer. Frame the camera from meter-space bounds without changing global camera-speed units; expose the mesh AABB/axes overlay and build/scene/page statistics in ImGui.
- Keep serialization separate. `Save VSVO...` chooses a destination, confirms overwrite, writes atomically, performs metadata/page/exhaustive validation, and reports the output hash. Opening or voxelizing an OBJ must not create, truncate, or modify a `.vsvo` file.
- Keep small analytic fixtures for exact measurement, coverage, material and coalescing assertions. Use the checked-in Stanford Bunny and Suzanne OBJs as read-only real-mesh inputs for analysis, generation, unsaved GPU preview, explicit temporary-file save/reload, TLAS and full-G-buffer coverage. Tests write only beneath their disposable system-temporary directories and never generate or overwrite project assets.

G17B promotion requires analytic coverage/material/coalescing tests and deterministic direct preview rendering. G17C promotion requires cancellation/failure safety, explicit-save-only behavior, atomic overwrite handling, saved-file round trip, and direct-preview versus saved/reloaded full-G-buffer equivalence. G17 is complete only after G17A, G17B and G17C pass in order.

#### M2.4: Complete the user-selected file-backed workflow

- Make the editor open any user-selected `.vsvo` through metadata-first selective residency, while retaining the synthetic scene, interactive unsaved OBJ preview, and load-all-pages as diagnostic/authoring modes. Loading a `.vsvo` never invokes the OBJ parser.
- Keep output ownership with the user: previewing and opening sources never invents a destination, regenerates a sibling file, modifies an existing asset, or writes into the repository. Only `Save VSVO...` generates a file selected by the user.
- Validate files produced by the explicit-save path through metadata, independent-page and exhaustive readers, then render the deserialized instance/BLAS scene through the unchanged milestone-1 raster path.

Promotion requires G18. At least one analytic test fixture and one user-selected editor save begin with `VSVO`, write-read-write is byte-identical, and the saved/reloaded scene renders equivalently to its transient preview. No named project asset or generated-file check-in is a gate.

#### M2.5: Scene-file validation and pre-occlusion performance baseline

- Build from a fresh build directory and run all synthetic-scene, TLAS/pass, end-to-end renderer, file, deserializer, OBJ builder, and full Engine tests.
- Capture per-pass GPU timings and resident/transient memory at 1080p, including visible-instance/voxel/candidate counts, exact intersection tests, requested/read/decoded/uploaded/evicted pages and bytes, residency hit rate, coarse fallbacks, and time to converge for fixed cameras.
- Retain fixed-camera analytic and explicitly selected scene checks for non-empty finite full G-buffer attachments, valid unit normals/material ranges, coverage/depth, and duplicate capacity.
- Repeat the removal audit so the new loader has not reintroduced legacy trace payloads or compatibility conversion.

Milestone 2 exits only after G1-G18 pass together: an explicitly saved scene file opens metadata-first, builds its TLAS without fine payloads, pins roots, selectively converges visible branches within configured budgets, and renders each resident cut identically to the CPU/reference cut and load-all diagnostic mode. Explicit-save output and spatial request order are deterministic; camera-focused tests do not read unrelated fine payloads; no dependency violation, partial publication, validation error, overflow, NaN/Inf, legacy ray path, automatic asset generation, or runtime dependency on the ignored reference checkout remains.

### Milestone 3: Depth-only rendering and TLAS occlusion culling

This milestone changes frame scheduling and transient resources, not the sparse-voxel BLAS or VSVO schema. Frustum-only milestone-2 rendering remains available as the reference mode and must stay output-equivalent.

#### M3.1: Promote the material-free depth-only renderer

- Expose passes 0-7 plus `WriteVoxelDepth` as a runtime entry point accepting the same scene, camera, viewport, TLAS visibility contract, external/native depth target, load/clear policy, and Engine depth convention as the full renderer.
- Give the depth path its own minimal bind layout: transforms, geometry, resolved surfaces and depth only. Do not bind albedo, material properties, emissive, G-buffer color targets, or lighting resources.
- Support the normal one-pass frustum-only schedule first. Preserve instance/voxel IDs and coverage as optional diagnostics, but do not require G-buffer allocation.
- Compare standalone depth-only output with `WriteVoxelGBuffer` depth from the identical accepted G1-G18 scene/camera fixtures, including reverse/forward depth variants supported by the build.

Promotion requires G19. Depth-only and full rendering produce identical hit coverage, stable IDs and device/linear depth, while the depth-only pass has no material-buffer access and no color-attachment dependency.

#### M3.2: Reuse the fast SPD HZB

- Genericize `VirtualGeometryDepthPyramidPass` into a shared rendering pass, or add a thin shared wrapper around its scene-independent machinery; keep existing virtual-geometry callers/tests green.
- Reuse `depth_pyramid_spd_source_depth.wgsl`, `depth_pyramid_spd.wgsl`, and `depth_reduce_to_pot.wgsl` rather than forking voxel copies.
- Build provisional and final R32F HZBs from native scene depth, expose first-valid-mip/mip-count metadata, and preserve the shader's reverse-Z `min`/forward-Z `max` conservative reduction.
- Allocate per-frame-slot final HZB history and explicit validity metadata. Reset the SPD global counter for every build/continuation, declare RenderGraph depth-read/storage-write/sample transitions, and rotate history only after the complete final depth/HZB work is submitted.

Promotion requires G20. Every mip matches the scalar CPU reduction for power-of-two, odd, 1 x N, N x 1, tiny and resized targets; all supported 4-to-8-local-mip shader variants and multi-dispatch continuations pass without stale counter or barrier errors.

#### M3.3: Add isolated conservative TLAS occlusion tests

- Port only the generic projection/HZB behavior from `culling_multipass.wgsl`: conservative TLAS-node, instance, and resident-octree-node bound projection; near-plane fallback; first-valid-mip-aware level choice; configurable mip bias; conservative 2 x 2 samples; forward/reverse-Z comparison; and depth bias.
- Frustum-test before HZB work. Current-frame final culling tests internal TLAS nodes, leaf instances, and BLAS traversal nodes; an occluded current node skips its subtree. Previous-frame pre-culling tests leaf instances with previous transforms/camera and BLAS nodes with the previous instance transform unless validated previous TLAS-node bounds are available. Near-plane intersection, viewport escape, invalid projection, missing/invalid HZB, degenerate projected bounds, non-finite input, and any uncertainty return visible.
- Add separate `CulledFrustum`, `CulledOcclusion`, `VisibleUncertain`, and history-invalid debug reasons/counters. Keep occlusion disabled by default until the complete two-phase gate passes.
- Differential-test individual CPU/GPU decisions with hand-built HZBs before integrating the two-phase normal-render schedule.

Promotion requires G21. GPU TLAS node/instance decisions match the independent CPU oracle, disabling occlusion is byte-identical to milestone 2, and no definitely visible analytic bound is rejected.

#### M3.4: Integrate previous/current-HZB two-phase culling

- Add `PreCullTLAS`, temporal visible stable compaction, phase-1 SVRaster plus material-free depth, provisional current HZB, complete current TLAS/BLAS traversal, complete phase-2 actual-scene SVRaster/G-buffer or depth rendering, and final HZB rebuild in the documented schedule.
- Pre-cull uses the previous complete final HZB only when valid. Current culling traverses the full TLAS/BLAS against provisional depth and rerenders the full accepted set from scratch. Anything rejected by stale temporal history is absent from provisional depth and therefore remains visible to current recovery.
- Invalidate previous HZB on first frame, resize, camera cut, projection/depth-mode change, scene/instance topology reload, or explicit history reset. Ordinary camera/instance motion is handled by current-frame recovery rather than accepting one-frame holes.
- Compare occlusion enabled/disabled frame sequences, not only static screenshots. Include a moving occluder, moving revealed instance, camera strafing around an occluder, teleport/cut, resize, empty scene, overflow, and alternating visibility.
- Measure TLAS/BLAS nodes tested, HZB samples, temporal/current visible instances, duplicate candidates, exact voxel tests, temporal depth time, both HZB builds, actual-scene render time, final deferred work, and total frame time. Occlusion is a correctness feature only after equivalence; enable it by default only when representative scenes show a documented net benefit.

Promotion requires G22. For every deterministic sequence, two-phase occlusion produces the same final resolved-surface winner, depth, stable IDs and G-buffer as frustum-only rendering within the existing policies; current-frame recovery prevents temporal holes; overflow remains safe; and the performance report states where the extra temporal depth/HZB work wins or loses.

Milestone 3 exits only after G1-G22 pass together. The renderer supports standalone depth-only output and conservative TLAS occlusion using shared Engine HZB infrastructure, with no VSVO change, no CPU visibility readback, and no reintroduction of ray-tracing or legacy voxel code.

### Milestone 4: Real-time camera-centered packed occupancy volume

This milestone consumes the accepted resident `SparseVoxelScene` and TLAS but does not modify rendering output, VSVO, or the source BLAS. The result is one configurable, single-resolution, world-aligned occupancy volume centered on the camera's containing coarse cell. Multiple cascades, materials, density, distance fields, filtering, persistence to disk, and hierarchical traversal are out of scope for G23-G26. Milestone 5 derives the separately gated occupancy pyramid and sparse DDA only after this base contract is green.

#### M4.1: Lock the packed layout and coordinate model

- Add `SparseVoxelOccupancyVolume` settings/state and mirrored C++/WGSL records for `worldExtent`, `logicalResolution`, `wordResolution`, `cellSize`, `cameraCell`, `cameraInnerCell`, `originCell`, `wrapOffset`, generation, scene revision, validity, pending delta and failure state.
- Implement checked allocation math: `ceilDiv(Nx, 32) * Ny * Nz` `R32Uint` texels plus the same number of atomic mirror words. Validate device 3D-texture dimensions, buffer sizes, integer conversions and the configured memory ceiling before creating resources.
- Implement independent CPU and WGSL logical-to-physical, word/bit, world lookup, camera-relative center, positive-modulo, tail-mask and newly exposed slab helpers. Use mathematical floor for negative positions.
- Add a tiny occupancy consumer/probe shader and a slice debug view before implementing scene splats, so address decoding is proven independently with manually uploaded bit patterns.

Promotion requires G23. Arbitrary valid dimensions—including nonmultiples of 32—address every logical cell exactly once, never expose padded bits, and reconstruct camera-relative positions from integer camera cells plus inner-cell coordinates without rebuilding while the camera stays inside one cell.

#### M4.2: Build the complete volume from the resident scene

- Allocate the persistent `R32Uint` 3D texture and atomic mirror, plus the bounded dirty-word and resident source buffers.
- Treat initialization as every valid bit being dirty. Stably compact every physical word with its valid-bit mask.
- Dispatch one invocation per resident sparse leaf (or its conservative world AABB), with no frustum or HZB dependency. Optionally use the TLAS volume query later to reduce invocations.
- Add `ClearDirtyWords`, direct dirty-cell `atomicOr` splatting, unique dirty-word texture copy and committed-state publication in that order. Do not build intermediate word-splat records.
- Define conservative transformed filled-box-to-coarse-cell overlap centrally. Confirm that appearance, source path ordering and duplicate instance enumeration cannot change final occupancy bits.

Promotion requires G24. Small analytic boxes, the synthetic shell, transformed shared BLAS instances and a loaded `.vsvo` fixture match the scalar CPU bit volume exactly at every word. Fixed source/dirty capacities are validated before destructive work, texture reads observe the copied generation, and no source material resource is bound.

#### M4.3: Add incremental toroidal camera scrolling

- Compute pending `cameraCell`, `cameraInnerCell`, `originCell`, signed cell delta and new wrap offset each frame. A sub-cell-only move updates metadata and dispatches zero clear/splat/copy work.
- For ordinary cell motion, classify the union of newly exposed X/Y/Z slabs into unique physical word/mask records. OR explicitly invalidated interior AABBs into the same masks. Clear partial words by bit mask, preserve retained bits, resplat current source boxes through those masks, and copy only dirty physical words.
- For a moved instance invalidate `oldBounds ∪ newBounds`; for removal/eviction invalidate old bounds; for addition/residency invalidate new bounds. Overlapping unchanged sources must be resplatted because clearing is regional rather than source-owned.
- Reuse freed physical slots through the wrap offset; never copy or shift retained volume contents. Use full rebuild for first use, teleport by at least one full dimension, resize/configuration change, invalid state, or any scene revision lacking complete invalidation bounds.
- Compare every incremental result with an independent from-scratch rebuild at the same camera and scene before allowing the incremental generation to be accepted.

Promotion requires G25. Long fixed-seed walks across positive, negative, wrap, diagonal, boundary, and sub-cell positions plus transform/add/remove/residency edits remain bit-identical to full rebuilds; updates regenerate only the expected slab/interior union and leave all retained physical bits unchanged.

#### M4.4: Integrate, harden, and measure the occupancy volume

- Schedule occupancy update after current camera/scene transforms are known and before any declared consumer. Add explicit atomic-buffer write/read and 3D storage-write/sampled-read RenderGraph transitions.
- Publish committed and pending metadata separately. Inject overflow/allocation/command-admission failures at every stage and prove that the prior committed texture/state remains readable, while a failed first build remains invalid. Restore an internally modified mirror from the committed texture before retry.
- Add Sponza camera paths, empty/outside-scene volumes, runtime enable/disable, resize, extent/resolution edits, camera teleport and scene-reload coverage. A scene transform/topology revision forces a full build for milestone 4.
- Expose debug slices, occupied-bit count, dirty-word ratio, source-instance/leaf counts, splat count, bytes, generation and per-pass timings. Measure stationary, sub-cell, one-cell, diagonal, wrap, teleport and full-build cases.

Promotion requires G26. The sampled 3D texture is bit-identical to the CPU reference and a same-frame full rebuild across all deterministic integration paths; no partial generation is published; retained data is never physically shifted; and representative one-cell Sponza movement demonstrates that dirty-word/source/splat work is substantially below a full rebuild. Milestone 4 exits only after G1-G26 pass cumulatively.

### Milestone 5: Globally anchored occupancy pyramid and sparse DDA

Milestone 5 derives a conservative hierarchy from the committed mip-0 occupancy volume. It does not resplat scene leaves at higher levels. A level-\(L\) bit is set exactly when at least one covered level-\(L-1\) child bit is set, so a clear ancestor proves that every represented base cell below it is clear. An occupied ancestor is only conservative evidence and requires descent. The hierarchy remains transient runtime state and is never serialized into VSVO.

Do not use ordinary hardware mip generation or blindly reduce adjacent physical texture coordinates. Packed X dimensions do not generally follow hardware mip dimensions—for example, 129 logical X cells require five base words and three level-1 words, while a hardware mip of a five-texel image is only two texels—and a toroidal seam can make physically adjacent cells globally unrelated. Keep mip 0 in the public `R32Uint` 3D texture. Store higher levels in exact packed-word ranges with explicit descriptors and manual integer loads; separate per-level textures may be evaluated later only as a measured cache optimization.

For level `L`, let `scale = 1 << L` and anchor cells to the global base grid:

```text
globalMipCell = floorDiv(globalBaseCell, scale)
levelOrigin   = floorDiv(baseOriginCell, scale)
activeEnd     = floorDiv(baseOriginCell + baseResolution - 1, scale)
activeCount   = activeEnd - levelOrigin + 1
```

Use mathematical signed floor division for negative coordinates. Because base origins need not align to `scale`, allocate a fixed worst-case resolution per axis:

```text
levelResolution = ceilDiv(baseResolution + scale - 1, scale)
wordResolution  = (ceilDiv(levelResolution.x, 32),
                   levelResolution.y,
                   levelResolution.z)
```

Cells above `activeCount` are padding and remain zero. Every level owns an independent `originCell`, `activeResolution`, `wrapOffset`, packed word range, and scale. When its origin changes by `deltaOrigin`, update `wrapOffset = positiveModulo(oldWrapOffset + deltaOrigin, levelResolution)`. A retained global mip cell then resolves to the same physical slot before and after the scroll, so no retained word or bit is copied or shifted.

#### M5.1: Lock hierarchy layout and full reduction

- Add checked CPU/WGSL level descriptors containing level, scale, fixed logical/word resolution, active resolution, signed global origin, wrap offset, packed word offset/count, validity and shared content generation.
- Build exact level ranges until `scale` reaches the largest base resolution. Count the committed hierarchy and its failure-safe mutation copy in the occupancy memory ceiling.
- After mip 0 is complete, build every higher level bottom-up. One invocation owns one destination word, evaluates only valid bits, loads children through global-to-logical-to-physical helpers, and writes the exact OR of up to eight children. Padding and tail bits stay zero.
- Validate full builds against an independent scalar pyramid for arbitrary, odd, non-cubic, negative-origin and wrapped volumes. Every occupied base bit has every ancestor set; no clear ancestor contains an occupied descendant.

Promotion requires G27. All higher packed levels and descriptors match the CPU oracle bit-for-bit, ordinary physical adjacency is never used across a toroidal seam, arbitrary valid base dimensions need no hardware-mip assumption, and consumers observe one coherent base/pyramid generation.

#### M5.2: Propagate incremental toroidal changes

- For a base scroll, project both outgoing old-world cells and incoming new-world cells into globally anchored parent cells. The outgoing projection is required because a parent can clear when its last represented child leaves the volume.
- At each level, stably compact unique dirty physical word/masks under both old and pending mappings. Clear/recompute only those bits using the current child level:

```text
newWord = (oldWord & ~dirtyMask) | (recomputedBits & dirtyMask)
```

- Propagate every touched parent cell upward initially. A later optimization may propagate only `oldWord XOR newWord` bits after equivalence is established.
- Scroll each level independently when its global origin crosses a `2^L` boundary. Reuse freed slots through its wrap offset, clear invalid padding, and use a level/full rebuild only for invalid history, resize, scene revision or a level-sized teleport.
- Publish base and all hierarchy levels under one content generation. Preserve or restore the previous committed hierarchy when any pending update stage fails.

Promotion requires G28. Fixed-seed positive, negative, diagonal, wrap and teleport walks match full pyramid rebuilds after every step; retained global cells keep their physical slots; one-cell motion touches only projected outgoing/incoming regions plus their ancestors; and no stale padding or parent bit survives.

#### M5.3: Add conservative sparse DDA

- Intersect rays with the exact mip-0 world AABB, reconstruct grid coordinates from signed camera/global cells rather than large absolute floats, and query the coarsest useful level containing the current point.
- When a queried cell is clear, advance directly to that global mip cell's exit boundary. When it is occupied, descend until mip 0. Guarantee strictly positive progress for boundary starts, zero direction components, negative directions and tied corner/edge crossings.
- Define the first consumer as approximate occupancy traversal: a mip-0 hit identifies a conservative coarse occupied cell. Treating that hit as a shadow blocker is explicitly approximate and may over-shadow. Any future exact hit path must use occupancy only for empty-space skipping and resolve against additional resident-scene data because the bit volume stores no source IDs.
- Add step-count, selected-level, skipped-distance, hit/miss and generation diagnostics. The DDA reads only a completely published hierarchy generation and fails closed to the slower mip-0 walk when hierarchy state is invalid.

Implement G29 in this order:

1. Extract reusable CPU/WGSL occupancy queries for mip 0 and packed higher levels. Each query takes a signed global cell, checks the level's active range and generation, then performs global-to-logical-to-toroidal-physical addressing. Traversal never steps in physical texture coordinates.
2. Add an independent scalar mip-0 CPU DDA over the exact volume AABB. Its result records status, first occupied global cell, parametric distance, query count and base-cell steps.
3. Add the CPU hierarchical DDA. At every current point, start at the highest active level containing it; descend through occupied cells, skip to a clear cell's world-space exit plane, and restart from the highest usable level. The coarsest level may contain multiple active cells and is not treated as a single root.
4. Centralize robust progress rules: direction-biased boundary cell selection, infinite exit time for zero axes, simultaneous edge/corner crossing, `nextafter(tExit, +infinity)` advancement, volume-exit clamping and a derived conservative query limit.
5. Add a batch-ray GPU ABI and RenderGraph probe pass binding the public mip-0 texture, packed mip words, level descriptors and pyramid state. Invalid or generation-mismatched pyramid state falls back to mip-0 traversal rather than returning empty.
6. Validate analytic and fixed-seed randomized rays against the scalar oracle across negative origins, toroidal camera walks and interior transform/add/remove invalidations. Require identical hit/miss and first occupied base cell plus reduced queries on sparse fixtures.
7. Only after coherent committed base-plus-pyramid publication is enforced, connect the first approximate shadow/visibility consumer and expose selected-level, skipped-distance, step-limit and generation diagnostics.

Promotion requires G29. CPU/GPU hierarchical DDA agrees with a scalar mip-0 DDA on hit/miss and first occupied coarse cell across analytic and randomized rays, never skips an occupied base cell, terminates with bounded forward progress, remains correct across every toroidal camera walk, and demonstrates reduced cell steps in sparse fixtures.

Implementation status (2026-07-23): the checked CPU hierarchy, packed GPU ABI, reusable RenderGraph resource chain, public `R32Uint` 3D-texture publication, globally anchored mip reduction, and Vulkan buffer/texture equivalence tests are implemented. Production volume generation is GPU-only. The CPU performs an O(level-count + invalidation-count) metadata update and uploads at most three camera-slab/interior-invalidation boxes; it neither allocates scalar base/mip word mirrors nor scans base cells, rasterizes sources, enumerates source leaves, builds dirty-word vectors, sorts/uniques work, or propagates mip dirtiness. Incremental GPU marking now visits only the packed words covered by those regions, analytically handles 32 logical X cells per invocation, atomically deduplicates physical words, and drives base clear/publication through indirect dispatch. Before marking, camera-generated entering slabs are paired with the physical slabs they recycle from the old toroidal volume and culled on the GPU when neither side overlaps a scene instance. Diagonal scrolls evaluate the outgoing slab against the complete old-volume range on the other axes. A camera move whose entering and outgoing regions are known empty therefore commits the new toroidal metadata with zero dirty mip-0 words. The resulting GPU dirty-word count, rather than the CPU-submitted query count, also writes every upper-mip indirect dispatch: rejected empty motion executes no base or mip reduction work. Explicit old/new transform or residency invalidations are never removed by this camera-only cull. Full initialization uses direct bounded word dispatches so it does not contend on a global append counter. GPU passes then reject scene instances outside the surviving dirty boxes, indirectly traverse their resident BLAS nodes, transform and atomically splat reached leaves, and publish only listed base words. There is no CPU source AABB/leaf list, transformed-AABB scratch list, atomic source compaction, per-source count/scan, emitted word-splat list, or variable splat-record capacity.

The production sample owns and schedules this resource chain before consumers and binds the renderer's existing instance, BLAS-root, hierarchy-node, and center/length buffers. The current intermediate implementation still scans the instance array for dirty-box overlap before its per-level indirect BLAS queues, and every non-empty GPU dirty generation still rebuilds the complete packed occupancy pyramid. Rejected empty camera motion now skips that pyramid entirely, but these remain the two explicit performance blockers for genuinely dirty regions. The next implementation step is the packed dirty HPB plus occupancy-specific TLAS traversal described above, followed by indirect dirty-parent propagation for the occupancy pyramid. Metadata-first streaming updates the same physical hierarchy buffers, so occupancy sees the current resident cut without rebuilding `SparseVoxelSceneManager` or uploading a separate CPU leaf list. Instance transforms currently supply old/new root bounds; camera movement independently dirties only newly exposed toroidal slabs. Static/sub-cell frames take a zero-work indirect path. The scalar incremental dirty-word/mip path remains the independent oracle.

Focused Vulkan coverage now compares both the direct leaf-splat oracle path and the production instance/BLAS traversal bit-for-bit with the scalar oracle for a full build, a toroidal multi-axis camera scroll, and transformed-instance old/new invalidation, including every packed mip word. Exact AABB boundaries are selected in cell coordinates rather than with subnormal floating-point offsets, fixing a zero-plane false-empty case on flush-to-zero hardware. Bounded renderer regressions require nonzero camera-primary hits after the default build, after recompiling with a larger occupancy cell, and on the last frame of an eight-frame camera-yaw walk. The CPU no longer performs occupancy leaf enumeration or uploads on camera/residency changes.

G29 now has independent scalar mip-0 and hierarchical CPU traversal, robust boundary-biased/zero-axis/next-representable progress, query-limit and skip diagnostics, analytic cases, fixed-seed randomized equivalence across toroidal walks and an interior transform, plus a Vulkan batch-ray pass that reads the public base texture and packed mips. GPU cell ownership at an exact boundary is selected in cell-coordinate space instead of with a subnormal world-space offset, because hardware flush-to-zero may otherwise leave camera rays that start on world-zero planes repeatedly querying the same coarse cell. The GPU consumer validates base/pyramid generations and falls back to mip 0 when hierarchy state is invalid; focused tests cover hits, misses, negative/zero directions, distance limits, camera scroll, interior invalidation, exact-boundary perspective rays and forced fallback, plus 256 deterministic randomized GPU rays per published state. Base state, level descriptors and pyramid state are uploaded to frame-local staging buffers and copied to persistent committed buffers only after the final mip pass, so a same-frame consumer cannot observe a new generation with incomplete descriptors or hierarchy data. A staged initial full rebuild must remain pending through its first GPU submission; a same-state CPU update must not replace it with an empty dirty set before the base splat executes. The sample has two opt-in half-resolution consumers. `Occupancy shadow` reconstructs resolved world surfaces, emits surface-to-directional-light rays, runs the shared hierarchical DDA, and displays conservative hit/miss blockers. `Occupancy hit cell` emits camera-primary rays without requiring raster coverage and displays the exact signed global mip-0 cell stored in each successful DDA result; a deterministic hash is used only to turn that triplet into a debug color. Misses are black and query-limit failures are red. The bounded debug launch also reads back hit/miss/query-limit counts so camera-primary coverage cannot silently regress to an empty volume or a non-progressing DDA. Ray count is zero outside these views, so normal rendering performs no occupancy queries beyond lightweight early-out dispatches. These views are intentionally approximate and do not claim an authored source/object intersection because the 1-bit structure stores occupancy only.

The remaining hardening work is per-pass GPU dirty-work/performance telemetry and UI exposure for the diagnostics already recorded in each DDA result. A stronger device-loss recovery scheme may double-buffer the words and texture later; it is not required to complicate the direct splat algorithm.

## Test plan

Create focused binaries/fixtures rather than extending the legacy monolithic tests:

- `SparseVoxelCPUReferenceTests`: independent scalar pass pipeline, analytic cases, pinned SVRaster parity fixtures, and fixed-seed properties.
- `SparseVoxelBootstrapTests`: exterior-shell generation, BLAS-array validation, bootstrap visible-instance output, and exact indirect arguments.
- `SparseVoxelSceneTests`: generated BLAS/instance data, validation, transforms, and ABI layout.
- `SparseVoxelTLASTests`: deterministic CPU build and GPU culling/compaction/indirect arguments.
- `SparseVoxelSVRasterParityTests`: identity-instance packed bounds, quadrant bitsets, duplicate counts, 64-bit keys, stable order, tile ranges, and first-hit behavior against small independently recorded reference fixtures.
- `SparseVoxelRasterPassTests`: preprocess through resolved surface, full G-buffer commit, and debug present, with intermediate readback entry points.
- `SparseVoxelSceneFileTests`: milestone-2 writer, reader, corruption, and render equivalence.
- `SparseVoxelStreamingTests`: milestone-2 CPU scheduler and GPU request/install/uninstall tests, physical-allocation translation, dependency safety, spatial priority, delayed/failing IO, and eviction.
- `SparseVoxelOBJAnalysisTests`: raw/meter measurements, unit presets/custom scale, root-grid derivation, estimates, diagnostics, and no-allocation analysis behavior.
- `SparseVoxelOBJBuilderTests`: offline voxelization/coalescing and deterministic fixtures.
- `SparseVoxelOBJAuthoringTests`: headless ImGui state-model events, async generation/cancellation, preview publication, explicit-save-only behavior, CLI parity, and preview/save/reload rendering equivalence.
- `SparseVoxelRendererIntegrationTests`: headless generated and deserialized fixed-camera frames.
- `SparseVoxelDepthOnlyTests`: milestone-3 minimal bindings, depth commit, full-path equivalence, resize/load/clear and depth-convention fixtures.
- `SparseVoxelHZBOcclusionTests`: scalar/GPU HZB mips, isolated TLAS decisions, temporal pre/final culling, recovery and frame-sequence equivalence.
- `SparseVoxelOccupancyVolumeTests`: milestone-4 layout/addressing, full build, conservative splats, toroidal dirty masks, incremental/full equivalence, texture consumers, failures and performance counters.

Register each in the voxel test CMake wiring with stage labels `svraster.g01` through `svraster.g26`, plus cumulative `svraster.m1`, `svraster.m2`, `svraster.m3`, and `svraster.m4` labels. Keep small buffers and deterministic CPU or analytic references so a failed stage reports its first differing record/pixel/word, key words, bit masks, instance/voxel IDs, and expected/actual counts. CI promotion runs the current gate label and all earlier labels; milestone exits also run the full Engine suite.

### CPU-reference oracle tests

- Verify the CPU oracle against hand-computed single-voxel projections, tile coverage, scans, keys, ranges, entering/exiting face normals, material decode, G-buffer values, motion vectors, and device depth before comparing any GPU pass to it.
- Verify its identity-instance key, ordering, range, and first-valid-hit outputs against immutable fixtures captured from the pinned SVRaster revision. Store the fixture-generation command and reference commit beside the fixture, but do not require Python, CUDA, or the checkout at test runtime.
- Run the complete scalar pipeline on tiny 8 x 8 and 32 x 24 targets and compare it with independently enumerated expected pixels for empty, single-voxel, depth-stack, six-face-normal, material-channel, camera-inside, eight-octant, tile-edge, temporal-motion, and transformed-instance scenes.
- Cross-check optimized CPU helpers used by production scene construction, such as path decode and frustum math, against the independent test oracle rather than using a production helper as its own expected result.
- Compile/link enforcement must prove `SparseVoxelRasterReference` is referenced only by test targets. The sample, runtime library, asset compiler, and shipped shader packages must have no dependency on it.
- Centralize named comparison policies: exact discrete/packed-material equality, projection-bound equality after specified rounding, normalized-normal angular tolerance, half-float/material decode tolerance, motion tolerance, and depth `abs/rel` tolerance. A test may not choose a wider local tolerance without changing and reviewing that policy.
- Run fixed-seed property cases in normal CI and a larger seed/count matrix in a scheduled or pre-merge stress job. Always print and persist a reproducible failing seed.

### Milestone-1 exterior-shell tests

- Exterior shell contains exactly 96,776 entries; all levels equal 7 and all paths are unique/canonical.
- Decoding every path yields integer grid coordinates in `[0, 127]`, and every emitted coordinate satisfies `x in {0,127} || y in {0,127} || z in {0,127}`.
- Every boundary coordinate is present exactly once and no coordinate with all components in `[1,126]` is present.
- CPU path decode reproduces the expected local center/full side length `rootExtent / 128` and matches resident upload arrays.
- For a fixed seed, every path's pseudo-random albedo, roughness, metallic, emissive, and material-ID words are byte-identical across repeated generation and enumeration orders; albedo alpha is 255, sRGB channels respect the configured lower bound, scalar properties are in range, emission is finite/non-negative, and reserved bits are zero.
- Golden path/seed pairs produce the documented SplitMix64-derived complete packed material exactly, including half-float emission rounding.
- Changing only the material seed leaves paths/levels/centers unchanged while changing a statistically meaningful majority of material records; collisions are allowed and material uniqueness is not an invariant.
- The shell contains both emissive and non-emissive voxels and spans documented roughness/metallic intervals so milestone-1 G-buffer validation cannot accidentally test only default values.
- The shell-specific test rejects a wrong count or any interior coordinate. Generic `validateSparseVoxelBLASArrays` accepts the valid shell and rejects invalid levels/paths, mismatched parallel arrays, duplicate paths, non-opaque albedo, non-canonical properties/emission, reserved bits, and unsupported flags without imposing the shell predicate on future models.
- `EmitBootstrapVisibleInstance` writes instance 0, base 0, count 96,776, visible-instance count 1, total-visible-voxel count 96,776, and exact preprocess indirect arguments after every clear.
- Small single-voxel, depth-stack, eight-octant, tile-boundary, empty, and overflow fixtures remain deterministic and validate without constructing a scene/TLAS.
- C++/WGSL ABI reflection matches every documented size, alignment, array stride, and field offset.

### Generalized scene tests, late milestone 1 and milestone 2

- Wrapping the shell in a one-BLAS/one-instance `SparseVoxelScene` preserves all parallel arrays byte-for-byte.
- Transform/inverse tests cover identity plus the fixed milestone-1 transform; singular, non-finite, or mismatched inverses are rejected.
- Instance world AABBs contain all eight transformed BLAS-root corners.
- The one-instance TLAS has valid ranges, acyclic reachability, exactly-once membership, parent containment, and deterministic construction.
- Bootstrap and TLAS visibility producers emit equivalent records/arguments for an in-frustum identity instance; the TLAS producer emits zero raster work outside the frustum.
- Multi-model, shared-BLAS, overlapping-instance, random adaptive, mixed-level, and deeper-TLAS property tests are milestone-2 generalization coverage rather than raster bring-up prerequisites.
- Page metadata AABBs exactly and conservatively bound every node cube in the page; transforming them through all shared-BLAS instances preserves containment.
- Fixed cameras looking at disjoint model regions request disjoint fine-page sets after common dependencies; moving the camera changes priority deterministically without requiring unrelated page payload reads.
- A fine-page request recursively admits missing ancestors. Random valid completion orders never publish a page before its dependencies, while dependency cycles, forward edges, duplicate edges, and cross-model edges are rejected.
- With delayed IO, traversal continues to select the coarse parent. After install it selects the finer children; after descendant-first eviction it returns to the identical coarse parent without a missing or overlapping interval.
- Logical page/node IDs and final output are invariant under randomized physical page-slot allocation, upload batching, and shared-instance enumeration order.

### Renderer-pass tests

Build these as GPU tests with small fixed buffers and read back the output of each pass. Do not rely only on a final screenshot to diagnose ordering failures.

- **`ClearFrame`:** all active counters, visible records, tile ranges, indirect-argument slots, overflow/debug records, resolved-surface/coverage images reset to defined values; G-buffer attachments use their defined render-pass clears; late-M1 TLAS queues/flags are included once allocated, and no stale previous-frame entry survives.
- **`EmitBootstrapVisibleInstance`:** the exact one-instance record, counts, and indirect arguments are written on-GPU; corrupt/stale values from a previous frame are overwritten after clear.
- **`PreprocessVisibleVoxels`:** one invocation per visible voxel produces SVRaster-style packed projected rectangles, local camera quadrant bitsets, instance/voxel IDs, and duplicate counts matching the CPU/reference calculation for on-screen, clipped, off-screen, image-edge, near-plane, identity, and the fixed transform.
- **`ScanDuplicateCounts`:** per-visible-voxel exclusive offsets, total candidates, emission arguments, and capacity diagnostics match a CPU scan for zero and non-workgroup-aligned counts.
- **`EmitTileCandidates`:** every unsorted 64-bit key equals SVRaster's `(tileId << 48) | (path XOR quadrantPattern)` calculation; full 48-bit ranks survive; instanced payload fields and deterministic order match CPU emission for shared BLASes, tiles, quadrants, and equal keys.
- **`SortTileCandidates`:** stable full 64-bit order matches CPU sort for both words, all quadrants/levels, equal keys, tile transitions, and counts spanning radix workgroups. The single-identity-instance fixture must match the reference key/payload order exactly apart from the widened payload representation.
- **`BuildTileRanges`:** first/end pairs match CPU ranges for empty, first, middle, last, single-entry, and repeated-entry tiles, with no stale ranges after a smaller next frame.
- **`ResolveSolidVoxels`:** full tile-range scanning, exact `tEnter/tExit`, first forward surface/local face, instance/voxel IDs, binary coverage, surface distance, camera-inside behavior, compact record layout, and miss values match analytic cases and the CPU resolver within documented tolerances.
- **Solid adjacency, faces, and ties:** rays hit all six faces plus shared edges/corners; camera-inside rays choose the exit face; documented epsilon/tie rules produce stable IDs and normals without cracks or flicker.
- **Opaque semantics:** a hit never blends with farther shell voxels or the background.
- **Per-voxel materials:** two voxels with identical geometry and different albedo, roughness, metallic, emission, material ID, or flags change only the expected winning G-buffer channels. A farther voxel's material never leaks into the nearest result; material UV is always exact zero in version 1.
- **`WriteVoxelGBuffer`:** albedo linearization, geometry/shading normals, material-properties packing, HDR emissive, stable IDs, and fragment depth match CPU/analytic outputs for every hit; misses retain exact clear values. Attachment formats and channel meanings match the existing Engine frame target.
- **G-buffer normals:** identity, rotation, reflection, non-uniform scale, face/edge ties, and camera-inside exit faces produce finite unit normals with the expected orientation; geometry and shading normals are identical in version 1.
- **G-buffer depth:** projected winning world positions reproduce the expected Engine device depth across near/far boundaries and the active normal/reversed depth convention; depth testing against a known raster primitive selects the nearer surface correctly.
- **G-buffer motion:** static camera/static instance yields zero; camera-only, instance-only, combined motion, first frame, and previous-state reset match CPU reprojection. Disocclusion/miss pixels retain the cleared motion value.
- **`DebugPresent`:** every G-buffer/debug attachment reaches the intended render target after resize/recreation and uses the correct visualization and color-space conversion; it does not alter G-buffer contents.
- **Late-M1 `SeedTLASRoot`:** the one-instance scene writes exactly its configured root; an empty scene emits zero work and invalid roots fail CPU validation before dispatch.
- **Late-M1 `PrepareTLASDispatch[d]`/`TraverseTLAS[d]`:** indirect arguments, queue counts, and the one instance's inside/outside/intersecting decisions match CPU frustum results for every clip plane.
- **Late-M1 visibility compaction/prepare:** the in-frustum result is byte-identical to `EmitBootstrapVisibleInstance`; an out-of-frustum result has zero visible instances/voxels and a zero preprocess dispatch.
- **Late-M1 producer swap:** bootstrap and TLAS paths produce identical preprocess, duplicate, candidate, sorted, range, resolved-surface, full G-buffer, and debug-present outputs for the identity shell camera.
- **Milestone-1/2 occlusion-disabled contract:** varying/poisoning the reserved HZB input cannot change TLAS output while the occlusion flag is off.
- **Milestone-2 cross-instance nearest hit:** overlapping translated/rotated/scaled instances select the exact nearest solid voxel even when TLAS order, storage order, and local Morton order differ; reversing TLAS/instance storage order does not change the image.
- **Milestone-2 coincident ties:** coincident voxels, equal-depth instances, and reflections use deterministic instance/voxel tie breaking.
- **Milestone-2 serialized opacity:** the authoring compiler canonicalizes alpha to 255 and the loader rejects any other serialized value.
- **End-to-end generated render:** every fixed exterior-shell G-buffer attachment is finite, non-empty where appropriate, materially varied, deterministic within GPU tolerance, and free of validation errors; the attachment set can be consumed by an existing deferred-lighting test without voxel-specific shading.
- **Failure behavior:** empty scene and every bounded-buffer overflow complete safely, zero unsafe downstream dispatches, and surface the expected stage/capacity diagnostic.

### File-generation and deserialization tests

- Header and chunk-table golden-layout test, including the 128-byte page record and exact conservative page AABBs.
- None/LZ4 round trips.
- Deterministic byte-for-byte output.
- Write-read-write byte identity for all named generated presets.
- Direct-generated versus deserialized BLAS path/level/albedo/roughness/metallic/emissive/material-ID/flags/instance equality and deterministic rebuilt-TLAS equality.
- Direct-generated versus deserialized resolved-surface and every G-buffer attachment equivalence.
- Source/settings hash mismatch reporting.
- Corrupt checksum, bad magic/version/endian, duplicate/missing chunks, overlapping/out-of-bounds chunks, truncation, integer overflow, count/record-size mismatch, decompression failure, and trailing-data policy tests.
- Invalid scene payload tests: mismatched parallel array counts, overlapping BLAS ranges, invalid instance BLAS index, singular/non-affine/non-finite transform, duplicate path, ancestor overlap, invalid level/albedo/property/emission, non-canonical order/half-float/reserved bits, non-opaque alpha, unsupported material flags, and a density chunk unsupported by version 1.
- One BLAS with many instances and many BLASes with one instance each preserve sharing and canonical order.
- `worldToModel`, instance world bounds, and TLAS are regenerated rather than trusted from serialized derived data.
- Random path/level encode-decode property tests.
- Adaptive-set uniqueness and non-overlap property tests.
- Sponza metadata and full-payload load test.
- Metadata-only open performs no `PagePayloads` read or allocation. Individual reads touch only the requested aligned compressed ranges, and the exhaustive loader remains byte/logically identical to loading every page through the streaming API.
- Arbitrary dependency-closed resident sets, randomized valid install orders, descendant-first uninstall orders, and same-level nonadjacent spatial selections all reproduce the scalar authored cut.

### Selective page-streaming tests

- CPU priority goldens cover frustum rejection, near-plane fallback, projected-error ties, distance ties, shared-BLAS max reduction, level ordering, page-ID tie breaks, and deterministic camera paths.
- GPU request emission returns exactly the missing refinement page for an oversized resident leaf, deduplicates repeated nodes/instances, reduces the maximum priority, respects request capacity, and emits nothing for installed children, terminal leaves, culled instances, or below-threshold nodes.
- Recursive admission never schedules a page ahead of a missing dependency. A fine request with a cold chain installs ancestors level by level and renders a valid cut after every completed batch.
- Physical allocation tests randomize page bases and holes. Every translated child pointer lands inside the installed physical page, materials/path/center data remain parallel, and stable logical IDs/ties remain unchanged.
- Install tests poison destination buffers, delay copies, and inspect the pre-barrier state; `childCount` cannot become nonzero until payload and sidecars are complete. Uninstall sets parents to coarse leaves before any child allocation is reused.
- Root pages remain pinned. Eviction rejects resident dependents, orders descendants before ancestors, honors byte/node/page budgets, and converges to the CPU LRU/spatial policy with deterministic ties.
- IO/decode/upload failure, cancellation, scene reload, stale generation completion, checksum failure, request overflow, allocator exhaustion, and rewrite-capacity failure preserve the last published dependency-closed cut and produce bounded diagnostics.
- Selective and `--load-all-pages` modes produce identical logical selected nodes and every raster/G-buffer attachment after requested pages converge. Before convergence, selective output equals the independently computed coarser resident cut.
- Instrumented file reads prove cameras focused on one branch do not read unrelated fine-page payloads; resident nodes/bytes never exceed configured budgets except for an explicitly checked indivisible-page admission rule.

### OBJ generation tests

- Hand-authored OBJ fixtures have exact source-space min/max/center/extents/diagonal, vertex/triangle/object/material and degenerate counts before any voxel output is allocated.
- Meter, centimeter, millimeter, inch, foot and custom `metersPerSourceUnit` presets produce exact meter-space measurements. OBJ units remain explicitly unknown until selected; no heuristic changes the scale.
- Unit and axis conversions affect all positions, model root extents and instance translations consistently. Camera framing and the bounds overlay use the converted meter-space bounds.
- Grid derivation goldens cover centered/off-origin/non-cubic meshes, boundary-aligned extents, levels 1 and 16, next-representable values, requested versus effective voxel size, and the snapped root origin/cube.
- Invalid/zero/negative/non-finite scales or voxel sizes, empty/non-finite geometry, level overflow, checked allocation overflow and configured memory/work limits disable voxelization with exact diagnostics and no output allocation.
- Estimates conservatively bound emitted cells, temporary bytes and expected page counts for analytic fixtures; changing only presentation/UI state does not change the settings hash.
- Conservative voxel coverage fixtures.
- Model-local root-cube and path assignment, with scene placement retained as instance transforms.
- Coincident-cell deduplication and coverage-weighted linear albedo/roughness/metallic/emissive combination, including deterministic folding of any source vertex color/base-color factor into albedo.
- Solid occupancy from conservative source coverage; partial coverage changes authoring material weighting but never runtime opacity.
- Deterministic sibling coalescing only for eight occupied children, including rejection of seven-child and any albedo/roughness/metallic/emissive/flag-error case.
- Analytic OBJ in-memory generation, explicit temporary-file round trip, and renderer smoke tests.
- Analysis and voxelization jobs can be delayed, cancelled, failed and superseded. A stale generation never replaces the current preview, and the renderer continues using the last complete scene throughout.
- Opening/analyzing/voxelizing an OBJ creates no `.vsvo`; cancelling the save dialog writes nothing. Explicit save uses a temporary sibling, validates it, atomically replaces only the selected destination, and requires confirmation before overwrite.
- The ImGui controller and headless tests share the same settings structure and builder. Identical source bytes/settings yield identical analysis, logical scene, settings hash, page records and final VSVO bytes.
- An unsaved in-memory preview and its explicitly saved/reloaded file produce identical selected logical cuts and every full G-buffer attachment at fixed cameras.
- User-selected scene metadata counts, deserialization, upload, and fixed-camera render; no named asset is generated as part of a test or milestone.

### Integration tests

- Sample starts and renders with `--generated-scene` when no file is provided.
- ImGui can open a tiny OBJ, display raw and meter-space measurements before voxelization, reject an invalid voxel size, build with a valid meter-space size, render the unsaved preview, and optionally save/reload it without restarting the renderer.
- While OBJ analysis/build/save is pending or fails, input/camera/UI remain responsive and the previously committed scene continues rendering. Changing source units or voxel settings invalidates only the pending generation.
- An explicitly saved `.vsvo` deserializes, rebuilds its TLAS, and uploads model/instance/voxel counts matching metadata.
- An explicitly saved `.vsvo` opens metadata-first, pins only root pages initially, requests visible fine pages spatially, and remains interactively renderable while requests are pending.
- No `.evxl` asset, EVXL reader/writer/API, cache option, test fixture, build entry, active documentation, `EVXL`/`EVX2` loader branch, compatibility test, or legacy runtime structure remains anywhere in the tracked working tree outside this migration plan's deletion record.
- Headless fixed-camera render has finite valid base color, normals, material properties, emissive, motion, and depth; nonzero coverage; and stable per-attachment metrics.
- Camera crossing every BLAS-local direction-sign boundary has no sudden nearest-surface/material/normal change beyond the expected geometric face or visibility change.
- Moving two synthetic transformed solid instances through each other selects the correct nearest material/normal/depth, produces correct motion vectors, and needs no CPU visibility readback.
- Resize/recreate-swapchain rebuilds tile-dependent resources correctly.
- A clean build has no dependency on CUDA, PyTorch, or the ignored reference checkout.

### Milestone-3 depth-only, HZB, and occlusion tests

- **Depth-only contract:** no material/G-buffer resource is bound or read; coverage, IDs, linear surface distance and hardware depth match the full G-buffer path exactly for the same resolved inputs.
- **Depth load/clear:** clear, load, equal, nearer, farther, overlapping surface, empty and resize cases follow both supported depth conventions and never write color attachments.
- **Phase-1 temporal depth:** the previous-HZB pre-cull result runs passes 2-7 and material-free `WriteVoxelDepth`; no material buffer or color attachment is bound, even when the actual-scene phase writes a full G-buffer.
- **SPD base/reduction:** HZB construction produces conservative reverse-Z minimum or forward-Z maximum depth for every valid mip, with exact CPU equality on finite inputs.
- **SPD dimensions:** power-of-two, odd, prime, 1 x N, N x 1, 1 x 1, downsized output, skipped same-size mip 0, 4-to-8 local mip variants and multi-dispatch continuation all match expected dimensions/values.
- **SPD lifecycle:** global counters reset for consecutive builds; barriers/layout transitions are validation-clean; resize destroys/rebuilds views safely; no stale mip survives a smaller or empty next frame.
- **History rotation:** each frame slot samples only the last completed final HZB with matching viewport/projection/depth metadata. First frame, camera cut, projection change, resize, scene reload and explicit reset invalidate it deterministically.
- **Projected TLAS bounds:** node/instance rectangles or enclosing-sphere bounds choose the expected first-valid-mip-relative level and remain conservative at viewport/near-plane edges.
- **Occlusion decisions:** definitely visible, definitely occluded, near-plane-crossing, camera-inside, offscreen-crossing, zero-area, non-finite and uncertain bounds follow the conservative policy; uncertain means visible.
- **Sampling and bias:** center/2 x 2 conservative samples, mip bias, depth bias, edge clamping and forward/reverse-Z comparisons match the CPU oracle and repository virtual-geometry fixtures.
- **Traversal ordering:** frustum rejection happens before HZB sampling; occluded internal nodes skip descendants; visible/uncertain leaves test instances; stable compaction remains ascending by instance ID.
- **Disabled contract:** disabling occlusion makes visibility lists, indirect arguments, raster intermediates and final G-buffer byte-identical to milestone 2.
- **Temporal/current composition:** current culling starts from the full current frustum-visible TLAS and never treats the temporal accepted list as authoritative. A stale temporal rejection leaves clear provisional depth and is recovered as visible.
- **Phase-2 actual-scene render:** the complete current accepted set runs passes 2-7 from cleared scratch and performs one ordinary terminal G-buffer/depth commit. Final color, material, normal, motion, identity, coverage and depth remain a coherent record from one winning voxel.
- **Temporal recovery:** moving/removed occluders, newly exposed instances, camera strafes, gradual motion and alternating visibility produce no one-frame holes because final current-HZB culling recovers stale pre-cull rejections.
- **Cut/resize recovery:** invalid history bypasses pre-cull occlusion, normally renders every frustum-visible instance through phase 1, builds provisional depth from that G-buffer pass, and produces a valid final HZB for the next frame.
- **Final equivalence:** deterministic multi-frame depth-only and full-G-buffer sequences with occlusion enabled match frustum-only final depth, IDs and all G-buffer channels; false-positive visibility is allowed but false occlusion is forbidden.
- **Failure behavior:** temporal/current visibility, TLAS queues, duplicate candidates and HZB resource failures report their stage, zero unsafe downstream work and retain conservative visibility/fallback output.

### Milestone-4 packed occupancy-volume tests

- **Allocation math:** exact `wordResolution`, word count and byte size for 1, 31, 32, 33, odd, non-cubic and device-limit dimensions; reject zero, overflow, unsupported 3D extent and memory-cap violations before allocation.
- **ABI reflection:** C++/WGSL size, alignment, offsets and signedness match for committed/pending state, dirty words and source bounds. Padded X bits are initialized and remain zero.
- **Camera decomposition:** positive, negative, exact-boundary, next-representable-value, large-valid and rejected-out-of-range positions produce the exact mathematical-floor `cameraCell`, `[0,1)` inner coordinate, snapped origin and camera-relative centers expected by the CPU oracle.
- **Address bijection:** every logical cell maps to one in-range physical word/bit for zero and wrapped offsets; inverse lookup returns the original cell; no two logical cells alias; nonmultiples of 32 never expose tail bits.
- **Manual texture probe:** manually uploaded words are read through the production `occupancyAtWorldPosition` helper for inside/outside, all 32 bit positions, row/slice boundaries, wrapped axes and negative origins. Slice/box debug views use the same lookup and do not mutate the volume.
- **Renderer isolation:** enabling, disabling, poisoning, rebuilding or scrolling the occupancy volume cannot change TLAS render visibility, indirect arguments, resolved surfaces, depth, IDs or any G-buffer attachment.
- **`ClassifyDirtyWords`:** zero, full, positive/negative one-cell, multi-cell, multi-axis, wrap and partial-tail moves emit stable unique word indices with exact OR-union bit masks. One X-cell move dirties only one bit column per Y/Z row; Y/Z moves dirty the expected complete rows/slices.
- **Sub-cell stationary update:** arbitrary motion that remains in the same `cameraCell` changes only `cameraInnerCell`, emits zero dirty words/splats/copies, preserves content generation and reconstructs updated exact camera-relative positions.
- **Interior invalidation:** stationary-camera transform, add, remove, topology, and residency edits produce the exact toroidally mapped word/bit union for their supplied bounds. Moves cover old and new bounds, removals clear old-only occupancy, overlapping unchanged sources are restored, and every result matches a fresh full rebuild including all mip levels.
- **Volume TLAS query:** full and dirty-slab AABBs accept exactly CPU-intersecting instances in stable ID order, including geometry behind the view camera; changing frustum/HZB inputs cannot change occupancy visibility lists.
- **Transformed coverage:** identity, translation, rotation, reflection, non-uniform scale, boundary-touching, large adaptive leaves and shared BLAS instances emit the conservative coarse-cell set defined by the scalar oracle. Non-finite transforms are rejected by the scene contract rather than entering the splat pass.
- **`ClearDirtyWords`:** poison patterns prove exact masked atomic clearing. Partial X words preserve nondirty bits, full Y/Z words clear all valid bits, tail padding stays zero, and overlapping axis slabs are applied once without lost retained data.
- **Direct occupancy splat:** zero, one, cross-word, cross-row, cross-slice, wrapped-X and dirty-clipped source boxes produce the exact scalar occupied-cell set. One/many/colliding sources, duplicate instances and randomized invocation order produce the same bitwise OR; no write targets a retained-only or out-of-range bit.
- **`CopyDirtyOccupancyWordsToTexture`:** every and only dirty texture word matches the atomic mirror; retained texture words remain byte-identical. Consecutive smaller updates expose no stale copied word, and RenderGraph validation proves storage/texture barriers before sampling.
- **Full-build oracle:** empty, one-box, shell, overlapping transformed instances and loaded tiny `.vsvo` scenes produce texture words bit-identical to an independently constructed CPU volume and the expected occupied-bit count.
- **Incremental equivalence:** after every step of long fixed-seed positive/negative/diagonal/wrapping camera walks, the committed incremental texture, addressing metadata and world queries equal a fresh full build at the same camera. Compare retained physical words separately to prove they were not copied or rewritten.
- **Large-change behavior:** first build, teleport by `>= resolution` on any axis, resize, extent/resolution change, enable-after-disable, invalid history and a scene revision without complete invalidation bounds select full rebuild; ordinary cell moves and bounded scene edits select incremental; all reason codes and generation changes are deterministic.
- **Transactional failure:** inject source/dirty-capacity, allocation and command-admission failures before destructive work. A prior committed texture/state/hash remains unchanged and readable, and a failed first generation remains invalid with zero unsafe downstream work.
- **Integration/performance:** fixed Sponza paths record full versus dirty word counts, resident and optionally TLAS-filtered source invocations, bytes copied and per-pass time. Stationary/sub-cell updates dispatch no content work, and accepted one-cell paths satisfy a documented dirty-work ratio threshold without making timing alone a correctness gate.

### Milestone-5 occupancy-pyramid and sparse-DDA tests

- **Level layout:** power-of-two, odd, prime, non-cubic and one-cell axes produce exact fixed worst-case level resolutions, packed word offsets/counts and memory totals through the coarsest requested scale. Hardware mip dimensions are never assumed.
- **Global anchoring:** signed floor division maps positive/negative base cells to stable parents. Retained global cells keep identical physical addresses when independent level origins/wraps scroll; inactive alignment padding and X-tail bits remain zero.
- **Full OR reduction:** every level word matches an independent scalar reduction, including children split across X words, rows, slices, active bounds and toroidal seams. Random base patterns prove no occupied descendant exists below a clear ancestor.
- **Outgoing/incoming propagation:** one- and multi-axis base motion projects both departing and entering global cells to unique parent masks. Parents clear when their final child departs and set when any incoming child is occupied.
- **Incremental equivalence:** positive, negative, diagonal, wrap and teleport walks compare every hierarchy word/descriptor with a fresh full build. Retained physical words outside dirty masks remain byte-identical and upper-level touched work shrinks geometrically.
- **Generation/failure:** base and every hierarchy level publish one generation after all reductions complete. Injected failure preserves or restores the prior complete pyramid and makes consumers fall back to mip-0 traversal.
- **DDA analytic rays:** outside entry, inside start, occupied start, axis-aligned, zero component, negative direction, edge/corner tie, exact boundary, very short/long and complete-miss rays agree with scalar mip-0 traversal and always make positive progress.
- **DDA randomized rays:** fixed-seed rays across randomized packed volumes and camera walks never skip the first occupied base cell. Hierarchical and scalar results agree while sparse scenes report fewer visited cells.

### Removal audit

Use targeted legacy-structure searches over `samples/voxelRenderer`, `src/voxel`, `src/runtime/gpusparsevoxel`, `assets/shaders/voxel`, and voxel tests for:

```text
CWBVH
TraceLod
MicroTree
macroGrid
ScreenRay
GlobalOctreeRay
NoisyCosineGI
SortedCWBVH
GPUSparseVoxelPrimary
trace_sparse_voxel
EVXL
EVX2
.evxl
VoxelSceneCPU
VoxelCullingScene
VoxelCullingBrickGPU
VoxelBVH8NodeGPU
```

Any remaining occurrence must be either outside the voxel renderer's scope or explicitly justified. General software-BVH and virtual-geometry raytracing code is not part of this deletion.

Additionally run a case-insensitive whole-tree search for `EVXL`, `.evxl`, `VoxelSceneFileCompression`, `readVoxelSceneFile`, and `writeVoxelSceneFile`. Exclude only VCS/build metadata, `ignore/svraster`, and this plan. Unlike general BVH terms, no surviving EVXL match is allowed or justified.

## Risks and mitigations

- **License contamination:** keep the reference ignored; implement from the published algorithm; do not paste CUDA; review attribution and distribution status before release.
- **Correlated CPU/GPU bugs:** keep the CPU oracle scalar and structurally independent, validate it against hand-computed and pinned-reference fixtures, and never share an algorithm implementation merely to make differential tests agree.
- **Floating-point false confidence:** define projection rounding, depth convention, and tolerances centrally before GPU implementation. Require exact discrete results plus conservative geometric invariants instead of accepting a visually similar image.
- **Gate erosion:** CI labels are cumulative, skipped GPU jobs do not satisfy promotion, and changing fixtures, tolerances, capacities, or goldens requires an explained review rather than an automatic rebaseline.
- **64-bit key support:** prove stable full-width sorting with adversarial tests. The current `RadixSort2Pass` is only 32-bit; truncating tile/path bits would diverge from SVRaster and can break tile ranges.
- **Cross-instance ordering:** local Morton order cannot globally order overlapping transformed BLASes. Do not invent a divergent depth-key format for version 1; scan the full tile list and use the exact first forward box boundary for the final solid surface. Cover transformed overlap, camera-inside, and storage-order adversarial cases.
- **Full instanced tile scans:** scanning every covering candidate is the correctness-first cost of keeping SVRaster's key with arbitrary transforms. Measure tile-list length and exact tests on Sponza; optimize only after the parity renderer is green, and do not change the reference key without a separate measured design/test update.
- **TLAS queue/list overflow:** every append/scan has a checked capacity and stage-specific diagnostic. Zero downstream indirect work after overflow so a culling failure cannot become an out-of-bounds raster write.
- **Duplicate-list memory spikes:** measure Sponza, expose capacity, use checked counters, and fail the frame safely. Later optimizations can cull leaves or split oversized projected voxels.
- **Near-plane projection:** conservative projection of a voxel crossing the near plane needs an explicit fallback rectangle; silently dropping its behind-plane corners can under-bound it.
- **Adaptive cracks or overlap:** validate full parent/child containment offline, keep complete sibling groups in one page, and replay arbitrary dependency-closed install sets plus descendant-first uninstalls to prove every published resident cut is disjoint and hole-free.
- **Solid coalescing:** merging a partially occupied sibling group would invent solid volume and hide geometry. Require all eight children to be occupied before coalescing and test this invariant in the builder and loader.
- **OBJ unit ambiguity:** OBJ has no standard length unit. Never infer scale from mesh size or asset identity; require a visible meters-per-source-unit setting, show both raw and meter-space bounds, and include the conversion in the settings hash.
- **Voxel-size explosion:** a plausible-looking value in the wrong unit can produce impossible grids. Derive level, root cube and conservative work/memory estimates before allocating, disable the build on checked limits, and report the minimum viable voxel size in meters.
- **Interactive build stalls or stale publication:** parsing and voxelization run in cancellable generation-tagged jobs while the last complete scene keeps rendering. No worker completion may directly mutate live GPU/scene state.
- **Accidental authoring writes:** opening and previewing never chooses or writes a cache path. Save is a separate confirmed action with atomic replacement and read-back validation; cancelled dialogs, failed builds and stale previews write nothing.
- **World transforms:** validate inverses and conservative bounds centrally. Keep voxel intersection in BLAS-local space while preserving world-ray parameter `t`; test rotation, reflection, and non-uniform scale explicitly.
- **Normal correctness under transforms:** derive the normal from the winning slab and use the inverse transpose, not `modelToWorld`; define face ties and camera-inside exit faces so normals cannot flicker or become undefined.
- **G-buffer integration drift:** reuse the existing Engine attachment formats/channel meanings and test them against `MaterialGBufferOutput`; do not add a voxel-only lighting interpretation or silently swap linear/sRGB conventions.
- **Temporal artifacts:** preserve previous camera and instance transforms independently from the TLAS, reset history on cuts/reloads, and test static, camera-only, instance-only, combined motion, and disocclusion.
- **HZB convention mismatch:** use the shared SPD implementation and expose its first valid mip, dimensions, reduction operator and depth convention as metadata. Never assume mip 0 is populated or mix reverse-Z comparisons with forward-Z reduction.
- **Temporal false occlusion:** previous HZB is never the final authority. Traverse again against provisional current depth and emit unsubmitted visible instances; uncertain bounds and invalid history stay visible.
- **Two-phase duplication/state loss:** preserve pre-submitted flags and both resolved-surface maps through deterministic merge. Test that submitted-set union is complete, intersection is empty, equal-depth IDs use the normal tie rule, and scratch reuse cannot overwrite phase-1 winners.
- **Two-phase/HZB cost exceeds savings:** capture temporal depth raster work, both HZB builds, both culls, actual-scene raster/commit work and avoided raster work separately. Keep frustum-only scheduling available and do not enable occlusion by default without representative net-positive measurements.
- **Opaque-source quality:** OBJ-derived occupancy is not a trained radiance field. Expect a blocky solid-voxel rendition of Sponza, including conservative thickening of partially covered source cells. Per-voxel PBR/emissive attributes, exact box-face normals, and opaque nearest-surface depth are the intended first acceptance target.
- **Selective-streaming holes:** never expose a page merely because its IO completed. Publish only after every dependency, payload/sidecar upload, translated rewrite, and barrier succeeds; continue rendering the coarse parent on delay or failure.
- **Residency budget thrash:** reduce shared-instance requests by page ID, use deterministic maximum spatial priority and hysteresis/prefetch margins, pin roots, evict descendants before ancestors, and expose request/upload/eviction/bytes-per-frame counters before tuning budgets.
- **Logical/physical address confusion:** file-global node IDs are identities, not GPU pointers. Translate rewrites through the resident page allocation, keep stable IDs in a sidecar, randomize physical allocation in tests, and reject every translated range outside its page.
- **Packed texture write races:** portable WGSL cannot rely on atomic OR directly into a storage texture. Keep the atomic word mirror and one-writer-per-dirty-word texture synchronization; do not replace it with racing load/OR/store texture writes.
- **Toroidal partial-word corruption:** an X scroll can free only some bits of a retained physical word. Build exact dirty masks, use masked atomic clear, keep tail bits zero, and compare retained physical words before/after every incremental fixture.
- **Negative/large-world addressing drift:** mathematical floor, signed cell coordinates, positive modulo and checked integer range must be identical on CPU/GPU. Use camera-cell plus inner-cell relative math rather than subtracting large nearly equal floats in consumers.
- **Coarse splat cost:** a large adaptive source leaf may cover many destination cells. Measure covered dirty cells and worst-leaf work; consider a later word-at-a-time or hierarchical fill optimization only after the direct scalar/GPU contract is green.
- **Occupancy generation tearing:** pending wrap/origin metadata must never become visible with an incompletely cleared or copied texture. Publish metadata and increment generation only after all update passes succeed; retain the prior committed generation on failure.
- **Approximation misuse:** the bit volume is conservative coarse occupancy, not source topology, visibility, material, density or signed distance. Name APIs/debug views accordingly and do not make primary rendering correctness depend on it.
- **Dynamic-scene staleness:** incremental scene revisions require a complete invalidation set. Moves dirty old and new bounds, removals/evictions dirty old bounds, and additions/residency dirty new bounds; all current sources are resplatted through the union. Missing or uncertain bounds force a full rebuild rather than leaving stale bits.

## Definition of done

- `ignore/svraster` remains a reproducible reference checkout at the recorded commit.
- The Engine builds and runs without CUDA, PyTorch, or a runtime dependency on that checkout.
- Promotion gates G1-G26 pass cumulatively, including the required headless Vulkan job; no gate is satisfied solely by a skipped test or final-image comparison.
- The independent scalar CPU oracle passes its analytic and pinned-reference fixtures, remains test-only, and matches every applicable GPU intermediate buffer under the documented exact/tolerance policy.
- The renderer can run without file I/O from deterministic generated multi-BLAS, transformed-instance scenes.
- Every clear, TLAS seed/prepare/traverse, visibility compaction/voxel scan, page request/deduplication, physical page install/uninstall rewrite, indirect preprocess, duplicate scan/emission, stable 64-bit sort, tile-range, exact solid-surface resolve, G-buffer commit, debug-present, occupancy volume query, direct toroidal splat, masked clear and texture-sync pass has a focused GPU test with CPU or analytic expected results.
- Milestone 1 writes the existing Engine base-color, geometry-normal, shading-normal, material-properties, emissive, motion-vector, identity, coverage, and hardware-depth targets from per-voxel albedo/roughness/metallic/emissive data; an existing deferred-lighting test consumes them without voxel-specific shading.
- TLAS frustum culling produces deterministic visible instances and drives the SVRaster work indirectly with no CPU visibility readback in milestones 1/2; milestone 3 adds the gated conservative HZB path without changing that baseline contract.
- G13 deletes `assets/meshes/obj/sponza.evxl` and every EVXL implementation, test, cache path, build entry, and active document before file-format work begins; the repository-wide EVXL audit stays empty thereafter.
- No generated `.vsvo` is required in the repository; every output comes from an explicit user Save destination or a disposable test temporary directory.
- The sparse-voxel authoring/runtime contract uses meters internally. Every OBJ import records an explicit meters-per-source-unit value; raw and meter-space mesh bounds are visible before voxelization, and target voxel size is specified and reported in meters.
- The ImGui authoring workflow analyzes before it enables voxelization, reports derived grid/level/cost limits, renders a complete unsaved in-memory preview through the production path, and writes no file until the user explicitly chooses `Save VSVO...` and a destination.
- The new deserializer validates it metadata-first, installs pinned roots, and selectively reads only requested finer pages; no legacy conversion is invoked.
- File generation has page-AABB/golden-layout, deterministic, round-trip, malformed-input, arbitrary dependency-closed resident-cut, complete per-voxel material, and direct-versus-deserialized full-G-buffer equivalence coverage.
- Spatial page priority is deterministic across shared BLAS instances; dependencies always precede publication, delayed/failed requests retain coarse parents, descendant-first eviction preserves a valid cut, randomized physical allocation does not change logical output, and configured resident/read/upload budgets are never silently exceeded.
- The voxel sample renders it exclusively through TLAS cull/compact and SVRaster preprocess/bin/sort/solid-resolve passes.
- All voxel-specific raytracing paths, CWBVH/BVH8/brick runtime structures, legacy formats, UI modes, acceleration payloads, shaders, and tests listed above are removed.
- New file, solid builder, material packing, ordering, nearest-surface/face resolver, G-buffer, and Sponza integration tests pass.
- Milestone 3 depth-only rendering uses the same resolve/depth convention without material bindings; its output matches the full path.
- The shared fast SPD pass builds validated provisional/final HZBs. Previous-HZB phase-1 culling followed by a complete normal render, current-HZB recovery culling followed by a complete late normal render, and deterministic surface merge match frustum-only multi-frame output while recovering temporal disocclusion and never double-submitting an instance.
- Occlusion performance counters and representative Sponza measurements document whether it is enabled by default; correctness does not depend on a positive performance result.
- Milestone 4 exposes a validated `R32Uint` 3D texture with one bit per logical coarse voxel, exact checked `ceilDiv(Nx, 32) * Ny * Nz` storage, zero tail bits, and shared CPU/WGSL camera-cell/sub-cell/toroidal address helpers.
- Initial/full occupancy builds and every incremental camera-scroll or interior-invalidation generation match the independent CPU volume bit-for-bit. Sub-cell motion without a scene edit performs no content work; ordinary cell motion and bounded scene edits clear/resplat only their union masks; teleport, resize, invalid history, and scene revisions without complete bounds rebuild fully.
- Occupancy update failures never publish partial texture/state, the public texture is synchronized before consumers, and Sponza measurements report memory, dirty-work ratio, splat counts and timings without making the approximation part of primary-render correctness.
- A clean full build/test run and fixed-camera Sponza smoke render pass with no validation errors or overflow.
