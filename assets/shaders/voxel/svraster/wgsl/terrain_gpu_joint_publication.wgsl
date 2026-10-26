#include "sparse_voxel_brick_common.wgsl"

const TERRAIN_RASTER_STATUS_READY : u32 = 2u;
const TERRAIN_TREE64_STATUS_READY : u32 = 2u;
const TERRAIN_JOINT_STATUS_APPROVED : u32 = 1u;
const TERRAIN_JOINT_STATUS_REJECTED : u32 = 2u;
const TERRAIN_PUBLICATION_VALID : u32 = 1u;
const TERRAIN_PUBLICATION_DYNAMIC_INSTANCE : u32 = 2u;
const TERRAIN_TREE64_PAGE_VALID : u32 = 1u;
const TERRAIN_JOINT_FORCE_LEAF_RASTER : u32 = 1u;
const TERRAIN_JOINT_DEFER_VISIBILITY : u32 = 2u;
const TERRAIN_JOINT_SOURCE_WORLD_SPACE : u32 = 4u;
const TERRAIN_JOINT_FLAT_SURFACE_CONTEXT : u32 = 1u;
const TREE64_DYNAMIC_VALID : u32 = 1u;
const TREE64_DYNAMIC_SKIP_RECEIVER_CELL : u32 = 4u;
const TREE64_DYNAMIC_TRANSLATION_ONLY : u32 = 8u;

struct TerrainTree64Node {
    childBaseAndFlags : u32,
    populationMaskLo : u32,
    populationMaskHi : u32,
}

struct TerrainTree64NodePadded {
    childBaseAndFlags : u32,
    populationMaskLo : u32,
    populationMaskHi : u32,
    reserved0 : u32,
}

struct TerrainTree64DynamicInstance {
    worldToModel : mat4x4<f32>,
    worldBoundsMinimum : vec4<f32>,
    worldBoundsMaximum : vec4<f32>,
    modelIndex : u32,
    stableInstanceId : u32,
    contentGeneration : u32,
    flags : u32,
    terrainSlot : u32,
    terrainGeneration : u32,
    regionMappingId : u32,
    receiverCellSizeBits : u32,
}

struct TerrainPageState {
    generation : u32,
    currentLOD : u32,
    publishedFrame : u32,
    flags : u32,
}

struct TerrainTree64Descriptor {
    terrainSlot : u32,
    terrainGeneration : u32,
    internalLOD : u32,
    flags : u32,
    nodePageSlot : u32,
    nodePageGeneration : u32,
    rootNodeOffset : u32,
    nodeCount : u32,
    materialPageSlot : u32,
    materialPageGeneration : u32,
    materialCellOffset : u32,
    materialCellCount : u32,
    geometryRevisionLo : u32,
    geometryRevisionHi : u32,
    modelSide : u32,
    modelCellSizeBits : u32,
}

struct TerrainRasterResult {
    status : u32,
    overflowStage : u32,
    validationErrors : u32,
    levelCount : u32,
    nodeCount : u32,
    childCount : u32,
    materialCount : u32,
    rootCount : u32,
    occupiedVoxelCount : u32,
    terrainGeneration : u32,
    semanticHashLow : u32,
    semanticHashHigh : u32,
    buildRevisionLow : u32,
    buildRevisionHigh : u32,
    reserved0 : u32,
    reserved1 : u32,
}

struct TerrainTree64Result {
    status : u32,
    overflowStage : u32,
    validationErrors : u32,
    side : u32,
    nodeCount : u32,
    levelCount : u32,
    occupiedVoxelCount : u32,
    terrainGeneration : u32,
    semanticHashLow : u32,
    semanticHashHigh : u32,
    geometryRevisionLow : u32,
    geometryRevisionHigh : u32,
    visitedNodeCount : u32,
    maximumStack : u32,
    reserved0 : u32,
    reserved1 : u32,
    descriptor : TerrainTree64Descriptor,
}

struct TerrainJointRequest {
    terrainSlot : u32,
    rasterResultIndex : u32,
    tree64ResultIndex : u32,
    publishedFrame : u32,
    pageFlags : u32,
    internalLOD : u32,
    regionMappingId : u32,
    clipmapLOD : u32,
    nodeOffset : u32,
    nodeCapacity : u32,
    childOffset : u32,
    childCapacity : u32,
    rootOffset : u32,
    rootCapacity : u32,
    materialOffset : u32,
    materialCapacity : u32,
    paletteOffset : u32,
    paletteCapacity : u32,
    tree64NodesPerPage : u32,
    materialPageSlot : u32,
    materialPageGeneration : u32,
    materialCellOffset : u32,
    materialCellCount : u32,
    forceLeafRaster : u32,
    rasterNodeSourceOffset : u32,
    rasterChildSourceOffset : u32,
    rasterRootSourceOffset : u32,
    rasterMaterialSourceOffset : u32,
    tree64NodeSourceOffset : u32,
    nodePageSlot : u32,
    nodePageGeneration : u32,
    dynamicContentGeneration : u32,
    receiverCellSizeBits : u32,
    finerClipPacked : u32,
    pageSide : u32,
    reserved0 : u32,
    translation : vec4<f32>,
    canonicalTranslationScale : vec4<i32>,
    expectedPageState : TerrainPageState,
    expectedTree64 : TerrainTree64Descriptor,
}

struct TerrainPublicationRecord {
    terrainSlot : u32,
    flags : u32,
    reserved0 : u32,
    reserved1 : u32,
    expectedPageState : TerrainPageState,
    replacementPageState : TerrainPageState,
    expectedTree64 : TerrainTree64Descriptor,
    replacementTree64 : TerrainTree64Descriptor,
    replacementDynamicInstance : TerrainTree64DynamicInstance,
}

struct TerrainJointResult {
    status : u32,
    rejectionReason : u32,
    terrainSlot : u32,
    terrainGeneration : u32,
    rasterNodeCount : u32,
    tree64NodeCount : u32,
    occupiedVoxelCount : u32,
    reserved0 : u32,
}

struct TerrainJointStage {
    requestCount : u32,
    stage : u32,
    reserved0 : u32,
    reserved1 : u32,
}

@group(0) @binding(0) var<storage, read> requests : array<TerrainJointRequest>;
@group(0) @binding(1) var<storage, read> rasterResults : array<TerrainRasterResult>;
@group(0) @binding(2) var<storage, read> rasterNodes : array<SparseVoxelBrickNode>;
@group(0) @binding(3) var<storage, read> rasterOccupancy : array<SparseVoxelBrickOccupancy>;
@group(0) @binding(4) var<storage, read> rasterMaterialWords : array<u32>;
@group(0) @binding(5) var<storage, read> rasterMicroSurfaces : array<vec2<u32>>;
@group(0) @binding(6) var<storage, read> rasterChildren : array<u32>;
@group(0) @binding(7) var<storage, read> rasterRoots : array<u32>;
@group(0) @binding(8) var<storage, read> tree64Results : array<TerrainTree64Result>;
@group(0) @binding(9) var<storage, read> tree64Nodes : array<TerrainTree64Node>;
@group(0) @binding(10) var<storage, read_write> results : array<TerrainJointResult>;
@group(0) @binding(11) var<storage, read_write> records : array<TerrainPublicationRecord>;
@group(0) @binding(12) var<storage, read_write> outputNodes : array<SparseVoxelBrickNode>;
@group(0) @binding(13) var<storage, read_write> outputOccupancy : array<SparseVoxelBrickOccupancy>;
@group(0) @binding(14) var<storage, read_write> outputMaterialWords : array<u32>;
@group(0) @binding(15) var<storage, read_write> outputMicroSurfaces : array<vec2<u32>>;
@group(0) @binding(16) var<storage, read_write> outputChildren : array<u32>;
@group(0) @binding(17) var<storage, read_write> outputRoots : array<u32>;
@group(0) @binding(18) var<storage, read_write> outputTree64Nodes : array<TerrainTree64Node>;
@group(0) @binding(19) var<storage, read_write> outputTree64NodesPadded : array<TerrainTree64NodePadded>;
@group(0) @binding(20) var<storage, read_write> outputTree64Instances : array<TerrainTree64DynamicInstance>;
@group(0) @binding(21) var<storage, read_write> outputLODGroups : array<SparseVoxelBrickLODGroup>;
@group(0) @binding(22) var<storage, read_write> outputNodeLODGroupIndices : array<u32>;
@group(0) @binding(23) var<storage, read_write> outputRepresentativeLODGroupIndices : array<u32>;
@group(0) @binding(24) var<uniform> stageUniform : TerrainJointStage;

fn reject(index : u32, request : TerrainJointRequest, reason : u32) {
    results[index] = TerrainJointResult(
        TERRAIN_JOINT_STATUS_REJECTED, reason, request.terrainSlot,
        0u, 0u, 0u, 0u, 0u);
    records[index].flags = 0u;
}

fn rangeFits(offset : u32, count : u32, capacity : u32) -> bool {
    return offset <= capacity && count <= capacity - offset;
}

fn invalidLODGroup() -> SparseVoxelBrickLODGroup {
    return SparseVoxelBrickLODGroup(
        vec4<f32>(0.0), vec4<i32>(0, 0, 0, 1),
        0u, 0u, 0u, 0u, 0.0, 0.0, 0.0,
        SPARSE_VOXEL_BRICK_LOD_GROUP_INVALID_INDEX);
}

fn occupiedMinimum(packed : u32, axis : u32) -> u32 {
    return (packed >> (axis * 3u)) & 7u;
}

fn occupiedMaximum(packed : u32, axis : u32) -> u32 {
    return (packed >> (9u + axis * 3u)) & 7u;
}

fn lodGroupBoundaryFlags(
    node : SparseVoxelBrickNode,
    request : TerrainJointRequest) -> u32 {
    var flags = 0u;
    let nodeSide = node.minimumVoxelSize.w * f32(SPARSE_VOXEL_BRICK_SIDE);
    let pageMinimum = request.translation.xz -
        vec2<f32>(request.translation.w * 0.5);
    let pageMaximum = pageMinimum + vec2<f32>(request.translation.w);
    let epsilon = max(node.minimumVoxelSize.w * 0.25,
        abs(request.translation.w) * 1.0e-5);
    if (abs(node.minimumVoxelSize.x - pageMinimum.x) <= epsilon &&
        occupiedMinimum(node.packedOccupiedBounds, 0u) == 0u) {
        flags |= 1u;
    }
    if (abs(node.minimumVoxelSize.x + nodeSide - pageMaximum.x) <= epsilon &&
        occupiedMaximum(node.packedOccupiedBounds, 0u) == 7u) {
        flags |= 2u;
    }
    if (abs(node.minimumVoxelSize.z - pageMinimum.y) <= epsilon &&
        occupiedMinimum(node.packedOccupiedBounds, 2u) == 0u) {
        flags |= 16u;
    }
    if (abs(node.minimumVoxelSize.z + nodeSide - pageMaximum.y) <= epsilon &&
        occupiedMaximum(node.packedOccupiedBounds, 2u) == 7u) {
        flags |= 32u;
    }
    return flags;
}

fn makeRepresentativeLODGroup(
    node : SparseVoxelBrickNode,
    representativeIndex : u32,
    request : TerrainJointRequest) -> SparseVoxelBrickLODGroup {
    let groupSide = node.minimumVoxelSize.w * f32(SPARSE_VOXEL_BRICK_SIDE);
    let canonicalSide =
        node.canonicalGridOriginScale.w * i32(SPARSE_VOXEL_BRICK_SIDE);
    let error = select(node.minimumVoxelSize.w, node.selfError,
        node.selfError > 0.0);
    var flags = lodGroupBoundaryFlags(node, request);
    if ((request.forceLeafRaster & TERRAIN_JOINT_SOURCE_WORLD_SPACE) != 0u &&
        node.childCount == 8u &&
        (flags & SPARSE_VOXEL_BRICK_LOD_GROUP_BOUNDARY_MASK) == 0u) {
        // GPU-v2 terrain parents are built by reducing the same conservative
        // column/heightfield data as their children. Certify only interior
        // generated parents here: they can coarsen terrain inside a chunk like
        // virtual-geometry cluster groups. Page-edge parents still need an
        // explicit neighbor-paired seam proof before a boundary representative
        // may cover them, so keep their boundary bits un-certified.
        flags |= SPARSE_VOXEL_BRICK_LOD_GROUP_SURFACE_REPRESENTATIVE_CERTIFIED;
    }
    return SparseVoxelBrickLODGroup(
        vec4<f32>(node.minimumVoxelSize.xyz, groupSide),
        vec4<i32>(node.canonicalGridOriginScale.xyz, canonicalSide),
        0u, node.childCount, node.level & 0xffu,
        flags,
        error, error, error, representativeIndex);
}

fn makeTerrainDynamicInstance(
    request : TerrainJointRequest,
    raster : TerrainRasterResult,
    tree : TerrainTree64Result) -> TerrainTree64DynamicInstance {
    var pageMinimum = request.translation.xyz;
    pageMinimum.y += bitcast<f32>(raster.reserved0);
    let extent = bitcast<f32>(tree.descriptor.modelCellSizeBits) *
        f32(tree.descriptor.modelSide);
    pageMinimum.x -= request.translation.w * 0.5;
    pageMinimum.z -= request.translation.w * 0.5;
    let identity = mat4x4<f32>(
        vec4<f32>(1.0, 0.0, 0.0, 0.0),
        vec4<f32>(0.0, 1.0, 0.0, 0.0),
        vec4<f32>(0.0, 0.0, 1.0, 0.0),
        vec4<f32>(-pageMinimum, 1.0));
    let dynamicFlags = TREE64_DYNAMIC_VALID |
        TREE64_DYNAMIC_SKIP_RECEIVER_CELL |
        TREE64_DYNAMIC_TRANSLATION_ONLY;
    let requestedReceiverCellSize =
        bitcast<f32>(request.receiverCellSizeBits);
    let receiverCellSizeBits = select(
        tree.descriptor.modelCellSizeBits,
        request.receiverCellSizeBits,
        requestedReceiverCellSize > 0.0);
    return TerrainTree64DynamicInstance(
        identity, vec4<f32>(pageMinimum, 0.0),
        vec4<f32>(pageMinimum + vec3<f32>(extent), 0.0),
        request.terrainSlot, request.terrainSlot,
        request.dynamicContentGeneration, dynamicFlags,
        request.terrainSlot, raster.terrainGeneration,
        request.regionMappingId, receiverCellSizeBits);
}

#if TERRAIN_JOINT_SHADER_STAGE == 0
fn validate(index : u32) {
    if (index >= stageUniform.requestCount || index >= arrayLength(&requests) ||
        index >= arrayLength(&results) || index >= arrayLength(&records)) {
        return;
    }
    let request = requests[index];
    records[index].flags = 0u;
    if (request.rasterResultIndex >= arrayLength(&rasterResults) ||
        request.tree64ResultIndex >= arrayLength(&tree64Results) ||
        request.pageFlags == 0u || request.internalLOD > 31u ||
        request.canonicalTranslationScale.w <= 0 ||
        request.tree64NodesPerPage == 0u ||
        request.terrainSlot >= arrayLength(&outputTree64Instances) ||
        request.nodePageGeneration == 0u ||
        request.materialPageGeneration == 0u ||
        (request.finerClipPacked != 0u &&
            (request.pageSide == 0u || request.pageSide > 255u ||
             !(request.translation.w > 0.0))) ||
        (request.materialOffset & 3u) != 0u ||
        (request.rasterMaterialSourceOffset & 3u) != 0u ||
        any(request.translation != request.translation)) {
        reject(index, request, 1u);
        return;
    }
    let raster = rasterResults[request.rasterResultIndex];
    let tree = tree64Results[request.tree64ResultIndex];
    if (raster.status != TERRAIN_RASTER_STATUS_READY ||
        raster.validationErrors != 0u || raster.nodeCount == 0u ||
        raster.rootCount == 0u) {
        reject(index, request, 2u);
        return;
    }
    if (tree.status != TERRAIN_TREE64_STATUS_READY ||
        tree.validationErrors != 0u || tree.nodeCount == 0u ||
        tree.visitedNodeCount != tree.nodeCount) {
        reject(index, request, 3u);
        return;
    }
    if (raster.terrainGeneration == 0u ||
        raster.terrainGeneration != tree.terrainGeneration) {
        reject(index, request, 4u);
        return;
    }
    if (raster.buildRevisionLow != tree.geometryRevisionLow ||
        raster.buildRevisionHigh != tree.geometryRevisionHigh) {
        reject(index, request, 5u);
        return;
    }
    if (raster.occupiedVoxelCount != tree.occupiedVoxelCount) {
        reject(index, request, 6u);
        return;
    }
    if (raster.nodeCount > request.nodeCapacity ||
        raster.childCount > request.childCapacity ||
        raster.materialCount > request.materialCapacity ||
        raster.rootCount > request.rootCapacity ||
        !rangeFits(request.rasterNodeSourceOffset, raster.nodeCount,
            arrayLength(&rasterNodes)) ||
        !rangeFits(request.rasterNodeSourceOffset, raster.nodeCount,
            arrayLength(&rasterOccupancy)) ||
        !rangeFits(request.rasterChildSourceOffset, raster.childCount,
            arrayLength(&rasterChildren)) ||
        (((request.forceLeafRaster & TERRAIN_JOINT_SOURCE_WORLD_SPACE) == 0u) &&
            !rangeFits(request.rasterMaterialSourceOffset, raster.materialCount,
                arrayLength(&rasterMicroSurfaces))) ||
        !rangeFits(request.rasterMaterialSourceOffset / 4u,
            (raster.materialCount + 3u) / 4u,
            arrayLength(&rasterMaterialWords)) ||
        !rangeFits(request.rasterRootSourceOffset, raster.rootCount,
            arrayLength(&rasterRoots)) ||
        !rangeFits(request.nodeOffset, raster.nodeCount,
            arrayLength(&outputNodes)) ||
        !rangeFits(request.nodeOffset, raster.nodeCount,
            arrayLength(&outputOccupancy)) ||
        !rangeFits(request.childOffset, raster.childCount,
            arrayLength(&outputChildren)) ||
        !rangeFits(request.materialOffset, raster.materialCount,
            arrayLength(&outputMicroSurfaces)) ||
        !rangeFits(request.materialOffset / 4u,
            (raster.materialCount + 3u) / 4u,
            arrayLength(&outputMaterialWords)) ||
        !rangeFits(request.rootOffset, raster.rootCount,
            arrayLength(&outputRoots))) {
        reject(index, request, 7u);
        return;
    }
    if (!rangeFits(request.nodeOffset, request.nodeCapacity,
            arrayLength(&outputLODGroups)) ||
        !rangeFits(request.nodeOffset, request.nodeCapacity,
            arrayLength(&outputNodeLODGroupIndices)) ||
        !rangeFits(request.nodeOffset, request.nodeCapacity,
            arrayLength(&outputRepresentativeLODGroupIndices))) {
        reject(index, request, 7u);
        return;
    }
    let descriptor = tree.descriptor;
    if (!rangeFits(request.tree64NodeSourceOffset, tree.nodeCount,
            arrayLength(&tree64Nodes)) ||
        descriptor.nodeCount != tree.nodeCount ||
        descriptor.terrainSlot != request.terrainSlot ||
        descriptor.terrainGeneration != tree.terrainGeneration ||
        descriptor.internalLOD != request.internalLOD ||
        descriptor.geometryRevisionLo != tree.geometryRevisionLow ||
        descriptor.geometryRevisionHi != tree.geometryRevisionHigh ||
        descriptor.nodePageSlot != request.nodePageSlot ||
        descriptor.nodePageGeneration != request.nodePageGeneration ||
        descriptor.nodePageSlot >
            arrayLength(&outputTree64Nodes) / request.tree64NodesPerPage ||
        descriptor.nodePageSlot * request.tree64NodesPerPage >
            arrayLength(&outputTree64Nodes) ||
        tree.nodeCount > arrayLength(&outputTree64Nodes) -
            descriptor.nodePageSlot * request.tree64NodesPerPage) {
        reject(index, request, 8u);
        return;
    }
    if ((descriptor.flags & TERRAIN_TREE64_PAGE_VALID) == 0u ||
        descriptor.rootNodeOffset >= descriptor.nodeCount ||
        descriptor.modelSide == 0u ||
        !(bitcast<f32>(descriptor.modelCellSizeBits) > 0.0)) {
        reject(index, request, 9u);
        return;
    }
    results[index] = TerrainJointResult(
        TERRAIN_JOINT_STATUS_APPROVED, 0u, request.terrainSlot,
        raster.terrainGeneration, raster.nodeCount, tree.nodeCount,
        raster.occupiedVoxelCount, 0u);
}

@compute @workgroup_size(64, 1, 1)
fn validateTerrainGPUJointPublication(
    @builtin(global_invocation_id) gid : vec3<u32>) {
    validate(gid.x);
}
#elif TERRAIN_JOINT_SHADER_STAGE == 1
fn relocateNode(source : SparseVoxelBrickNode,
    request : TerrainJointRequest, raster : TerrainRasterResult) -> SparseVoxelBrickNode {
    var node = source;
    var translation = request.translation.xyz;
    translation.y += bitcast<f32>(raster.reserved0);
    let sourceWorldSpace = (request.forceLeafRaster &
        TERRAIN_JOINT_SOURCE_WORLD_SPACE) != 0u;
    if (!sourceWorldSpace) {
        node.minimumVoxelSize += vec4<f32>(translation, 0.0);
    }
    node.occupancyIndex += request.nodeOffset;
    node.materialBase += request.materialOffset;
    if (node.parentIndex != SPARSE_VOXEL_BRICK_INVALID_INDEX) {
        node.parentIndex += request.nodeOffset;
    }
    if (node.childCount != 0u) {
        node.firstChild += request.childOffset;
    }
    node.level = (node.level & 0xffu) |
        ((request.clipmapLOD & 0xffu) << 8u) |
        select(0u, SPARSE_VOXEL_BRICK_FORCE_LEAF_RASTER,
            (request.forceLeafRaster &
                TERRAIN_JOINT_FORCE_LEAF_RASTER) != 0u);
    node.instanceIndex = request.terrainSlot;
    if (request.finerClipPacked != 0u && request.pageSide != 0u) {
        let packed = request.finerClipPacked;
        let pageMinimum = request.translation.xz -
            vec2<f32>(request.translation.w * 0.5);
        let pageVoxel = request.translation.w / f32(request.pageSide);
        let finer = vec4<f32>(
            pageMinimum.x + f32(packed & 0xffu) * pageVoxel,
            pageMinimum.y + f32((packed >> 8u) & 0xffu) * pageVoxel,
            pageMinimum.x + f32((packed >> 16u) & 0xffu) * pageVoxel,
            pageMinimum.y + f32((packed >> 24u) & 0xffu) * pageVoxel);
        let brickExtent = node.minimumVoxelSize.w *
            f32(SPARSE_VOXEL_BRICK_SIDE);
        let clippedMinimum = max(node.minimumVoxelSize.xz, finer.xy);
        let clippedMaximum = min(
            node.minimumVoxelSize.xz + vec2<f32>(brickExtent), finer.zw);
        if (all(clippedMinimum < clippedMaximum)) {
            // This rectangle removes coarse geometry, so round inward. A
            // sub-step overlap is safe under depth testing; rounding outward
            // would create the missing square/crack the ownership mask exists
            // to prevent.
            let localMinimum = clamp(vec2<u32>(ceil(
                (clippedMinimum - node.minimumVoxelSize.xz) /
                    node.minimumVoxelSize.w *
                    SPARSE_VOXEL_BRICK_TERRAIN_CLIP_QUANTIZATION)),
                vec2<u32>(0u), vec2<u32>(64u));
            let localMaximum = clamp(vec2<u32>(floor(
                (clippedMaximum - node.minimumVoxelSize.xz) /
                    node.minimumVoxelSize.w *
                    SPARSE_VOXEL_BRICK_TERRAIN_CLIP_QUANTIZATION)),
                vec2<u32>(0u), vec2<u32>(64u));
            if (all(localMinimum < localMaximum)) {
                node.instanceIndex = SPARSE_VOXEL_BRICK_TERRAIN_CLIP_FLAG |
                    localMinimum.x | (localMinimum.y << 7u) |
                    (localMaximum.x << 14u) |
                    (localMaximum.y << 21u);
            }
        }
    }
    // The physical raster arena retains retired page bytes until their fence
    // is reclaimed. Carry the logical generation in every node so culling can
    // reject an old allocation after the slot publishes a replacement.
    node.reservedTailPadding = raster.terrainGeneration;
    node.paletteBase += request.paletteOffset;
    node.regionMappingId = request.regionMappingId;
    if (!sourceWorldSpace) {
        let sourceFinestVoxel = rasterNodes[request.rasterNodeSourceOffset]
            .minimumVoxelSize.w / f32(request.canonicalTranslationScale.w);
        let resolvedCanonicalY = i32(round(translation.y / sourceFinestVoxel));
        node.canonicalGridOriginScale = vec4<i32>(
            node.canonicalGridOriginScale.xyz *
                request.canonicalTranslationScale.www +
                request.canonicalTranslationScale.xyz + vec3<i32>(0, resolvedCanonicalY, 0),
            node.canonicalGridOriginScale.w *
                request.canonicalTranslationScale.w);
    }
    return node;
}

@compute @workgroup_size(64, 1, 1)
fn relocateTerrainGPUJointPublication(
    @builtin(global_invocation_id) gid : vec3<u32>) {
    let requestIndex = gid.y;
    let local = gid.x;
    if (requestIndex >= stageUniform.requestCount ||
        results[requestIndex].status != TERRAIN_JOINT_STATUS_APPROVED) {
        return;
    }
    let request = requests[requestIndex];
    let raster = rasterResults[request.rasterResultIndex];
    let tree = tree64Results[request.tree64ResultIndex];
    if (local < request.nodeCapacity) {
        let destination = request.nodeOffset + local;
        outputNodeLODGroupIndices[destination] =
            SPARSE_VOXEL_BRICK_LOD_GROUP_INVALID_INDEX;
        outputRepresentativeLODGroupIndices[destination] =
            SPARSE_VOXEL_BRICK_LOD_GROUP_INVALID_INDEX;
        outputLODGroups[destination] = invalidLODGroup();
    }
    if (local < raster.nodeCount) {
        let destination = request.nodeOffset + local;
        let relocated = relocateNode(
            rasterNodes[request.rasterNodeSourceOffset + local],
            request, raster);
        outputNodes[destination] = relocated;
        outputOccupancy[request.nodeOffset + local] = rasterOccupancy[
            request.rasterNodeSourceOffset + local];
        if (relocated.parentIndex != SPARSE_VOXEL_BRICK_INVALID_INDEX &&
            relocated.parentIndex >= request.nodeOffset &&
            relocated.parentIndex < request.nodeOffset + request.nodeCapacity) {
            outputNodeLODGroupIndices[destination] = relocated.parentIndex;
        }
        if (relocated.childCount != 0u) {
            outputRepresentativeLODGroupIndices[destination] = destination;
            outputLODGroups[destination] =
                makeRepresentativeLODGroup(relocated, destination, request);
        }
    }
    if (local < raster.childCount) {
        outputChildren[request.childOffset + local] =
            rasterChildren[request.rasterChildSourceOffset + local] +
                request.nodeOffset;
    }
    if (local < raster.rootCount) {
        outputRoots[request.rootOffset + local] =
            rasterRoots[request.rasterRootSourceOffset + local] +
                request.nodeOffset;
    }
    if (local < raster.materialCount) {
        if ((request.forceLeafRaster & TERRAIN_JOINT_SOURCE_WORLD_SPACE) != 0u) {
            // GPU-v2 has no analytic micro-geometry payload, but this stream's
            // high byte also classifies terrain for G-buffer shading. Zero
            // made generated terrain take the legacy model micro-grid path.
            // Keep the patch empty and publish only FlatSurface context.
            outputMicroSurfaces[request.materialOffset + local] = vec2<u32>(
                0u, TERRAIN_JOINT_FLAT_SURFACE_CONTEXT << 24u);
        } else {
            outputMicroSurfaces[request.materialOffset + local] =
                rasterMicroSurfaces[request.rasterMaterialSourceOffset + local];
        }
    }
    if (local < (raster.materialCount + 3u) / 4u) {
        outputMaterialWords[request.materialOffset / 4u + local] =
            rasterMaterialWords[
                request.rasterMaterialSourceOffset / 4u + local];
    }
    if (local < tree.nodeCount) {
        let treeBase = tree.descriptor.nodePageSlot *
            request.tree64NodesPerPage;
        outputTree64Nodes[treeBase + local] = tree64Nodes[
            request.tree64NodeSourceOffset + local];
        let source = tree64Nodes[request.tree64NodeSourceOffset + local];
        outputTree64NodesPadded[treeBase + local] = TerrainTree64NodePadded(
            source.childBaseAndFlags, source.populationMaskLo,
            source.populationMaskHi, 0u);
    }
    if (local == 0u) {
        let deferred = (request.forceLeafRaster &
            TERRAIN_JOINT_DEFER_VISIBILITY) != 0u;
        // A deferred build may reuse an active stable terrain slot. Never
        // mutate that visible instance while the replacement payload is
        // hidden. Stage 2 emits the complete instance as compact install
        // metadata; the CPU-controlled final CAS installs it later.
        if (!deferred) {
            outputTree64Instances[request.terrainSlot] =
                makeTerrainDynamicInstance(request, raster, tree);
        }
    }
}
#elif TERRAIN_JOINT_SHADER_STAGE == 2
@compute @workgroup_size(64, 1, 1)
fn finalizeTerrainGPUJointPublication(
    @builtin(global_invocation_id) gid : vec3<u32>) {
    let index = gid.x;
    if (index >= stageUniform.requestCount ||
        results[index].status != TERRAIN_JOINT_STATUS_APPROVED) {
        return;
    }
    let request = requests[index];
    let tree = tree64Results[request.tree64ResultIndex];
    var replacementTree = tree.descriptor;
    replacementTree.materialPageSlot = request.materialPageSlot;
    replacementTree.materialPageGeneration = request.materialPageGeneration;
    replacementTree.materialCellOffset = request.materialCellOffset;
    replacementTree.materialCellCount = request.materialCellCount;
    var replacementState = TerrainPageState(
        results[index].terrainGeneration, request.internalLOD,
        request.publishedFrame, request.pageFlags);
    if ((request.forceLeafRaster &
            TERRAIN_JOINT_DEFER_VISIBILITY) != 0u) {
        // The generated payload and instance are unreachable while their
        // descriptor/page-state pair remains exactly as it was. Runtime
        // streaming installs the complete ownership group atomically after
        // all child builds have completed.
        replacementState = request.expectedPageState;
        replacementTree = request.expectedTree64;
    }
    let dynamicFlags = TREE64_DYNAMIC_VALID |
        TREE64_DYNAMIC_SKIP_RECEIVER_CELL |
        TREE64_DYNAMIC_TRANSLATION_ONLY;
    let publicationFlags = select(
        TERRAIN_PUBLICATION_VALID | TERRAIN_PUBLICATION_DYNAMIC_INSTANCE,
        TERRAIN_PUBLICATION_VALID,
        (request.forceLeafRaster & TERRAIN_JOINT_DEFER_VISIBILITY) != 0u);
    let replacementInstance = makeTerrainDynamicInstance(
        request, rasterResults[request.rasterResultIndex], tree);
    records[index] = TerrainPublicationRecord(
        request.terrainSlot, publicationFlags,
        request.dynamicContentGeneration, dynamicFlags,
        request.expectedPageState, replacementState,
        request.expectedTree64, replacementTree,
        replacementInstance);
}
#endif
