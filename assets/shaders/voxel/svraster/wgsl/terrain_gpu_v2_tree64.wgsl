#include "sparse_voxel_brick_common.wgsl"
#include "sparse_voxel_tree64_common.wgsl"

#ifndef TERRAIN_V2_TREE64_WORKGROUP
#define TERRAIN_V2_TREE64_WORKGROUP 64
#endif

const STATUS_BUILDING : u32 = 1u;
const STATUS_READY : u32 = 2u;
const STATUS_OVERFLOW : u32 = 3u;
const STATUS_INVALID : u32 = 4u;
const STATUS_STALE_RASTER : u32 = 5u;
const STATUS_VALIDATION_FAILED : u32 = 6u;
const RASTER_STATUS_READY : u32 = 2u;
const RESULT_FLAGS : u32 = 15u;

struct TerrainGPUV2Tree64Config {
    modelSide : u32,
    levelCount : u32,
    candidateCount : u32,
    nodeCapacity : u32,
    voxelsX : u32,
    voxelsY : u32,
    voxelsZ : u32,
    rasterNodeCapacity : u32,
    chunkMinimumCanonicalX : i32,
    chunkMinimumCanonicalY : i32,
    chunkMinimumCanonicalZ : i32,
    canonicalStep : u32,
    expectedTerrainGeneration : u32,
    terrainSlot : u32,
    nodePageSlot : u32,
    nodePageGeneration : u32,
    internalLOD : u32,
    voxelSizeBits : u32,
    geometryRevisionLow : u32,
    geometryRevisionHigh : u32,
    flagBase : u32,
    offsetBase : u32,
    level0SumsBase : u32,
    level0OffsetsBase : u32,
    level1SumsBase : u32,
    level1OffsetsBase : u32,
    scratchWords : u32,
    generatorRevision : u32,
    levelOffset0 : u32,
    levelOffset1 : u32,
    levelOffset2 : u32,
    levelOffset3 : u32,
    levelOffset4 : u32,
    levelOffset5 : u32,
    levelOffset6 : u32,
    levelOffset7 : u32,
    levelCount0 : u32,
    levelCount1 : u32,
    levelCount2 : u32,
    levelCount3 : u32,
    levelCount4 : u32,
    levelCount5 : u32,
    levelCount6 : u32,
    levelCount7 : u32,
    reserved : vec4<u32>,
}

@group(0) @binding(0) var<storage, read> configBuffer : array<TerrainGPUV2Tree64Config>;
@group(0) @binding(1) var<storage, read> rasterNodes : array<SparseVoxelBrickNode>;
@group(0) @binding(2) var<storage, read> rasterOccupancy : array<SparseVoxelBrickOccupancy>;
@group(0) @binding(3) var<storage, read> rasterResult : array<u32>;
@group(0) @binding(4) var<storage, read_write> masks : array<atomic<u32>>;
@group(0) @binding(5) var<storage, read_write> scratch : array<u32>;
@group(0) @binding(6) var<storage, read_write> treeNodes : array<SparseVoxelTree64Node>;
@group(0) @binding(7) var<storage, read_write> result : array<atomic<u32>>;

var<workgroup> sharedValues : array<u32, 256>;
var<private> laneIndex : u32;

fn config() -> TerrainGPUV2Tree64Config { return configBuffer[laneIndex]; }
fn raster_node_base() -> u32 { return laneIndex * config().reserved.x; }
fn mask_base() -> u32 { return laneIndex * config().reserved.y * 2u; }
fn scratch_base() -> u32 { return laneIndex * config().reserved.z; }
fn node_base() -> u32 { return laneIndex * config().reserved.w; }
fn raster_result_base() -> u32 { return laneIndex * 16u; }
fn result_base() -> u32 { return laneIndex * 32u; }
fn resolvedChunkMinimumCanonicalY() -> i32 {
    let c = config();
    let finestVoxel = bitcast<f32>(c.voxelSizeBits) /
        f32(c.canonicalStep);
    return i32(round(bitcast<f32>(rasterResult[raster_result_base() + 15]) / finestVoxel));
}
fn levelOffset(level : u32) -> u32 {
    let c = config();
    switch level {
        case 0u: { return c.levelOffset0; }
        case 1u: { return c.levelOffset1; }
        case 2u: { return c.levelOffset2; }
        case 3u: { return c.levelOffset3; }
        case 4u: { return c.levelOffset4; }
        case 5u: { return c.levelOffset5; }
        case 6u: { return c.levelOffset6; }
        default: { return c.levelOffset7; }
    }
}
fn levelCandidateCount(level : u32) -> u32 {
    let c = config();
    switch level {
        case 0u: { return c.levelCount0; }
        case 1u: { return c.levelCount1; }
        case 2u: { return c.levelCount2; }
        case 3u: { return c.levelCount3; }
        case 4u: { return c.levelCount4; }
        case 5u: { return c.levelCount5; }
        case 6u: { return c.levelCount6; }
        default: { return c.levelCount7; }
    }
}
fn building() -> bool { return atomicLoad(&result[result_base() + 0]) == STATUS_BUILDING; }
fn maskLow(candidate : u32) -> u32 { return atomicLoad(&masks[mask_base() + candidate * 2u]); }
fn maskHigh(candidate : u32) -> u32 { return atomicLoad(&masks[mask_base() + candidate * 2u + 1u]); }
fn maskOccupied(candidate : u32) -> bool { return (maskLow(candidate) | maskHigh(candidate)) != 0u; }
fn rootCandidate() -> u32 { return levelOffset(config().levelCount - 1u); }

fn morton4(x0 : u32, y0 : u32, z0 : u32, axis0 : u32) -> u32 {
    var x = x0;
    var y = y0;
    var z = z0;
    var axis = axis0;
    var value = 0u;
    var shift = 0u;
    while (axis > 1u) {
        value |= (x & 3u) << shift;
        value |= (y & 3u) << (shift + 2u);
        value |= (z & 3u) << (shift + 4u);
        x >>= 2u;
        y >>= 2u;
        z >>= 2u;
        axis >>= 2u;
        shift += 6u;
    }
    return value;
}

@compute @workgroup_size(1, 1, 1)
fn resetTerrainGPUV2Tree64(@builtin(global_invocation_id) gid : vec3<u32>) {
    laneIndex = gid.z;
    let c = config();
    atomicStore(&result[result_base() + 0], STATUS_BUILDING);
    for (var index = 1u; index < 32u; index++) { atomicStore(&result[result_base() + index], 0u); }
    atomicStore(&result[result_base() + 3], RESULT_FLAGS);
    atomicStore(&result[result_base() + 4], c.candidateCount);
    atomicStore(&result[result_base() + 6], c.levelCount);
    atomicStore(&result[result_base() + 7], 0u);
    atomicStore(&result[result_base() + 9], rasterResult[raster_result_base() + 8]);
    atomicStore(&result[result_base() + 10], rasterResult[raster_result_base() + 9]);
    atomicStore(&result[result_base() + 11], c.generatorRevision);
    atomicStore(&result[result_base() + 15], TERRAIN_V2_TREE64_WORKGROUP);
    let valid = c.modelSide >= 4u && c.levelCount > 0u && c.levelCount <= 8u &&
        c.candidateCount > 0u && c.nodeCapacity > 0u && c.canonicalStep > 0u &&
        c.rasterNodeCapacity > 0u && rasterResult[raster_result_base() + 5] <= c.rasterNodeCapacity &&
        levelOffset(c.levelCount - 1u) == 0u;
    if (!valid) {
        atomicStore(&result[result_base() + 0], STATUS_INVALID);
    } else if (rasterResult[raster_result_base() + 0] != RASTER_STATUS_READY ||
               rasterResult[raster_result_base() + 9] != c.expectedTerrainGeneration) {
        atomicStore(&result[result_base() + 0], STATUS_STALE_RASTER);
    }
}

@compute @workgroup_size(TERRAIN_V2_TREE64_WORKGROUP, 1, 1)
fn clearTerrainGPUV2Tree64Masks(@builtin(global_invocation_id) gid : vec3<u32>) {
    laneIndex = gid.z;
    if (gid.x < config().candidateCount) {
        atomicStore(&masks[mask_base() + gid.x * 2u], 0u);
        atomicStore(&masks[mask_base() + gid.x * 2u + 1u], 0u);
    }
}

@compute @workgroup_size(TERRAIN_V2_TREE64_WORKGROUP, 1, 1)
fn emitTerrainGPUV2Tree64TerminalMasks(@builtin(global_invocation_id) gid : vec3<u32>) {
    laneIndex = gid.z;
    if (!building()) { return; }
    let c = config();
    let nodeIndex = gid.x / 16u;
    let word = gid.x & 15u;
    if (nodeIndex >= rasterResult[raster_result_base() + 5] || nodeIndex >= c.rasterNodeCapacity) { return; }
    let node = rasterNodes[raster_node_base() + nodeIndex];
    if (node.occupancyIndex >= c.rasterNodeCapacity || node.canonicalGridOriginScale.w != i32(c.canonicalStep)) {
        atomicOr(&result[result_base() + 2], 1u);
        return;
    }
    let delta = node.canonicalGridOriginScale.xyz - vec3<i32>(
        c.chunkMinimumCanonicalX, resolvedChunkMinimumCanonicalY(),
        c.chunkMinimumCanonicalZ);
    if (any(delta < vec3<i32>(0)) || any((delta % vec3<i32>(i32(c.canonicalStep))) != vec3<i32>(0))) {
        atomicOr(&result[result_base() + 2], 1u);
        return;
    }
    let base = vec3<u32>(delta / vec3<i32>(i32(c.canonicalStep)));
    var bits = rasterOccupancy[raster_node_base() + node.occupancyIndex].cells[word].bits;
    while (bits != 0u) {
        let bit = u32(firstTrailingBit(bits));
        bits &= bits - 1u;
        let cell = word * 32u + bit;
        let voxel = base + vec3<u32>(cell & 7u, (cell >> 3u) & 7u, cell >> 6u);
        if (any(voxel >= vec3<u32>(c.voxelsX, c.voxelsY, c.voxelsZ))) {
            atomicOr(&result[result_base() + 2], 1u);
            continue;
        }
        let candidate = levelOffset(0u) + morton4(voxel.x >> 2u,
            voxel.y >> 2u, voxel.z >> 2u, c.modelSide >> 2u);
        let child = (voxel.x & 3u) + 4u * (voxel.y & 3u) +
            16u * (voxel.z & 3u);
        if (child < 32u) {
            atomicOr(&masks[mask_base() + candidate * 2u], 1u << child);
        } else {
            atomicOr(&masks[mask_base() + candidate * 2u + 1u], 1u << (child - 32u));
        }
    }
}

fn buildUpperMask(index : u32, level : u32) {
    let c = config();
    if (!building() || level >= c.levelCount || index >= levelCandidateCount(level)) { return; }
    let first = levelOffset(level - 1u) + index * 64u;
    var low = 0u;
    var high = 0u;
    for (var child = 0u; child < 64u; child++) {
        if (maskOccupied(first + child)) {
            if (child < 32u) { low |= 1u << child; }
            else { high |= 1u << (child - 32u); }
        }
    }
    let destination = levelOffset(level) + index;
    atomicStore(&masks[mask_base() + destination * 2u], low);
    atomicStore(&masks[mask_base() + destination * 2u + 1u], high);
}

#define UPPER_ENTRY(LEVEL) \
@compute @workgroup_size(TERRAIN_V2_TREE64_WORKGROUP, 1, 1) \
fn buildTerrainGPUV2Tree64Upper##LEVEL(@builtin(global_invocation_id) gid : vec3<u32>) { laneIndex = gid.z; buildUpperMask(gid.x, LEVEL##u); }
UPPER_ENTRY(1)
UPPER_ENTRY(2)
UPPER_ENTRY(3)
UPPER_ENTRY(4)
UPPER_ENTRY(5)
UPPER_ENTRY(6)
UPPER_ENTRY(7)

@compute @workgroup_size(TERRAIN_V2_TREE64_WORKGROUP, 1, 1)
fn flagTerrainGPUV2Tree64Candidates(@builtin(global_invocation_id) gid : vec3<u32>) {
    laneIndex = gid.z;
    let c = config();
    if (!building() || gid.x >= c.candidateCount) { return; }
    scratch[scratch_base() + c.flagBase + gid.x] = select(0u, 1u,
        maskOccupied(gid.x) || gid.x == rootCandidate());
}

fn scanCount(level : u32) -> u32 {
    let count = config().candidateCount;
    if (level == 0u) { return count; }
    let level0 = (count + 255u) / 256u;
    if (level == 1u) { return level0; }
    return (level0 + 255u) / 256u;
}
fn scanInput(level : u32, index : u32) -> u32 {
    let c = config();
    if (level == 0u) { return scratch[scratch_base() + c.flagBase + index]; }
    if (level == 1u) { return scratch[scratch_base() + c.level0SumsBase + index]; }
    return scratch[scratch_base() + c.level1SumsBase + index];
}
fn scanStore(level : u32, index : u32, value : u32) {
    let c = config();
    if (level == 0u) { scratch[scratch_base() + c.offsetBase + index] = value; }
    else if (level == 1u) { scratch[scratch_base() + c.level0OffsetsBase + index] = value; }
    else { scratch[scratch_base() + c.level1OffsetsBase + index] = value; }
}
fn scanStoreSum(level : u32, index : u32, value : u32) {
    let c = config();
    if (level == 0u) { scratch[scratch_base() + c.level0SumsBase + index] = value; }
    else if (level == 1u) { scratch[scratch_base() + c.level1SumsBase + index] = value; }
}
fn scanBlock(local : u32, group : u32, level : u32) {
    let count = scanCount(level);
    let index = group * 256u + local;
    var inputValue = 0u;
    if (index < count) { inputValue = scanInput(level, index); }
    sharedValues[local] = inputValue;
    workgroupBarrier();
    var stride = 1u;
    while (stride < 256u) {
        let destination = (local + 1u) * stride * 2u - 1u;
        if (destination < 256u) { sharedValues[destination] += sharedValues[destination - stride]; }
        stride *= 2u;
        workgroupBarrier();
    }
    if (local == 255u) { scanStoreSum(level, group, sharedValues[255u]); sharedValues[255u] = 0u; }
    workgroupBarrier();
    stride = 128u;
    while (stride > 0u) {
        let destination = (local + 1u) * stride * 2u - 1u;
        if (destination < 256u) {
            let left = sharedValues[destination - stride];
            sharedValues[destination - stride] = sharedValues[destination];
            sharedValues[destination] += left;
        }
        stride /= 2u;
        workgroupBarrier();
    }
    if (index < count) { scanStore(level, index, sharedValues[local]); }
}

#define SCAN_ENTRY(LEVEL) \
@compute @workgroup_size(256, 1, 1) \
fn scanTerrainGPUV2Tree64Level##LEVEL(@builtin(local_invocation_id) lid : vec3<u32>, @builtin(workgroup_id) wid : vec3<u32>) { laneIndex = wid.z; scanBlock(lid.x, wid.x, LEVEL##u); }
SCAN_ENTRY(0)
SCAN_ENTRY(1)
SCAN_ENTRY(2)

@compute @workgroup_size(256, 1, 1)
fn addTerrainGPUV2Tree64Level1(@builtin(global_invocation_id) gid : vec3<u32>) {
    laneIndex = gid.z;
    let c = config();
    let count = (c.candidateCount + 255u) / 256u;
    if (building() && gid.x < count) {
        scratch[scratch_base() + c.level0OffsetsBase + gid.x] += scratch[scratch_base() + c.level1OffsetsBase + gid.x / 256u];
    }
}
@compute @workgroup_size(256, 1, 1)
fn addTerrainGPUV2Tree64Level0(@builtin(global_invocation_id) gid : vec3<u32>) {
    laneIndex = gid.z;
    let c = config();
    if (building() && gid.x < c.candidateCount) {
        scratch[scratch_base() + c.offsetBase + gid.x] += scratch[scratch_base() + c.level0OffsetsBase + gid.x / 256u];
    }
}

@compute @workgroup_size(1, 1, 1)
fn validateTerrainGPUV2Tree64Capacity(@builtin(global_invocation_id) gid : vec3<u32>) {
    laneIndex = gid.z;
    if (!building()) { return; }
    let c = config();
    if (atomicLoad(&result[result_base() + 2]) != 0u) {
        atomicStore(&result[result_base() + 0], STATUS_VALIDATION_FAILED);
        return;
    }
    let last = c.candidateCount - 1u;
    let count = scratch[scratch_base() + c.offsetBase + last] + scratch[scratch_base() + c.flagBase + last];
    atomicStore(&result[result_base() + 5], count);
    if (count > c.nodeCapacity) {
        atomicStore(&result[result_base() + 0], STATUS_OVERFLOW);
        atomicStore(&result[result_base() + 1], 2u);
    }
}

fn candidateLevel(candidate : u32) -> u32 {
    let c = config();
    for (var level = 0u; level < 8u; level++) {
        if (level < c.levelCount && candidate >= levelOffset(level) &&
            candidate < levelOffset(level) + levelCandidateCount(level)) { return level; }
    }
    return 0xffffffffu;
}

@compute @workgroup_size(TERRAIN_V2_TREE64_WORKGROUP, 1, 1)
fn emitTerrainGPUV2Tree64Nodes(@builtin(global_invocation_id) gid : vec3<u32>) {
    laneIndex = gid.z;
    let c = config();
    if (!building() || gid.x >= c.candidateCount || scratch[scratch_base() + c.flagBase + gid.x] == 0u) { return; }
    let destination = scratch[scratch_base() + c.offsetBase + gid.x];
    if (destination >= c.nodeCapacity) { return; }
    let level = candidateLevel(gid.x);
    treeNodes[node_base() + destination] = SparseVoxelTree64Node(
        select(0u, 1u, level == 0u), maskLow(gid.x), maskHigh(gid.x));
}

fn linkLevel(index : u32, level : u32) {
    let c = config();
    if (!building() || level >= c.levelCount || index >= levelCandidateCount(level)) { return; }
    let candidate = levelOffset(level) + index;
    if (!maskOccupied(candidate)) { return; }
    let destination = scratch[scratch_base() + c.offsetBase + candidate];
    let firstChild = levelOffset(level - 1u) + index * 64u;
    treeNodes[node_base() + destination].childBaseAndFlags = scratch[scratch_base() + c.offsetBase + firstChild] << 1u;
}

#define LINK_ENTRY(LEVEL) \
@compute @workgroup_size(TERRAIN_V2_TREE64_WORKGROUP, 1, 1) \
fn linkTerrainGPUV2Tree64Level##LEVEL(@builtin(global_invocation_id) gid : vec3<u32>) { laneIndex = gid.z; linkLevel(gid.x, LEVEL##u); }
LINK_ENTRY(1)
LINK_ENTRY(2)
LINK_ENTRY(3)
LINK_ENTRY(4)
LINK_ENTRY(5)
LINK_ENTRY(6)
LINK_ENTRY(7)

@compute @workgroup_size(TERRAIN_V2_TREE64_WORKGROUP, 1, 1)
fn validateTerrainGPUV2Tree64Nodes(@builtin(global_invocation_id) gid : vec3<u32>) {
    laneIndex = gid.z;
    let c = config();
    if (!building() || gid.x >= c.candidateCount || scratch[scratch_base() + c.flagBase + gid.x] == 0u) { return; }
    let destination = scratch[scratch_base() + c.offsetBase + gid.x];
    let node = treeNodes[node_base() + destination];
    let level = candidateLevel(gid.x);
    if (node.populationMaskLo != maskLow(gid.x) || node.populationMaskHi != maskHigh(gid.x) ||
        ((node.childBaseAndFlags & 1u) != 0u) != (level == 0u)) {
        atomicOr(&result[result_base() + 2], 1u);
    }
    let children = countOneBits(node.populationMaskLo) + countOneBits(node.populationMaskHi);
    atomicMax(&result[result_base() + 13], children);
    if (level == 0u) {
        atomicAdd(&result[result_base() + 8], children);
    } else if (children != 0u) {
        let local = gid.x - levelOffset(level);
        let first = levelOffset(level - 1u) + local * 64u;
        let expected = scratch[scratch_base() + c.offsetBase + first];
        let childBase = node.childBaseAndFlags >> 1u;
        if (childBase != expected || childBase + children > atomicLoad(&result[result_base() + 5])) {
            atomicOr(&result[result_base() + 2], 2u);
        } else {
            for (var child = 0u; child < children; child++) {
                let childNode = treeNodes[node_base() + childBase + child];
                if ((childNode.populationMaskLo | childNode.populationMaskHi) == 0u) {
                    atomicOr(&result[result_base() + 2], 4u);
                }
            }
        }
    }
    if (gid.x == rootCandidate() && destination != 0u) { atomicOr(&result[result_base() + 2], 8u); }
    atomicAdd(&result[result_base() + 12], 1u);
}

@compute @workgroup_size(1, 1, 1)
fn finalizeTerrainGPUV2Tree64(@builtin(global_invocation_id) gid : vec3<u32>) {
    laneIndex = gid.z;
    if (!building()) { return; }
    let c = config();
    if (atomicLoad(&result[result_base() + 8]) != rasterResult[raster_result_base() + 8]) { atomicOr(&result[result_base() + 2], 16u); }
    if (atomicLoad(&result[result_base() + 12]) != atomicLoad(&result[result_base() + 5])) { atomicOr(&result[result_base() + 2], 1u); }
    if (atomicLoad(&result[result_base() + 2]) != 0u) {
        atomicStore(&result[result_base() + 0], STATUS_VALIDATION_FAILED);
        return;
    }
    atomicStore(&result[result_base() + 16], c.terrainSlot);
    atomicStore(&result[result_base() + 17], c.expectedTerrainGeneration);
    atomicStore(&result[result_base() + 18], c.internalLOD);
    atomicStore(&result[result_base() + 19], 1u);
    atomicStore(&result[result_base() + 20], c.nodePageSlot);
    atomicStore(&result[result_base() + 21], c.nodePageGeneration);
    atomicStore(&result[result_base() + 22], 0u);
    atomicStore(&result[result_base() + 23], atomicLoad(&result[result_base() + 5]));
    atomicStore(&result[result_base() + 28], c.geometryRevisionLow);
    atomicStore(&result[result_base() + 29], c.geometryRevisionHigh);
    atomicStore(&result[result_base() + 30], c.modelSide);
    atomicStore(&result[result_base() + 31], c.voxelSizeBits);
    atomicStore(&result[result_base() + 0], STATUS_READY);
}
