# Sparse Instance SDF

## Goal

Build an adaptive sparse SDF representation for the static mesh instances in the scene. The traceable unit is an instance SDF surface, not a merged global distance field.

Every static mesh instance registered in the scene owns a distinct SDF surface descriptor. The descriptor may reference immutable mesh-local brick payloads when that is useful for storage, but the runtime acceleration structure and hit reporting must treat each scene instance as its own surface:

```text
ray
  -> TLAS sparse octree
  -> one or more instance SDF surfaces
  -> local sparse brick SDF traversal/marching
  -> hit with instance id, local position, world position, normal
```

Do not build a global/min-composited SDF for this representation. The world-space structure is only an acceleration structure for finding candidate instance SDFs.

The desired quality behavior is error-driven:

- high allowed error produces very coarse silhouettes
- lower allowed error recursively adds sparse SDF levels
- the final SDF surface converges toward the original mesh surface

## Non-Goals

- Do not merge all static meshes into a single global SDF.
- Do not lose instance identity during SDF tracing.
- Do not use triangle density as the final refinement metric.
- Do not require watertight meshes; triangle soups should still produce useful signed fields.
- Do not store full material data in SDF voxels. SDF hit shading should resolve through instance/material/surface-cache data.

## High-Level Data Model

There are two acceleration layers:

```text
SceneSdfTLAS
  sparse world octree, max depth 16
  leaf cells reference one or more instance SDF surfaces

InstanceSdfSurface
  per-scene-instance traceable surface
  owns world/local transforms, bounds, quality metadata
  references an adaptive sparse brick SDF asset or unique per-instance payload

SparseSdfAsset
  adaptive hierarchy of 8x8x8 bricks
  stores narrow-band signed distances
  stores error values and optional surface/material lookup hints
```

The TLAS answers "which instance SDFs might this world ray hit?" The instance SDF answers "where does this ray hit this particular mesh surface?"

## VSDF File Format

The offline artifact is a `.vsdf` sidecar written when building `.virtualgeometry` files. It must be generated from the original imported mesh triangles and scene instances, not from the simplified virtual-geometry cluster representation. Virtual geometry can simplify rendering data; VSDF needs the authored geometry surface for validation and SDF error emission.

V1 stores one independently compressed SDF payload per scene instance. This keeps the format simple and guarantees that every scene instance has its own traceable SDF surface. Later versions may add shared mesh-local payloads plus per-instance overrides, but the runtime contract should remain instance-preserving.

All integer fields are little-endian. Offsets are byte offsets from the beginning of the file. All tables should be 16-byte aligned.

```cpp
static constexpr uint32_t VSDF_MAGIC = 0x46445356u; // "VSDF"
static constexpr uint32_t VSDF_VERSION = 1u;

struct VSDFHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t headerSize;
  uint32_t flags;

  uint32_t instanceCount;
  uint32_t instanceDirectoryStride;
  uint64_t instanceDirectoryOffset;

  uint64_t stringTableOffset;
  uint64_t stringTableSize;

  float sceneBoundsMin[3];
  uint32_t _padding0;
  float sceneBoundsMax[3];
  uint32_t _padding1;

  uint64_t sourceGeometryHash;
  uint64_t buildSettingsHash;
};
```

The instance directory is the file's primary index. It gives the loader enough metadata to build scene bounds, schedule streaming, and jump directly to one compressed instance payload.

```cpp
enum VSDFCompression : uint32_t {
  VSDF_COMPRESSION_NONE = 0,
  VSDF_COMPRESSION_LZ4 = 1,
};

enum VSDFDistanceFormat : uint32_t {
  VSDF_DISTANCE_R8_SNORM = 0,
  VSDF_DISTANCE_R16_SNORM = 1,
  VSDF_DISTANCE_R16_FLOAT = 2,
};

struct VSDFInstanceDirectoryEntry {
  uint32_t instanceId;
  uint32_t sourceMeshIndex;
  uint32_t materialRangeBegin;
  uint32_t materialRangeCount;

  uint64_t instancePayloadOffset;
  uint64_t compressedSize;
  uint64_t uncompressedSize;

  uint32_t compression; // VSDF_COMPRESSION_LZ4 for v1
  uint32_t distanceFormat;
  uint32_t maxLevel;
  uint32_t flags;

  float worldBoundsMin[3];
  float conservativeScale;
  float worldBoundsMax[3];
  float requestedError;
};
```

Each instance payload is LZ4-compressed as a single independent block in v1. Independent blocks keep random access and test fixtures simple: loading one instance does not require decompressing the whole scene.

```cpp
struct VSDFInstancePayloadHeader {
  uint32_t payloadVersion;
  uint32_t nodeCount;
  uint32_t brickCount;
  uint32_t distanceByteCount;

  uint32_t rootNode;
  uint32_t brickResolution; // 8
  uint32_t narrowBandVoxels;
  uint32_t maxLevel;

  float localBoundsMin[3];
  float rootVoxelSize;
  float localBoundsMax[3];
  float requestedError;

  float localToWorld[16];
  float worldToLocal[16];

  uint32_t nodesOffset;
  uint32_t bricksOffset;
  uint32_t distancesOffset;
  uint32_t triangleDebugOffset; // optional, 0 if absent
};

struct VSDFNodeDisk {
  uint32_t brickIndex;
  uint32_t firstChild;
  uint32_t childMaskWords[16];
  uint16_t level;
  uint16_t flags;
  uint32_t _padding0;
};

struct VSDFBrickDisk {
  uint32_t distanceOffset;
  uint32_t minMaxDistancePacked;
  float voxelSize;
  float maxError;
};
```

The uncompressed payload layout is:

```text
VSDFInstancePayloadHeader
VSDFNodeDisk[nodeCount]
VSDFBrickDisk[brickCount]
distance bytes
optional validation/debug records
```

Distance bytes are packed by brick. For `R8_SNORM`, every brick contributes exactly `8 * 8 * 8` bytes. The decoded value is:

```text
normalized = snorm_to_float(stored)
signedDistanceLocal = normalized * narrowBandVoxels * brick.voxelSize
```

### Virtual Geometry Build Integration

The virtual geometry importers should grow an optional `--emit-vsdf` path:

```text
gltf/obj importer
  read original scene mesh triangles and instance transforms
  build virtual geometry as today
  build one adaptive SDF payload per scene instance
  write model.virtualgeometry
  write model.vsdf
```

The VSDF builder should sit beside the existing virtual geometry file creation code and reuse the same source scene data before it is simplified or partitioned. LZ4 is already linked by the editor/importer path, so the first writer can use `LZ4_compress_default` with one compressed block per `VSDFInstanceDirectoryEntry`.

The importer should fail the VSDF sidecar independently from the `.virtualgeometry` file when requested by a permissive debug flag, but CI tests should run in strict mode where a requested VSDF build failure fails the import.

## Sparse SDF Asset

Use a brixelizer-like adaptive hierarchy. Each node covers an axis-aligned local-space region and owns one 8x8x8 brick. If a coarse cell needs more accuracy, that cell points to a child node, which contains another 8x8x8 brick over the cell's local region.

```cpp
struct SparseSdfAsset {
  AABB localBounds;
  float rootVoxelSize;
  uint32_t brickResolution; // 8
  uint32_t narrowBandVoxels; // usually 4
  uint32_t maxLevel;
  uint32_t rootNode;
  Buffer<SdfNode> nodes;
  Buffer<SdfBrickMeta> bricks;
  Buffer<int8_t> brickDistances; // or uint16/float debug variants
};

struct SdfNode {
  uint32_t brickIndex;
  uint32_t firstChild;
  uint32_t childMaskWords[16]; // 512 child bits total, one per brick cell
  uint16_t level;
  uint16_t flags;
};

struct SdfBrickMeta {
  uint32_t distanceOffset;
  uint32_t minMaxDistancePacked;
  float voxelSize;
  float maxError;
};
```

Each brick stores signed distances normalized to the local brick voxel size:

```text
storedDistance = clamp(signedDistance / voxelSize, -band, +band)
```

Good first formats:

- `R8_SNORM` for the runtime path
- `R16_SNORM` or `R16F` for validation and debug captures
- optional one-voxel border later if atlas filtering across brick edges becomes a problem

The root brick gives the bad/coarse silhouette. Child bricks refine only the cells where the current zero surface fails the error metric.

## Instance SDF Surface

Each static scene instance gets an `InstanceSdfSurface` entry.

```cpp
struct InstanceSdfSurface {
  uint32_t instanceId;
  uint32_t assetId;
  uint32_t materialRange;
  uint32_t flags;

  Mat4 localToWorld;
  Mat4 worldToLocal;
  Mat4 previousLocalToWorld;

  AABB localBounds;
  AABB worldBounds;
  float conservativeScale;
  float requestedError;
};
```

The important rule is that the TLAS and hit results reference `instanceId`, not only `assetId`. If ten copies of the same mesh exist in the scene, the TLAS contains ten traceable SDF surfaces.

If an instance has non-uniform scale, either bake a unique per-instance SDF surface in world/local-deformed space or use conservative distance scaling:

```text
worldDistance = localDistance * min(axisScale)
```

This underestimates distance, which is safe for occlusion and hit finding, but increases ray-marching steps. Unique per-instance SDF payloads can be generated later for severe non-uniform scale.

## Field Queries For Building

The high-quality target still needs two exact or near-exact queries against the source mesh:

```text
unsignedDistance(p) = closest point distance to triangle BVH
inside(p) = windingNumber(p) > 0.5
signedDistance(p) = inside ? -unsignedDistance(p) : unsignedDistance(p)
```

Fast winding numbers are a good fit for the sign query because they work better than simple ray parity for triangle soups, holes, and non-manifold meshes. The winding-number tree can be built once per source mesh and reused for all brick samples and validation samples.

The current phase-3 builder uses a faster narrow-band approximation while the sparse topology is being proven:

- triangle BVH over original imported triangles
- exact triangle-box SAT overlap for conservative sparse voxelization
- per-brick local triangle candidate lists
- narrow-band closest-triangle distance emission, capped to the brick band
- nearest-triangle-normal sign as a temporary soup-friendly sign approximation

This is faster than evaluating full closest distance and winding number everywhere, but the sign is not final. Replace it with fast winding numbers before relying on closed-volume behavior.

## Error Metric

Refinement is controlled by how well the current SDF zero surface represents the mesh, not by triangle count alone.

For each candidate brick/cell, compute:

```text
E = max(E_mesh_to_sdf, E_sdf_to_mesh, E_sign)
```

Where:

```text
E_mesh_to_sdf
  sample points on source triangles inside or near the cell
  evaluate the current approximate SDF at each point
  error = max(abs(phiApprox(surfacePoint)))

E_sdf_to_mesh
  find approximate zero crossings in the current brick cell
  measure exact distance from those zero points to the mesh
  error = max(distanceToMesh(zeroPoint))

E_sign
  compare approximate sign and exact winding-number sign at corners,
  center, and optional stratified samples
  sign mismatch = large penalty
```

Use a relative artist-facing quality value and resolve it per instance:

```text
requestedErrorWorld = requestedErrorRelative * instanceAabbDiagonal
refine if E > requestedErrorWorld
```

For internal heuristics, also track voxel-relative error:

```text
normalizedError = E / localVoxelSize
```

This makes it easy to avoid refining below a level where quantization and narrow-band clamping dominate the result.

### Current Volume Error Heuristic

For the first sparse builder, topology is generated before expensive SDF emission using conservative sparse voxelization. For each brick:

```text
voxelize the 8x8x8 child cells against source triangles
close the surface shell by `surfaceCloseVoxels` cells
flood-fill exterior empty cells from the brick boundary
remove exterior cells
voxelizedMeshVolume = sum(volume(childCell) for remaining enclosed/shell cells)
currentVoxelVolume = volume(current node region)
volumeSlack = targetVolumeRatio^3 * rootInstanceVolume
meshToVoxelRatio = voxelizedMeshVolume / (currentVoxelVolume + volumeSlack)
targetVolumeRatio = requestedError
```

The occupied child cells are full voxels that sit on top of the triangle mesh surface. The voxelized mesh volume is therefore conservative for the current node; it should not rely on voxel centers being inside the mesh. The closure pass seals small open spots before measuring volume. The exterior flood fill then removes obvious outside air after the first surface voxelization.

The closed proxy is used only for the volume/error decision. Final SDF emission and child refinement use the original triangle-overlapped surface cells, so synthetic cap cells do not become fake SDF surfaces.

If `meshToVoxelRatio > targetVolumeRatio`, all occupied child cells are refined, subject to `maxLevel`. If `meshToVoxelRatio <= targetVolumeRatio`, the node is accepted as a coarse conservative cover and the SDF is emitted for that voxelized region. This gives predictable adaptive LOD behavior:

- high `requestedError` leaves only coarse root or shallow bricks
- lower `requestedError` allocates deeper bricks around denser mesh coverage
- optional guard levels can force one or more extra levels under current leaves to reduce cracks

The volume heuristic is deliberately conservative. It handles tiny triangles because triangle-box overlap is tested against the whole child cell, not only at voxel centers. Large triangles are also handled because they mark every child cell they cross. The tradeoff is that thin sheets can occupy more volume than their actual surface thickness; later validation can replace the heuristic with mesh-to-SDF and SDF-to-mesh error while keeping the same sparse node format.

Two guards keep the sparse tree from exploding in dense regions:

- add `targetVolumeRatio^3 * rootInstanceVolume` slack to the ratio denominator, so tiny conservative voxels do not report `meshToVoxelRatio = 1` forever
- refine only the strongest `maxRefinedCellsPerBrick` occupied children, sorted by triangle overlap count; the current default is 16

Unselected occupied cells are still represented by the parent brick, so the cover remains conservative while deeper detail goes to the highest-benefit cells first.

After topology is allocated, the builder emits SDF samples for the allocated bricks in parallel. The default worker count is 64:

```text
build conservative sparse voxel topology
parallel_for allocated bricks:
  collect local triangle candidates
  compute narrow-band SDF samples
  write brick distance payload
```

## Adaptive Build Algorithm

```text
BuildSdfNode(region, level):
  voxelize the 512 child cells with triangle-box overlap
  close/fill the local voxel shell and remove exterior cells
  meshToVoxelRatio = enclosed child volume / (current node volume + volume slack)

  if level == maxLevel or meshToVoxelRatio <= targetVolumeRatio:
    optionally add guard levels, then return leaf

  for selected surface-overlapped child cells:
    childRegion = cell bounds
    child = BuildSdfNode(childRegion, level + 1)
    attach child to this cell

  return node

EmitSdfBricks():
  run a 64-worker brick queue
  emit 8x8x8 narrow-band SDF samples for each allocated node
```

`cellNeedsRefinement` should be conservative but not blind:

```text
cell has exact triangle-box overlap
future: cell has sign variation
future: cell has abs(distance) inside the narrow band
future: cell's mesh-to-sdf or sdf-to-mesh error exceeds the tolerance
cell contains small/thin triangle coverage
```

Small triangles and thin sheets need special handling because voxel-center samples can miss them entirely. During brick emission, expand tiny triangles by half a voxel diagonal for visibility:

```text
expandRadius = 0.5 * voxelDiagonal
emittedDistance = distance(sample, triangle) - expandRadius
```

This makes thin features visible to the SDF without pretending the material surface moved. Hit shading should still resolve back to the original triangle/card/surface data.

## Runtime Instance SDF Sampling

Sampling descends from coarse to fine:

```text
SampleInstanceSdf(instance, worldPosition):
  localPosition = instance.worldToLocal * worldPosition
  node = asset.rootNode

  loop:
    localUVW = positionToNodeUVW(localPosition, node)
    value = sample node brick
    cell = floor(localUVW * 8)

    if node has child for cell:
      node = child
      continue

    return value * node.voxelSize * instance.conservativeScale
```

For debugging, expose the selected level and leaf brick id. This will make the error slider visually obvious: high error should show rays hitting coarse bricks; low error should show deeper leaf levels around silhouettes and thin details.

## Instance SDF Ray Marching

Trace each candidate instance in local space:

```text
TraceInstanceSdf(instance, worldRay):
  localRay.origin = instance.worldToLocal * worldRay.origin
  localRay.direction = instance.worldToLocalLinear * worldRay.direction
  tMin, tMax = intersect localRay with asset.localBounds

  # t remains in world-ray units because localRay.direction was not normalized.
  t = max(tMin, worldRay.tMin)

  while t < tMax and steps < maxSteps:
    pLocal = localRay.origin + t * localRay.direction
    dLocal = SampleSparseSdfLocal(asset, pLocal)
    dWorld = dLocal * instance.conservativeScale

    if dWorld < hitThreshold:
      return hit(instanceId, worldPosition, normal, level, brickId)

    t += max(dWorld * stepScale, minStep)

  return miss
```

Normals can be computed by central differences against the instance SDF:

```text
normalLocal = normalize(vec3(
  sample(p + dx) - sample(p - dx),
  sample(p + dy) - sample(p - dy),
  sample(p + dz) - sample(p - dz)
))

normalWorld = normalize(transpose(instance.worldToLocal3x3) * normalLocal)
```

Use a hit threshold proportional to the finest active voxel size at the sampled point.

## TLAS Sparse Octree

Use a sparse world-space octree as the top-level acceleration structure. It is not an SDF and does not store distances. It only maps world regions to candidate instance surfaces.

```cpp
struct SdfTlas {
  AABB worldBounds;
  uint32_t rootNode;
  uint32_t maxDepth; // 16
  Buffer<SdfTlasNode> nodes;
  Buffer<uint32_t> leafInstanceIndices;
  Buffer<InstanceSdfSurface> instances;
};

struct SdfTlasNode {
  uint32_t childMask;        // 8 children
  uint32_t firstChild;       // compact child array offset
  uint32_t firstInstance;    // leaf list offset
  uint16_t instanceCount;
  uint16_t depth;
  uint32_t flags;
};
```

Leaf cells can reference one or more instances. Large instances may appear in multiple leaves. Use a per-ray mailbox to avoid tracing the same instance more than once:

```text
if instance.lastRayId == currentRayId:
  skip
else:
  instance.lastRayId = currentRayId
  trace instance SDF
```

On GPU, use a small fixed-size per-ray candidate cache or a hashed mailbox. On CPU, a stamped array is enough.

### TLAS Build

Build or refit the TLAS from instance world bounds:

```text
BuildTlas(instances):
  root bounds = scene SDF bounds
  for each instance:
    InsertInstance(root, instance, depth = 0)

InsertInstance(node, instance, depth):
  if depth == maxDepth:
    append instance to node leaf list
    return

  if node is small enough or instance overlaps too many children:
    append instance to node leaf list
    return

  for each child overlapped by instance.worldBounds:
    create child if needed
    InsertInstance(child, instance, depth + 1)
```

Useful stopping rules:

```text
depth >= 16
node instance count <= leafTargetCount
node size <= minimumWorldCellSize
instance overlaps more than 4 children
instance bounds cover most of the node
```

This avoids duplicating huge meshes into thousands of leaves. Very large meshes stay higher in the tree.

For dynamic editor changes, support two paths:

- refit bounds and rebuild changed leaf lists for small changes
- full rebuild when many instances move or scene bounds change

Static scene runtime can start with a full CPU build and GPU upload.

## SDFSceneManager

Add a runtime `SDFSceneManager` for VSDF-backed instance SDFs. It should be separate from `GPUSignedDistanceFieldsSceneManager` because this path does not stamp or merge into a global SDF.

Suggested files:

```text
src/runtime/sdf/SDFSceneFile.hpp
src/runtime/sdf/SDFSceneFile.cpp
src/runtime/sdf/SDFSceneManager.hpp
src/runtime/sdf/SDFSceneManager.cpp
src/runtime/sdf/rendering/SDFTracePass.hpp
assets/shaders/sdf/wgsl/sdf-trace.wgsl
```

Responsibilities:

```text
SDFSceneFile
  open .vsdf
  validate header and directory
  read instance directory entries
  decompress one LZ4 instance payload at a time
  expose immutable decoded BLAS payload views

SDFSceneManager
  own decoded or streamed SDF BLAS payloads
  own per-scene-instance records
  build/refit the sparse-octree TLAS
  upload TLAS nodes, TLAS leaf refs, instance table, BLAS nodes, BLAS bricks, and distance data
  expose GPU buffer handles to SDFTracePass
  preserve mapping from VSDF instance id to runtime scene instance id
```

Runtime terminology:

```text
BLAS
  the adaptive sparse SDF hierarchy for one instance surface
  loaded from one compressed VSDF instance payload

TLAS
  sparse world octree over instance world bounds
  leaves reference BLAS instance ids
```

Even when two instances share the same source mesh, v1 should register two BLAS records because each VSDF instance has its own compressed payload. If memory pressure becomes painful, add payload deduplication later behind the same instance-preserving API.

### GPU Resources

The manager should upload compact POD buffers:

```cpp
struct SDFInstanceGPU {
  mat4x4<f32> localToWorld;
  mat4x4<f32> worldToLocal;
  vec3<f32> worldBoundsMin;
  uint32_t blasIndex;
  vec3<f32> worldBoundsMax;
  float conservativeScale;
  uint32_t instanceId;
  uint32_t materialRangeBegin;
  uint32_t materialRangeCount;
  uint32_t flags;
};

struct SDFBlasGPU {
  vec3<f32> localBoundsMin;
  uint32_t rootNode;
  vec3<f32> localBoundsMax;
  float rootVoxelSize;
  uint32_t nodeOffset;
  uint32_t brickOffset;
  uint32_t distanceOffset;
  uint32_t distanceFormat;
};

struct SDFTlasNodeGPU {
  vec3<f32> boundsMin;
  uint32_t childMask;
  vec3<f32> boundsMax;
  uint32_t firstChild;
  uint32_t firstInstanceRef;
  uint32_t instanceRefCount;
  uint32_t depth;
  uint32_t flags;
};
```

Initial buffers:

```text
SDFInstances
SDFBlases
SDFBlasNodes
SDFBlasBricks
SDFDistanceBytes
SDFTlasNodes
SDFTlasInstanceRefs
```

The first implementation can keep all decoded VSDF instances resident. Streaming can come later by pinning coarse BLAS roots and paging fine brick distance ranges.

## Fast GPU Trace Kernel

The GPU trace kernel consumes ray packets and writes compact hit records. It should not shade. Shading is a later resolve pass that uses `instanceId`, `materialRange`, and local hit data.

```cpp
struct SDFTraceRayGPU {
  vec3<f32> origin;
  float tMin;
  vec3<f32> direction;
  float tMax;
  uint32_t rayId;
  uint32_t flags;
  uint32_t _padding0;
  uint32_t _padding1;
};

struct SDFTraceHitGPU {
  vec3<f32> worldPosition;
  float t;
  vec3<f32> worldNormal;
  uint32_t instanceId;
  vec3<f32> localPosition;
  uint32_t blasIndex;
  uint32_t sdfNode;
  uint32_t brickIndex;
  uint32_t sdfLevel;
  uint32_t flags;
};
```

Kernel flow:

```text
for each ray:
  hitBest = miss
  traverse TLAS in increasing t
  for each referenced instance:
    skip if mailbox already tested the instance
    intersect world ray with instance world bounds
    if bounds hit can beat hitBest:
      trace instance BLAS SDF
      update hitBest
  write hitBest
```

The fast path should use:

- nearest-first TLAS traversal with a small explicit stack
- fixed-size per-ray mailbox for duplicate instance refs
- local-space SDF marching against the BLAS root bounds
- adaptive SDF sampling that descends to the finest resident node for the current point
- central-difference normal estimation only after a hit

The first kernel can be a debug/validation compute pass. Once the hit contract is stable, GI and reflection passes can call the same WGSL helpers.

## Sparse DDA TLAS Traversal

Traverse the sparse octree before marching any instance SDF. The traversal should visit cells in increasing ray `t` so nearest hits can terminate early.

High-level traversal:

```text
TraceSceneSdf(ray):
  hitBest = miss
  stack.push(root, intersect(ray, rootBounds))

  while stack not empty:
    item = pop nearest item

    if item.tMin > hitBest.t:
      continue

    node = nodes[item.node]

    trace instances attached to node/leaf

    if node has children:
      childHits = intersect ray with occupied child bounds
      push child hits sorted by tMin

  return hitBest
```

For the first implementation, a small explicit stack with sorted child intersections is simpler and reliable.

Once this works, replace child testing with sparse DDA within a node:

```text
TraverseSparseNodeDda(node, ray, tEnter, tExit):
  compute child grid coordinate at tEnter
  compute tMax/tDelta for the 2x2x2 child grid

  while inside node and t < tExit:
    childBit = coord.x + coord.y * 2 + coord.z * 4
    if child exists:
      push/visit child
    step to next child cell by smallest tMax axis
```

For deeper levels, repeat the same logic per node. Because each octree node has only 2x2x2 children, child-AABB sorting may be fast enough. Sparse DDA becomes more attractive if the TLAS evolves into a wider sparse voxel tree.

## Scene Trace Algorithm

Final world trace:

```text
TraceSparseInstanceSdfScene(worldRay, tMin, tMax):
  hitBest.t = tMax
  traverse TLAS in increasing t

  for each candidate instance from visited leaves:
    if candidate already tested by this ray:
      continue

    instanceRange = intersect(worldRay, instance.worldBounds)
    if no intersection or instanceRange.tMin > hitBest.t:
      continue

    hit = TraceInstanceSdf(instance, worldRay, instanceRange)
    if hit and hit.t < hitBest.t:
      hitBest = hit

  return hitBest
```

The result should preserve enough identity for shading and debugging:

```cpp
struct SdfSceneHit {
  uint32_t instanceId;
  uint32_t assetId;
  uint32_t leafNode;
  uint32_t sdfNode;
  uint32_t brickIndex;
  uint16_t sdfLevel;
  float t;
  Vec3 worldPosition;
  Vec3 worldNormal;
  Vec3 localPosition;
};
```

## Quality Controls

Expose these settings per asset or per import profile:

```text
requestedErrorWorld
maxSdfLevel
rootResolution
narrowBandVoxels
distanceFormat
smallTriangleExpansion
maxBuildSamplesPerBrick
```

Expose these settings for runtime tracing:

```text
tlasMaxDepth = 16
tlasLeafTargetInstanceCount
maxInstanceTraceSteps
hitThresholdScale
stepScale
minStepScale
enableMailbox
debugMaxSdfLevel
```

Debug views should include:

- TLAS leaf occupancy
- per-ray candidate instance count
- instance SDF level hit
- SDF error heatmap
- current SDF zero surface preview
- mesh-vs-SDF silhouette comparison

## Implementation Plan And Tests

Each phase should land with its own focused tests. Prefer small deterministic fixtures first, then Sponza-scale validation once the file and trace paths are stable.

### Phase 1: VSDF Disk Structures

Implementation:

- Add `SDFSceneFile` structs for `VSDFHeader`, `VSDFInstanceDirectoryEntry`, and instance payload records.
- Add read/write helpers with little-endian validation and 16-byte alignment checks.
- Add LZ4 block compression/decompression for one instance payload.
- Reject malformed offsets, overlapping payloads, unknown compression modes, bad magic, and unsupported versions.

Tests:

- `VSDFHeaderRoundTrip`: write a header and two instance directory entries, reload them, and compare every field.
- `VSDFRejectsBadMagic`: mutate the magic/version and verify load fails.
- `VSDFRejectsBadOffsets`: point an instance payload outside the file and verify load fails.
- `VSDFLZ4InstanceRoundTrip`: compress one tiny payload with one node, one brick, and 512 `R8_SNORM` distances, then decompress and compare bytes.
- `VSDFMultipleInstanceRandomAccess`: write three compressed instance payloads and load only instance 1; verify the loader does not need to decode instances 0 or 2.

### Phase 2: Offline Adaptive SDF Builder

Implementation:

- Build a triangle closest-point BVH for source geometry.
- Build a winding-number sign query or an initial robust parity fallback.
- Emit root 8x8x8 bricks.
- Compute mesh-to-SDF, SDF-to-mesh, and sign error.
- Recursively refine until `requestedErrorWorld` or `maxLevel`.
- Serialize each scene instance as one compressed VSDF payload.

Tests:

- `SDFSingleTrianglePlane`: build an SDF for one triangle and verify samples near the triangle have small unsigned distance.
- `SDFCubeSign`: build a closed cube and verify inside samples are negative and outside samples are positive.
- `SDFErrorRefinesMore`: build the same mesh with high and low error tolerances; verify the low-error asset has more nodes/bricks and lower measured validation error.
- `SDFThinTriangleConservativeEmission`: place a very thin triangle between voxel centers and verify the emitted SDF still creates near-surface coverage.
- `SDFDeterministicBuild`: build the same fixture twice and verify identical node counts, brick counts, and payload hash.

### Phase 3: Virtual Geometry Import Integration

Implementation:

- Add importer options such as `--emit-vsdf`, `--vsdf-output`, `--sdf-error`, and `--sdf-max-level`.
- Generate `.vsdf` from original mesh/scene triangles while generating `.virtualgeometry`.
- Keep `.virtualgeometry` success independent from optional VSDF generation in permissive mode, but strict in CI/import-validation mode.

Tests:

- `ImporterWritesVSDFSidecar`: run the OBJ or glTF importer on a tiny fixture with `--emit-vsdf` and verify both output files exist.
- `ImporterVSDFInstanceCountMatchesScene`: import a fixture with multiple nodes/instances and verify `VSDFHeader::instanceCount`.
- `ImporterUsesOriginalTriangles`: import a fixture where simplification would remove a small triangle; verify the VSDF validation/debug data still sees the triangle.
- `ImporterCompressionStatsValid`: verify every directory entry has `compression == VSDF_COMPRESSION_LZ4`, nonzero compressed size, and correct uncompressed size.

### Phase 4: SDFSceneManager CPU Runtime

Implementation:

- Add `SDFSceneManager`.
- Load `.vsdf` files and register one runtime SDF surface per VSDF instance.
- Store decoded BLAS payloads, instance transforms, and instance id mappings.
- Build a CPU sparse-octree TLAS over instance world bounds with max depth 16.
- Add CPU debug traversal for deterministic tests.

Tests:

- `SDFSceneManagerLoadsAllInstances`: load a multi-instance VSDF and verify runtime instance count, BLAS count, and bounds.
- `SDFSceneManagerPreservesInstanceIds`: verify a hit/candidate can be mapped back to the VSDF instance id and scene instance id.
- `SDFTlasMaxDepth16`: build pathological clustered instances and verify no node depth exceeds 16.
- `SDFTlasLargeInstanceStaysHigh`: insert a large scene-covering instance and verify it is not duplicated into an excessive number of leaves.
- `SDFTlasCandidateQuery`: query a ray/AABB region and verify expected instance ids are returned.

### Phase 5: GPU Upload And Buffer Validation

Implementation:

- Upload `SDFInstances`, `SDFBlases`, `SDFBlasNodes`, `SDFBlasBricks`, `SDFDistanceBytes`, `SDFTlasNodes`, and `SDFTlasInstanceRefs`.
- Add a small compute validation pass that reads selected records and writes summary counters.
- Add debug views for TLAS leaf occupancy and BLAS bounds.

Tests:

- `SDFGpuUploadCounts`: verify GPU-visible counts match CPU manager counts.
- `SDFGpuReadbackFirstInstance`: read back a validation buffer containing first instance id, BLAS root node, and TLAS root bounds.
- `SDFGpuDistanceDecode`: decode known `R8_SNORM` and `R16_SNORM` brick samples on GPU and compare against CPU decoded distances within tolerance.
- `SDFGpuTlasLeafRefsValid`: GPU validation checks every leaf ref points to a valid instance index.

### Phase 6: Fast GPU Trace Kernel

Implementation:

- Add `SDFTracePass` and `assets/shaders/sdf/wgsl/sdf-trace.wgsl`.
- Implement stack-based nearest-first TLAS traversal.
- Add a fixed per-ray mailbox for duplicate instance references.
- March candidate BLAS SDFs in local space.
- Write `SDFTraceHitGPU` records.

Tests:

- `SDFGpuTraceSingleCubeHit`: shoot a ray into a cube SDF and verify hit, instance id, and approximate hit distance.
- `SDFGpuTraceSingleCubeMiss`: shoot a ray outside the cube bounds and verify miss.
- `SDFGpuTraceNearestInstance`: place two instances along a ray and verify the closer instance wins.
- `SDFGpuTraceDuplicateLeafMailbox`: force an instance into multiple TLAS leaves and verify the trace reports one hit without excessive duplicate candidate tests.
- `SDFGpuTraceNormalFacesRay`: verify the returned normal opposes or is consistently oriented against the incoming ray.

### Phase 7: Sponza Instance Validation

Implementation:

- Build `sponza.vsdf` while building the Sponza `.virtualgeometry` asset.
- Generate one SDF payload per Sponza scene instance.
- Pick `N = 8` triangles from the original source scene. Prefer triangles spread across different instances.
- Add a debug view that draws those 8 rays, their expected triangle points, and the reported SDF hit points.
- For each picked triangle, create a ray using the triangle normal as the direction directly into the triangle:

```text
trianglePoint = barycentric sample near triangle center
rayDirection = -triangleWorldNormal
rayOrigin = trianglePoint + triangleWorldNormal * rayStartOffset
expectedHitDistance = rayStartOffset
```

The phrase "using the triangle normal as direction directly into the triangle" means the ray should start slightly outside the surface and travel opposite the outward normal. If the source normal orientation is unreliable, test both `normal` and `-normal` and require one of them to hit near the triangle.

Tests:

- `SponzaVSDFOneSdfPerInstance`: load Sponza VSDF and verify VSDF instance count equals the imported static scene instance count.
- `SponzaVSDFAllInstancesHavePayloads`: verify every directory entry has a valid LZ4 payload, at least one node, at least one brick, and non-empty distance data.
- `SponzaNormalRay8TrianglesCPU`: CPU trace the 8 generated normal rays through the TLAS and BLAS path; every ray must hit the expected instance.
- `SponzaNormalRay8TrianglesGPU`: GPU trace the same 8 rays; every ray must hit and report the expected instance id.
- `SponzaNormalRayHitDistance`: for each ray, require `abs(hit.t - expectedHitDistance) <= max(2 * localLeafVoxelWorldSize, hitDistanceTolerance)`.
- `SponzaNormalRayHitPosition`: require `distance(hit.worldPosition, trianglePoint) <= max(2 * localLeafVoxelWorldSize, positionTolerance)`.
- `SponzaNormalRayDebugDump`: on failure, dump ray origin/direction, expected instance, reported instance, hit distance, triangle id, SDF level, brick id, and nearest triangle distance.

### Phase 8: Sparse DDA Optimization

Implementation:

- Add sparse DDA traversal inside TLAS nodes or replace child sorting if profiling shows stack child sorting is expensive.
- Keep stack traversal as a validation/debug mode.
- Record traversal counters: nodes visited, leaf refs tested, BLAS marches, SDF steps.

Tests:

- `SDFDdaMatchesStackTraversal`: compare DDA and stack traversal hit records over a deterministic ray set.
- `SDFDdaNeverMissesStackHit`: every stack hit must be reproduced by DDA within tolerance.
- `SDFDdaTraversalCounters`: verify DDA does not increase candidate instance tests on representative fixtures.

### Phase 9: Shading Integration

Implementation:

- Feed `SDFTraceHitGPU` into a resolve pass.
- Use `instanceId`, `materialRange`, and local hit position to resolve material or surface-cache data.
- Keep the trace pass visibility-only.

Tests:

- `SDFHitCarriesMaterialRange`: verify hit records can resolve the expected material range for a known fixture.
- `SDFHitLocalPositionStable`: trace the same fixture under two transforms and verify local hit position remains stable in instance space.
- `SDFVisibilityOnlyTrace`: verify the trace kernel does not bind or require material textures.

## Key Rule

This representation is per-instance and identity-preserving:

```text
world acceleration structure: sparse TLAS octree
visibility surface: adaptive sparse SDF per instance
composition: none
hit identity: required
```

That gives controllable silhouettes from the adaptive SDF error metric while avoiding the loss of instance identity and subtraction problems that come with a merged global SDF.
