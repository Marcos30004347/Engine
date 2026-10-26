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

struct FineRasterResult {
    status : u32,
    overflowStage : u32,
    validationErrors : u32,
    flags : u32,
    denseBrickCount : u32,
    nodeCount : u32,
    materialCount : u32,
    rootCount : u32,
    occupiedVoxelCount : u32,
    terrainGeneration : u32,
    generatorRevision : u32,
    selectedWorkgroup : u32,
    scratchWords : u32,
    perNodeVoxelScratchWords : u32,
    microSurfaceBytes : u32,
    reserved : u32,
}

struct V2Tree64Result {
    status : u32,
    overflowStage : u32,
    validationErrors : u32,
    flags : u32,
    candidateCount : u32,
    nodeCount : u32,
    levelCount : u32,
    rootNodeOffset : u32,
    occupiedVoxelCount : u32,
    rasterOccupiedVoxelCount : u32,
    terrainGeneration : u32,
    generatorRevision : u32,
    visitedNodeCount : u32,
    maximumChildCount : u32,
    fullPayloadHashWords : u32,
    selectedWorkgroup : u32,
    descriptor : TerrainTree64Descriptor,
}

struct RasterHierarchyResult {
    status : u32,
    overflowStage : u32,
    validationErrors : u32,
    flags : u32,
    leafNodeCount : u32,
    nodeCount : u32,
    materialCount : u32,
    childCount : u32,
    rootCount : u32,
    terrainGeneration : u32,
    generatorRevision : u32,
    selectedWorkgroup : u32,
    levelCount : u32,
    scratchWords : u32,
    microSurfaceBytes : u32,
    selectedMode : u32,
}

struct RasterResult {
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

struct Tree64Result {
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

@group(0) @binding(0) var<storage, read> fineResults : array<FineRasterResult>;
@group(0) @binding(1) var<storage, read> hierarchyResults : array<RasterHierarchyResult>;
@group(0) @binding(2) var<storage, read> v2TreeResults : array<V2Tree64Result>;
@group(0) @binding(3) var<storage, read_write> rasterResults : array<RasterResult>;
@group(0) @binding(4) var<storage, read_write> treeResults : array<Tree64Result>;

@compute @workgroup_size(1, 1, 1)
fn adaptTerrainGPUV2RendererPayload(@builtin(global_invocation_id) gid : vec3<u32>) {
    if (gid.x != 0u) { return; }
    let lane = gid.z;
    let fine = fineResults[lane];
    let raster = hierarchyResults[lane];
    let tree = v2TreeResults[lane];
    rasterResults[lane] = RasterResult(
        raster.status, raster.overflowStage, raster.validationErrors,
        raster.levelCount, raster.nodeCount, raster.childCount,
        raster.materialCount, raster.rootCount,
        fine.occupiedVoxelCount, raster.terrainGeneration,
        0u, 0u, tree.descriptor.geometryRevisionLo,
        tree.descriptor.geometryRevisionHi, fine.reserved, 0u);
    treeResults[lane] = Tree64Result(
        tree.status, tree.overflowStage, tree.validationErrors,
        tree.descriptor.modelSide, tree.nodeCount, tree.levelCount,
        tree.occupiedVoxelCount, tree.terrainGeneration, 0u, 0u,
        tree.descriptor.geometryRevisionLo,
        tree.descriptor.geometryRevisionHi, tree.visitedNodeCount,
        tree.maximumChildCount, 0u, 0u, tree.descriptor);
}

// Shipping terrain rasterization publishes only the independently generated
// solid 8^3 leaf bricks. The spatial page quadtree already is the terrain LOD;
// constructing and then forcibly ignoring another conservative raster parent
// hierarchy costs most of the generation frame and can only change geometry
// if it accidentally leaks into the draw cut. Tree64 remains independently
// generated for ray traversal.
@compute @workgroup_size(1, 1, 1)
fn adaptTerrainGPUV2RendererLeafPayload(
    @builtin(global_invocation_id) gid : vec3<u32>) {
    if (gid.x != 0u) { return; }
    let lane = gid.z;
    let fine = fineResults[lane];
    let tree = v2TreeResults[lane];
    rasterResults[lane] = RasterResult(
        fine.status, fine.overflowStage, fine.validationErrors,
        1u, fine.nodeCount, 0u, fine.materialCount, fine.rootCount,
        fine.occupiedVoxelCount, fine.terrainGeneration,
        0u, 0u, tree.descriptor.geometryRevisionLo,
        tree.descriptor.geometryRevisionHi, fine.reserved, 0u);
    treeResults[lane] = Tree64Result(
        tree.status, tree.overflowStage, tree.validationErrors,
        tree.descriptor.modelSide, tree.nodeCount, tree.levelCount,
        tree.occupiedVoxelCount, tree.terrainGeneration, 0u, 0u,
        tree.descriptor.geometryRevisionLo,
        tree.descriptor.geometryRevisionHi, tree.visitedNodeCount,
        tree.maximumChildCount, 0u, 0u, tree.descriptor);
}
