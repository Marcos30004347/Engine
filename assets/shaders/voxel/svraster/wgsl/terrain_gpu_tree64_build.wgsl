#include "sparse_voxel_tree64_common.wgsl"

const TERRAIN_TREE64_STATUS_BUILDING : u32 = 1u;
const TERRAIN_TREE64_STATUS_READY : u32 = 2u;
const TERRAIN_TREE64_STATUS_OVERFLOW : u32 = 3u;
const TERRAIN_TREE64_STATUS_INVALID : u32 = 5u;
const TERRAIN_TREE64_MAXIMUM_LEVELS : u32 = 8u;

struct TerrainTree64Config {
    side : u32,
    levelCount : u32,
    columnSide : u32,
    columnCount : u32,
    maskCapacity : u32,
    nodeCapacity : u32,
    expectedTerrainGeneration : u32,
    internalLOD : u32,
    terrainSlot : u32,
    nodePageSlot : u32,
    nodePageGeneration : u32,
    occupiedVoxelCount : u32,
    geometryRevisionLow : u32,
    geometryRevisionHigh : u32,
    voxelSizeBits : u32,
    edgeBits : u32,
    levelOffsets : array<u32, TERRAIN_TREE64_MAXIMUM_LEVELS>,
    levelCounts : array<u32, TERRAIN_TREE64_MAXIMUM_LEVELS>,
    levelHorizontalAxes : array<u32, TERRAIN_TREE64_MAXIMUM_LEVELS>,
    levelVerticalAxes : array<u32, TERRAIN_TREE64_MAXIMUM_LEVELS>,
}

struct TerrainTree64Column {
    topY : u32,
    firstSolidY : u32,
    packedMaterialContext : u32,
    packedFlags : u32,
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

struct TerrainTree64Stage {
    stage : u32,
    level : u32,
    reserved0 : u32,
    reserved1 : u32,
}

@group(0) @binding(0) var<storage, read> config : TerrainTree64Config;
@group(0) @binding(1) var<storage, read> columns : array<TerrainTree64Column>;
@group(0) @binding(2) var<storage, read_write> masks : array<SparseVoxelTree64Node>;
@group(0) @binding(3) var<storage, read_write> outputNodes : array<SparseVoxelTree64Node>;
@group(0) @binding(4) var<storage, read_write> maskNodeIndices : array<u32>;
@group(0) @binding(5) var<storage, read_write> result : TerrainTree64Result;
@group(0) @binding(6) var<uniform> stageUniform : TerrainTree64Stage;

#if TERRAIN_TREE64_SHADER_STAGE != 0
fn axisIndex(index : u32, extent : vec3<u32>) -> vec3<u32> {
    return vec3<u32>(index % extent.x,
        (index / extent.x) % extent.y,
        index / (extent.x * extent.y));
}

fn linearIndex(value : vec3<u32>, extent : vec3<u32>) -> u32 {
    return value.x + extent.x * (value.y + extent.y * value.z);
}

fn levelExtent(level : u32) -> vec3<u32> {
    return vec3<u32>(config.levelHorizontalAxes[level],
        config.levelVerticalAxes[level],
        config.levelHorizontalAxes[level]);
}

fn maskNonempty(node : SparseVoxelTree64Node) -> bool {
    return node.populationMaskLo != 0u || node.populationMaskHi != 0u;
}

fn maskCount(node : SparseVoxelTree64Node) -> u32 {
    return countOneBits(node.populationMaskLo) +
        countOneBits(node.populationMaskHi);
}

fn maskContains(node : SparseVoxelTree64Node, child : u32) -> bool {
    return tree64PopulationContains(node, child);
}

fn maskCountBefore(node : SparseVoxelTree64Node, child : u32) -> u32 {
    return tree64PopulationCountBefore(node, child);
}

fn setMaskBit(node : SparseVoxelTree64Node, child : u32)
    -> SparseVoxelTree64Node {
    var value = node;
    if (child < 32u) {
        value.populationMaskLo |= 1u << child;
    } else {
        value.populationMaskHi |= 1u << (child - 32u);
    }
    return value;
}
#endif

#if TERRAIN_TREE64_SHADER_STAGE == 1
fn denseMask(level : u32, coordinate : vec3<u32>)
    -> SparseVoxelTree64Node {
    return masks[config.levelOffsets[level] +
        linearIndex(coordinate, levelExtent(level))];
}

fn buildTerminal(index : u32) {
    if (index >= config.levelCounts[0]) {
        return;
    }
    let terminal = axisIndex(index, levelExtent(0u));
    let origin = terminal * 4u;
    var node = SparseVoxelTree64Node(TREE64_TERMINAL_FLAG, 0u, 0u);
    for (var child = 0u; child < 64u; child += 1u) {
        let cell = origin + axisIndex(child, vec3<u32>(4u));
        if (cell.x >= config.columnSide || cell.z >= config.columnSide ||
            cell.y >= config.side) {
            continue;
        }
        let column = columns[cell.z * config.columnSide + cell.x];
        if (cell.y >= column.firstSolidY && cell.y <= column.topY) {
            node = setMaskBit(node, child);
        }
    }
    masks[config.levelOffsets[0] + index] = node;
}

fn buildUpper(level : u32, index : u32) {
    if (level == 0u || level >= config.levelCount ||
        index >= config.levelCounts[level]) {
        return;
    }
    let coordinate = axisIndex(index, levelExtent(level));
    let childOrigin = coordinate * 4u;
    var node = SparseVoxelTree64Node(0u, 0u, 0u);
    for (var child = 0u; child < 64u; child += 1u) {
        let childCoordinate = childOrigin +
            axisIndex(child, vec3<u32>(4u));
        if (all(childCoordinate < levelExtent(level - 1u)) &&
            maskNonempty(denseMask(level - 1u, childCoordinate))) {
            node = setMaskBit(node, child);
        }
    }
    masks[config.levelOffsets[level] + index] = node;
}
#endif

#if TERRAIN_TREE64_SHADER_STAGE == 2
fn failOverflow(stage : u32) {
    result.status = TERRAIN_TREE64_STATUS_OVERFLOW;
    result.overflowStage = stage;
}

fn compactTree() {
    if (result.status != TERRAIN_TREE64_STATUS_BUILDING) {
        return;
    }
    var required = 1u;
    for (var level = 0u; level + 1u < config.levelCount; level += 1u) {
        for (var index = 0u; index < config.levelCounts[level]; index += 1u) {
            required += select(0u, 1u,
                maskNonempty(masks[config.levelOffsets[level] + index]));
        }
    }
    result.nodeCount = required;
    if (required > config.nodeCapacity) {
        failOverflow(2u);
        return;
    }

    for (var index = 0u; index < config.maskCapacity; index += 1u) {
        maskNodeIndices[index] = 0xffffffffu;
    }
    maskNodeIndices[config.levelOffsets[config.levelCount - 1u]] = 0u;
    var nextNode = 1u;
    var cursor = config.levelCount;
    while (cursor > 1u) {
        let level = cursor - 1u;
        let extent = levelExtent(level);
        for (var local = 0u; local < config.levelCounts[level]; local += 1u) {
            let address = config.levelOffsets[level] + local;
            let nodeIndex = maskNodeIndices[address];
            if (nodeIndex == 0xffffffffu) { continue; }
            let source = masks[address];
            let children = maskCount(source);
            let childBase = select(0u, nextNode, children != 0u);
            outputNodes[nodeIndex] = SparseVoxelTree64Node(
                childBase << 1u, source.populationMaskLo,
                source.populationMaskHi);
            let coordinate = axisIndex(local, extent);
            for (var child = 0u; child < 64u; child += 1u) {
                if (!maskContains(source, child)) { continue; }
                let childCoordinate = coordinate * 4u +
                    axisIndex(child, vec3<u32>(4u));
                if (any(childCoordinate >= levelExtent(level - 1u))) {
                    continue;
                }
                let childAddress = config.levelOffsets[level - 1u] +
                    linearIndex(childCoordinate, levelExtent(level - 1u));
                maskNodeIndices[childAddress] = childBase +
                    maskCountBefore(source, child);
            }
            nextNode += children;
        }
        cursor -= 1u;
    }
    for (var local = 0u; local < config.levelCounts[0]; local += 1u) {
        let address = config.levelOffsets[0] + local;
        let nodeIndex = maskNodeIndices[address];
        if (nodeIndex != 0xffffffffu) {
            outputNodes[nodeIndex] = masks[address];
        }
    }
    if (nextNode != required) {
        result.validationErrors |= 32u;
        result.status = TERRAIN_TREE64_STATUS_INVALID;
    }
}
#endif

#if TERRAIN_TREE64_SHADER_STAGE == 3
fn hashMix(value : u32) {
    result.semanticHashLow =
        (result.semanticHashLow ^ value) * 16777619u;
    result.semanticHashHigh = (result.semanticHashHigh ^
        (value ^ result.semanticHashLow ^ 0x9e3779b9u)) * 16777619u;
}

fn validateAndHash() {
    if (result.status != TERRAIN_TREE64_STATUS_BUILDING ||
        result.nodeCount == 0u) {
        return;
    }
    var errors = result.validationErrors;
    var occupiedVoxelCount = 0u;
    for (var index = 0u; index < config.columnCount; index += 1u) {
        let column = columns[index];
        if (column.firstSolidY <= column.topY && column.topY < config.side) {
            occupiedVoxelCount += column.topY - column.firstSolidY + 1u;
        } else {
            errors |= 16u;
        }
    }
    result.occupiedVoxelCount = occupiedVoxelCount;
    var visited = 0u;
    var visitedSum = 0u;
    for (var level = 0u; level < config.levelCount; level += 1u) {
        let extent = levelExtent(level);
        for (var local = 0u; local < config.levelCounts[level]; local += 1u) {
            let address = config.levelOffsets[level] + local;
            let nodeIndex = maskNodeIndices[address];
            if (nodeIndex == 0xffffffffu) { continue; }
            if (nodeIndex >= result.nodeCount) {
                errors |= 1u;
                continue;
            }
            visited += 1u;
            visitedSum += nodeIndex;
            let node = outputNodes[nodeIndex];
            let expected = masks[address];
            if (node.populationMaskLo != expected.populationMaskLo ||
                node.populationMaskHi != expected.populationMaskHi) {
                errors |= 2u;
            }
            if (level == 0u) {
                if ((node.childBaseAndFlags & TREE64_TERMINAL_FLAG) == 0u ||
                    (node.childBaseAndFlags >> 1u) != 0u) { errors |= 4u; }
                continue;
            }
            if ((node.childBaseAndFlags & TREE64_TERMINAL_FLAG) != 0u) {
                errors |= 4u;
                continue;
            }
            let childCount = maskCount(node);
            let childBase = node.childBaseAndFlags >> 1u;
            if ((childCount == 0u && childBase != 0u) ||
                (childCount != 0u && (childBase == 0u ||
                 childBase + childCount > result.nodeCount))) {
                errors |= 8u;
                continue;
            }
            let coordinate = axisIndex(local, extent);
            for (var child = 0u; child < 64u; child += 1u) {
                if (!maskContains(node, child)) { continue; }
                let childCoordinate = coordinate * 4u +
                    axisIndex(child, vec3<u32>(4u));
                if (any(childCoordinate >= levelExtent(level - 1u))) {
                    errors |= 8u;
                    continue;
                }
                let childAddress = config.levelOffsets[level - 1u] +
                    linearIndex(childCoordinate, levelExtent(level - 1u));
                if (maskNodeIndices[childAddress] !=
                    childBase + maskCountBefore(node, child)) { errors |= 8u; }
            }
        }
    }
    result.visitedNodeCount = visited;
    if (visited != result.nodeCount ||
        visitedSum != result.nodeCount * (result.nodeCount - 1u) / 2u) {
        errors |= 1u;
    }
    result.validationErrors = errors;
    if (errors != 0u) {
        result.status = TERRAIN_TREE64_STATUS_INVALID;
        return;
    }

    result.semanticHashLow = 2166136261u;
    result.semanticHashHigh = 2166136261u ^ 0x9e3779b9u;
    hashMix(config.side);
    hashMix(result.nodeCount);
    for (var index = 0u; index < result.nodeCount; index += 1u) {
        hashMix(outputNodes[index].childBaseAndFlags);
        hashMix(outputNodes[index].populationMaskLo);
        hashMix(outputNodes[index].populationMaskHi);
    }
    result.descriptor.terrainSlot = config.terrainSlot;
    result.descriptor.terrainGeneration = config.expectedTerrainGeneration;
    result.descriptor.internalLOD = config.internalLOD;
    result.descriptor.flags = 1u;
    result.descriptor.nodePageSlot = config.nodePageSlot;
    result.descriptor.nodePageGeneration = config.nodePageGeneration;
    result.descriptor.rootNodeOffset = 0u;
    result.descriptor.nodeCount = result.nodeCount;
    result.descriptor.materialPageSlot = 0u;
    result.descriptor.materialPageGeneration = 0u;
    result.descriptor.materialCellOffset = 0u;
    result.descriptor.materialCellCount = 0u;
    result.descriptor.geometryRevisionLo = config.geometryRevisionLow;
    result.descriptor.geometryRevisionHi = config.geometryRevisionHigh;
    result.descriptor.modelSide = config.side;
    result.descriptor.modelCellSizeBits = config.voxelSizeBits;
    result.status = TERRAIN_TREE64_STATUS_READY;
}
#endif

#if TERRAIN_TREE64_SHADER_STAGE == 0
@compute @workgroup_size(1, 1, 1)
fn resetTerrainTree64(@builtin(global_invocation_id) gid : vec3<u32>) {
    if (gid.x != 0u) { return; }
    result.status = TERRAIN_TREE64_STATUS_BUILDING;
    result.overflowStage = 0u;
    result.validationErrors = 0u;
    result.side = config.side;
    result.nodeCount = 0u;
    result.levelCount = config.levelCount;
    // Validation derives occupancy from GPU-owned columns. Config carries the
    // CPU-oracle expectation only and is never the production source of truth.
    result.occupiedVoxelCount = 0u;
    result.terrainGeneration = config.expectedTerrainGeneration;
    result.semanticHashLow = 0u;
    result.semanticHashHigh = 0u;
    result.geometryRevisionLow = config.geometryRevisionLow;
    result.geometryRevisionHigh = config.geometryRevisionHigh;
    result.visitedNodeCount = 0u;
    result.maximumStack = 0u;
    result.descriptor = TerrainTree64Descriptor(
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u);
}

#elif TERRAIN_TREE64_SHADER_STAGE == 1
@compute @workgroup_size(64, 1, 1)
fn buildTerrainTree64Masks(@builtin(global_invocation_id) gid : vec3<u32>) {
    if (result.status != TERRAIN_TREE64_STATUS_BUILDING) {
        return;
    }
    if (stageUniform.stage == 1u) {
        buildTerminal(gid.x);
    } else if (stageUniform.stage == 2u) {
        buildUpper(stageUniform.level, gid.x);
    }
}

#elif TERRAIN_TREE64_SHADER_STAGE == 2
@compute @workgroup_size(1, 1, 1)
fn compactTerrainTree64(@builtin(global_invocation_id) gid : vec3<u32>) {
    if (gid.x == 0u) { compactTree(); }
}

#elif TERRAIN_TREE64_SHADER_STAGE == 3
@compute @workgroup_size(1, 1, 1)
fn validateTerrainTree64(@builtin(global_invocation_id) gid : vec3<u32>) {
    if (gid.x == 0u) { validateAndHash(); }
}
#endif
