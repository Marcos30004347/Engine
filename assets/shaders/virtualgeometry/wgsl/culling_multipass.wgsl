// ============================================================================
// Virtual Geometry Hierarchical Culling Shader (Multi-Pass Version)
// ============================================================================

#include "virtualgeometrydata.wgsl"

#ifndef OCCLUSION_HIZ_MIP_BIAS
#define OCCLUSION_HIZ_MIP_BIAS 0
#endif

const WORKGROUP_SIZE                  : u32 = 256u;
const SENTINEL_VALUE                  : u32 = 0xFFFFFFFFu;
const SW_PIXELS_PER_TRIANGLE_THRESHOLD: f32 = 10.0;
const HIERARCHY_GLOBAL_PULL_PER_WORKGROUP: u32 = 64u;
const MAX_LOCAL_QUEUE_PROCESS_ITERS      : u32 = 8u;
const HIERARCHY_LOCAL_QUEUE_CAPACITY     : u32 = WORKGROUP_SIZE;
const DEBUG_MAX_CLUSTERS_PER_PAGE        : u32 = 512u;
const STREAMING_SELECTION_MAX            : u32 = 128u;
const STREAMING_SELECTION_WORKGROUP_SIZE : u32 = 64u;

var<workgroup> hierarchyLocalQueueIndex    : array<u32, HIERARCHY_LOCAL_QUEUE_CAPACITY>;
var<workgroup> hierarchyLocalQueueInstance : array<u32, HIERARCHY_LOCAL_QUEUE_CAPACITY>;
var<workgroup> hierarchyLocalQueuePage     : array<u32, HIERARCHY_LOCAL_QUEUE_CAPACITY>;
var<workgroup> hierarchyLocalQueuePadding  : array<u32, HIERARCHY_LOCAL_QUEUE_CAPACITY>;
var<workgroup> hierarchyLocalQueueSize     : atomic<u32>;
var<workgroup> hierarchyIterationItemCount : u32;
var<workgroup> hierarchyFlushCount         : u32;
var<workgroup> streamingInstallReductionPriorities : array<u32, STREAMING_SELECTION_WORKGROUP_SIZE>;
var<workgroup> streamingInstallReductionPages      : array<u32, STREAMING_SELECTION_WORKGROUP_SIZE>;
var<workgroup> streamingEvictReductionPriorities   : array<u32, STREAMING_SELECTION_WORKGROUP_SIZE>;
var<workgroup> streamingEvictReductionPages        : array<u32, STREAMING_SELECTION_WORKGROUP_SIZE>;
var<workgroup> streamingSelectedInstallPages       : array<u32, STREAMING_SELECTION_MAX>;
var<workgroup> streamingSelectedEvictPages         : array<u32, STREAMING_SELECTION_MAX>;
struct StreamingPageCandidate {
    globalPageIndex : u32,
    priority        : u32,
}

struct StreamingCullingUniforms {
    view                    : mat4x4<f32>,
    proj                    : mat4x4<f32>,
    viewPosition            : vec4<f32>,
    viewport                : vec2<u32>,
    error                   : f32,
    instances_count         : u32,
    clusters_count          : u32,
    nearPlane               : f32,
    farPlane                : f32,
    hiZLevels               : u32,
    cullingFlags            : u32,
    registeredPages         : u32,
    streamingSelectionCount : u32,
    _padding                : vec2<u32>,
}

// ============================================================================
// Bindings
// ============================================================================

@group(0) @binding(0)  var<uniform>            uniforms            : StreamingCullingUniforms;
@group(0) @binding(1)  var<storage, read>       instances           : array<InstanceData>;
@group(0) @binding(2)  var<storage, read>       hierarchy           : array<VirtualMeshHierarchy>;
@group(0) @binding(3)  var<storage, read_write> counters            : array<atomic<u32>, 16>;
@group(0) @binding(4)  var<storage, read_write> hierarchyQueueRead  : array<QueueElement>;
@group(0) @binding(5)  var<storage, read_write> hierarchyQueueWrite : array<QueueElement>;
@group(0) @binding(6)  var<storage, read_write> clusterQueue        : array<QueueElement>;
@group(0) @binding(7)  var<storage, read>       meshPartTransforms  : array<mat4x4<f32>>;
@group(0) @binding(8)  var<storage, read_write> indirectArgs        : IndirectDispatchArgs;
@group(0) @binding(9)  var<storage, read>       pageTable           : array<PageTableEntry>;
@group(0) @binding(10) var<storage, read_write> pagePriorities      : array<atomic<u32>>;
@group(0) @binding(11) var hiZTexture  : texture_2d<f32>;
@group(0) @binding(12) var hiZSampler  : sampler;
@group(0) @binding(13) var<storage, read_write> hwDrawIndirectBuffer  : array<u32>;
@group(0) @binding(14) var<storage, read_write> hwVisibleClusterInfos : array<VisibleClusterInfo>;
@group(0) @binding(15) var<storage, read>       pagesBuffer           : array<u32>;
@group(0) @binding(16) var<storage, read_write> swDrawIndirectBuffer  : array<u32>;
@group(0) @binding(17) var<storage, read_write> swVisibleClusterInfos : array<VisibleClusterInfo>;
@group(0) @binding(20) var<storage, read_write> pageInstallCandidates : array<StreamingPageCandidate>;
@group(0) @binding(21) var<storage, read_write> pageEvictCandidates   : array<StreamingPageCandidate>;
@group(0) @binding(22) var<storage, read>       decodedClusterCullData : array<DecodedClusterCullData>;
#ifdef DEBUG_BINDINGS
struct HierarchyDebugRecord {
    reason           : u32,
    parentErrorPxBits: u32,
    thresholdPxBits  : u32,
    instanceIndex    : u32,
}

struct ClusterDebugRecord {
    reason           : u32,
    selfErrorPxBits  : u32,
    parentErrorPxBits: u32,
    nodeIndex        : u32,
}

const HIER_DEBUG_NOT_VISITED                : u32 = 0u;
const HIER_DEBUG_ENQUEUED_CHILDREN          : u32 = 1u;
const HIER_DEBUG_ENQUEUED_CLUSTERS          : u32 = 2u;
const HIER_DEBUG_CULLED_PARENT_ERROR_SMALL  : u32 = 10u;
const HIER_DEBUG_CULLED_FRUSTUM             : u32 = 11u;
const HIER_DEBUG_CULLED_OCCLUSION           : u32 = 12u;
const HIER_DEBUG_CULLED_NOT_INSTALLED       : u32 = 13u;
const HIER_DEBUG_CULLED_INVALID_CHILD_START : u32 = 14u;
const HIER_DEBUG_CULLED_PROJECTED_TOO_SMALL : u32 = 15u;

const CLUSTER_DEBUG_NOT_VISITED             : u32 = 0u;
const CLUSTER_DEBUG_RENDERED_HW             : u32 = 1u;
const CLUSTER_DEBUG_RENDERED_SW             : u32 = 2u;
const CLUSTER_DEBUG_INVALID_QUEUE_ELEMENT   : u32 = 10u;
const CLUSTER_DEBUG_INVALID_PAGE_INDEX      : u32 = 11u;
const CLUSTER_DEBUG_PAGE_NOT_INSTALLED      : u32 = 12u;
const CLUSTER_DEBUG_ZERO_TRIANGLES          : u32 = 13u;
const CLUSTER_DEBUG_LOCAL_BIT_OUT_OF_RANGE  : u32 = 14u;
const CLUSTER_DEBUG_DISABLED_BY_MASK        : u32 = 15u;
const CLUSTER_DEBUG_SELF_ERROR_TOO_HIGH     : u32 = 16u;
const CLUSTER_DEBUG_PARENT_ERROR_TOO_LOW    : u32 = 17u;
const CLUSTER_DEBUG_CONE_CULLED             : u32 = 18u;
const CLUSTER_DEBUG_PROJECTED_TOO_SMALL     : u32 = 19u;

@group(0) @binding(18) var<storage, read_write> hierarchyDebugBuffer : array<HierarchyDebugRecord>;
@group(0) @binding(19) var<storage, read_write> clusterDebugBuffer   : array<ClusterDebugRecord>;

fn debugClusterLinearIndex(globalPageIdx: u32, localIdx: u32) -> u32 {
    return globalPageIdx * DEBUG_MAX_CLUSTERS_PER_PAGE + localIdx;
}

fn writeHierarchyDebug(nodeIndex: u32, reason: u32, parentErrorPx: f32, thresholdPx: f32, instanceIdx: u32) {
    if (nodeIndex < arrayLength(&hierarchyDebugBuffer)) {
        hierarchyDebugBuffer[nodeIndex].reason            = reason;
        hierarchyDebugBuffer[nodeIndex].parentErrorPxBits = bitcast<u32>(parentErrorPx);
        hierarchyDebugBuffer[nodeIndex].thresholdPxBits   = bitcast<u32>(thresholdPx);
        hierarchyDebugBuffer[nodeIndex].instanceIndex     = instanceIdx;
    }
}

fn writeClusterDebug(globalPageIdx: u32, localIdx: u32, reason: u32, selfErrorPx: f32, parentErrorPx: f32, nodeIndex: u32) {
    if (localIdx >= DEBUG_MAX_CLUSTERS_PER_PAGE) { return; }
    let idx = debugClusterLinearIndex(globalPageIdx, localIdx);
    if (idx < arrayLength(&clusterDebugBuffer)) {
        clusterDebugBuffer[idx].reason            = reason;
        clusterDebugBuffer[idx].selfErrorPxBits   = bitcast<u32>(selfErrorPx);
        clusterDebugBuffer[idx].parentErrorPxBits = bitcast<u32>(parentErrorPx);
        clusterDebugBuffer[idx].nodeIndex         = nodeIndex;
    }
}
#endif

// ============================================================================
// Page / descriptor helpers
// ============================================================================

fn readDesc(pageBase: u32, localIdx: u32, fieldOff: u32) -> u32 {
    return pagesBuffer[pageBase + PAGE_HEADER_WORDS + localIdx * MESHLET_DESC_WORDS + fieldOff];
}

fn readDecodedCluster(entry: PageTableEntry, localIdx: u32) -> DecodedClusterCullData {
    return decodedClusterCullData[globalClusterIndex(entry, localIdx)];
}

fn readTriangleCount(entry: PageTableEntry, localIdx: u32) -> u32 {
    return readDecodedCluster(entry, localIdx).triangleCount;
}

fn readVertexCount(entry: PageTableEntry, localIdx: u32) -> u32 {
    return readDesc(pageWordBase(entry), localIdx, DESC_VERT_COUNT);
}

// ============================================================================
// AABB from quantised position descriptor fields
// ============================================================================

fn meshletLocalAABB(clusterData: DecodedClusterCullData) -> AABB {
    var r: AABB;
    r.min = vec3<f32>(clusterData.localMinX, clusterData.localMinY, clusterData.localMinZ);
    r.max = vec3<f32>(clusterData.localMaxX, clusterData.localMaxY, clusterData.localMaxZ);
    return r;
}

// ============================================================================
// Page helpers
// ============================================================================

fn computePriority(node: VirtualMeshHierarchy, instance: InstanceData, nodeModelMatrix: mat4x4<f32>, projectedParentError: f32) -> u32 {
    let worldCenter = (nodeModelMatrix * vec4<f32>(node.max_center_x, node.max_center_y, node.max_center_z, 1.0)).xyz;
    let distanceToCamera = max(distance(worldCenter, uniforms.viewPosition.xyz), 0.001);
    let farDistance = max(uniforms.farPlane, node.max_radius + 1.0);
    let closeness = clamp(1.0 - (distanceToCamera / farDistance), 0.0, 1.0);
    let errorRatio = projectedParentError / max(uniforms.error, 0.0001);
    let urgency = clamp((errorRatio - 1.0) / 3.0, 0.0, 1.0);
    let radiusCoverage = clamp(node.max_radius / distanceToCamera, 0.0, 1.0);
    let perClusterScore = u32(round(clamp(
        (closeness * 0.55 + urgency * 0.30 + radiusCoverage * 0.15) * 100.0,
        1.0,
        100.0
    )));
    let clusterDemand = max(1u, min(node.child_count, 8u));
    return perClusterScore * clusterDemand;
}

fn resolveGlobalPageIndex(rawPageIndex: u32, pageTableOffset: u32) -> u32 {
    if (rawPageIndex == SENTINEL_VALUE) { return SENTINEL_VALUE; }
    return pageTableOffset + (rawPageIndex & PAGE_INDEX_MASK);
}

fn areNodeClustersInstalled(node: VirtualMeshHierarchy) -> bool {
    return (node.pageIndex & PAGE_NOT_INSTALLED_BIT) == 0u;
}

fn resolveMeshPartModelMatrix(instance: InstanceData, meshPartIndex: u32) -> mat4x4<f32> {
    if (meshPartIndex == SENTINEL_VALUE) {
        return instance.modelMatrix;
    }
    return instance.modelMatrix * meshPartTransforms[instance.meshPartTransformsOffset + meshPartIndex];
}

fn clearStreamingCandidate(candidateIndex: u32) {
    if (candidateIndex < uniforms.streamingSelectionCount) {
        pageInstallCandidates[candidateIndex].globalPageIndex = SENTINEL_VALUE;
        pageInstallCandidates[candidateIndex].priority = 0u;
        pageEvictCandidates[candidateIndex].globalPageIndex = SENTINEL_VALUE;
        pageEvictCandidates[candidateIndex].priority = 0u;
    }
}

fn insertDescendingCandidate(
    priorities: ptr<function, array<u32, STREAMING_SELECTION_MAX>>,
    pageIndices: ptr<function, array<u32, STREAMING_SELECTION_MAX>>,
    priority: u32,
    pageIndex: u32
) {
    if (priority == 0u || pageIndex == SENTINEL_VALUE) {
        return;
    }

    for (var i = 0u; i < STREAMING_SELECTION_MAX; i++) {
        if ((*pageIndices)[i] == pageIndex) {
            if (priority > (*priorities)[i]) {
                (*priorities)[i] = priority;
            }
            return;
        }
    }

    var insertAt = STREAMING_SELECTION_MAX;
    for (var i = 0u; i < STREAMING_SELECTION_MAX; i++) {
        if ((*pageIndices)[i] == SENTINEL_VALUE || priority > (*priorities)[i]) {
            insertAt = i;
            break;
        }
    }

    if (insertAt == STREAMING_SELECTION_MAX) {
        return;
    }

    for (var i = STREAMING_SELECTION_MAX - 1u; i > insertAt; i--) {
        (*priorities)[i] = (*priorities)[i - 1u];
        (*pageIndices)[i] = (*pageIndices)[i - 1u];
    }
    (*priorities)[insertAt] = priority;
    (*pageIndices)[insertAt] = pageIndex;
}

fn isPreviouslySelectedInstallPage(pageIndex: u32, selectedCount: u32) -> bool {
    for (var i = 0u; i < selectedCount; i++) {
        if (streamingSelectedInstallPages[i] == pageIndex) {
            return true;
        }
    }

    return false;
}

fn isPreviouslySelectedEvictPage(pageIndex: u32, selectedCount: u32) -> bool {
    for (var i = 0u; i < selectedCount; i++) {
        if (streamingSelectedEvictPages[i] == pageIndex) {
            return true;
        }
    }

    return false;
}

fn descendingCandidateWins(candidatePriority: u32, candidatePage: u32, currentPriority: u32, currentPage: u32) -> bool {
    if (candidatePage == SENTINEL_VALUE) {
        return false;
    }
    if (currentPage == SENTINEL_VALUE) {
        return true;
    }
    if (candidatePriority > currentPriority) {
        return true;
    }
    if (candidatePriority < currentPriority) {
        return false;
    }
    return candidatePage < currentPage;
}

fn ascendingCandidateWins(candidatePriority: u32, candidatePage: u32, currentPriority: u32, currentPage: u32) -> bool {
    if (candidatePage == SENTINEL_VALUE) {
        return false;
    }
    if (currentPage == SENTINEL_VALUE) {
        return true;
    }
    if (candidatePriority < currentPriority) {
        return true;
    }
    if (candidatePriority > currentPriority) {
        return false;
    }
    return candidatePage < currentPage;
}

fn insertAscendingCandidate(
    priorities: ptr<function, array<u32, STREAMING_SELECTION_MAX>>,
    pageIndices: ptr<function, array<u32, STREAMING_SELECTION_MAX>>,
    priority: u32,
    pageIndex: u32
) {
    if (pageIndex == SENTINEL_VALUE) {
        return;
    }

    for (var i = 0u; i < STREAMING_SELECTION_MAX; i++) {
        if ((*pageIndices)[i] == pageIndex) {
            if (priority < (*priorities)[i]) {
                (*priorities)[i] = priority;
            }
            return;
        }
    }

    var insertAt = STREAMING_SELECTION_MAX;
    for (var i = 0u; i < STREAMING_SELECTION_MAX; i++) {
        if ((*pageIndices)[i] == SENTINEL_VALUE || priority < (*priorities)[i]) {
            insertAt = i;
            break;
        }
    }

    if (insertAt == STREAMING_SELECTION_MAX) {
        return;
    }

    for (var i = STREAMING_SELECTION_MAX - 1u; i > insertAt; i--) {
        (*priorities)[i] = (*priorities)[i - 1u];
        (*pageIndices)[i] = (*pageIndices)[i - 1u];
    }
    (*priorities)[insertAt] = priority;
    (*pageIndices)[insertAt] = pageIndex;
}

// ============================================================================
// AABB / frustum / occlusion helpers
// ============================================================================

fn transformAABB(aabbMin: vec3<f32>, aabbMax: vec3<f32>, m: mat4x4<f32>) -> AABB {
    var corners = array<vec3<f32>, 8>(
        vec3<f32>(aabbMin.x, aabbMin.y, aabbMin.z), vec3<f32>(aabbMax.x, aabbMin.y, aabbMin.z),
        vec3<f32>(aabbMin.x, aabbMax.y, aabbMin.z), vec3<f32>(aabbMax.x, aabbMax.y, aabbMin.z),
        vec3<f32>(aabbMin.x, aabbMin.y, aabbMax.z), vec3<f32>(aabbMax.x, aabbMin.y, aabbMax.z),
        vec3<f32>(aabbMin.x, aabbMax.y, aabbMax.z), vec3<f32>(aabbMax.x, aabbMax.y, aabbMax.z),
    );
    var wMin = (m * vec4<f32>(corners[0], 1.0)).xyz;
    var wMax = wMin;
    for (var i = 1u; i < 8u; i++) {
        let wc = (m * vec4<f32>(corners[i], 1.0)).xyz;
        wMin = min(wMin, wc); wMax = max(wMax, wc);
    }
    var r: AABB; r.min = wMin; r.max = wMax; return r;
}

fn unionAABBs(aMin: vec3<f32>, aMax: vec3<f32>, bMin: vec3<f32>, bMax: vec3<f32>) -> AABB {
    var r: AABB;
    r.min = min(aMin, bMin);
    r.max = max(aMax, bMax);
    return r;
}

fn sphereBounds(center: vec3<f32>, radius: f32) -> AABB {
    var r: AABB;
    let extent = vec3<f32>(radius);
    r.min = center - extent;
    r.max = center + extent;
    return r;
}

fn nodeCullAABB(node: VirtualMeshHierarchy, isSkinned: bool) -> AABB {
    let geomMin = vec3<f32>(node.min_x, node.min_y, node.min_z);
    let geomMax = vec3<f32>(node.max_x, node.max_y, node.max_z);
    if (!isSkinned) {
        var result: AABB;
        result.min = geomMin;
        result.max = geomMax;
        return result;
    }

    // The hierarchy stores a parent-LOD sphere used for error evaluation.
    // Include its local-space AABB in node culling so occlusion does not rely
    // on the tighter geometric box alone.
    let parentCenter = vec3<f32>(node.max_center_x, node.max_center_y, node.max_center_z);
    let parentExtent = vec3<f32>(node.max_radius);
    let parentMin = parentCenter - parentExtent;
    let parentMax = parentCenter + parentExtent;

    return unionAABBs(geomMin, geomMax, parentMin, parentMax);
}

fn clusterCullAABB(
    clusterData: DecodedClusterCullData,
    isSkinned: bool
) -> AABB {
    var result = meshletLocalAABB(clusterData);
    if (!isSkinned) {
        return result;
    }

    let selfCenter = vec3<f32>(
        clusterData.selfCenterX,
        clusterData.selfCenterY,
        clusterData.selfCenterZ,
    );
    let selfRadius = clusterData.selfRadius;
    let selfBounds = sphereBounds(selfCenter, selfRadius);
    result = unionAABBs(result.min, result.max, selfBounds.min, selfBounds.max);

    let parentCenter = vec3<f32>(
        clusterData.parentCenterX,
        clusterData.parentCenterY,
        clusterData.parentCenterZ,
    );
    let parentRadius = clusterData.parentRadius;
    let parentBounds = sphereBounds(parentCenter, parentRadius);
    result = unionAABBs(result.min, result.max, parentBounds.min, parentBounds.max);

    return result;
}

fn conservativeSkinnedWorldAABB(
    localAABB: AABB,
    instanceModelMatrix: mat4x4<f32>,
    meshPartModelMatrix: mat4x4<f32>,
    meshPartIndex: u32
) -> AABB {
    var result = transformAABB(localAABB.min, localAABB.max, meshPartModelMatrix);
    if (meshPartIndex != SENTINEL_VALUE) {
        let restPoseWorldAABB = transformAABB(localAABB.min, localAABB.max, instanceModelMatrix);
        result = unionAABBs(result.min, result.max, restPoseWorldAABB.min, restPoseWorldAABB.max);
    }
    return result;
}

fn testAABBAgainstPlane(aabbMin: vec3<f32>, aabbMax: vec3<f32>, plane: vec4<f32>) -> bool {
    var pv = aabbMin;
    if (plane.x >= 0.0) { pv.x = aabbMax.x; }
    if (plane.y >= 0.0) { pv.y = aabbMax.y; }
    if (plane.z >= 0.0) { pv.z = aabbMax.z; }
    return dot(plane.xyz, pv) + plane.w >= 0.0;
}

fn normalizePlane(plane: vec4<f32>) -> vec4<f32> {
    let len = max(length(plane.xyz), 1e-8);
    return plane / len;
}

fn frustumCullAABB(aabbMin: vec3<f32>, aabbMax: vec3<f32>, view: mat4x4<f32>, proj: mat4x4<f32>) -> bool {
    var corners = array<vec3<f32>, 8>(
        vec3<f32>(aabbMin.x, aabbMin.y, aabbMin.z), vec3<f32>(aabbMax.x, aabbMin.y, aabbMin.z),
        vec3<f32>(aabbMin.x, aabbMax.y, aabbMin.z), vec3<f32>(aabbMax.x, aabbMax.y, aabbMin.z),
        vec3<f32>(aabbMin.x, aabbMin.y, aabbMax.z), vec3<f32>(aabbMax.x, aabbMin.y, aabbMax.z),
        vec3<f32>(aabbMin.x, aabbMax.y, aabbMax.z), vec3<f32>(aabbMax.x, aabbMax.y, aabbMax.z),
    );

    let invProjX = 1.0 / max(abs(proj[0][0]), 1e-8);
    let invProjY = 1.0 / max(abs(proj[1][1]), 1e-8);

    var allOutsideLeft   = true;
    var allOutsideRight  = true;
    var allOutsideBottom = true;
    var allOutsideTop    = true;
    var allOutsideNear   = true;

    for (var i = 0u; i < 8u; i++) {
        let v = view * vec4<f32>(corners[i], 1.0);
        let leftPlane   = v.z * invProjX;
        let rightPlane  = -v.z * invProjX;
        let bottomPlane = v.z * invProjY;
        let topPlane    = -v.z * invProjY;

        if (v.x >= leftPlane)   { allOutsideLeft = false; }
        if (v.x <= rightPlane)  { allOutsideRight = false; }
        if (v.y >= bottomPlane) { allOutsideBottom = false; }
        if (v.y <= topPlane)    { allOutsideTop = false; }
        if (v.z <= -uniforms.nearPlane) { allOutsideNear = false; }
    }

    return !(allOutsideLeft || allOutsideRight || allOutsideBottom || allOutsideTop || allOutsideNear);
}

fn isFrustumCullingEnabled() -> bool {
    return (uniforms.cullingFlags & CULLING_FLAG_FRUSTUM) != 0u;
}

fn isOcclusionCullingEnabled() -> bool {
    return (uniforms.cullingFlags & CULLING_FLAG_OCCLUSION) != 0u;
}

fn isStreamingPriorityUpdateEnabled() -> bool {
    return (uniforms.cullingFlags & CULLING_FLAG_STREAMING_PRIOS) != 0u;
}

struct ScreenProjectionResult {
    bounds: vec4<f32>,
    rawBounds: vec4<f32>,
    intersectsNearPlane: u32,
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

fn clampAbsMin(x: f32, minAbsValue: f32) -> f32 {
    if (x >= 0.0) {
        return max(x, minAbsValue);
    }
    return min(x, -minAbsValue);
}

fn getConservativeInstanceScale(m: mat4x4<f32>) -> f32 {
    let sx = length(vec3<f32>(m[0].x, m[0].y, m[0].z));
    let sy = length(vec3<f32>(m[1].x, m[1].y, m[1].z));
    let sz = length(vec3<f32>(m[2].x, m[2].y, m[2].z));
    return max(max(sx, sy), max(sz, 1e-6));
}

fn projectedFootprintTooSmall(bounds: vec4<f32>, viewportPx: vec2<f32>) -> bool {
    #ifdef PROJECTED_SIZE_CULLING
        let sizePx = max((bounds.zw - bounds.xy) * viewportPx, vec2<f32>(0.0));
        return sizePx.x < f32(CULL_PROJECT_MIN_WIDTH) && sizePx.y < f32(CULL_PROJECT_MIN_HEIGHT);
    #else
        return false;
    #endif
}

fn projectViewSpaceSphereToScreen(centerView: vec3<f32>, radius: f32) -> ScreenProjectionResult {
    let center = vec3<f32>(centerView.x, centerView.y, -centerView.z);
    if (center.z < radius + uniforms.nearPlane) {
        return ScreenProjectionResult(
            vec4<f32>(0.0, 0.0, 1.0, 1.0),
            vec4<f32>(0.0, 0.0, 1.0, 1.0),
            1u
        );
    }

    let cr = center * radius;
    let czr2 = max(center.z * center.z - radius * radius, 0.0);

    let vx = sqrt(center.x * center.x + czr2);
    let minx = (vx * center.x - cr.z) / clampAbsMin(vx * center.z + cr.x, 1e-6);
    let maxx = (vx * center.x + cr.z) / clampAbsMin(vx * center.z - cr.x, 1e-6);

    let vy = sqrt(center.y * center.y + czr2);
    let miny = (vy * center.y - cr.z) / clampAbsMin(vy * center.z + cr.y, 1e-6);
    let maxy = (vy * center.y + cr.z) / clampAbsMin(vy * center.z - cr.y, 1e-6);

    let p00 = abs(uniforms.proj[0][0]);
    let p11 = abs(uniforms.proj[1][1]);
    var bounds = vec4<f32>(minx * p00, miny * p11, maxx * p00, maxy * p11);

    #ifdef USE_REVERSE_Z
        bounds = bounds.xwzy * vec4<f32>(0.5, -0.5, 0.5, -0.5) + vec4<f32>(0.5);
    #else
        bounds = bounds * 0.5 + vec4<f32>(0.5);
    #endif

    return ScreenProjectionResult(
        clamp(bounds, vec4<f32>(0.0), vec4<f32>(1.0)),
        bounds,
        0u
    );
}

fn projectAABBToScreen(aabbMin: vec3<f32>, aabbMax: vec3<f32>) -> ScreenProjectionResult {
    let sphere = aabbBoundingSphere(aabbMin, aabbMax);
    let centerView = (uniforms.view * vec4<f32>(sphere.center, 1.0)).xyz;
    return projectViewSpaceSphereToScreen(centerView, sphere.radius);
}

fn getHiZLevelFromScreenBounds(screenBounds: vec4<f32>, maxLevel: u32) -> u32 {
    // The depth pyramid pass leaves mip 0 unused when the HZB is built from a
    // same-resolution depth buffer, so mip 1 becomes the first valid reduced
    // level. Match mip selection to the first valid level instead of treating
    // the unused full-resolution mip as the base of the hierarchy.
    //
    // For the single-sample occlusion test we need to emulate the safety margin
    // that a 2x2 reduction sample would provide. Picking one mip coarser keeps
    // the projected sphere inside a conservatively reduced texel footprint more
    // often, which avoids false occlusion when only a small sliver is visible.
    let minLevel = select(0u, 1u, uniforms.hiZLevels > 1u);
    let hiZBaseSize = vec2<f32>(textureDimensions(hiZTexture, i32(minLevel)));
    let pxSize = max((screenBounds.zw - screenBounds.xy) * hiZBaseSize, vec2<f32>(0.0));
    let maxExtent = max(pxSize.x, pxSize.y);
    let localLevel = u32(max(ceil(log2(max(maxExtent, 1.0))), 0.0));
    let conservativeLevel = min(minLevel + localLevel + 1u, maxLevel);
#if OCCLUSION_HIZ_MIP_BIAS > 0
    let bias = min(conservativeLevel, u32(OCCLUSION_HIZ_MIP_BIAS));
    return conservativeLevel - bias;
#else
    return conservativeLevel;
#endif
}

fn getHiZTexelAtUv(screenUv: vec2<f32>, level: u32) -> vec2<i32> {
    let mipDims = textureDimensions(hiZTexture, i32(level));
    let mipSize = max(vec2<f32>(mipDims), vec2<f32>(1.0));
    let texel = clamp(
        vec2<i32>(screenUv * mipSize),
        vec2<i32>(0),
        vec2<i32>(mipDims) - vec2<i32>(1)
    );
    return texel;
}

fn viewDepthToNDC_Normal(viewZ: f32, near: f32, far: f32) -> f32 {
    return (far / (far - near)) + (far * near) / ((far - near) * viewZ);
}

fn viewDepthToNDC_Reverse(viewZ: f32, near: f32, far: f32) -> f32 {
    return near / (-viewZ);
}

fn sampleHiZDepthAtUv(screenUv: vec2<f32>, level: u32) -> f32 {
    let texel = getHiZTexelAtUv(screenUv, level);
    return textureLoad(hiZTexture, texel, i32(level)).r;
}

fn initConservativeHiZDepth() -> f32 {
    #ifdef USE_REVERSE_Z
        return 1.0;
    #else
        return 0.0;
    #endif
}

fn mergeConservativeHiZDepth(current: f32, sampleDepth: f32) -> f32 {
    #ifdef USE_REVERSE_Z
        return min(current, sampleDepth);
    #else
        return max(current, sampleDepth);
    #endif
}

fn sampleConservativeHiZDepth2x2(screenUv: vec2<f32>, level: u32) -> f32 {
    let mipDims = textureDimensions(hiZTexture, i32(level));
    let mipSize = max(vec2<f32>(mipDims), vec2<f32>(1.0));
    let bilinearBase = clamp(
        vec2<i32>(floor(screenUv * mipSize - vec2<f32>(0.5))),
        vec2<i32>(0),
        vec2<i32>(mipDims) - vec2<i32>(1)
    );
    let maxTexel = vec2<i32>(mipDims) - vec2<i32>(1);

    var hzbDepth = initConservativeHiZDepth();
    for (var y = 0; y < 2; y++) {
        for (var x = 0; x < 2; x++) {
            let texel = min(bilinearBase + vec2<i32>(x, y), maxTexel);
            let sampleDepth = textureLoad(hiZTexture, texel, i32(level)).r;
            hzbDepth = mergeConservativeHiZDepth(hzbDepth, sampleDepth);
        }
    }

    return hzbDepth;
}

fn isVisibleAgainstHiZ(objectDepth: f32, hzbDepth: f32) -> bool {
    #ifdef USE_REVERSE_Z
        return objectDepth > hzbDepth;
    #else
        return objectDepth < hzbDepth;
    #endif
}

fn occlusionTestViewSpaceSphere(centerView: vec3<f32>, radius: f32) -> bool {
    let projection = projectViewSpaceSphereToScreen(centerView, radius);
    if (projection.intersectsNearPlane != 0u) {
        return true;
    }

    if (projection.rawBounds.x < 0.0 || projection.rawBounds.y < 0.0 ||
        projection.rawBounds.z > 1.0 || projection.rawBounds.w > 1.0) {
        return true;
    }

    let width = (projection.bounds.z - projection.bounds.x) * f32(textureDimensions(hiZTexture, i32(select(0u, 1u, uniforms.hiZLevels > 1u))).x);
    let height = (projection.bounds.w - projection.bounds.y) * f32(textureDimensions(hiZTexture, i32(select(0u, 1u, uniforms.hiZLevels > 1u))).y);
    if (width <= 0.0 || height <= 0.0) {
        return true;
    }

    let level = getHiZLevelFromScreenBounds(projection.bounds, uniforms.hiZLevels - 1u);
    let sampleUv = clamp((projection.bounds.xy + projection.bounds.zw) * 0.5, vec2<f32>(0.0), vec2<f32>(1.0));
    let hzbDepth = sampleConservativeHiZDepth2x2(sampleUv, level);
    let viewZ = min(centerView.z + radius, -uniforms.nearPlane);

    #ifdef USE_REVERSE_Z
        let objectDepth = viewDepthToNDC_Reverse(viewZ, uniforms.nearPlane, uniforms.farPlane);
        return isVisibleAgainstHiZ(objectDepth, hzbDepth);
    #else
        let objectDepth = viewDepthToNDC_Normal(viewZ, uniforms.nearPlane, uniforms.farPlane);
        return isVisibleAgainstHiZ(objectDepth, hzbDepth);
    #endif
}

fn occlusionTestLocalSphere(centerLocal: vec3<f32>, radiusLocal: f32, modelMatrix: mat4x4<f32>) -> bool {
    let worldCenter = (modelMatrix * vec4<f32>(centerLocal, 1.0)).xyz;
    let worldRadius = radiusLocal * getConservativeInstanceScale(modelMatrix);
    let centerView = (uniforms.view * vec4<f32>(worldCenter, 1.0)).xyz;
    return occlusionTestViewSpaceSphere(centerView, worldRadius);
}

fn visibleInAnyFrustum(worldMin: vec3<f32>, worldMax: vec3<f32>) -> bool {
    #ifdef FRUSTUM_CULLING
        if (!isFrustumCullingEnabled()) {
            return true;
        }
        return frustumCullAABB(worldMin, worldMax, uniforms.view, uniforms.proj);
    #else
        return true;
    #endif
}

fn visibleInAnyOcclusion(
    localCenter : vec3<f32>,
    localRadius : f32,
    modelMatrix : mat4x4<f32>
) -> bool {
    #ifdef OCCLUSION_CULLING
        if (!isOcclusionCullingEnabled()) {
            return true;
        }
        return occlusionTestLocalSphere(localCenter, localRadius, modelMatrix);
    #else
        return true;
    #endif
}

fn isClusterConeBackfacingPerspective(
    centerLocal : vec3<f32>,
    radiusLocal : f32,
    cone        : ClusterCone,
    modelMatrix : mat4x4<f32>,
    viewPos     : vec3<f32>
) -> bool {
    if (cone.cutoff >= 1.0) {
        return false;
    }

    let worldAxisRaw = (modelMatrix * vec4<f32>(cone.axis, 0.0)).xyz;
    let axisLenSq = dot(worldAxisRaw, worldAxisRaw);
    if (axisLenSq <= 1e-8) {
        return false;
    }

    let worldAxis = worldAxisRaw * inverseSqrt(axisLenSq);
    let worldCenter = (modelMatrix * vec4<f32>(centerLocal, 1.0)).xyz;
    let centerToEye = worldCenter - viewPos;
    let centerDist = length(centerToEye);
    let worldRadius = radiusLocal * getInstanceScale(modelMatrix);
    if (centerDist <= worldRadius) {
        return false;
    }

    return dot(centerToEye, worldAxis) >= cone.cutoff * centerDist + worldRadius;
}

// ============================================================================
// LOD selection
// ============================================================================

fn getInstanceScale(m: mat4x4<f32>) -> f32 {
    // Use the largest basis length so non-uniform scaling does not
    // underestimate projected error or cone bounds and cull visible clusters.
    return getConservativeInstanceScale(m);
}

fn calculateLodDistance(
    centerLocal : vec3<f32>,
    radiusLocal : f32,
    modelMatrix : mat4x4<f32>,
    viewPos     : vec3<f32>,
    nearClamp   : f32
) -> f32 {
    let ws = getInstanceScale(modelMatrix);
    let wc = (modelMatrix * vec4<f32>(centerLocal, 1.0)).xyz;
    return max(distance(wc, viewPos) - radiusLocal * ws, nearClamp);
}

fn calculateProjectedError(
    centerLocal : vec3<f32>,
    radiusLocal : f32,
    errorLocal  : f32,
    modelMatrix : mat4x4<f32>,
    viewPos     : vec3<f32>
) -> f32 {
    let ws = getInstanceScale(modelMatrix);
    let wc = (modelMatrix * vec4<f32>(centerLocal, 1.0)).xyz;
    let d  = calculateLodDistance(centerLocal, radiusLocal, modelMatrix, viewPos, uniforms.nearPlane);
    return (errorLocal * ws / d) * uniforms.proj[1][1] * 0.5 * f32(uniforms.viewport.y);
}

// Returns true if we should descend into this node's children.
// Tests only the parent (coarser) error: if the simplified representation is
// not accurate enough for the current view, we must visit finer children.
fn shouldVisitChildNodes(
    node        : VirtualMeshHierarchy,
    modelMatrix : mat4x4<f32>,
    viewPos     : vec3<f32>,
    errorThresh : f32
) -> bool {
    let parentCenter = vec3<f32>(node.max_center_x, node.max_center_y, node.max_center_z);
    let projParentError = calculateProjectedError(
        parentCenter, node.max_radius, node.max_parent_lod_error, modelMatrix, viewPos
    );
    return projParentError >= errorThresh;
}

// Returns true if a cluster's own geometry is accurate enough to render.
// Tests only the self (finer) error: if it is below the threshold, this
// cluster is sufficiently detailed for the current view distance.
// selfError is passed explicitly so callers can override it (e.g. 0.0 for
// streaming leaf nodes that must always render regardless of view distance).
fn isClusterCoarseEnough(
    clusterData    : DecodedClusterCullData,
    modelMatrix    : mat4x4<f32>,
    viewPos        : vec3<f32>,
    errorThresh    : f32,
    selfError      : f32
) -> bool {
    let selfCenter = vec3<f32>(
        clusterData.selfCenterX,
        clusterData.selfCenterY,
        clusterData.selfCenterZ,
    );
    let selfRadius = clusterData.selfRadius;
    let projSelfError = calculateProjectedError(
        selfCenter, selfRadius, selfError, modelMatrix, viewPos
    );
    return projSelfError < errorThresh;
}

fn computePixelsPerTriangle(screenPx: vec2<f32>, tris: u32, siblings: u32) -> f32 {
    return (screenPx.x * screenPx.y) / (f32(max(tris, 1u)) * f32(max(siblings, 1u)));
}

fn invalidQueueElement() -> QueueElement {
    return QueueElement(SENTINEL_VALUE, SENTINEL_VALUE, SENTINEL_VALUE, SENTINEL_VALUE);
}

fn spillHierarchyElementToGlobalQueue(element: QueueElement) {
    let globalIndex = atomicAdd(&counters[HIERARCHY_QUEUE_SIZE_INDEX], 1u);
    hierarchyQueueWrite[globalIndex] = element;
}

fn pushHierarchyElementToLocalOrGlobalQueue(element: QueueElement) {
    let localIndex = atomicAdd(&hierarchyLocalQueueSize, 1u);
    if (localIndex < HIERARCHY_LOCAL_QUEUE_CAPACITY) {
        hierarchyLocalQueueIndex[localIndex] = element.index;
        hierarchyLocalQueueInstance[localIndex] = element.instanceIndex;
        hierarchyLocalQueuePage[localIndex] = element.pageIndex;
        hierarchyLocalQueuePadding[localIndex] = element._padding;
        return;
    }

    spillHierarchyElementToGlobalQueue(element);
}

// ============================================================================
// PASS 1: INIT
// ============================================================================

@compute @workgroup_size(1)
fn initSync() {
    for (var i = 0u; i < 16u; i++) {
        atomicStore(&counters[i], 0u);
    }
    if (isStreamingPriorityUpdateEnabled()) {
        for (var i = 0u; i < uniforms.registeredPages; i++) {
            let entry = pageTable[i];
            if (entry.prioritySlot < uniforms.registeredPages) {
                atomicStore(&pagePriorities[entry.prioritySlot], 0u);
            }
        }
    }
    #ifdef DEBUG_BINDINGS
    for (var i = 0u; i < arrayLength(&hierarchyDebugBuffer); i++) {
        hierarchyDebugBuffer[i].reason            = HIER_DEBUG_NOT_VISITED;
        hierarchyDebugBuffer[i].parentErrorPxBits = bitcast<u32>(0.0);
        hierarchyDebugBuffer[i].thresholdPxBits   = bitcast<u32>(0.0);
        hierarchyDebugBuffer[i].instanceIndex     = SENTINEL_VALUE;
    }
    for (var i = 0u; i < arrayLength(&clusterDebugBuffer); i++) {
        clusterDebugBuffer[i].reason            = CLUSTER_DEBUG_NOT_VISITED;
        clusterDebugBuffer[i].selfErrorPxBits   = bitcast<u32>(0.0);
        clusterDebugBuffer[i].parentErrorPxBits = bitcast<u32>(0.0);
        clusterDebugBuffer[i].nodeIndex         = SENTINEL_VALUE;
    }
    #endif
}

// ============================================================================
// PASS 2: SETUP ROOT NODES
// ============================================================================

@compute @workgroup_size(WORKGROUP_SIZE)
fn setupRootNodes(@builtin(global_invocation_id) globalId: vec3<u32>) {
    if (globalId.x >= uniforms.instances_count) { return; }
    let idx = atomicAdd(&counters[HIERARCHY_QUEUE_SIZE_INDEX], 1u);
    hierarchyQueueWrite[idx].index         = instances[globalId.x].hierarchyStartOffset;
    hierarchyQueueWrite[idx].instanceIndex = globalId.x;
    hierarchyQueueWrite[idx].pageIndex     = SENTINEL_VALUE;
    hierarchyQueueWrite[idx]._padding      = 0u;
}

// ============================================================================
// PASS 3: PREPARE INDIRECT DISPATCH
// ============================================================================

@compute @workgroup_size(1)
fn prepareIndirectDispatch() {
    let sz = atomicLoad(&counters[HIERARCHY_QUEUE_SIZE_INDEX]);
    atomicStore(&counters[READ_QUEUE_SIZE_INDEX], sz);
    indirectArgs.x = (sz + HIERARCHY_GLOBAL_PULL_PER_WORKGROUP - 1u) / HIERARCHY_GLOBAL_PULL_PER_WORKGROUP;
    indirectArgs.y = 1u;
    indirectArgs.z = 1u;
    atomicStore(&counters[HIERARCHY_QUEUE_SIZE_INDEX], 0u);
}

// ============================================================================
// PASS 4: PROCESS HIERARCHY NODES
// ============================================================================

fn processHierarchyNodeElement(element: QueueElement) {
    let nodeIndex   = element.index;
    let instanceIdx = element.instanceIndex;
    if (nodeIndex == SENTINEL_VALUE || instanceIdx == SENTINEL_VALUE) { return; }

    let instance = instances[instanceIdx];
    let node     = hierarchy[nodeIndex];
    let hierarchyLevel = element._padding;
    let forceTraversal = (node.flags & HIERARCHY_FORCE_TRAVERSAL_FLAG) != 0u;
    let nodeModelMatrix = resolveMeshPartModelMatrix(instance, node.meshPartIndex);

    let parentCenter = vec3<f32>(node.max_center_x, node.max_center_y, node.max_center_z);
    var projParentError = uniforms.error;
    var errorThreshold = uniforms.error;
    if (!forceTraversal) {
        projParentError = calculateProjectedError(
            parentCenter, node.max_radius, node.max_parent_lod_error, nodeModelMatrix, uniforms.viewPosition.xyz
        );
    }
    if (!forceTraversal && projParentError < errorThreshold) {
        #ifdef DEBUG_BINDINGS
        writeHierarchyDebug(nodeIndex, HIER_DEBUG_CULLED_PARENT_ERROR_SMALL, projParentError, errorThreshold, instanceIdx);
        #endif
        return;
    }

    let rawPageIndex = node.pageIndex;

    if (!forceTraversal) {
        let localCullAABB = nodeCullAABB(node, node.meshPartIndex != SENTINEL_VALUE);
        let worldAABB = conservativeSkinnedWorldAABB(localCullAABB, instance.modelMatrix, nodeModelMatrix, node.meshPartIndex);
        let localCullSphere = aabbBoundingSphere(localCullAABB.min, localCullAABB.max);

        #ifdef FRUSTUM_CULLING
        if (!visibleInAnyFrustum(worldAABB.min, worldAABB.max)) {
            #ifdef DEBUG_BINDINGS
            writeHierarchyDebug(nodeIndex, HIER_DEBUG_CULLED_FRUSTUM, projParentError, errorThreshold, instanceIdx);
            #endif
            return;
        }
        #endif

        let screenProjection = projectAABBToScreen(worldAABB.min, worldAABB.max);
        if (screenProjection.intersectsNearPlane == 0u &&
            projectedFootprintTooSmall(
                screenProjection.rawBounds,
                vec2<f32>(f32(uniforms.viewport.x), f32(uniforms.viewport.y))
            )) {
            #ifdef DEBUG_BINDINGS
            writeHierarchyDebug(nodeIndex, HIER_DEBUG_CULLED_PROJECTED_TOO_SMALL, projParentError, errorThreshold, instanceIdx);
            #endif
            return;
        }

        #ifdef OCCLUSION_CULLING
        var cameraVisible = true;
        cameraVisible = visibleInAnyOcclusion(
            localCullSphere.center,
            localCullSphere.radius,
            nodeModelMatrix
        );
        if (!cameraVisible) {
            #ifdef DEBUG_BINDINGS
            writeHierarchyDebug(nodeIndex, HIER_DEBUG_CULLED_OCCLUSION, projParentError, errorThreshold, instanceIdx);
            #endif
            return;
        }
        #endif
    }

    if (isStreamingPriorityUpdateEnabled() && rawPageIndex != SENTINEL_VALUE) {
        let globalPageIdx = resolveGlobalPageIndex(rawPageIndex, instance.pageTableOffset);
        if (globalPageIdx < uniforms.registeredPages) {
            let entry = pageTable[globalPageIdx];
            if (entry.prioritySlot < uniforms.registeredPages) {
                atomicAdd(&pagePriorities[entry.prioritySlot], computePriority(node, instance, nodeModelMatrix, projParentError));
            }
        }
    }

    let isLeaf = (node.flags & 1u) != 0u;

    if (isLeaf) {
        if (!areNodeClustersInstalled(node)) {
            #ifdef DEBUG_BINDINGS
            writeHierarchyDebug(nodeIndex, HIER_DEBUG_CULLED_NOT_INSTALLED, projParentError, uniforms.error, instanceIdx);
            #endif
            return;
        }
        if (node.child_start == SENTINEL_VALUE) {
            #ifdef DEBUG_BINDINGS
            writeHierarchyDebug(nodeIndex, HIER_DEBUG_CULLED_INVALID_CHILD_START, projParentError, uniforms.error, instanceIdx);
            #endif
            return;
        }

        let globalPageIdx = resolveGlobalPageIndex(rawPageIndex, instance.pageTableOffset);
        let baseIndex     = atomicAdd(&counters[CLUSTER_QUEUE_SIZE_INDEX], node.child_count);
        for (var i = 0u; i < node.child_count; i++) {
            clusterQueue[baseIndex + i].index         = node.child_start + i;
            clusterQueue[baseIndex + i].instanceIndex = instanceIdx;
            clusterQueue[baseIndex + i].pageIndex     = globalPageIdx;
            clusterQueue[baseIndex + i]._padding      = nodeIndex;
        }
        #ifdef DEBUG_BINDINGS
        writeHierarchyDebug(nodeIndex, HIER_DEBUG_ENQUEUED_CLUSTERS, projParentError, uniforms.error, instanceIdx);
        #endif
        return;
    }

    let hierarchyRoot = instance.hierarchyStartOffset;
    for (var i = 0u; i < node.child_count; i++) {
        pushHierarchyElementToLocalOrGlobalQueue(QueueElement(
            hierarchyRoot + node.child_start + i,
            instanceIdx,
            SENTINEL_VALUE,
            hierarchyLevel + 1u
        ));
    }
    #ifdef DEBUG_BINDINGS
    writeHierarchyDebug(nodeIndex, HIER_DEBUG_ENQUEUED_CHILDREN, projParentError, uniforms.error, instanceIdx);
    #endif
}

@compute @workgroup_size(WORKGROUP_SIZE)
fn processHierarchyNodes(
    @builtin(local_invocation_id) localId: vec3<u32>,
    @builtin(workgroup_id) workgroupId: vec3<u32>
) {
    let readQueueSize = atomicLoad(&counters[READ_QUEUE_SIZE_INDEX]);
    let globalReadBase = workgroupId.x * HIERARCHY_GLOBAL_PULL_PER_WORKGROUP;
    if (globalReadBase >= readQueueSize) { return; }

    if (localId.x == 0u) {
        atomicStore(&hierarchyLocalQueueSize, 0u);
        hierarchyIterationItemCount = 0u;
        hierarchyFlushCount = 0u;
    }

    workgroupBarrier();

    if (localId.x < HIERARCHY_GLOBAL_PULL_PER_WORKGROUP) {
        let globalReadIndex = globalReadBase + localId.x;
        if (globalReadIndex < readQueueSize) {
            let localWriteIndex = atomicAdd(&hierarchyLocalQueueSize, 1u);
            let element = hierarchyQueueRead[globalReadIndex];
            hierarchyLocalQueueIndex[localWriteIndex] = element.index;
            hierarchyLocalQueueInstance[localWriteIndex] = element.instanceIndex;
            hierarchyLocalQueuePage[localWriteIndex] = element.pageIndex;
            hierarchyLocalQueuePadding[localWriteIndex] = element._padding;
        }
    }

    for (var iter = 0u; iter < MAX_LOCAL_QUEUE_PROCESS_ITERS; iter++) {
        workgroupBarrier();

        if (localId.x == 0u) {
            hierarchyIterationItemCount = min(atomicLoad(&hierarchyLocalQueueSize), HIERARCHY_LOCAL_QUEUE_CAPACITY);
        }

        workgroupBarrier();

        var element = invalidQueueElement();
        if (localId.x < hierarchyIterationItemCount) {
            let localQueueIndex = localId.x;
            element = QueueElement(
                hierarchyLocalQueueIndex[localQueueIndex],
                hierarchyLocalQueueInstance[localQueueIndex],
                hierarchyLocalQueuePage[localQueueIndex],
                hierarchyLocalQueuePadding[localQueueIndex]
            );
        }

        workgroupBarrier();

        if (localId.x == 0u) {
            atomicStore(&hierarchyLocalQueueSize, 0u);
        }

        workgroupBarrier();

        processHierarchyNodeElement(element);
    }

    workgroupBarrier();

    if (localId.x == 0u) {
        hierarchyFlushCount = min(atomicLoad(&hierarchyLocalQueueSize), HIERARCHY_LOCAL_QUEUE_CAPACITY);
    }

    workgroupBarrier();

    for (var flushOffset = localId.x; flushOffset < hierarchyFlushCount; flushOffset += WORKGROUP_SIZE) {
        let localQueueIndex = flushOffset;
        spillHierarchyElementToGlobalQueue(QueueElement(
            hierarchyLocalQueueIndex[localQueueIndex],
            hierarchyLocalQueueInstance[localQueueIndex],
            hierarchyLocalQueuePage[localQueueIndex],
            hierarchyLocalQueuePadding[localQueueIndex]
        ));
    }
}

// ============================================================================
// PASS 5: PREPARE CLUSTER DISPATCH
// ============================================================================

@compute @workgroup_size(1)
fn prepareClusterDispatch() {
    let qs = atomicLoad(&counters[CLUSTER_QUEUE_SIZE_INDEX]);
    indirectArgs.x = (qs + WORKGROUP_SIZE - 1u) / WORKGROUP_SIZE;
    indirectArgs.y = 1u;
    indirectArgs.z = 1u;
}

// ============================================================================
// PASS 6: PROCESS CLUSTERS
// ============================================================================

fn processClusterElement(element: QueueElement) {
    if (element.index         == SENTINEL_VALUE ||
        element.instanceIndex == SENTINEL_VALUE ||
        element.pageIndex     == SENTINEL_VALUE) {
        #ifdef DEBUG_BINDINGS
        if (element.pageIndex != SENTINEL_VALUE && element.index != SENTINEL_VALUE) {
            writeClusterDebug(element.pageIndex, element.index, CLUSTER_DEBUG_INVALID_QUEUE_ELEMENT, 0.0, 0.0, element._padding);
        }
        #endif
        return;
    }

    let globalPageIdx   = element.pageIndex;
    let meshletLocalIdx = element.index;
    let instanceIdx     = element.instanceIndex;
    let nodeIndex       = element._padding;
    let siblingCount    = hierarchy[nodeIndex].child_count;

    if (globalPageIdx >= arrayLength(&pageTable)) {
        #ifdef DEBUG_BINDINGS
        writeClusterDebug(globalPageIdx, meshletLocalIdx, CLUSTER_DEBUG_INVALID_PAGE_INDEX, 0.0, 0.0, nodeIndex);
        #endif
        return;
    }
    let pageEntry = pageTable[globalPageIdx];
    if (pageEntry.isInstalled == 0u) {
        #ifdef DEBUG_BINDINGS
        writeClusterDebug(globalPageIdx, meshletLocalIdx, CLUSTER_DEBUG_PAGE_NOT_INSTALLED, 0.0, 0.0, nodeIndex);
        #endif
        return;
    }

    let triangleCount = readTriangleCount(pageEntry, meshletLocalIdx);
    if (triangleCount == 0u) {
        #ifdef DEBUG_BINDINGS
        writeClusterDebug(globalPageIdx, meshletLocalIdx, CLUSTER_DEBUG_ZERO_TRIANGLES, 0.0, 0.0, nodeIndex);
        #endif
        return;
    }

    let instance = instances[instanceIdx];
    let clusterModelMatrix = resolveMeshPartModelMatrix(instance, hierarchy[nodeIndex].meshPartIndex);
    let clusterData = readDecodedCluster(pageEntry, meshletLocalIdx);

    // ── LOD + cut masks ───────────────────────────────────────────────────────
    // Per leaf-node flags pack two 8-bit masks:
    //   [15:8]  streamingLeafsBitset
    //   [23:16] enabledClustersBitset
    // localBit is this cluster's index inside the node/group [0..child_count).
    let nodeFlags = hierarchy[nodeIndex].flags;
    let localBit  = meshletLocalIdx - hierarchy[nodeIndex].child_start;
    if (localBit >= 8u) {
        #ifdef DEBUG_BINDINGS
        writeClusterDebug(globalPageIdx, meshletLocalIdx, CLUSTER_DEBUG_LOCAL_BIT_OUT_OF_RANGE, 0.0, 0.0, nodeIndex);
        #endif
        return;
    }

    let streamingMask = (nodeFlags >> HIERARCHY_STREAMING_MASK_SHIFT) & HIERARCHY_CHILD_MASK;
    let enabledMask   = (nodeFlags >> HIERARCHY_ENABLED_MASK_SHIFT) & HIERARCHY_CHILD_MASK;
    if (((enabledMask >> localBit) & 1u) == 0u) {
        #ifdef DEBUG_BINDINGS
        writeClusterDebug(globalPageIdx, meshletLocalIdx, CLUSTER_DEBUG_DISABLED_BY_MASK, 0.0, 0.0, nodeIndex);
        #endif
        return;
    }

    let isStreamingLeaf = ((streamingMask >> localBit) & 1u) != 0u;
    let rawSelfError    = clusterData.selfError;
    let effectiveSelfError = select(rawSelfError, 0.0, isStreamingLeaf);
    let selfCenter = vec3<f32>(
        clusterData.selfCenterX,
        clusterData.selfCenterY,
        clusterData.selfCenterZ,
    );
    let selfRadius = clusterData.selfRadius;
    let clusterCone = ClusterCone(
        vec3<f32>(
            clusterData.coneAxisX,
            clusterData.coneAxisY,
            clusterData.coneAxisZ
        ),
        clusterData.coneCutoff
    );
    let projSelfError = calculateProjectedError(
        selfCenter, selfRadius, effectiveSelfError,
        clusterModelMatrix, uniforms.viewPosition.xyz
    );
    let selfErrorThreshold = uniforms.error;
    if (projSelfError >= selfErrorThreshold) {
        #ifdef DEBUG_BINDINGS
        writeClusterDebug(globalPageIdx, meshletLocalIdx, CLUSTER_DEBUG_SELF_ERROR_TOO_HIGH, projSelfError, -1.0, nodeIndex);
        #endif
        return;
    }

    var projParentError: f32 = -1.0;
    if (!isStreamingLeaf) {
        let parentError  = clusterData.parentError;
        let parentCenter = vec3<f32>(
            clusterData.parentCenterX,
            clusterData.parentCenterY,
            clusterData.parentCenterZ,
        );
        let parentRadius = clusterData.parentRadius;
        projParentError = calculateProjectedError(
            parentCenter, parentRadius, parentError,
            clusterModelMatrix, uniforms.viewPosition.xyz
        );
        let parentErrorThreshold = uniforms.error;
        if (projParentError < parentErrorThreshold) {
            #ifdef DEBUG_BINDINGS
            writeClusterDebug(globalPageIdx, meshletLocalIdx, CLUSTER_DEBUG_PARENT_ERROR_TOO_LOW, projSelfError, projParentError, nodeIndex);
            #endif
            return;
        }
    }

    if (isClusterConeBackfacingPerspective(
        selfCenter,
        selfRadius,
        clusterCone,
        clusterModelMatrix,
        uniforms.viewPosition.xyz
    )) {
        #ifdef DEBUG_BINDINGS
        writeClusterDebug(globalPageIdx, meshletLocalIdx, CLUSTER_DEBUG_CONE_CULLED, projSelfError, projParentError, nodeIndex);
        #endif
        return;
    }

    let localAABB = clusterCullAABB(clusterData, hierarchy[nodeIndex].meshPartIndex != SENTINEL_VALUE);
    let worldAABB = conservativeSkinnedWorldAABB(localAABB, instance.modelMatrix, clusterModelMatrix, hierarchy[nodeIndex].meshPartIndex);
    let localCullSphere = aabbBoundingSphere(localAABB.min, localAABB.max);

    var cameraVisible = true;
    #ifdef FRUSTUM_CULLING
    {
        cameraVisible = visibleInAnyFrustum(worldAABB.min, worldAABB.max);
    }
    #endif
    let screenProjection = projectAABBToScreen(worldAABB.min, worldAABB.max);
    if (cameraVisible &&
        screenProjection.intersectsNearPlane == 0u &&
        projectedFootprintTooSmall(
            screenProjection.rawBounds,
            vec2<f32>(f32(uniforms.viewport.x), f32(uniforms.viewport.y))
        )) {
        #ifdef DEBUG_BINDINGS
        writeClusterDebug(globalPageIdx, meshletLocalIdx, CLUSTER_DEBUG_PROJECTED_TOO_SMALL, projSelfError, projParentError, nodeIndex);
        #endif
        return;
    }
    #ifdef OCCLUSION_CULLING
    if (cameraVisible) {
        cameraVisible = visibleInAnyOcclusion(
            localCullSphere.center,
            localCullSphere.radius,
            clusterModelMatrix
        );
    }
    #endif

    if (cameraVisible) {
        let screenBounds = screenProjection.bounds;
        let screenSizePx = (screenBounds.zw - screenBounds.xy) *
                           vec2<f32>(f32(uniforms.viewport.x), f32(uniforms.viewport.y));
        let pixelsPerTri = computePixelsPerTriangle(screenSizePx, triangleCount, siblingCount);

        let useSoftware = false;

        if (useSoftware) {
            let outIdx = atomicAdd(&counters[SW_VISIBLE_CLUSTER_COUNT_INDEX], 1u);
            swVisibleClusterInfos[outIdx].pageIndex             = globalPageIdx;
            swVisibleClusterInfos[outIdx].pageLocalClusterIndex = meshletLocalIdx;
            swVisibleClusterInfos[outIdx].instanceIndex         = instanceIdx;
            swVisibleClusterInfos[outIdx]._padding              = nodeIndex;
            let slot = outIdx * 4u;
            swDrawIndirectBuffer[slot + 0u] = triangleCount * 3u;
            swDrawIndirectBuffer[slot + 1u] = 1u;
            swDrawIndirectBuffer[slot + 2u] = 0u;
            swDrawIndirectBuffer[slot + 3u] = outIdx;
            #ifdef DEBUG_BINDINGS
            writeClusterDebug(globalPageIdx, meshletLocalIdx, CLUSTER_DEBUG_RENDERED_SW, projSelfError, projParentError, nodeIndex);
            #endif
        } else {
            let outIdx = atomicAdd(&counters[HW_VISIBLE_CLUSTER_COUNT_INDEX], 1u);
            hwVisibleClusterInfos[outIdx].pageIndex             = globalPageIdx;
            hwVisibleClusterInfos[outIdx].pageLocalClusterIndex = meshletLocalIdx;
            hwVisibleClusterInfos[outIdx].instanceIndex         = instanceIdx;
            hwVisibleClusterInfos[outIdx]._padding              = nodeIndex;
            #ifndef DRAW_INDIRECT_COUNT_DISABLED
            let slot = outIdx * 4u;
            hwDrawIndirectBuffer[slot + 0u] = triangleCount * 3u;
            hwDrawIndirectBuffer[slot + 1u] = 1u;
            hwDrawIndirectBuffer[slot + 2u] = 0u;
            hwDrawIndirectBuffer[slot + 3u] = outIdx;
            #endif
            #ifdef DEBUG_BINDINGS
            writeClusterDebug(globalPageIdx, meshletLocalIdx, CLUSTER_DEBUG_RENDERED_HW, projSelfError, projParentError, nodeIndex);
            #endif
        }
    }
}

@compute @workgroup_size(WORKGROUP_SIZE)
fn processClusters(@builtin(global_invocation_id) globalId: vec3<u32>) {
    let clusterQueueSize = atomicLoad(&counters[CLUSTER_QUEUE_SIZE_INDEX]);
    if (globalId.x >= clusterQueueSize) { return; }
    processClusterElement(clusterQueue[globalId.x]);
}

@compute @workgroup_size(STREAMING_SELECTION_WORKGROUP_SIZE)
fn selectStreamingPages(@builtin(local_invocation_id) localId: vec3<u32>) {
    let lane = localId.x;
    let outputCount = min(uniforms.streamingSelectionCount, STREAMING_SELECTION_MAX);

    for (var i = lane; i < STREAMING_SELECTION_MAX; i += STREAMING_SELECTION_WORKGROUP_SIZE) {
        streamingSelectedInstallPages[i] = SENTINEL_VALUE;
        streamingSelectedEvictPages[i] = SENTINEL_VALUE;
        if (i < outputCount) {
            clearStreamingCandidate(i);
        }
    }
    workgroupBarrier();

    for (var selectionIndex = 0u; selectionIndex < outputCount; selectionIndex++) {
        var localInstallPriority = 0u;
        var localInstallPage = SENTINEL_VALUE;
        var localEvictPriority = 0xFFFFFFFFu;
        var localEvictPage = SENTINEL_VALUE;

        for (var pageIndex = lane; pageIndex < uniforms.registeredPages; pageIndex += STREAMING_SELECTION_WORKGROUP_SIZE) {
            let entry = pageTable[pageIndex];
            if (entry.prioritySlot >= uniforms.registeredPages) {
                continue;
            }

            let priority = atomicLoad(&pagePriorities[entry.prioritySlot]);
            if (entry.isInstalled == 0u) {
                if (priority == 0u || isPreviouslySelectedInstallPage(pageIndex, selectionIndex)) {
                    continue;
                }
                if (descendingCandidateWins(priority, pageIndex, localInstallPriority, localInstallPage)) {
                    localInstallPriority = priority;
                    localInstallPage = pageIndex;
                }
            } else {
                if (isPreviouslySelectedEvictPage(pageIndex, selectionIndex)) {
                    continue;
                }
                if (ascendingCandidateWins(priority, pageIndex, localEvictPriority, localEvictPage)) {
                    localEvictPriority = priority;
                    localEvictPage = pageIndex;
                }
            }
        }

        streamingInstallReductionPriorities[lane] = localInstallPriority;
        streamingInstallReductionPages[lane] = localInstallPage;
        streamingEvictReductionPriorities[lane] = localEvictPriority;
        streamingEvictReductionPages[lane] = localEvictPage;
        workgroupBarrier();

        var stride = STREAMING_SELECTION_WORKGROUP_SIZE / 2u;
        loop {
            if (lane < stride) {
                let otherLane = lane + stride;

                if (descendingCandidateWins(
                    streamingInstallReductionPriorities[otherLane],
                    streamingInstallReductionPages[otherLane],
                    streamingInstallReductionPriorities[lane],
                    streamingInstallReductionPages[lane])) {
                    streamingInstallReductionPriorities[lane] = streamingInstallReductionPriorities[otherLane];
                    streamingInstallReductionPages[lane] = streamingInstallReductionPages[otherLane];
                }

                if (ascendingCandidateWins(
                    streamingEvictReductionPriorities[otherLane],
                    streamingEvictReductionPages[otherLane],
                    streamingEvictReductionPriorities[lane],
                    streamingEvictReductionPages[lane])) {
                    streamingEvictReductionPriorities[lane] = streamingEvictReductionPriorities[otherLane];
                    streamingEvictReductionPages[lane] = streamingEvictReductionPages[otherLane];
                }
            }

            workgroupBarrier();
            if (stride == 1u) {
                break;
            }
            stride = stride / 2u;
        }

        if (lane == 0u) {
            let installPage = streamingInstallReductionPages[0];
            let evictPage = streamingEvictReductionPages[0];

            streamingSelectedInstallPages[selectionIndex] = installPage;
            streamingSelectedEvictPages[selectionIndex] = evictPage;

            pageInstallCandidates[selectionIndex].globalPageIndex = installPage;
            pageInstallCandidates[selectionIndex].priority = select(streamingInstallReductionPriorities[0], 0u, installPage == SENTINEL_VALUE);
            pageEvictCandidates[selectionIndex].globalPageIndex = evictPage;
            pageEvictCandidates[selectionIndex].priority = select(streamingEvictReductionPriorities[0], 0u, evictPage == SENTINEL_VALUE);
        }
        workgroupBarrier();
    }
}

#ifdef DRAW_INDIRECT_COUNT_DISABLED
// ============================================================================
// PASS 6b: PREPARE HW DRAW INDIRECT ARGS (flat-vertex-stream variant)
//
// Runs as a single-thread dispatch after processClusters.
// Writes one VkDrawIndirectCommand covering all visible clusters:
//   vertexCount   = visibleClusterCount * CLUSTER_SIZE * 3
//   instanceCount = 1
//   firstVertex   = 0
//   firstInstance = 0
// The vertex shader derives the cluster slot from:
//   vertexIndex / (CLUSTER_SIZE * 3)
// ============================================================================

@compute @workgroup_size(1)
fn prepareHWDrawIndirectArgs() {
    let clusterCount = atomicLoad(&counters[HW_VISIBLE_CLUSTER_COUNT_INDEX]);
    hwDrawIndirectBuffer[0] = clusterCount * CLUSTER_SIZE * 3u;
    hwDrawIndirectBuffer[1] = 1u;
    hwDrawIndirectBuffer[2] = 0u;
    hwDrawIndirectBuffer[3] = 0u;
}
#endif
