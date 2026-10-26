const SPARSE_VOXEL_INVALID_INDEX : u32 = 0xffffffffu;
const SPARSE_VOXEL_INDIRECT_SLOT_COUNT : u32 = 4u;
const SPARSE_VOXEL_INDIRECT_SLOT_PREPROCESS : u32 = 0u;
const SPARSE_VOXEL_OVERFLOW_NONE : u32 = 0u;
const SPARSE_VOXEL_OVERFLOW_VISIBLE_INSTANCES : u32 = 1u;
const SPARSE_VOXEL_OVERFLOW_VISIBLE_VOXELS : u32 = 2u;
const SPARSE_VOXEL_OVERFLOW_PROJECTED_VOXELS : u32 = 3u;
const SPARSE_VOXEL_OVERFLOW_TILE_CANDIDATES : u32 = 4u;
const SPARSE_VOXEL_OVERFLOW_TILE_RANGES : u32 = 5u;
const SPARSE_VOXEL_OVERFLOW_PAGE_REQUESTS : u32 = 6u;
const SPARSE_VOXEL_OVERFLOW_LOD_QUEUE : u32 = 7u;
const SPARSE_VOXEL_OVERFLOW_TLAS_QUEUE : u32 = 8u;
const SPARSE_VOXEL_OVERFLOW_OCCUPANCY_LAYOUT : u32 = 9u;
const SPARSE_VOXEL_OVERFLOW_OCCUPANCY_DIRTY_WORDS : u32 = 10u;
const SPARSE_VOXEL_OVERFLOW_OCCUPANCY_WORD_SPLATS : u32 = 11u;
const SPARSE_VOXEL_CULLING_FRUSTUM : u32 = 1u;
const SPARSE_VOXEL_CULLING_OCCLUSION : u32 = 2u;
const SPARSE_VOXEL_CULLING_PREVIOUS_FRAME : u32 = 4u;
const SPARSE_VOXEL_CULLING_RECOVERY : u32 = 8u;
const SPARSE_VOXEL_CULLING_REVERSE_Z : u32 = 16u;
const SPARSE_VOXEL_CULLING_PREPASS_GUARANTEE : u32 = 64u;
const SPARSE_VOXEL_CULLING_OCCUPANCY_QUERY : u32 = 128u;

const SPARSE_VOXEL_OCCLUSION_VISIBLE : u32 = 0u;
const SPARSE_VOXEL_OCCLUSION_CULLED_FRUSTUM : u32 = 1u;
const SPARSE_VOXEL_OCCLUSION_CULLED : u32 = 2u;
const SPARSE_VOXEL_OCCLUSION_HISTORY_INVALID : u32 = 3u;
const SPARSE_VOXEL_OCCLUSION_NEAR_PLANE : u32 = 4u;
const SPARSE_VOXEL_OCCLUSION_VIEWPORT_ESCAPE : u32 = 5u;
const SPARSE_VOXEL_OCCLUSION_INVALID_PROJECTION : u32 = 6u;
const SPARSE_VOXEL_OCCLUSION_DEGENERATE_BOUNDS : u32 = 7u;
const SPARSE_VOXEL_OCCLUSION_MOVING_PREVIOUS_TRANSFORM : u32 = 8u;
const SPARSE_VOXEL_LOD_SEEDS_FROM_VISIBLE_INSTANCES : u32 = 32u;
const SPARSE_VOXEL_TILE_SIZE : u32 = 4u;
const SPARSE_VOXEL_SURFACE_TIE_EPSILON : f32 = 1.0e-5;
const SPARSE_VOXEL_SURFACE_HIT : u32 = 1u;
const SPARSE_VOXEL_SURFACE_CAMERA_INSIDE : u32 = 2u;
// The resolver produced exact geometry but no appearance, so voxelIndex does not
// address the material array. Set by resolvers over representations that store
// occupancy only, such as the Tree64 clipmap.
const SPARSE_VOXEL_SURFACE_NO_MATERIAL : u32 = 4u;
// The resolver carries appearance inline, in reserved0 and reserved1, instead
// of a voxelIndex into the scene material array. Set by the brick resolver,
// whose hierarchy has its own eight-byte packed material per occupied voxel:
//
//   reserved0 : R (0..7) | G (8..15) | B (16..23) | roughness (24..31)
//   reserved1 : emissive (0..7) | metallic (8..15)
//   reserved2 : bottom RGB (0..23) | terrain context (24..31)
//
// See SparseVoxelBrickMaterialGPU and packSparseVoxelBrickMaterial().
const SPARSE_VOXEL_SURFACE_PACKED_MATERIAL : u32 = 8u;
// The brick traversal hit a descriptor-free analytic terrain patch. This is
// distinct from exact micro terrain, whose resolved staircase faces retain
// their near-field normal gating.
const SPARSE_VOXEL_SURFACE_ANALYTIC_TERRAIN : u32 = 16u;
const SPARSE_VOXEL_RESOLVED_BOTTOM_ALBEDO_MASK : u32 = 0x00ffffffu;
const SPARSE_VOXEL_RESOLVED_CONTEXT_SHIFT : u32 = 24u;
const SPARSE_VOXEL_RESOLVED_CONTEXT_MASK : u32 = 0xffu;

// Marks a temporal id that identifies one exact represented surface cell.
// Resolvers without an LOD-stable cell id use a coarser face class instead;
// motion heuristics may shorten that history because it can alias unrelated
// receivers. Exact ids have already passed the stronger validation and must
// not lose convergence merely because the camera moved.
const SPARSE_VOXEL_TEMPORAL_ID_EXACT : u32 = 0x80000000u;
// Luminance that a full 255 in the packed emissive byte stands for. Must match
// kSparseVoxelBrickEmissiveScale on the C++ side.
const SPARSE_VOXEL_PACKED_EMISSIVE_SCALE : f32 = 16.0;
const SPARSE_VOXEL_GBUFFER_HAS_PREVIOUS_FRAME : u32 = 1u;
// Optional source-material face modes (SparseVoxelMaterial.reserved1).
const SPARSE_VOXEL_MATERIAL_BLEND_TOP_BOTTOM_FACES : u32 = 1u << 0u;
const SPARSE_VOXEL_MATERIAL_BLEND_SIDE_FACES : u32 = 1u << 1u;
// The compact brick representation keeps the same modes after its 8-bit
// secondary palette reference.
const SPARSE_VOXEL_PACKED_SECONDARY_PALETTE_SHIFT : u32 = 20u;
const SPARSE_VOXEL_PACKED_BLEND_TOP_BOTTOM_FACES : u32 = 1u << 28u;
const SPARSE_VOXEL_PACKED_BLEND_SIDE_FACES : u32 = 1u << 29u;
const SPARSE_VOXEL_PACKED_SECONDARY_ENABLED : u32 = 1u << 30u;

struct SparseVoxelBLAS {
    rootCenterExtent : vec4<f32>,
    firstVoxel : u32,
    voxelCount : u32,
    maxLevel : u32,
    flags : u32,
}

struct SparseVoxelPathLevel {
    pathLo : u32,
    pathHi : u32,
    level : u32,
    flags : u32,
}

struct SparseVoxelCenterLength {
    center : vec3<f32>,
    length : f32,
}

struct SparseVoxelMaterial {
    albedoRGBA8 : u32,
    roughnessMetallicUNorm8 : u32,
    emissiveRG16F : u32,
    emissiveB16F : u32,
    materialId : u32,
    flags : u32,
    reserved0 : u32,
    reserved1 : u32,
}

struct SparseVoxelMicroMaterial {
    geometry : u32,
    shading : u32,
}

struct SparseVoxelInstance {
    modelToWorld : mat4x4<f32>,
    worldToModel : mat4x4<f32>,
    previousModelToWorld : mat4x4<f32>,
    worldBoundsMin : vec4<f32>,
    worldBoundsMax : vec4<f32>,
    blasIndex : u32,
    flags : u32,
    reserved0 : u32,
    reserved1 : u32,
}

struct SparseVoxelTLASNode {
    worldBoundsMin : vec4<f32>,
    worldBoundsMax : vec4<f32>,
    firstChild : u32,
    childCount : u32,
    firstInstance : u32,
    instanceCount : u32,
}

struct SparseVoxelBLASRootRange {
    firstRoot : u32,
    rootCount : u32,
    maxLevel : u32,
    flags : u32,
}

struct SparseVoxelTLASScanBlock {
    visible : u32,
    roots : u32,
    reserved0 : u32,
    reserved1 : u32,
}

struct TLASQueueElement {
    nodeIndex : u32,
    depth : u32,
    reserved0 : u32,
    reserved1 : u32,
}

struct VisibleInstance {
    instanceIndex : u32,
    firstVisibleVoxel : u32,
    visibleVoxelCount : u32,
    flags : u32,
}

struct VisibleVoxel {
    instanceIndex : u32,
    voxelIndex : u32,
    reserved0 : u32,
    reserved1 : u32,
}

struct SparseVoxelLODNode {
    firstChild : u32,
    childCount : u32,
    voxelIndex : u32,
    level : u32,
}

struct SparseVoxelResidentNodeMetadata {
    logicalNodeIndex : u32,
    refinementPageIndex : u32,
    owningPageIndex : u32,
    flags : u32,
}

struct SparseVoxelLODQueueElement {
    instanceIndex : u32,
    nodeIndex : u32,
    reserved0 : u32,
    reserved1 : u32,
}

struct SparseVoxelLODRecoveryState {
    rejectedNodeCount : atomic<u32>,
    temporalVisibleVoxelCount : atomic<u32>,
    recoverySeedCount : atomic<u32>,
    recoveredVisibleVoxelCount : atomic<u32>,
}

struct SparseVoxelLODSeed {
    instanceIndex : u32,
    firstRoot : u32,
    rootCount : u32,
    firstQueueEntry : u32,
}

struct SparseVoxelPageRequest {
    pageIndex : u32,
    priority : u32,
    instanceIndex : u32,
    flags : u32,
}

struct SparseVoxelPhysicalPageRewrite {
    nodeIndex : u32,
    firstChild : u32,
    childCount : u32,
    flags : u32,
}

struct SparseVoxelPageBounds {
    localBoundsMin : vec4<f32>,
    localBoundsMax : vec4<f32>,
}

struct ProjectedVoxel {
    packedInclusivePixelBounds : vec2<u32>,
    quadrantBitset : u32,
    duplicateCount : u32,
    instanceIndex : u32,
    voxelIndex : u32,
    reserved0 : u32,
    reserved1 : u32,
}

struct TileCandidate {
    keyLo : u32,
    keyHi : u32,
    instanceIndex : u32,
    voxelIndex : u32,
    quadrant : u32,
    projectedVoxelIndex : u32,
    reserved0 : u32,
    reserved1 : u32,
}

struct SparseVoxelTileRange {
    begin : u32,
    end : u32,
    reserved0 : u32,
    reserved1 : u32,
}

struct ResolvedVoxelSurface {
    surfaceT : f32,
    instanceIndex : u32,
    voxelIndex : u32,
    packedLocalFace : u32,
    hitFlags : u32,
    reserved0 : u32,
    reserved1 : u32,
    reserved2 : u32,
}

struct VoxelMaterialSampleAddress {
    canonicalMicroCoordinate : vec3<i32>,
    // Zero selects the material-global mapping. Nonzero values identify a
    // logical generated region, never an atlas layer, page, or brick slot.
    regionMappingId : u32,
}

fn sparseVoxelResolvedContext(surface : ResolvedVoxelSurface) -> u32 {
    return select(
        0u,
        (surface.reserved2 >> SPARSE_VOXEL_RESOLVED_CONTEXT_SHIFT) &
            SPARSE_VOXEL_RESOLVED_CONTEXT_MASK,
        (surface.hitFlags & SPARSE_VOXEL_SURFACE_PACKED_MATERIAL) != 0u);
}

struct SparseVoxelGBufferPixel {
    ids : vec4<u32>,
    materialUv : vec4<f32>,
    baseColor : vec4<f32>,
    materialProperties : vec4<f32>,
    emissive : vec4<f32>,
    geometryNormal : vec4<f32>,
    normal : vec4<f32>,
    motionDepth : vec4<f32>,
}

struct IndirectDispatchArgs {
    x : u32,
    y : u32,
    z : u32,
    reserved : u32,
}

struct SparseVoxelFrameCounters {
    visibleInstanceCount : atomic<u32>,
    visibleVoxelCount : atomic<u32>,
    candidateCount : atomic<u32>,
    duplicateCount : atomic<u32>,
    overflowStage : atomic<u32>,
    requiredCount : atomic<u32>,
    capacity : atomic<u32>,
    occlusionCulledVoxelCount : atomic<u32>,
}

struct SparseVoxelBootstrapUniforms {
    voxelCount : u32,
    preprocessWorkgroupSize : u32,
    visibleInstanceCapacity : u32,
    visibleVoxelCapacity : u32,
}

struct SparseVoxelLODPassUniforms {
    firstRoot : u32,
    rootCount : u32,
    nodeCapacity : u32,
    depth : u32,
    pageCount : u32,
    requestWordCount : u32,
    streamingEnabled : u32,
    cullingFlags : u32,
    seedCount : u32,
    seedCapacity : u32,
    levelCapacity : u32,
    reserved2 : u32,
    hzbWidth : u32,
    hzbHeight : u32,
    hzbMipCount : u32,
    hzbFirstValidMip : u32,
    occlusionDepthBias : f32,
    occlusionMipBias : i32,
    historyValid : u32,
    reserved3 : u32,
    rejectionCapacity : u32,
    reserved4 : u32,
    reserved5 : u32,
    reserved6 : u32,
}

struct SparseVoxelTLASCullingUniforms {
    instanceCount : u32,
    nodeCount : u32,
    rootNode : u32,
    maxDepth : u32,
    queueCapacity : u32,
    visibleInstanceCapacity : u32,
    lodQueueCapacity : u32,
    scanBlockCount : u32,
    blasCount : u32,
    cullingFlags : u32,
    depth : u32,
    hzbWidth : u32,
    hzbHeight : u32,
    hzbMipCount : u32,
    hzbFirstValidMip : u32,
    occlusionDepthBias : f32,
    occlusionMipBias : i32,
    historyValid : u32,
    reserved : u32,
    occupancyQueryMinimum : array<vec4<f32>, 3>,
    occupancyQueryMaximum : array<vec4<f32>, 3>,
    occupancyQueryBoxCount : u32,
    reservedQuery0 : u32,
    reservedQuery1 : u32,
    reservedQuery2 : u32,
}

struct SparseVoxelStreamingPassUniforms {
    pageCount : u32,
    requestWordCount : u32,
    rewriteCount : u32,
    physicalNodeCapacity : u32,
}

struct SparseVoxelRasterUniforms {
    worldToCamera : mat4x4<f32>,
    cameraToWorld : mat4x4<f32>,
    cameraWorldPositionNear : vec4<f32>,
    focalPrincipal : vec4<f32>,
    viewportTiles : vec4<u32>,
    visibleVoxelCapacity : u32,
    projectedVoxelCapacity : u32,
    candidateCapacity : u32,
    tileRangeCapacity : u32,
    worldToClip : mat4x4<f32>,
    previousWorldToClip : mat4x4<f32>,
    gbufferFlags : u32,
    gbufferClearDepth : f32,
    lodPixelThreshold : f32,
    lodEnabled : u32,
}

struct SparseVoxelRadixUniforms {
    shift : u32,
    candidateCapacity : u32,
    reserved0 : u32,
    reserved1 : u32,
}
