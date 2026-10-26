#include "virtualgeometry/wgsl/virtualgeometrydata.wgsl"
#include "vsm-common.wgsl"

const WORKGROUP_SIZE: u32 = 64u;
const HIERARCHY_GLOBAL_PULL_PER_WORKGROUP: u32 = 64u;
const HIERARCHY_LOCAL_QUEUE_CAPACITY: u32 = WORKGROUP_SIZE;
const MAX_LOCAL_QUEUE_PROCESS_ITERS: u32 = 4u;
const SENTINEL_VALUE: u32 = 0xFFFFFFFFu;
const HIERARCHY_LEAF_FLAG: u32 = 1u << 0u;
const LIGHT_TYPE_DIRECTIONAL: u32 = 0u;
const LOCAL_LIGHT_NEAR_PLANE: f32 = 0.1;

const COUNTER_HIERARCHY_QUEUE_SIZE: u32 = 0u;
const COUNTER_CLUSTER_QUEUE_SIZE: u32 = 1u;
const COUNTER_READ_QUEUE_SIZE: u32 = 2u;
const COUNTER_SHADOW_VISIBLE_CLUSTER_COUNT: u32 = 3u;
const COUNTER_SHADOW_DRAW_OVERFLOW: u32 = 4u;
const COUNTER_BUDGETED_DIRTY_PAGE_COUNT: u32 = 5u;

var<workgroup> hierarchyLocalQueueIndex    : array<u32, HIERARCHY_LOCAL_QUEUE_CAPACITY>;
var<workgroup> hierarchyLocalQueueInstance : array<u32, HIERARCHY_LOCAL_QUEUE_CAPACITY>;
var<workgroup> hierarchyLocalQueueLayer    : array<u32, HIERARCHY_LOCAL_QUEUE_CAPACITY>;
var<workgroup> hierarchyLocalQueuePadding  : array<u32, HIERARCHY_LOCAL_QUEUE_CAPACITY>;
var<workgroup> hierarchyLocalQueueSize     : atomic<u32>;
var<workgroup> hierarchyIterationItemCount : u32;
var<workgroup> hierarchyFlushCount         : u32;

struct VSMCullingUniforms {
    instancesCount: u32,
    activeLayers: u32,
    maxVisibleClusterDrawsPerLayer: u32,
    pageTableResolution: u32,
    physicalPageSize: u32,
    hpbMipCount: u32,
    maxScenePages: u32,
    lodErrorThreshold: f32,
    maxHierarchyNodes: u32,
    maxDirtyPagesPerFrame: u32,
}

struct CascadeState {
    pageOffset: vec2<i32>,
    pageShift: vec2<i32>,
    _padding: vec4<u32>,
}

struct CascadeMatrix {
    view: mat4x4<f32>,
    proj: mat4x4<f32>,
    viewProj: mat4x4<f32>,
    worldExtent: f32,
    pageWorldSize: f32,
    lightIndex: u32,
    cascadeIndex: u32,
    pageTableResolution: u32,
    lightType: u32,
    faceIndex: u32,
    mipLevel: u32,
}

struct ShadowVisibleClusterInfo {
    pageIndex: u32,
    pageLocalClusterIndex: u32,
    instanceIndex: u32,
    layer: u32,
    meshPartIndex: u32,
#ifdef VSM_PER_PAGE_DRAW_COMMANDS
    pageCoordsPacked: u32,
    linearPageCoordsPacked: u32,
#else
    _padding: u32,
#endif
}

@group(0) @binding(0)  var<uniform> uniforms: VSMCullingUniforms;
@group(0) @binding(1)  var<storage, read> instances: array<InstanceData>;
@group(0) @binding(2)  var<storage, read> hierarchy: array<VirtualMeshHierarchy>;
@group(0) @binding(3)  var<storage, read_write> counters: array<atomic<u32>>;
@group(0) @binding(4)  var<storage, read_write> hierarchyQueueRead: array<QueueElement>;
@group(0) @binding(5)  var<storage, read_write> hierarchyQueueWrite: array<QueueElement>;
@group(0) @binding(6)  var<storage, read_write> clusterQueue: array<QueueElement>;
@group(0) @binding(7)  var<storage, read> meshPartTransforms: array<mat4x4<f32>>;
@group(0) @binding(8)  var<storage, read_write> indirectArgs: IndirectDispatchArgs;
@group(0) @binding(9)  var<storage, read> scenePageTable: array<PageTableEntry>;
@group(0) @binding(10) var<storage, read> scenePagesBuffer: array<u32>;
@group(0) @binding(11) var<storage, read> virtualPageTable: array<u32>;
@group(0) @binding(12) var<storage, read> cascadeStates: array<CascadeState>;
@group(0) @binding(13) var<storage, read> cascadeMatrices: array<CascadeMatrix>;
@group(0) @binding(14) var hpbTexture: texture_2d<f32>;
@group(0) @binding(15) var<storage, read_write> shadowDrawIndirectBuffer: array<u32>;
@group(0) @binding(16) var<storage, read_write> shadowVisibleClusterInfos: array<ShadowVisibleClusterInfo>;
@group(0) @binding(17) var<storage, read_write> processedPages: array<atomic<u32>>;
@group(0) @binding(18) var<storage, read_write> pageClusterCounts: array<atomic<u32>>;
@group(0) @binding(19) var<storage, read_write> layerVisibleCounts: array<atomic<u32>>;
@group(0) @binding(20) var<storage, read_write> dirtyPageCounts: array<atomic<u32>>;
@group(0) @binding(21) var<storage, read_write> dirtyPageList: array<u32>;
@group(0) @binding(22) var<storage, read_write> globalDirtyPageCount: atomic<u32>;
@group(0) @binding(23) var<storage, read_write> globalDirtyPageList: array<u32>;
@group(0) @binding(24) var<storage, read> virtualPageState: array<u32>;

/// @brief Reads a single word from a meshlet descriptor field in the scene pages buffer.
/// @param pageBase The word base of the page in the scene pages buffer.
/// @param localIdx The local meshlet index within the page.
/// @param fieldOff The word offset of the desired field within the meshlet descriptor.
/// @returns The descriptor word at the specified field.
fn readDesc(pageBase: u32, localIdx: u32, fieldOff: u32) -> u32 {
    return scenePagesBuffer[pageBase + PAGE_HEADER_WORDS + localIdx * MESHLET_DESC_WORDS + fieldOff];
}

/// @brief Constructs the local-space AABB of a meshlet from its quantized position descriptor fields.
/// @param pageBase The word base of the page in the scene pages buffer.
/// @param localIdx The local meshlet index within the page.
/// @param unitScale The per-instance unit scale factor.
/// @returns The local-space AABB of the meshlet.
fn meshletLocalAABB(pageBase: u32, localIdx: u32, unitScale: f32) -> AABB {
    let minQX = bitcast<f32>(readDesc(pageBase, localIdx, DESC_MIN_X));
    let minQY = bitcast<f32>(readDesc(pageBase, localIdx, DESC_MIN_Y));
    let minQZ = bitcast<f32>(readDesc(pageBase, localIdx, DESC_MIN_Z));
    let spanX = readDesc(pageBase, localIdx, DESC_POS_SPAN_X);
    let spanY = readDesc(pageBase, localIdx, DESC_POS_SPAN_Y);
    let spanZ = readDesc(pageBase, localIdx, DESC_POS_SPAN_Z);
    let qf = readDesc(pageBase, localIdx, DESC_QFACTOR);
    let dq = f32(1u << qf) * unitScale;

    var r: AABB;
    r.min = vec3<f32>(minQX, minQY, minQZ) / dq;
    r.max = vec3<f32>(
        (minQX + f32(spanX)) / dq,
        (minQY + f32(spanY)) / dq,
        (minQZ + f32(spanZ)) / dq);
    return r;
}

/// @brief Resolves a raw per-instance page index to a global scene page index.
/// @param rawPageIndex The raw page index stored in the hierarchy node.
/// @param pageTableOffset The page table base offset for the instance.
/// @returns The global scene page index, or SENTINEL_VALUE if the raw index is invalid.
fn resolveGlobalPageIndex(rawPageIndex: u32, pageTableOffset: u32) -> u32 {
    if (rawPageIndex == SENTINEL_VALUE) { return SENTINEL_VALUE; }
    return pageTableOffset + (rawPageIndex & PAGE_INDEX_MASK);
}

/// @brief Tests whether the clusters of a hierarchy node are currently installed (resident) in the scene page buffer.
/// @param node The hierarchy node to test.
/// @returns True if the node's page is installed.
fn areNodeClustersInstalled(node: VirtualMeshHierarchy) -> bool {
    return (node.pageIndex & PAGE_NOT_INSTALLED_BIT) == 0u;
}

/// @brief Resolves the model matrix for a mesh part, combining the instance model matrix with an optional per-part transform.
/// @param instance The instance data containing the base model matrix and transform offset.
/// @param meshPartIndex The local mesh part index, or SENTINEL_VALUE to use the instance model matrix directly.
/// @returns The resolved world-space model matrix for the mesh part.
fn resolveMeshPartModelMatrix(instance: InstanceData, meshPartIndex: u32) -> mat4x4<f32> {
    if (meshPartIndex == SENTINEL_VALUE) {
        return instance.modelMatrix;
    }
    return instance.modelMatrix * meshPartTransforms[instance.meshPartTransformsOffset + meshPartIndex];
}

/// @brief Transforms an AABB by a matrix and returns a new axis-aligned bounding box in the target space.
/// @param aabbMin The minimum corner of the input AABB.
/// @param aabbMax The maximum corner of the input AABB.
/// @param m The transformation matrix.
/// @returns A new AABB enclosing the transformed corners.
fn transformAABB(aabbMin: vec3<f32>, aabbMax: vec3<f32>, m: mat4x4<f32>) -> AABB {
    var corners = array<vec3<f32>, 8>(
        vec3<f32>(aabbMin.x, aabbMin.y, aabbMin.z), vec3<f32>(aabbMax.x, aabbMin.y, aabbMin.z),
        vec3<f32>(aabbMin.x, aabbMax.y, aabbMin.z), vec3<f32>(aabbMax.x, aabbMax.y, aabbMin.z),
        vec3<f32>(aabbMin.x, aabbMin.y, aabbMax.z), vec3<f32>(aabbMax.x, aabbMin.y, aabbMax.z),
        vec3<f32>(aabbMin.x, aabbMax.y, aabbMax.z), vec3<f32>(aabbMax.x, aabbMax.y, aabbMax.z));
    var wMin = (m * vec4<f32>(corners[0], 1.0)).xyz;
    var wMax = wMin;
    for (var i = 1u; i < 8u; i++) {
        let wc = (m * vec4<f32>(corners[i], 1.0)).xyz;
        wMin = min(wMin, wc);
        wMax = max(wMax, wc);
    }
    var r: AABB;
    r.min = wMin;
    r.max = wMax;
    return r;
}

/// @brief Computes the union of two AABBs.
/// @param aMin The minimum corner of the first AABB.
/// @param aMax The maximum corner of the first AABB.
/// @param bMin The minimum corner of the second AABB.
/// @param bMax The maximum corner of the second AABB.
/// @returns An AABB that encloses both inputs.
fn unionAABBs(aMin: vec3<f32>, aMax: vec3<f32>, bMin: vec3<f32>, bMax: vec3<f32>) -> AABB {
    var r: AABB;
    r.min = min(aMin, bMin);
    r.max = max(aMax, bMax);
    return r;
}

/// @brief Returns the axis-aligned bounding box of a sphere.
/// @param center The sphere center in local space.
/// @param radius The sphere radius.
/// @returns An AABB that tightly encloses the sphere.
fn sphereBounds(center: vec3<f32>, radius: f32) -> AABB {
    var r: AABB;
    let extent = vec3<f32>(radius);
    r.min = center - extent;
    r.max = center + extent;
    return r;
}

/// @brief Returns the culling AABB for a hierarchy node, optionally expanded for skinned meshes.
/// @param node The hierarchy node.
/// @param isSkinned True if the node belongs to a skinned mesh instance.
/// @returns The conservative local-space culling AABB.
fn nodeCullAABB(node: VirtualMeshHierarchy, isSkinned: bool) -> AABB {
    let geomMin = vec3<f32>(node.min_x, node.min_y, node.min_z);
    let geomMax = vec3<f32>(node.max_x, node.max_y, node.max_z);
    if (!isSkinned) {
        var result: AABB;
        result.min = geomMin;
        result.max = geomMax;
        return result;
    }

    let parentCenter = vec3<f32>(node.max_center_x, node.max_center_y, node.max_center_z);
    let parentExtent = vec3<f32>(node.max_radius);
    let parentMin = parentCenter - parentExtent;
    let parentMax = parentCenter + parentExtent;
    return unionAABBs(geomMin, geomMax, parentMin, parentMax);
}

/// @brief Returns the culling AABB for a cluster, optionally expanded with self and parent LOD sphere bounds for skinned meshes.
/// @param pageBase The word base of the page in the scene pages buffer.
/// @param localIdx The local meshlet index within the page.
/// @param unitScale The per-instance unit scale factor.
/// @param isSkinned True if the cluster belongs to a skinned mesh instance.
/// @returns The conservative local-space culling AABB.
fn clusterCullAABB(pageBase: u32, localIdx: u32, unitScale: f32, isSkinned: bool) -> AABB {
    var result = meshletLocalAABB(pageBase, localIdx, unitScale);
    if (!isSkinned) {
        return result;
    }

    let selfCenter = vec3<f32>(
        bitcast<f32>(readDesc(pageBase, localIdx, DESC_SELF_CX)),
        bitcast<f32>(readDesc(pageBase, localIdx, DESC_SELF_CY)),
        bitcast<f32>(readDesc(pageBase, localIdx, DESC_SELF_CZ)));
    let selfRadius = bitcast<f32>(readDesc(pageBase, localIdx, DESC_SELF_R));
    let selfBounds = sphereBounds(selfCenter, selfRadius);
    result = unionAABBs(result.min, result.max, selfBounds.min, selfBounds.max);

    let parentCenter = vec3<f32>(
        bitcast<f32>(readDesc(pageBase, localIdx, DESC_PAR_CX)),
        bitcast<f32>(readDesc(pageBase, localIdx, DESC_PAR_CY)),
        bitcast<f32>(readDesc(pageBase, localIdx, DESC_PAR_CZ)));
    let parentRadius = bitcast<f32>(readDesc(pageBase, localIdx, DESC_PAR_R));
    let parentBounds = sphereBounds(parentCenter, parentRadius);
    result = unionAABBs(result.min, result.max, parentBounds.min, parentBounds.max);
    return result;
}

/// @brief Returns a conservative world-space AABB for a skinned cluster by unioning the skinned and rest-pose transforms.
/// @param localAABB The local-space culling AABB of the cluster.
/// @param instanceModelMatrix The instance's base model matrix (rest pose).
/// @param meshPartModelMatrix The resolved mesh-part model matrix (current pose).
/// @param meshPartIndex The mesh part index, or SENTINEL_VALUE if no per-part transform is used.
/// @returns A conservative world-space AABB enclosing both poses.
fn conservativeSkinnedWorldAABB(localAABB: AABB, instanceModelMatrix: mat4x4<f32>, meshPartModelMatrix: mat4x4<f32>, meshPartIndex: u32) -> AABB {
    var result = transformAABB(localAABB.min, localAABB.max, meshPartModelMatrix);
    if (meshPartIndex != SENTINEL_VALUE) {
        let restPoseWorldAABB = transformAABB(localAABB.min, localAABB.max, instanceModelMatrix);
        result = unionAABBs(result.min, result.max, restPoseWorldAABB.min, restPoseWorldAABB.max);
    }
    return result;
}

fn getConservativeInstanceScale(m: mat4x4<f32>) -> f32 {
    let sx = length(vec3<f32>(m[0].x, m[0].y, m[0].z));
    let sy = length(vec3<f32>(m[1].x, m[1].y, m[1].z));
    let sz = length(vec3<f32>(m[2].x, m[2].y, m[2].z));
    return max(max(sx, sy), max(sz, 1e-6));
}

fn calculateProjectedShadowError(
    centerLocal: vec3<f32>,
    radiusLocal: f32,
    errorLocal: f32,
    modelMatrix: mat4x4<f32>,
    layer: u32
) -> f32 {
    let world_scale = getConservativeInstanceScale(modelMatrix);
    let world_error = errorLocal * world_scale;
    let world_center = (modelMatrix * vec4<f32>(centerLocal, 1.0)).xyz;
    let center_view = (cascadeMatrices[layer].view * vec4<f32>(world_center, 1.0)).xyz;
    let radius_world = radiusLocal * world_scale;
    var projected_error = world_error;

    if (cascadeMatrices[layer].lightType != LIGHT_TYPE_DIRECTIONAL) {
        let view_depth = max(-center_view.z - radius_world, LOCAL_LIGHT_NEAR_PLANE);
        projected_error = projected_error / view_depth;
    }

    let shadow_viewport_height = f32(max(layer_page_table_resolution(layer) * uniforms.physicalPageSize, 1u));
    return projected_error * abs(cascadeMatrices[layer].proj[1][1]) * 0.5 * shadow_viewport_height;
}

/// @brief Computes the flat virtual page table index for the given layer and page coordinate.
/// @param layer The cascade layer index.
/// @param pageCoord The 2D page coordinate within the layer.
/// @returns The flat array index into the virtual page table.
fn vsm_vpt_index(layer: u32, pageCoord: vec2<u32>) -> u32 {
    let pagesPerLayer = uniforms.pageTableResolution * uniforms.pageTableResolution;
    return layer * pagesPerLayer + pageCoord.y * uniforms.pageTableResolution + pageCoord.x;
}

fn layer_page_table_resolution(layer: u32) -> u32 {
    return max(cascadeMatrices[layer].pageTableResolution, 1u);
}

fn hpb_atlas_tiles_per_row() -> u32 {
    let atlas_dims = textureDimensions(hpbTexture, 0);
    return max(atlas_dims.x / max(uniforms.pageTableResolution, 1u), 1u);
}

fn hpb_base_tile_origin(layer: u32) -> vec2<u32> {
    let tiles_per_row = hpb_atlas_tiles_per_row();
    return vec2<u32>(
        (layer % tiles_per_row) * uniforms.pageTableResolution,
        (layer / tiles_per_row) * uniforms.pageTableResolution);
}

fn hpb_mip_tile_origin(layer: u32, level: u32) -> vec2<u32> {
    let base_origin = hpb_base_tile_origin(layer);
    return vec2<u32>(base_origin.x >> level, base_origin.y >> level);
}

fn hpb_local_texel_to_atlas_texel(layer: u32, local_texel: vec2<u32>, level: u32) -> vec2<i32> {
    let tile_origin = hpb_mip_tile_origin(layer, level);
    return vec2<i32>(tile_origin + local_texel);
}

/// @brief Packs a cascade layer index and a hierarchy node index into a single 32-bit word.
/// @param layer The cascade layer index (8-bit).
/// @param nodeIndex The hierarchy node index (24-bit).
/// @returns The packed word.
fn packLayerNode(layer: u32, nodeIndex: u32) -> u32 {
    return ((layer & 0xFFu) << 24u) | (nodeIndex & 0x00FFFFFFu);
}

/// @brief Extracts the cascade layer index from a packed layer-node word.
/// @param packed The packed 32-bit word.
/// @returns The cascade layer index.
fn unpackLayer(packed: u32) -> u32 {
    return packed >> 24u;
}

/// @brief Extracts the hierarchy node index from a packed layer-node word.
/// @param packed The packed 32-bit word.
/// @returns The hierarchy node index.
fn unpackNodeIndex(packed: u32) -> u32 {
    return packed & 0x00FFFFFFu;
}

fn packPageCoords(pageCoord: vec2<u32>) -> u32 {
    return (pageCoord.x & 0xFFFFu) | ((pageCoord.y & 0xFFFFu) << 16u);
}

fn packLinearPageCoords(pageCoord: vec2<i32>) -> u32 {
    let packedX = bitcast<u32>(pageCoord.x) & 0xFFFFu;
    let packedY = bitcast<u32>(pageCoord.y) & 0xFFFFu;
    return packedX | (packedY << 16u);
}

/// @brief Creates a sentinel (invalid) queue element with all fields set to SENTINEL_VALUE.
/// @returns An invalid QueueElement used to signal an empty slot.
fn invalidQueueElement() -> QueueElement {
    return QueueElement(SENTINEL_VALUE, SENTINEL_VALUE, SENTINEL_VALUE, SENTINEL_VALUE);
}

/// @brief Appends a hierarchy queue element to the global (cross-workgroup) hierarchy queue.
/// @param element The queue element to spill to the global queue.
fn spillHierarchyElementToGlobalQueue(element: QueueElement) {
    let globalIndex = atomicAdd(&counters[COUNTER_HIERARCHY_QUEUE_SIZE], 1u);
    hierarchyQueueWrite[globalIndex] = element;
}

/// @brief Pushes a hierarchy queue element to the workgroup-local queue, spilling to the global queue when it is full.
/// @param element The queue element to push.
fn pushHierarchyElementToLocalOrGlobalQueue(element: QueueElement) {
    let localIndex = atomicAdd(&hierarchyLocalQueueSize, 1u);
    if (localIndex < HIERARCHY_LOCAL_QUEUE_CAPACITY) {
        hierarchyLocalQueueIndex[localIndex] = element.index;
        hierarchyLocalQueueInstance[localIndex] = element.instanceIndex;
        hierarchyLocalQueueLayer[localIndex] = element.pageIndex;
        hierarchyLocalQueuePadding[localIndex] = element._padding;
        return;
    }

    spillHierarchyElementToGlobalQueue(element);
}

/// @brief Frustum-culls a world-space AABB against a shadow cascade's view-projection.
/// @param worldMin The minimum corner of the world-space AABB.
/// @param worldMax The maximum corner of the world-space AABB.
/// @param layer The cascade layer index.
/// @returns True if the AABB intersects the cascade frustum.
fn clampAbsMin(x: f32, minAbsValue: f32) -> f32 {
    if (x >= 0.0) {
        return max(x, minAbsValue);
    }
    return min(x, -minAbsValue);
}

struct SphereBounds {
    center: vec3<f32>,
    radius: f32,
}

fn aabbBoundingSphere(aabbMin: vec3<f32>, aabbMax: vec3<f32>) -> SphereBounds {
    let center = (aabbMin + aabbMax) * 0.5;
    let radius = length(aabbMax - center);
    return SphereBounds(center, radius);
}

fn fullLayerPageRange(layer: u32) -> vec4<i32> {
    let page_table_resolution = i32(layer_page_table_resolution(layer));
    return vec4<i32>(0, 0, page_table_resolution - 1, page_table_resolution - 1);
}

fn perspectiveFrustumCullAABB(worldMin: vec3<f32>, worldMax: vec3<f32>, layer: u32) -> bool {
    var corners = array<vec3<f32>, 8>(
        vec3<f32>(worldMin.x, worldMin.y, worldMin.z), vec3<f32>(worldMax.x, worldMin.y, worldMin.z),
        vec3<f32>(worldMin.x, worldMax.y, worldMin.z), vec3<f32>(worldMax.x, worldMax.y, worldMin.z),
        vec3<f32>(worldMin.x, worldMin.y, worldMax.z), vec3<f32>(worldMax.x, worldMin.y, worldMax.z),
        vec3<f32>(worldMin.x, worldMax.y, worldMax.z), vec3<f32>(worldMax.x, worldMax.y, worldMax.z));

    let invProjX = 1.0 / max(abs(cascadeMatrices[layer].proj[0][0]), 1e-8);
    let invProjY = 1.0 / max(abs(cascadeMatrices[layer].proj[1][1]), 1e-8);
    var allOutsideLeft = true;
    var allOutsideRight = true;
    var allOutsideBottom = true;
    var allOutsideTop = true;
    var allOutsideNear = true;

    for (var i = 0u; i < 8u; i++) {
        let view = cascadeMatrices[layer].view * vec4<f32>(corners[i], 1.0);
        let leftPlane = view.z * invProjX;
        let rightPlane = -view.z * invProjX;
        let bottomPlane = view.z * invProjY;
        let topPlane = -view.z * invProjY;

        if (view.x >= leftPlane) {
            allOutsideLeft = false;
        }
        if (view.x <= rightPlane) {
            allOutsideRight = false;
        }
        if (view.y >= bottomPlane) {
            allOutsideBottom = false;
        }
        if (view.y <= topPlane) {
            allOutsideTop = false;
        }
        if (view.z <= -LOCAL_LIGHT_NEAR_PLANE) {
            allOutsideNear = false;
        }
    }

    return !(allOutsideLeft || allOutsideRight || allOutsideBottom || allOutsideTop || allOutsideNear);
}

fn cascadeFrustumCullAABB(worldMin: vec3<f32>, worldMax: vec3<f32>, layer: u32) -> bool {
    if (cascadeMatrices[layer].lightType != LIGHT_TYPE_DIRECTIONAL) {
        return perspectiveFrustumCullAABB(worldMin, worldMax, layer);
    }

    var corners = array<vec3<f32>, 8>(
        vec3<f32>(worldMin.x, worldMin.y, worldMin.z), vec3<f32>(worldMax.x, worldMin.y, worldMin.z),
        vec3<f32>(worldMin.x, worldMax.y, worldMin.z), vec3<f32>(worldMax.x, worldMax.y, worldMin.z),
        vec3<f32>(worldMin.x, worldMin.y, worldMax.z), vec3<f32>(worldMax.x, worldMin.y, worldMax.z),
        vec3<f32>(worldMin.x, worldMax.y, worldMax.z), vec3<f32>(worldMax.x, worldMax.y, worldMax.z));

    var minClip = vec3<f32>(1e20, 1e20, 1e20);
    var maxClip = vec3<f32>(-1e20, -1e20, -1e20);
    for (var i = 0u; i < 8u; i++) {
        let clip = cascadeMatrices[layer].viewProj * vec4<f32>(corners[i], 1.0);
        let invW = select(1.0, 1.0 / clip.w, abs(clip.w) > 1e-6);
        let ndc = clip.xyz * invW;
        minClip = min(minClip, ndc);
        maxClip = max(maxClip, ndc);
    }

    return !(maxClip.x < -1.0 || minClip.x > 1.0 || maxClip.y < -1.0 || minClip.y > 1.0 || maxClip.z < 0.0 || minClip.z > 1.0);
}

fn projectWorldAABBToCascadePageRange(worldMin: vec3<f32>, worldMax: vec3<f32>, layer: u32) -> vec4<i32> {
    if (cascadeMatrices[layer].lightType != LIGHT_TYPE_DIRECTIONAL) {
        let sphere = aabbBoundingSphere(worldMin, worldMax);
        let centerView = (cascadeMatrices[layer].view * vec4<f32>(sphere.center, 1.0)).xyz;
        let center = vec3<f32>(centerView.x, centerView.y, -centerView.z);
        if (center.z < sphere.radius + LOCAL_LIGHT_NEAR_PLANE) {
            return fullLayerPageRange(layer);
        }

        let cr = center * sphere.radius;
        let czr2 = max(center.z * center.z - sphere.radius * sphere.radius, 0.0);
        let vx = sqrt(center.x * center.x + czr2);
        let minx = (vx * center.x - cr.z) / clampAbsMin(vx * center.z + cr.x, 1e-6);
        let maxx = (vx * center.x + cr.z) / clampAbsMin(vx * center.z - cr.x, 1e-6);
        let vy = sqrt(center.y * center.y + czr2);
        let miny = (vy * center.y - cr.z) / clampAbsMin(vy * center.z + cr.y, 1e-6);
        let maxy = (vy * center.y + cr.z) / clampAbsMin(vy * center.z - cr.y, 1e-6);

        let ndcMin = vec2<f32>(minx * abs(cascadeMatrices[layer].proj[0][0]), miny * abs(cascadeMatrices[layer].proj[1][1]));
        let ndcMax = vec2<f32>(maxx * abs(cascadeMatrices[layer].proj[0][0]), maxy * abs(cascadeMatrices[layer].proj[1][1]));
        var uvMin = clamp(ndcMin * 0.5 + vec2<f32>(0.5, 0.5), vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 1.0));
        var uvMax = clamp(ndcMax * 0.5 + vec2<f32>(0.5, 0.5), vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 1.0));

#ifdef VSM_PER_PAGE_DRAW_COMMANDS
        let clipmapResolution = max(layer_page_table_resolution(layer) * uniforms.physicalPageSize, 1u);
        let uvInflation = 1.0 / f32(clipmapResolution);
        uvMin = max(uvMin - vec2<f32>(uvInflation, uvInflation), vec2<f32>(0.0, 0.0));
        uvMax = min(uvMax + vec2<f32>(uvInflation, uvInflation), vec2<f32>(1.0, 1.0));
#endif

        let pagesXYf = f32(layer_page_table_resolution(layer));
        let minPageXY = vec2<i32>(floor(uvMin * pagesXYf));
        let maxPageXY = vec2<i32>(ceil(uvMax * pagesXYf) - vec2<f32>(1.0));
        return vec4<i32>(
            minPageXY.x,
            minPageXY.y,
            max(maxPageXY.x, minPageXY.x),
            max(maxPageXY.y, minPageXY.y));
    }

    var uvMin = vec2<f32>(1e30, 1e30);
    var uvMax = vec2<f32>(-1e30, -1e30);
    var corners = array<vec3<f32>, 8>(
        vec3<f32>(worldMin.x, worldMin.y, worldMin.z), vec3<f32>(worldMax.x, worldMin.y, worldMin.z),
        vec3<f32>(worldMin.x, worldMax.y, worldMin.z), vec3<f32>(worldMax.x, worldMax.y, worldMin.z),
        vec3<f32>(worldMin.x, worldMin.y, worldMax.z), vec3<f32>(worldMax.x, worldMin.y, worldMax.z),
        vec3<f32>(worldMin.x, worldMax.y, worldMax.z), vec3<f32>(worldMax.x, worldMax.y, worldMax.z));
    for (var i = 0u; i < 8u; i++) {
        let clip = cascadeMatrices[layer].viewProj * vec4<f32>(corners[i], 1.0);
        let invW = select(1.0, 1.0 / clip.w, abs(clip.w) > 1e-6);
        let ndc = clip.xyz * invW;
        let rawUV = ndc.xy * 0.5 + vec2<f32>(0.5, 0.5);
        uvMin = min(uvMin, rawUV);
        uvMax = max(uvMax, rawUV);
    }

#ifdef VSM_PER_PAGE_DRAW_COMMANDS
    let clipmapResolution = max(layer_page_table_resolution(layer) * uniforms.physicalPageSize, 1u);
    let uvInflation = 1.0 / f32(clipmapResolution);
    uvMin = uvMin - vec2<f32>(uvInflation, uvInflation);
    uvMax = uvMax + vec2<f32>(uvInflation, uvInflation);
#endif

    let pagesXYf = f32(layer_page_table_resolution(layer));
    let minPageXY = vec2<i32>(floor(uvMin * pagesXYf));
    let maxPageXY = vec2<i32>(ceil(uvMax * pagesXYf) - vec2<f32>(1.0));
    return vec4<i32>(
        minPageXY.x,
        minPageXY.y,
        max(maxPageXY.x, minPageXY.x),
        max(maxPageXY.y, minPageXY.y));
}

/// @brief Projects a transformed local-space AABB into page-space coordinates for the given cascade layer.
/// @param localMin The minimum corner of the local-space AABB.
/// @param localMax The maximum corner of the local-space AABB.
/// @param transform The model matrix transforming from local to world space.
/// @param layer The cascade layer index.
/// @returns The integer page range (minX, minY, maxX, maxY) in the cascade's page table.
fn projectTransformedAABBToCascadePageRange(localMin: vec3<f32>, localMax: vec3<f32>, transform: mat4x4<f32>, layer: u32) -> vec4<i32> {
    if (cascadeMatrices[layer].lightType == LIGHT_TYPE_DIRECTIONAL) {
        var uvMin = vec2<f32>(1e30, 1e30);
        var uvMax = vec2<f32>(-1e30, -1e30);
        var corners = array<vec3<f32>, 8>(
            vec3<f32>(localMin.x, localMin.y, localMin.z), vec3<f32>(localMax.x, localMin.y, localMin.z),
            vec3<f32>(localMin.x, localMax.y, localMin.z), vec3<f32>(localMax.x, localMax.y, localMin.z),
            vec3<f32>(localMin.x, localMin.y, localMax.z), vec3<f32>(localMax.x, localMin.y, localMax.z),
            vec3<f32>(localMin.x, localMax.y, localMax.z), vec3<f32>(localMax.x, localMax.y, localMax.z));
        for (var i = 0u; i < 8u; i++) {
            let clip = cascadeMatrices[layer].viewProj * (transform * vec4<f32>(corners[i], 1.0));
            let invW = select(1.0, 1.0 / clip.w, abs(clip.w) > 1e-6);
            let ndc = clip.xyz * invW;
            let rawUV = ndc.xy * 0.5 + vec2<f32>(0.5, 0.5);
            uvMin = min(uvMin, rawUV);
            uvMax = max(uvMax, rawUV);
        }

#ifdef VSM_PER_PAGE_DRAW_COMMANDS
        let clipmapResolution = max(layer_page_table_resolution(layer) * uniforms.physicalPageSize, 1u);
        let uvInflation = 1.0 / f32(clipmapResolution);
        uvMin = uvMin - vec2<f32>(uvInflation, uvInflation);
        uvMax = uvMax + vec2<f32>(uvInflation, uvInflation);
#endif

        let pagesXYf = f32(layer_page_table_resolution(layer));
        let minPageXY = vec2<i32>(floor(uvMin * pagesXYf));
        let maxPageXY = vec2<i32>(ceil(uvMax * pagesXYf) - vec2<f32>(1.0));
        return vec4<i32>(
            minPageXY.x,
            minPageXY.y,
            max(maxPageXY.x, minPageXY.x),
            max(maxPageXY.y, minPageXY.y));
    }

    let worldAABB = transformAABB(localMin, localMax, transform);
    return projectWorldAABBToCascadePageRange(worldAABB.min, worldAABB.max, layer);
}

/// @brief Computes the union of two integer page ranges.
/// @param a The first page range (minX, minY, maxX, maxY).
/// @param b The second page range (minX, minY, maxX, maxY).
/// @returns The smallest page range enclosing both inputs.
fn unionPageRanges(a: vec4<i32>, b: vec4<i32>) -> vec4<i32> {
    return vec4<i32>(
        min(a.x, b.x),
        min(a.y, b.y),
        max(a.z, b.z),
        max(a.w, b.w));
}

/// @brief Projects a local-space AABB conservatively into cascade page space, accounting for skinning.
/// @param localAABB The local-space culling AABB.
/// @param instanceModelMatrix The instance's base model matrix (rest pose).
/// @param meshPartModelMatrix The resolved mesh-part model matrix (current pose).
/// @param meshPartIndex The mesh part index, or SENTINEL_VALUE if there is no per-part transform.
/// @param layer The cascade layer index.
/// @returns The conservative integer page range in the cascade's page table.
fn projectLocalAABBToCascadePageRange(localAABB: AABB, instanceModelMatrix: mat4x4<f32>, meshPartModelMatrix: mat4x4<f32>, meshPartIndex: u32, layer: u32) -> vec4<i32> {
    let meshPartRange = projectTransformedAABBToCascadePageRange(localAABB.min, localAABB.max, meshPartModelMatrix, layer);
    if (meshPartIndex == SENTINEL_VALUE) {
        return meshPartRange;
    }

    let restPoseRange = projectTransformedAABBToCascadePageRange(localAABB.min, localAABB.max, instanceModelMatrix, layer);
    return unionPageRanges(meshPartRange, restPoseRange);
}

struct WrappedInterval {
    count: u32,
    min0: u32,
    max0: u32,
    min1: u32,
    max1: u32,
}

/// @brief Wraps an integer page coordinate into the range [0, resolution).
/// @param coord The signed page coordinate to wrap.
/// @param resolution The page table resolution.
/// @returns The wrapped unsigned page coordinate.
fn wrapPageCoord(coord: i32, resolution: i32) -> u32 {
    return u32(((coord % resolution) + resolution) % resolution);
}

/// @brief Splits a linear page coordinate interval into at most two contiguous wrapped sub-intervals.
/// @param minCoord The minimum (inclusive) coordinate of the interval.
/// @param maxCoord The maximum (inclusive) coordinate of the interval.
/// @param resolution The page table resolution used for wrapping.
/// @returns A WrappedInterval describing one or two contiguous ranges in wrapped space.
fn splitWrappedInterval(minCoord: i32, maxCoord: i32, resolution: u32) -> WrappedInterval {
    let resolutionI = i32(resolution);
    if ((maxCoord - minCoord + 1) >= resolutionI) {
        return WrappedInterval(1u, 0u, resolution - 1u, 0u, 0u);
    }

    let wrappedMin = wrapPageCoord(minCoord, resolutionI);
    let wrappedMax = wrapPageCoord(maxCoord, resolutionI);
    if (wrappedMin <= wrappedMax) {
        return WrappedInterval(1u, wrappedMin, wrappedMax, 0u, 0u);
    }

    return WrappedInterval(2u, 0u, wrappedMax, wrappedMin, resolution - 1u);
}

/// @brief Returns the minimum coordinate of a wrapped sub-interval by index.
/// @param interval The WrappedInterval containing one or two sub-intervals.
/// @param index 0 for the first sub-interval, 1 for the second.
/// @returns The minimum coordinate of the selected sub-interval.
fn getWrappedIntervalMin(interval: WrappedInterval, index: u32) -> u32 {
    if (index == 0u) {
        return interval.min0;
    }
    return interval.min1;
}

/// @brief Returns the maximum coordinate of a wrapped sub-interval by index.
/// @param interval The WrappedInterval containing one or two sub-intervals.
/// @param index 0 for the first sub-interval, 1 for the second.
/// @returns The maximum coordinate of the selected sub-interval.
fn getWrappedIntervalMax(interval: WrappedInterval, index: u32) -> u32 {
    if (index == 0u) {
        return interval.max0;
    }
    return interval.max1;
}

/// @brief Selects the appropriate HPB mip level for a given page diameter.
/// @param pageDiameter The diameter of the page region in page-space units.
/// @returns The HPB mip level index to use for that region size.
fn getHPBLevelFromPageDiameter(pageDiameter: f32) -> u32 {
    let unclampedLevel = u32(max(ceil(log2(max(pageDiameter, 1.0))), 0.0));
    return min(unclampedLevel, uniforms.hpbMipCount - 1u);
}

/// @brief Computes the HPB texel bounds for a given wrapped page rect and mip level.
/// @param pageMin The minimum wrapped page coordinate.
/// @param pageMax The maximum wrapped page coordinate.
/// @param level The HPB mip level index.
/// @returns The clamped texel bounds (minX, minY, maxX, maxY) in the HPB mip.
fn getHPBTexelBounds(layer: u32, pageMin: vec2<u32>, pageMax: vec2<u32>, level: u32) -> vec4<u32> {
    let atlas_mip_dims = textureDimensions(hpbTexture, i32(level));
    let local_mip_resolution = max(layer_page_table_resolution(layer) >> level, 1u);
    let page_table_size = f32(layer_page_table_resolution(layer));
    let local_mip_size = vec2<f32>(f32(local_mip_resolution), f32(local_mip_resolution));

    let minLocalTexF = floor((vec2<f32>(pageMin) / page_table_size) * local_mip_size);
    let maxLocalTexF = ceil((vec2<f32>(pageMax + vec2<u32>(1u)) / page_table_size) * local_mip_size) - vec2<f32>(1.0);

    let local_min_tex = clamp(
        vec2<i32>(minLocalTexF),
        vec2<i32>(0),
        vec2<i32>(i32(local_mip_resolution) - 1, i32(local_mip_resolution) - 1));
    let local_max_tex = clamp(
        max(vec2<i32>(maxLocalTexF), local_min_tex),
        vec2<i32>(0),
        vec2<i32>(i32(local_mip_resolution) - 1, i32(local_mip_resolution) - 1));

    let atlas_origin = vec2<i32>(hpb_mip_tile_origin(layer, level));
    let atlas_min_tex = clamp(
        atlas_origin + local_min_tex,
        vec2<i32>(0),
        vec2<i32>(atlas_mip_dims) - vec2<i32>(1));
    let atlas_max_tex = clamp(
        atlas_origin + local_max_tex,
        atlas_min_tex,
        vec2<i32>(atlas_mip_dims) - vec2<i32>(1));

    return vec4<u32>(u32(atlas_min_tex.x), u32(atlas_min_tex.y), u32(atlas_max_tex.x), u32(atlas_max_tex.y));
}

/// @brief Samples the HPB texture to test whether any texel in the given bounds is active (dirty pages present).
/// @param layer The cascade layer index (HPB array layer).
/// @param texelBounds The texel region to test (minX, minY, maxX, maxY).
/// @param level The HPB mip level to sample.
/// @returns True if any texel in the region has a value greater than 0.
fn sampleHPBAnyActive(layer: u32, texelBounds: vec4<u32>, level: u32) -> bool {
    _ = layer;
    for (var y = texelBounds.y; y <= texelBounds.w; y++) {
        for (var x = texelBounds.x; x <= texelBounds.z; x++) {
            if (textureLoad(hpbTexture, vec2<i32>(i32(x), i32(y)), i32(level)).r > 0.0) {
                return true;
            }
        }
    }
    return false;
}

/// @brief Tests whether a wrapped page rectangle in a cascade layer contains any dirty pages via the HPB.
/// @param layer The cascade layer index.
/// @param wrappedMin The minimum wrapped page coordinate.
/// @param wrappedMax The maximum wrapped page coordinate.
/// @returns True if the HPB indicates at least one dirty page within the rectangle.
fn hpbWrappedRectHasDirtyPages(layer: u32, wrappedMin: vec2<u32>, wrappedMax: vec2<u32>) -> bool {
    let pageSpan = vec2<f32>(f32(wrappedMax.x - wrappedMin.x + 1u), f32(wrappedMax.y - wrappedMin.y + 1u));
    let level = getHPBLevelFromPageDiameter(max(pageSpan.x, pageSpan.y));
    let texelBounds = getHPBTexelBounds(layer, wrappedMin, wrappedMax, level);
    return sampleHPBAnyActive(layer, texelBounds, level);
}

/// @brief Tests whether a linear page range in a cascade layer intersects any dirty pages, handling toroidal wrap.
/// @param layer The cascade layer index.
/// @param pageRange The linear page range (minX, minY, maxX, maxY) to test.
/// @returns True if any dirty page falls within the (wrapped) page range.
fn hpbRectHasDirtyPages(layer: u32, pageRange: vec4<i32>) -> bool {
    let page_table_resolution = layer_page_table_resolution(layer);
    let offset = cascadeStates[layer].pageOffset;
    let fullSpanX = (pageRange.z - pageRange.x + 1) >= i32(page_table_resolution);
    let fullSpanY = (pageRange.w - pageRange.y + 1) >= i32(page_table_resolution);
    let minPageX = select(pageRange.x, 0, fullSpanX);
    let maxPageX = select(pageRange.z, i32(page_table_resolution) - 1, fullSpanX);
    let minPageY = select(pageRange.y, 0, fullSpanY);
    let maxPageY = select(pageRange.w, i32(page_table_resolution) - 1, fullSpanY);
    let xIntervals = splitWrappedInterval(minPageX + offset.x, maxPageX + offset.x, page_table_resolution);
    let yIntervals = splitWrappedInterval(minPageY + offset.y, maxPageY + offset.y, page_table_resolution);

    for (var yIntervalIndex = 0u; yIntervalIndex < 2u; yIntervalIndex++) {
        if (yIntervalIndex >= yIntervals.count) {
            continue;
        }
        for (var xIntervalIndex = 0u; xIntervalIndex < 2u; xIntervalIndex++) {
            if (xIntervalIndex >= xIntervals.count) {
                continue;
            }

            let wrappedMin = vec2<u32>(
                getWrappedIntervalMin(xIntervals, xIntervalIndex),
                getWrappedIntervalMin(yIntervals, yIntervalIndex));
            let wrappedMax = vec2<u32>(
                getWrappedIntervalMax(xIntervals, xIntervalIndex),
                getWrappedIntervalMax(yIntervals, yIntervalIndex));
            if (hpbWrappedRectHasDirtyPages(layer, wrappedMin, wrappedMax)) {
                return true;
            }
        }
    }

    return false;
}

fn selectDirtyPageForDraw(layer: u32, wrappedPage: vec2<u32>, pageIndex: u32, isFallbackPage: bool) -> bool {
    if ((virtualPageState[pageIndex] & VSM_PAGE_STATE_SCHEDULED_BIT)==0u) { return false; }
    var selected = false;
    for (var attempt = 0u; attempt < 64u; attempt++) {
        let state = atomicLoad(&processedPages[pageIndex]);
        if (state == 1u) {
            selected = true;
            break;
        }
        if (state == 3u) {
            break;
        }
        if (state == 0u) {
            let claim = atomicCompareExchangeWeak(&processedPages[pageIndex], 0u, 2u);
            if (claim.exchanged) {
                // Bookkeeping already selected an exact coarse-first quota.
                // Count it here for diagnostics, without a second racing budget.
                atomicAdd(&counters[COUNTER_BUDGETED_DIRTY_PAGE_COUNT],1u);

                let globalDirtyPageIndex = atomicAdd(&globalDirtyPageCount, 1u);
                let dirtyPageLocalIndex = atomicAdd(&dirtyPageCounts[layer], 1u);
                let pagesPerLayer = uniforms.pageTableResolution * uniforms.pageTableResolution;
                dirtyPageList[layer * pagesPerLayer + dirtyPageLocalIndex] = pack_layered_coords(wrappedPage.x, wrappedPage.y, layer);
                globalDirtyPageList[globalDirtyPageIndex] = pack_layered_coords(wrappedPage.x, wrappedPage.y, layer);
                atomicStore(&processedPages[pageIndex], 1u);
                selected = true;
                break;
            }
        }
    }
    return selected;
}

/// @brief Emits a shadow draw call entry for a visible cluster into the indirect draw and visible cluster info buffers.
/// @param globalPageIdx The global scene page index of the cluster.
/// @param meshletLocalIdx The local meshlet index within the page.
/// @param instanceIdx The instance index.
/// @param layer The cascade layer index this draw is for.
/// @param meshPartIndex The mesh part index.
/// @param triangleCount The number of triangles in the cluster.
fn emitShadowDraw(
    globalPageIdx: u32,
    meshletLocalIdx: u32,
    instanceIdx: u32,
    layer: u32,
    meshPartIndex: u32,
#ifdef VSM_PER_PAGE_DRAW_COMMANDS
    wrappedPageCoord: vec2<u32>,
    linearPageCoord: vec2<i32>,
#endif
    triangleCount: u32
) {
#ifdef VSM_PER_PAGE_DRAW_COMMANDS
    let outIdx = atomicAdd(&counters[COUNTER_SHADOW_VISIBLE_CLUSTER_COUNT], 1u);
    let maxDrawCount = uniforms.maxVisibleClusterDrawsPerLayer * uniforms.activeLayers;
    if (outIdx >= maxDrawCount) {
        atomicAdd(&counters[COUNTER_SHADOW_DRAW_OVERFLOW], 1u);
        return;
    }
#else
    let localIdx = atomicAdd(&layerVisibleCounts[layer], 1u);
    if (localIdx >= uniforms.maxVisibleClusterDrawsPerLayer) {
        atomicAdd(&counters[COUNTER_SHADOW_DRAW_OVERFLOW], 1u);
        return;
    }

    atomicAdd(&counters[COUNTER_SHADOW_VISIBLE_CLUSTER_COUNT], 1u);
    let outIdx = layer * uniforms.maxVisibleClusterDrawsPerLayer + localIdx;
#endif

    shadowVisibleClusterInfos[outIdx].pageIndex = globalPageIdx;
    shadowVisibleClusterInfos[outIdx].pageLocalClusterIndex = meshletLocalIdx;
    shadowVisibleClusterInfos[outIdx].instanceIndex = instanceIdx;
    shadowVisibleClusterInfos[outIdx].layer = layer;
    shadowVisibleClusterInfos[outIdx].meshPartIndex = meshPartIndex;
#ifdef VSM_PER_PAGE_DRAW_COMMANDS
    shadowVisibleClusterInfos[outIdx].pageCoordsPacked = packPageCoords(wrappedPageCoord);
    shadowVisibleClusterInfos[outIdx].linearPageCoordsPacked = packLinearPageCoords(linearPageCoord);
#else
    shadowVisibleClusterInfos[outIdx]._padding = 0u;
#endif

#ifndef DRAW_INDIRECT_COUNT_DISABLED
    let slot = outIdx * 4u;
#ifdef VSM_IMPERFECT_POINT_SPLATS
    shadowDrawIndirectBuffer[slot + 0u] = triangleCount;
#else
    shadowDrawIndirectBuffer[slot + 0u] = triangleCount * 3u;
#endif
    shadowDrawIndirectBuffer[slot + 1u] = 1u;
    shadowDrawIndirectBuffer[slot + 2u] = 0u;
    shadowDrawIndirectBuffer[slot + 3u] = outIdx;
#endif
}

// Admit every scheduled allocated page before traversing casters. Empty pages
// must also be cleared and completed or their fine descendants wait forever.
@compute @workgroup_size(WORKGROUP_SIZE)
fn admit_scheduled_pages(@builtin(global_invocation_id) id: vec3<u32>) {
    let n=uniforms.pageTableResolution;
    if (id.x>=n*n*uniforms.activeLayers) { return; }
    let entry=virtualPageTable[id.x];
    if (!get_is_allocated(entry) || !get_is_dirty(entry)) { return; }
    let state=virtualPageState[id.x];
    if ((state & VSM_PAGE_STATE_SCHEDULED_BIT)==0u) { return; }
    _=selectDirtyPageForDraw(id.x/(n*n),vec2<u32>(id.x%n,(id.x/n)%n),id.x,get_page_state_is_fallback(state));
}

/// @brief Initializes all counters, processed-page flags, and indirect buffer entries to zero before the culling pass.
@compute @workgroup_size(WORKGROUP_SIZE)
fn init_sync(@builtin(global_invocation_id) global_id: vec3<u32>) {
    if (global_id.x < 8u) {
        atomicStore(&counters[global_id.x], 0u);
    }
    if (global_id.x == 8u) {
        atomicStore(&globalDirtyPageCount, 0u);
    }

    let totalPages = uniforms.pageTableResolution * uniforms.pageTableResolution * uniforms.activeLayers;
    if (global_id.x < totalPages) {
        atomicStore(&processedPages[global_id.x], 0u);
        atomicStore(&pageClusterCounts[global_id.x], 0u);
    }

    if (global_id.x < uniforms.activeLayers) {
        atomicStore(&layerVisibleCounts[global_id.x], 0u);
        atomicStore(&dirtyPageCounts[global_id.x], 0u);
    }

#ifdef DRAW_INDIRECT_COUNT_DISABLED
    let layerCommandWordCount = uniforms.activeLayers * 4u;
    if (global_id.x < layerCommandWordCount) {
        let layer = global_id.x / 4u;
        let layerWord = global_id.x % 4u;
        let slot = layer * uniforms.maxVisibleClusterDrawsPerLayer * 4u + layerWord;
        shadowDrawIndirectBuffer[slot] = 0u;
    }
#endif
}

/// @brief Enqueues the root hierarchy node for each (instance, cascade layer) pair into the hierarchy traversal queue.
@compute @workgroup_size(WORKGROUP_SIZE)
fn setup_root_nodes(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let totalRoots = uniforms.instancesCount * uniforms.activeLayers;
    if (global_id.x >= totalRoots) {
        return;
    }

    let instanceIdx = global_id.x / uniforms.activeLayers;
    let layer = global_id.x % uniforms.activeLayers;
    let idx = atomicAdd(&counters[COUNTER_HIERARCHY_QUEUE_SIZE], 1u);
    hierarchyQueueWrite[idx].index = instances[instanceIdx].hierarchyStartOffset;
    hierarchyQueueWrite[idx].instanceIndex = instanceIdx;
    hierarchyQueueWrite[idx].pageIndex = layer;
    hierarchyQueueWrite[idx]._padding = 0u;
}

/// @brief Prepares the indirect dispatch arguments for the next hierarchy traversal pass.
@compute @workgroup_size(1)
fn prepare_indirect_dispatch() {
    let size = atomicLoad(&counters[COUNTER_HIERARCHY_QUEUE_SIZE]);
    atomicStore(&counters[COUNTER_READ_QUEUE_SIZE], size);
    indirectArgs.x = (size + HIERARCHY_GLOBAL_PULL_PER_WORKGROUP - 1u) / HIERARCHY_GLOBAL_PULL_PER_WORKGROUP;
    indirectArgs.y = 1u;
    indirectArgs.z = 1u;
    atomicStore(&counters[COUNTER_HIERARCHY_QUEUE_SIZE], 0u);
}

/// @brief Processes a single hierarchy node, culling it against the cascade frustum and HPB, and enqueuing children or clusters.
/// @param element The queue element describing the hierarchy node, instance, and cascade layer.
fn processHierarchyNodeElement(element: QueueElement) {
    let nodeIndex = element.index;
    let instanceIdx = element.instanceIndex;
    let layer = element.pageIndex;
    if (nodeIndex == SENTINEL_VALUE || instanceIdx == SENTINEL_VALUE || layer >= uniforms.activeLayers || nodeIndex >= uniforms.maxHierarchyNodes) {
        return;
    }

    let instance = instances[instanceIdx];
    let node = hierarchy[nodeIndex];
    let nodeModelMatrix = resolveMeshPartModelMatrix(instance, node.meshPartIndex);
    let forceTraversal = (node.flags & HIERARCHY_FORCE_TRAVERSAL_FLAG) != 0u;
    if (!forceTraversal && uniforms.lodErrorThreshold > 0.0) {
        let parentCenter = vec3<f32>(node.max_center_x, node.max_center_y, node.max_center_z);
        let projectedParentError = calculateProjectedShadowError(
            parentCenter,
            node.max_radius,
            node.max_parent_lod_error,
            nodeModelMatrix,
            layer);
        if (projectedParentError < uniforms.lodErrorThreshold) {
            return;
        }
    }

    let localCullAABB = nodeCullAABB(node, node.meshPartIndex != SENTINEL_VALUE);
    let worldAABB = conservativeSkinnedWorldAABB(localCullAABB, instance.modelMatrix, nodeModelMatrix, node.meshPartIndex);
    if (!cascadeFrustumCullAABB(worldAABB.min, worldAABB.max, layer)) {
        return;
    }
    let projectedPageRange = projectLocalAABBToCascadePageRange(
        localCullAABB,
        instance.modelMatrix,
        nodeModelMatrix,
        node.meshPartIndex,
        layer);
    if (!hpbRectHasDirtyPages(layer, projectedPageRange)) {
        return;
    }

    if ((node.flags & HIERARCHY_LEAF_FLAG) != 0u) {
        if (!areNodeClustersInstalled(node) || node.child_start == SENTINEL_VALUE || node.child_count == 0u) {
            return;
        }

        let globalPageIdx = resolveGlobalPageIndex(node.pageIndex, instance.pageTableOffset);
        let baseIndex = atomicAdd(&counters[COUNTER_CLUSTER_QUEUE_SIZE], node.child_count);
        for (var i = 0u; i < node.child_count; i++) {
            clusterQueue[baseIndex + i].index = node.child_start + i;
            clusterQueue[baseIndex + i].instanceIndex = instanceIdx;
            clusterQueue[baseIndex + i].pageIndex = globalPageIdx;
            clusterQueue[baseIndex + i]._padding = packLayerNode(layer, nodeIndex);
        }
        return;
    }

    let hierarchyRoot = instance.hierarchyStartOffset;
    for (var i = 0u; i < node.child_count; i++) {
        pushHierarchyElementToLocalOrGlobalQueue(QueueElement(
            hierarchyRoot + node.child_start + i,
            instanceIdx,
            layer,
            0u));
    }
}

/// @brief Main hierarchy traversal pass that pulls nodes from the global queue and processes them with workgroup-local buffering.
@compute @workgroup_size(WORKGROUP_SIZE)
fn process_hierarchy_nodes(@builtin(local_invocation_id) local_id: vec3<u32>, @builtin(workgroup_id) workgroup_id: vec3<u32>) {
    let readQueueSize = atomicLoad(&counters[COUNTER_READ_QUEUE_SIZE]);
    let globalReadBase = workgroup_id.x * HIERARCHY_GLOBAL_PULL_PER_WORKGROUP;
    if (globalReadBase >= readQueueSize) { return; }

    if (local_id.x == 0u) {
        atomicStore(&hierarchyLocalQueueSize, 0u);
        hierarchyIterationItemCount = 0u;
        hierarchyFlushCount = 0u;
    }
    workgroupBarrier();

    if (local_id.x < HIERARCHY_GLOBAL_PULL_PER_WORKGROUP) {
        let globalReadIndex = globalReadBase + local_id.x;
        if (globalReadIndex < readQueueSize) {
            let localWriteIndex = atomicAdd(&hierarchyLocalQueueSize, 1u);
            let element = hierarchyQueueRead[globalReadIndex];
            hierarchyLocalQueueIndex[localWriteIndex] = element.index;
            hierarchyLocalQueueInstance[localWriteIndex] = element.instanceIndex;
            hierarchyLocalQueueLayer[localWriteIndex] = element.pageIndex;
            hierarchyLocalQueuePadding[localWriteIndex] = element._padding;
        }
    }

    for (var iter = 0u; iter < MAX_LOCAL_QUEUE_PROCESS_ITERS; iter++) {
        workgroupBarrier();
        if (local_id.x == 0u) {
            hierarchyIterationItemCount = min(atomicLoad(&hierarchyLocalQueueSize), HIERARCHY_LOCAL_QUEUE_CAPACITY);
        }
        workgroupBarrier();

        var element = invalidQueueElement();
        if (local_id.x < hierarchyIterationItemCount) {
            let localQueueIndex = local_id.x;
            element = QueueElement(
                hierarchyLocalQueueIndex[localQueueIndex],
                hierarchyLocalQueueInstance[localQueueIndex],
                hierarchyLocalQueueLayer[localQueueIndex],
                hierarchyLocalQueuePadding[localQueueIndex]);
        }

        workgroupBarrier();
        if (local_id.x == 0u) {
            atomicStore(&hierarchyLocalQueueSize, 0u);
        }
        workgroupBarrier();

        processHierarchyNodeElement(element);
    }

    workgroupBarrier();
    if (local_id.x == 0u) {
        hierarchyFlushCount = min(atomicLoad(&hierarchyLocalQueueSize), HIERARCHY_LOCAL_QUEUE_CAPACITY);
    }
    workgroupBarrier();

    for (var flushOffset = local_id.x; flushOffset < hierarchyFlushCount; flushOffset += WORKGROUP_SIZE) {
        spillHierarchyElementToGlobalQueue(QueueElement(
            hierarchyLocalQueueIndex[flushOffset],
            hierarchyLocalQueueInstance[flushOffset],
            hierarchyLocalQueueLayer[flushOffset],
            hierarchyLocalQueuePadding[flushOffset]));
    }
}

/// @brief Prepares the indirect dispatch arguments for the cluster processing pass based on the current cluster queue size.
@compute @workgroup_size(1)
fn prepare_cluster_dispatch() {
    let size = atomicLoad(&counters[COUNTER_CLUSTER_QUEUE_SIZE]);
    indirectArgs.x = (size + WORKGROUP_SIZE - 1u) / WORKGROUP_SIZE;
    indirectArgs.y = 1u;
    indirectArgs.z = 1u;
}

/// @brief Processes a single cluster queue element: validates it, performs frustum and HPB culling, then emits shadow draw calls for all dirty pages the cluster overlaps.
/// @param element The queue element describing the cluster, instance, page, layer, and node to process.
fn processClusterElement(element: QueueElement) {
    if (element.index == SENTINEL_VALUE || element.instanceIndex == SENTINEL_VALUE || element.pageIndex == SENTINEL_VALUE) {
        return;
    }

    let globalPageIdx = element.pageIndex;
    if (globalPageIdx >= uniforms.maxScenePages || globalPageIdx >= arrayLength(&scenePageTable)) {
        return;
    }

    let layer = unpackLayer(element._padding);
    let nodeIndex = unpackNodeIndex(element._padding);
    if (layer >= uniforms.activeLayers || nodeIndex >= uniforms.maxHierarchyNodes) {
        return;
    }

    let pageEntry = scenePageTable[globalPageIdx];
    if (pageEntry.isInstalled == 0u) {
        return;
    }

    let triangleCount = readDesc(pageWordBase(pageEntry), element.index, DESC_TRI_COUNT);
    if (triangleCount == 0u) {
        return;
    }

    let instance = instances[element.instanceIndex];
    let node = hierarchy[nodeIndex];
    let pageBase = pageWordBase(pageEntry);
    let unitScale = bitcast<f32>(instance.unit_scale_bits);
    let clusterModelMatrix = resolveMeshPartModelMatrix(instance, node.meshPartIndex);
    let forceTraversal = (node.flags & HIERARCHY_FORCE_TRAVERSAL_FLAG) != 0u;

    if (!forceTraversal && uniforms.lodErrorThreshold > 0.0) {
        let selfCenter = vec3<f32>(
            bitcast<f32>(readDesc(pageBase, element.index, DESC_SELF_CX)),
            bitcast<f32>(readDesc(pageBase, element.index, DESC_SELF_CY)),
            bitcast<f32>(readDesc(pageBase, element.index, DESC_SELF_CZ)));
        let selfRadius = bitcast<f32>(readDesc(pageBase, element.index, DESC_SELF_R));
        let selfError = bitcast<f32>(readDesc(pageBase, element.index, DESC_SELF_ERR));
        let projectedSelfError = calculateProjectedShadowError(selfCenter, selfRadius, selfError, clusterModelMatrix, layer);
        if (projectedSelfError >= uniforms.lodErrorThreshold) {
            return;
        }

        let parentCenter = vec3<f32>(
            bitcast<f32>(readDesc(pageBase, element.index, DESC_PAR_CX)),
            bitcast<f32>(readDesc(pageBase, element.index, DESC_PAR_CY)),
            bitcast<f32>(readDesc(pageBase, element.index, DESC_PAR_CZ)));
        let parentRadius = bitcast<f32>(readDesc(pageBase, element.index, DESC_PAR_R));
        let parentError = bitcast<f32>(readDesc(pageBase, element.index, DESC_PAR_ERR));
        let projectedParentError = calculateProjectedShadowError(parentCenter, parentRadius, parentError, clusterModelMatrix, layer);
        if (projectedParentError < uniforms.lodErrorThreshold) {
            return;
        }
    }

    let localAABB = clusterCullAABB(pageBase, element.index, unitScale, node.meshPartIndex != SENTINEL_VALUE);
    let worldAABB = conservativeSkinnedWorldAABB(localAABB, instance.modelMatrix, clusterModelMatrix, node.meshPartIndex);
    if (!cascadeFrustumCullAABB(worldAABB.min, worldAABB.max, layer)) {
        return;
    }
    let pageRange = projectLocalAABBToCascadePageRange(
        localAABB,
        instance.modelMatrix,
        clusterModelMatrix,
        node.meshPartIndex,
        layer);
    if (!hpbRectHasDirtyPages(layer, pageRange)) {
        return;
    }
    let page_table_resolution = layer_page_table_resolution(layer);
    let fullSpanX = (pageRange.z - pageRange.x + 1) >= i32(page_table_resolution);
    let fullSpanY = (pageRange.w - pageRange.y + 1) >= i32(page_table_resolution);
    let minPageX = select(pageRange.x, 0, fullSpanX);
    let maxPageX = select(pageRange.z, i32(page_table_resolution) - 1, fullSpanX);
    let minPageY = select(pageRange.y, 0, fullSpanY);
    let maxPageY = select(pageRange.w, i32(page_table_resolution) - 1, fullSpanY);

#ifndef VSM_PER_PAGE_DRAW_COMMANDS
    var emitted = false;
#endif
    for (var pageY = minPageY; pageY <= maxPageY; pageY++) {
        for (var pageX = minPageX; pageX <= maxPageX; pageX++) {
            let wrapped = virtual_page_coords_to_wrapped_coords(vec2<i32>(i32(pageX), i32(pageY)), cascadeStates[layer].pageOffset, i32(page_table_resolution));
            if (wrapped.x < 0 || wrapped.y < 0) {
                continue;
            }

            let wrappedU = vec2<u32>(u32(wrapped.x), u32(wrapped.y));
            if (textureLoad(hpbTexture, hpb_local_texel_to_atlas_texel(layer, wrappedU, 0u), 0).r <= 0.0) {
                continue;
            }

            let pageIndex = vsm_vpt_index(layer, wrappedU);
            let pageEntry = virtualPageTable[pageIndex];
            if (!get_is_allocated(pageEntry) || !get_is_dirty(pageEntry)) {
                continue;
            }

            let isFallbackPage = get_page_state_is_fallback(virtualPageState[pageIndex]);
            if (!selectDirtyPageForDraw(layer, wrappedU, pageIndex, isFallbackPage)) {
                continue;
            }
            atomicAdd(&pageClusterCounts[pageIndex], 1u);
#ifdef VSM_PER_PAGE_DRAW_COMMANDS
            emitShadowDraw(
                globalPageIdx,
                element.index,
                element.instanceIndex,
                layer,
                node.meshPartIndex,
                wrappedU,
                vec2<i32>(pageX, pageY),
                triangleCount);
#else
            if (!emitted) {
                emitShadowDraw(
                    globalPageIdx,
                    element.index,
                    element.instanceIndex,
                    layer,
                    node.meshPartIndex,
                    triangleCount);
                emitted = true;
            }
#endif
        }
    }
}

/// @brief Main cluster processing pass that dispatches one thread per cluster queue entry and processes each via processClusterElement.
@compute @workgroup_size(WORKGROUP_SIZE)
fn process_clusters(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let clusterQueueSize = atomicLoad(&counters[COUNTER_CLUSTER_QUEUE_SIZE]);
    if (global_id.x >= clusterQueueSize) {
        return;
    }
    processClusterElement(clusterQueue[global_id.x]);
}

#ifdef DRAW_INDIRECT_COUNT_DISABLED
/// @brief Writes one indirect draw command per layer that covers the layer's emitted visible-cluster records.
@compute @workgroup_size(1)
fn prepare_shadow_draw_indirect_args() {
#ifdef VSM_PER_PAGE_DRAW_COMMANDS
    let clusterCount = min(atomicLoad(&counters[COUNTER_SHADOW_VISIBLE_CLUSTER_COUNT]), uniforms.maxVisibleClusterDrawsPerLayer * uniforms.activeLayers);
#ifdef VSM_IMPERFECT_POINT_SPLATS
    shadowDrawIndirectBuffer[0u] = CLUSTER_SIZE;
#else
    shadowDrawIndirectBuffer[0u] = CLUSTER_SIZE * 3u;
#endif
    shadowDrawIndirectBuffer[1u] = clusterCount;
    shadowDrawIndirectBuffer[2u] = 0u;
    shadowDrawIndirectBuffer[3u] = 0u;
#else
    for (var layer = 0u; layer < uniforms.activeLayers; layer++) {
        let slot = layer * uniforms.maxVisibleClusterDrawsPerLayer * 4u;
        let clusterCount = min(atomicLoad(&layerVisibleCounts[layer]), uniforms.maxVisibleClusterDrawsPerLayer);
#ifdef VSM_IMPERFECT_POINT_SPLATS
        shadowDrawIndirectBuffer[slot + 0u] = CLUSTER_SIZE;
#else
        shadowDrawIndirectBuffer[slot + 0u] = CLUSTER_SIZE * 3u;
#endif
        shadowDrawIndirectBuffer[slot + 1u] = clusterCount;
        shadowDrawIndirectBuffer[slot + 2u] = 0u;
        shadowDrawIndirectBuffer[slot + 3u] = 0u;
    }
#endif
}
#endif
