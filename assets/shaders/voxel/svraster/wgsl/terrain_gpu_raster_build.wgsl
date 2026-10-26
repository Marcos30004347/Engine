#include "sparse_voxel_brick_common.wgsl"

const TERRAIN_RASTER_MAXIMUM_LEVELS : u32 = 12u;
const TERRAIN_RASTER_CELL_DATA_BASE : u32 = 16u;
const TERRAIN_RASTER_STATUS_BUILDING : u32 = 1u;
const TERRAIN_RASTER_STATUS_READY : u32 = 2u;
const TERRAIN_RASTER_STATUS_OVERFLOW : u32 = 3u;
const TERRAIN_RASTER_STATUS_INVALID : u32 = 5u;

struct TerrainRasterLevel {
    minimumX : i32,
    minimumY : i32,
    minimumZ : i32,
    candidateBase : u32,
    sizeX : u32,
    sizeY : u32,
    sizeZ : u32,
    reserved : u32,
}

struct TerrainRasterConfig {
    side : u32,
    maximumVerticalVoxels : u32,
    maximumLevelCount : u32,
    columnCount : u32,
    nodeCapacity : u32,
    childCapacity : u32,
    materialCapacity : u32,
    rootCapacity : u32,
    denseCandidateCapacity : u32,
    materialWordCapacity : u32,
    paletteCount : u32,
    expectedTerrainGeneration : u32,
    buildRevisionLow : u32,
    buildRevisionHigh : u32,
    voxelSizeBits : u32,
    edgeBits : u32,
    levels : array<TerrainRasterLevel, TERRAIN_RASTER_MAXIMUM_LEVELS>,
}

struct TerrainRasterColumn {
    topY : u32,
    firstSolidY : u32,
    packedMaterialContext : u32,
    packedFlags : u32,
    firstSolidYWithoutBoundaryStitch : u32,
    lowestNeighbourY : u32,
    packedContexts : u32,
    reserved : u32,
}

struct TerrainRasterMaterial {
    paletteEntry : u32,
    packed0 : u32,
    packed1 : u32,
    reserved : u32,
}

struct TerrainRasterCandidate {
    occupiedCount : u32,
    nodeIndex : u32,
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

struct TerrainRasterStage {
    stage : u32,
    level : u32,
    reserved0 : u32,
    reserved1 : u32,
}

@group(0) @binding(0) var<storage, read> config : TerrainRasterConfig;
@group(0) @binding(1) var<storage, read> columns : array<TerrainRasterColumn>;
@group(0) @binding(2) var<storage, read> materialTable : array<TerrainRasterMaterial>;
@group(0) @binding(3) var<storage, read_write> nodes : array<SparseVoxelBrickNode>;
@group(0) @binding(4) var<storage, read_write> occupancies : array<SparseVoxelBrickOccupancy>;
@group(0) @binding(5) var<storage, read_write> materialWords : array<u32>;
@group(0) @binding(6) var<storage, read_write> microSurfaceData : array<vec2<u32>>;
@group(0) @binding(7) var<storage, read_write> childIndices : array<u32>;
@group(0) @binding(8) var<storage, read_write> rootIndices : array<u32>;
@group(0) @binding(9) var<storage, read_write> candidates : array<TerrainRasterCandidate>;
@group(0) @binding(10) var<storage, read_write> scratch : array<u32>;
@group(0) @binding(11) var<storage, read_write> result : TerrainRasterResult;
// Word 14 of the Phase-7 result is the resolved page elevation origin.
@group(0) @binding(12) var<storage, read> columnResult : array<u32>;
@group(0) @binding(13) var<uniform> stageUniform : TerrainRasterStage;

fn floorDivide(value : i32, divisor : i32) -> i32 {
    var quotient = value / divisor;
    if (value % divisor < 0) {
        quotient -= 1;
    }
    return quotient;
}

fn positiveModulo(value : i32, divisor : i32) -> u32 {
    var remainder = value % divisor;
    if (remainder < 0) {
        remainder += divisor;
    }
    return u32(remainder);
}

fn candidateCount(level : TerrainRasterLevel) -> u32 {
    return level.sizeX * level.sizeY * level.sizeZ;
}

fn candidateKey(level : TerrainRasterLevel, local : u32) -> vec3<i32> {
    let yz = level.sizeY * level.sizeZ;
    let x = local / yz;
    let remainder = local - x * yz;
    let y = remainder / level.sizeZ;
    let z = remainder - y * level.sizeZ;
    return vec3<i32>(level.minimumX + i32(x),
        level.minimumY + i32(y), level.minimumZ + i32(z));
}

fn candidateAddress(levelIndex : u32, key : vec3<i32>) -> u32 {
    let level = config.levels[levelIndex];
    let local = key - vec3<i32>(level.minimumX, level.minimumY, level.minimumZ);
    if (any(local < vec3<i32>(0)) ||
        local.x >= i32(level.sizeX) || local.y >= i32(level.sizeY) ||
        local.z >= i32(level.sizeZ)) {
        return SPARSE_VOXEL_BRICK_INVALID_INDEX;
    }
    return level.candidateBase +
        (u32(local.x) * level.sizeY + u32(local.y)) * level.sizeZ +
        u32(local.z);
}

fn levelCell(levelIndex : u32, gridCell : vec3<i32>) -> u32 {
    let key = vec3<i32>(floorDivide(gridCell.x, 8),
        floorDivide(gridCell.y, 8), floorDivide(gridCell.z, 8));
    let address = candidateAddress(levelIndex, key);
    if (address == SPARSE_VOXEL_BRICK_INVALID_INDEX ||
        candidates[address].nodeIndex == SPARSE_VOXEL_BRICK_INVALID_INDEX) {
        return 0u;
    }
    let local = vec3<u32>(positiveModulo(gridCell.x, 8),
        positiveModulo(gridCell.y, 8), positiveModulo(gridCell.z, 8));
    let cell = local.x + 8u * (local.y + 8u * local.z);
    return scratch[TERRAIN_RASTER_CELL_DATA_BASE +
        candidates[address].nodeIndex * 512u + cell];
}

fn contextRank(context : u32) -> u32 {
    if (context >= 10u && context <= 13u) {
        return 4u;
    }
    if ((context & 0x80u) != 0u) {
        return 3u;
    }
    return select(0u, 1u, context == 1u);
}

fn fineCell(key : vec3<i32>, cell : u32) -> u32 {
    let x = cell & 7u;
    let y = (cell >> 3u) & 7u;
    let z = cell >> 6u;
    let grid = key * 8 + vec3<i32>(i32(x), i32(y), i32(z));
    let halfSide = i32(config.side / 2u);
    let columnX = grid.x + halfSide;
    let columnZ = grid.z + halfSide;
    if (columnX < 0 || columnZ < 0 || columnX >= i32(config.side) ||
        columnZ >= i32(config.side) || grid.y < 0 ||
        grid.y >= i32(config.maximumVerticalVoxels)) {
        return 0u;
    }
    let column = columns[u32(columnZ) * config.side + u32(columnX)];
    let voxelY = u32(grid.y);
    if (voxelY < column.firstSolidY || voxelY > column.topY) {
        return 0u;
    }
    let materialId = column.packedMaterialContext & 0xffu;
    let paletteEntry = materialTable[materialId].paletteEntry;
    if (paletteEntry == SPARSE_VOXEL_BRICK_INVALID_INDEX ||
        paletteEntry >= config.paletteCount) {
        return 0u;
    }
    var context = (column.packedMaterialContext >> 8u) & 0xffu;
    if (voxelY != column.topY) {
        context = 0u;
        if (voxelY < column.firstSolidYWithoutBoundaryStitch) {
            context = (column.packedContexts >> 8u) & 0xffu;
        }
        if (column.lowestNeighbourY < voxelY) {
            context = column.packedContexts & 0xffu;
        }
    }
    return 0x80000000u | (context << 8u) | paletteEntry;
}

fn parentCell(levelIndex : u32, key : vec3<i32>, cell : u32) -> u32 {
    let x = cell & 7u;
    let y = (cell >> 3u) & 7u;
    let z = cell >> 6u;
    let base = key * 16 + vec3<i32>(i32(x * 2u), i32(y * 2u), i32(z * 2u));
    var selected = 0u;
    var selectedRank = 0u;
    for (var dz = 0u; dz < 2u; dz += 1u) {
        for (var dy = 0u; dy < 2u; dy += 1u) {
            for (var dx = 0u; dx < 2u; dx += 1u) {
                let value = levelCell(levelIndex - 1u,
                    base + vec3<i32>(i32(dx), i32(dy), i32(dz)));
                if ((value & 0x80000000u) == 0u) {
                    continue;
                }
                let rank = contextRank((value >> 8u) & 0xffu);
                if ((selected & 0x80000000u) == 0u || rank > selectedRank) {
                    selected = value;
                    selectedRank = rank;
                }
            }
        }
    }
    return selected;
}

fn sourceCell(levelIndex : u32, key : vec3<i32>, cell : u32) -> u32 {
    if (levelIndex == 0u) {
        return fineCell(key, cell);
    }
    return parentCell(levelIndex, key, cell);
}

fn failOverflow(stage : u32) {
    result.status = TERRAIN_RASTER_STATUS_OVERFLOW;
    result.overflowStage = stage;
}

fn writeMaterial(address : u32, value : u32) {
    let word = address >> 2u;
    let shift = (address & 3u) * 8u;
    materialWords[word] = (materialWords[word] & ~(0xffu << shift)) |
        ((value & 0xffu) << shift);
}

fn readMaterial(address : u32) -> u32 {
    return (materialWords[address >> 2u] >> ((address & 3u) * 8u)) & 0xffu;
}

fn emitNode(levelIndex : u32, key : vec3<i32>, nodeIndex : u32,
    occupiedCount : u32) {
    let scale = 1u << levelIndex;
    let voxelSize = bitcast<f32>(config.voxelSizeBits) * f32(scale);
    let materialBase = result.materialCount;
    var occupancy : SparseVoxelBrickOccupancy;
    var prefix = 0u;
    var minimum = vec3<u32>(7u);
    var maximum = vec3<u32>(0u);
    var coarseLow = 0u;
    var coarseHigh = 0u;
    for (var word = 0u; word < 16u; word += 1u) {
        var bits = 0u;
        for (var bit = 0u; bit < 32u; bit += 1u) {
            let cell = word * 32u + bit;
            let data = sourceCell(levelIndex, key, cell);
            scratch[TERRAIN_RASTER_CELL_DATA_BASE + nodeIndex * 512u + cell] = data;
            if ((data & 0x80000000u) == 0u) {
                continue;
            }
            bits |= 1u << bit;
            let x = cell & 7u;
            let y = (cell >> 3u) & 7u;
            let z = cell >> 6u;
            minimum = min(minimum, vec3<u32>(x, y, z));
            maximum = max(maximum, vec3<u32>(x, y, z));
            let coarse = (x >> 1u) + 4u * ((y >> 1u) + 4u * (z >> 1u));
            if (coarse < 32u) {
                coarseLow |= 1u << coarse;
            } else {
                coarseHigh |= 1u << (coarse - 32u);
            }
            let address = materialBase + prefix + countOneBits(bits & ((1u << bit) - 1u));
            writeMaterial(address, data);
            microSurfaceData[address] = vec2<u32>(0u, ((data >> 8u) & 0xffu) << 24u);
        }
        occupancy.cells[word] = SparseVoxelBrickOccupancyCell(bits, prefix);
        prefix += countOneBits(bits);
    }
    occupancies[nodeIndex] = occupancy;

    var node : SparseVoxelBrickNode;
    node.minimumVoxelSize = vec4<f32>(vec3<f32>(key * 8) * voxelSize, voxelSize);
    node.packedOccupiedBounds = minimum.x | (minimum.y << 3u) |
        (minimum.z << 6u) | (maximum.x << 9u) |
        (maximum.y << 12u) | (maximum.z << 15u);
    node.coarseOccupancyLow = coarseLow;
    node.coarseOccupancyHigh = coarseHigh;
    node.occupancyIndex = nodeIndex;
    node.materialBase = materialBase;
    node.parentIndex = SPARSE_VOXEL_BRICK_INVALID_INDEX;
    node.childCount = 0u;
    node.level = levelIndex;
    node.occupiedCount = occupiedCount;
    node.firstChild = result.childCount;
    node.instanceIndex = SPARSE_VOXEL_BRICK_INVALID_INDEX;
    node.paletteBase = 0u;
    node.regionMappingId = 0u;
    node.selfError = voxelSize * 0.25;
    node.parentError = bitcast<f32>(0x7f7fffffu);
    node.reservedTailPadding = 0u;
    node.canonicalGridOriginScale = vec4<i32>(key * i32(8u * scale), i32(scale));

    if (levelIndex > 0u) {
        var childMaximumError = 0.0;
        var childCount = 0u;
        for (var octant = 0u; octant < 8u; octant += 1u) {
            let childKey = key * 2 + vec3<i32>(i32(octant & 1u),
                i32((octant >> 1u) & 1u), i32((octant >> 2u) & 1u));
            let childAddress = candidateAddress(levelIndex - 1u, childKey);
            if (childAddress == SPARSE_VOXEL_BRICK_INVALID_INDEX ||
                candidates[childAddress].nodeIndex == SPARSE_VOXEL_BRICK_INVALID_INDEX) {
                continue;
            }
            let childNode = candidates[childAddress].nodeIndex;
            childIndices[result.childCount + childCount] = childNode;
            childMaximumError = max(childMaximumError, nodes[childNode].selfError);
            childCount += 1u;
        }
        var worstDeviation = childMaximumError;
        let finerVoxelSize = voxelSize * 0.5;
        for (var cell = 0u; cell < 512u; cell += 1u) {
            let data = scratch[TERRAIN_RASTER_CELL_DATA_BASE + nodeIndex * 512u + cell];
            if ((data & 0x80000000u) == 0u) {
                continue;
            }
            let x = cell & 7u;
            let y = (cell >> 3u) & 7u;
            let z = cell >> 6u;
            let base = key * 16 + vec3<i32>(i32(x * 2u), i32(y * 2u), i32(z * 2u));
            var lowest = 3u;
            var highest = 0u;
            for (var dz = 0u; dz < 2u; dz += 1u) {
                for (var dx = 0u; dx < 2u; dx += 1u) {
                    var solid = 0u;
                    for (var dy = 0u; dy < 2u; dy += 1u) {
                        let child = levelCell(levelIndex - 1u,
                            base + vec3<i32>(i32(dx), i32(dy), i32(dz)));
                        solid += select(0u, 1u, (child & 0x80000000u) != 0u);
                    }
                    lowest = min(lowest, solid);
                    highest = max(highest, solid);
                }
            }
            worstDeviation = max(worstDeviation, f32(highest - lowest) * finerVoxelSize);
        }
        node.selfError = max(worstDeviation, voxelSize * 0.25);
        node.childCount = childCount;
        for (var child = 0u; child < childCount; child += 1u) {
            let childNode = childIndices[result.childCount + child];
            nodes[childNode].parentIndex = nodeIndex;
            nodes[childNode].parentError = node.selfError;
        }
        result.childCount += childCount;
    }
    nodes[nodeIndex] = node;
    result.materialCount += occupiedCount;
    // Tree64 represents the finest solid voxel set. Coarser brick nodes are
    // acceleration replicas of those voxels, not additional occupancy. The
    // old total counted every hierarchy level and made a valid multi-level
    // raster fail the joint raster/Tree64 occupancy check.
    if (levelIndex == 0u) {
        result.occupiedVoxelCount += occupiedCount;
    }
}

fn buildLevel(levelIndex : u32) {
    if (result.status != TERRAIN_RASTER_STATUS_BUILDING ||
        levelIndex >= config.maximumLevelCount ||
        levelIndex != result.levelCount) {
        return;
    }
    if (levelIndex > 0u && scratch[(levelIndex - 1u) * 2u] <= 1u) {
        return;
    }
    let level = config.levels[levelIndex];
    let count = candidateCount(level);
    var emitted = 0u;
    var materialNeeded = 0u;
    var childNeeded = 0u;
    for (var local = 0u; local < count; local += 1u) {
        let key = candidateKey(level, local);
        var occupied = 0u;
        for (var cell = 0u; cell < 512u; cell += 1u) {
            let data = sourceCell(levelIndex, key, cell);
            occupied += select(0u, 1u, (data & 0x80000000u) != 0u);
        }
        let address = level.candidateBase + local;
        candidates[address].occupiedCount = occupied;
        candidates[address].nodeIndex = SPARSE_VOXEL_BRICK_INVALID_INDEX;
        if (occupied != 0u) {
            candidates[address].nodeIndex = result.nodeCount + emitted;
            emitted += 1u;
            materialNeeded += occupied;
            if (levelIndex > 0u) {
                for (var octant = 0u; octant < 8u; octant += 1u) {
                    let childKey = key * 2 + vec3<i32>(i32(octant & 1u),
                        i32((octant >> 1u) & 1u), i32((octant >> 2u) & 1u));
                    let childAddress = candidateAddress(levelIndex - 1u, childKey);
                    if (childAddress != SPARSE_VOXEL_BRICK_INVALID_INDEX &&
                        candidates[childAddress].nodeIndex != SPARSE_VOXEL_BRICK_INVALID_INDEX) {
                        childNeeded += 1u;
                    }
                }
            }
        }
    }
    // The CPU oracle constructs a tentative coarser map and retains it only
    // when sparse compaction actually shrinks. Signed pages that straddle zero
    // can keep the same dense bounds forever, so this data-dependent stop is
    // required instead of relying on geometric candidate dimensions.
    if (levelIndex > 0u && emitted >= scratch[(levelIndex - 1u) * 2u]) {
        for (var local = 0u; local < count; local += 1u) {
            candidates[level.candidateBase + local].nodeIndex =
                SPARSE_VOXEL_BRICK_INVALID_INDEX;
        }
        return;
    }
    if (result.nodeCount + emitted > config.nodeCapacity) {
        failOverflow(2u);
        return;
    }
    if (result.childCount + childNeeded > config.childCapacity) {
        failOverflow(3u);
        return;
    }
    if (result.materialCount + materialNeeded > config.materialCapacity) {
        failOverflow(4u);
        return;
    }
    scratch[levelIndex * 2u] = emitted;
    scratch[levelIndex * 2u + 1u] = result.nodeCount;
    for (var local = 0u; local < count; local += 1u) {
        let address = level.candidateBase + local;
        if (candidates[address].nodeIndex == SPARSE_VOXEL_BRICK_INVALID_INDEX) {
            continue;
        }
        emitNode(levelIndex, candidateKey(level, local),
            candidates[address].nodeIndex, candidates[address].occupiedCount);
    }
    result.nodeCount += emitted;
    result.levelCount = levelIndex + 1u;
}

fn hashMixLow(hash : u32, value : u32) -> u32 {
    return (hash ^ value) * 16777619u;
}

fn hashMix(value : u32) {
    result.semanticHashLow = hashMixLow(result.semanticHashLow, value);
    result.semanticHashHigh = hashMixLow(result.semanticHashHigh,
        value ^ result.semanticHashLow ^ 0x9e3779b9u);
}

fn nodeOccupied(nodeIndex : u32, cell : u32) -> bool {
    return (occupancies[nodes[nodeIndex].occupancyIndex].cells[cell >> 5u].bits &
        (1u << (cell & 31u))) != 0u;
}

fn validateAndHash() {
    if (result.status != TERRAIN_RASTER_STATUS_BUILDING || result.levelCount == 0u) {
        return;
    }
    let rootLevel = result.levelCount - 1u;
    let rootCount = scratch[rootLevel * 2u];
    let rootBase = scratch[rootLevel * 2u + 1u];
    if (rootCount == 0u || rootCount > config.rootCapacity) {
        failOverflow(5u);
        return;
    }
    result.rootCount = rootCount;
    for (var root = 0u; root < rootCount; root += 1u) {
        rootIndices[root] = rootBase + root;
    }

    var errors = 0u;
    for (var index = 0u; index < result.nodeCount; index += 1u) {
        let node = nodes[index];
        var prefix = 0u;
        for (var word = 0u; word < 16u; word += 1u) {
            let occupancy = occupancies[node.occupancyIndex].cells[word];
            if (occupancy.prefix != prefix) {
                errors |= 1u;
            }
            prefix += countOneBits(occupancy.bits);
        }
        if (prefix != node.occupiedCount ||
            node.materialBase + node.occupiedCount > result.materialCount ||
            node.firstChild + node.childCount > result.childCount) {
            errors |= 1u;
        }
        if (node.level == rootLevel) {
            if (node.parentIndex != SPARSE_VOXEL_BRICK_INVALID_INDEX) {
                errors |= 4u;
            }
        } else if (node.parentIndex == SPARSE_VOXEL_BRICK_INVALID_INDEX ||
            node.parentIndex >= result.nodeCount) {
            errors |= 4u;
        }
        for (var child = 0u; child < node.childCount; child += 1u) {
            let childIndex = childIndices[node.firstChild + child];
            if (childIndex >= result.nodeCount ||
                nodes[childIndex].parentIndex != index ||
                nodes[childIndex].level + 1u != node.level ||
                nodes[childIndex].parentError != node.selfError ||
                nodes[childIndex].selfError > node.selfError) {
                errors |= 2u;
                continue;
            }
            let childNode = nodes[childIndex];
            for (var cell = 0u; cell < 512u; cell += 1u) {
                if (!nodeOccupied(childIndex, cell)) {
                    continue;
                }
                let local = vec3<i32>(i32(cell & 7u),
                    i32((cell >> 3u) & 7u), i32(cell >> 6u));
                let finest = childNode.canonicalGridOriginScale.xyz +
                    local * childNode.canonicalGridOriginScale.w;
                let parentLocal = (finest - node.canonicalGridOriginScale.xyz) /
                    node.canonicalGridOriginScale.w;
                if (any(parentLocal < vec3<i32>(0)) ||
                    any(parentLocal >= vec3<i32>(8)) ||
                    !nodeOccupied(index, u32(parentLocal.x) +
                        8u * (u32(parentLocal.y) + 8u * u32(parentLocal.z)))) {
                    errors |= 8u;
                }
            }
        }
    }
    result.validationErrors = errors;
    if (errors != 0u) {
        result.status = TERRAIN_RASTER_STATUS_INVALID;
        return;
    }

    result.semanticHashLow = 2166136261u;
    result.semanticHashHigh = 2166136261u ^ 0x9e3779b9u;
    hashMix(result.nodeCount);
    hashMix(result.childCount);
    hashMix(result.materialCount);
    hashMix(result.rootCount);
    for (var index = 0u; index < result.nodeCount; index += 1u) {
        let node = nodes[index];
        hashMix(bitcast<u32>(node.minimumVoxelSize.x));
        hashMix(bitcast<u32>(node.minimumVoxelSize.y));
        hashMix(bitcast<u32>(node.minimumVoxelSize.z));
        hashMix(bitcast<u32>(node.minimumVoxelSize.w));
        hashMix(node.packedOccupiedBounds);
        hashMix(node.coarseOccupancyLow);
        hashMix(node.coarseOccupancyHigh);
        hashMix(node.occupancyIndex);
        hashMix(node.materialBase);
        hashMix(node.parentIndex);
        hashMix(node.childCount);
        hashMix(node.level);
        hashMix(node.occupiedCount);
        hashMix(node.firstChild);
        hashMix(node.instanceIndex);
        hashMix(node.paletteBase);
        hashMix(node.regionMappingId);
        hashMix(bitcast<u32>(node.selfError));
        hashMix(bitcast<u32>(node.parentError));
        hashMix(bitcast<u32>(node.canonicalGridOriginScale.x));
        hashMix(bitcast<u32>(node.canonicalGridOriginScale.y));
        hashMix(bitcast<u32>(node.canonicalGridOriginScale.z));
        hashMix(bitcast<u32>(node.canonicalGridOriginScale.w));
    }
    for (var index = 0u; index < result.nodeCount; index += 1u) {
        for (var word = 0u; word < 16u; word += 1u) {
            let occupancy = occupancies[index].cells[word];
            hashMix(occupancy.bits);
            hashMix(occupancy.prefix);
        }
    }
    for (var address = 0u; address < result.materialCount; address += 1u) {
        hashMix(readMaterial(address));
    }
    for (var address = 0u; address < result.materialCount; address += 1u) {
        hashMix(microSurfaceData[address].x);
        hashMix(microSurfaceData[address].y);
    }
    for (var index = 0u; index < result.childCount; index += 1u) {
        hashMix(childIndices[index]);
    }
    for (var index = 0u; index < result.rootCount; index += 1u) {
        hashMix(rootIndices[index]);
    }
    result.status = TERRAIN_RASTER_STATUS_READY;
}

@compute @workgroup_size(1, 1, 1)
fn runTerrainRasterStage(@builtin(global_invocation_id) gid : vec3<u32>) {
    if (any(gid != vec3<u32>(0u))) {
        return;
    }
    if (stageUniform.stage == 0u) {
        result.status = TERRAIN_RASTER_STATUS_BUILDING;
        result.overflowStage = 0u;
        result.validationErrors = 0u;
        result.levelCount = 0u;
        result.nodeCount = 0u;
        result.childCount = 0u;
        result.materialCount = 0u;
        result.rootCount = 0u;
        result.occupiedVoxelCount = 0u;
        result.terrainGeneration = config.expectedTerrainGeneration;
        result.semanticHashLow = 0u;
        result.semanticHashHigh = 0u;
        result.buildRevisionLow = config.buildRevisionLow;
        result.buildRevisionHigh = config.buildRevisionHigh;
        result.reserved0 = columnResult[14u];
        result.reserved1 = 0u;
        for (var index = 0u; index < config.denseCandidateCapacity; index += 1u) {
            candidates[index] = TerrainRasterCandidate(0u,
                SPARSE_VOXEL_BRICK_INVALID_INDEX);
        }
        for (var index = 0u; index < TERRAIN_RASTER_CELL_DATA_BASE; index += 1u) {
            scratch[index] = 0u;
        }
        for (var index = 0u; index < config.materialWordCapacity; index += 1u) {
            materialWords[index] = 0u;
        }
        return;
    }
    if (stageUniform.stage == 1u) {
        buildLevel(stageUniform.level);
        return;
    }
    if (stageUniform.stage == 2u) {
        validateAndHash();
    }
}
