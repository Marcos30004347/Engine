#include "sparse_voxel_brick_common.wgsl"

#ifndef TERRAIN_V2_RASTER_HIERARCHY_WORKGROUP
#define TERRAIN_V2_RASTER_HIERARCHY_WORKGROUP 64
#endif

const STATUS_BUILDING : u32 = 1u;
const STATUS_READY : u32 = 2u;
const STATUS_OVERFLOW : u32 = 3u;
const STATUS_INVALID : u32 = 4u;
const STATUS_STALE_RASTER : u32 = 5u;
const STATUS_VALIDATION_FAILED : u32 = 6u;
const RASTER_STATUS_READY : u32 = 2u;
const INVALID_INDEX : u32 = 0xffffffffu;
const FORCE_LEAF_RASTER : u32 = 1u << 16u;

struct Config {
    bricksX : u32, bricksY : u32, bricksZ : u32, brickSide : u32,
    levelCount : u32, totalDenseCandidates : u32,
    maximumParentCandidates : u32, expectedTerrainGeneration : u32,
    leafNodeCapacity : u32, leafMaterialCapacity : u32,
    leafMaterialWordCapacity : u32, nodeCapacity : u32,
    materialCapacity : u32, materialWordCapacity : u32,
    childCapacity : u32, rootCapacity : u32,
    canonicalStep : u32,
    chunkMinimumCanonicalX : i32, chunkMinimumCanonicalY : i32,
    chunkMinimumCanonicalZ : i32,
    chunkMinimumWorldX : f32, chunkMinimumWorldY : f32,
    chunkMinimumWorldZ : f32, voxelSizeMetres : f32,
    regionMappingId : u32, paletteBase : u32, generatorRevision : u32,
    selectedMode : u32,
    summaryBase : u32, countBase : u32, offsetBase : u32,
    level0SumsBase : u32, level0OffsetsBase : u32,
    level1SumsBase : u32, level1OffsetsBase : u32,
    mappingBase : u32, levelStatsBase : u32, scratchWords : u32,
    flags : u32, reserved : u32,
    levelDenseOffset0 : u32, levelDenseOffset1 : u32,
    levelDenseOffset2 : u32, levelDenseOffset3 : u32,
    levelDenseOffset4 : u32, levelDenseOffset5 : u32,
    levelDenseOffset6 : u32, levelDenseOffset7 : u32,
    levelDenseCount0 : u32, levelDenseCount1 : u32,
    levelDenseCount2 : u32, levelDenseCount3 : u32,
    levelDenseCount4 : u32, levelDenseCount5 : u32,
    levelDenseCount6 : u32, levelDenseCount7 : u32,
    levelAxesX0 : u32, levelAxesX1 : u32, levelAxesX2 : u32, levelAxesX3 : u32,
    levelAxesX4 : u32, levelAxesX5 : u32, levelAxesX6 : u32, levelAxesX7 : u32,
    levelAxesY0 : u32, levelAxesY1 : u32, levelAxesY2 : u32, levelAxesY3 : u32,
    levelAxesY4 : u32, levelAxesY5 : u32, levelAxesY6 : u32, levelAxesY7 : u32,
    levelAxesZ0 : u32, levelAxesZ1 : u32, levelAxesZ2 : u32, levelAxesZ3 : u32,
    levelAxesZ4 : u32, levelAxesZ5 : u32, levelAxesZ6 : u32, levelAxesZ7 : u32,
}

struct HeightSample {
    heightUnits : i32,
    canonicalX : i32,
    canonicalZ : i32,
    flags : u32,
}

@group(0) @binding(0) var<storage, read> configBuffer : array<Config>;
@group(0) @binding(1) var<storage, read> leafNodes : array<SparseVoxelBrickNode>;
@group(0) @binding(2) var<storage, read> leafOccupancy : array<SparseVoxelBrickOccupancy>;
@group(0) @binding(3) var<storage, read> leafMaterials : array<u32>;
@group(0) @binding(4) var<storage, read> rasterResult : array<u32>;
@group(0) @binding(5) var<storage, read_write> scratch : array<u32>;
@group(0) @binding(6) var<storage, read_write> nodes : array<SparseVoxelBrickNode>;
@group(0) @binding(7) var<storage, read_write> occupancies : array<SparseVoxelBrickOccupancy>;
@group(0) @binding(8) var<storage, read_write> materialWords : array<atomic<u32>>;
@group(0) @binding(9) var<storage, read_write> childIndices : array<u32>;
@group(0) @binding(10) var<storage, read_write> rootIndices : array<u32>;
@group(0) @binding(11) var<storage, read_write> result : array<atomic<u32>>;
@group(0) @binding(12) var<storage, read> heightSamples : array<HeightSample>;

var<workgroup> sharedValues : array<vec4<u32>, 256>;
var<private> activeLane : u32;

fn config() -> Config {
    var c = configBuffer[activeLane];
    let base = activeLane * c.scratchWords;
    c.summaryBase += base; c.countBase += base; c.offsetBase += base;
    c.level0SumsBase += base; c.level0OffsetsBase += base;
    c.level1SumsBase += base; c.level1OffsetsBase += base;
    c.mappingBase += base; c.levelStatsBase += base;
    return c;
}
fn resultBase() -> u32 { return activeLane * 16u; }
fn rasterBase() -> u32 { return activeLane * 16u; }
fn leafNodeBase() -> u32 { return activeLane * configBuffer[activeLane].leafNodeCapacity; }
fn leafMaterialBase() -> u32 { return activeLane * configBuffer[activeLane].leafMaterialWordCapacity; }
fn nodeBase() -> u32 { return activeLane * configBuffer[activeLane].nodeCapacity; }
fn materialBase() -> u32 { return activeLane * configBuffer[activeLane].materialWordCapacity; }
fn childBase() -> u32 { return activeLane * configBuffer[activeLane].childCapacity; }
fn rootBase() -> u32 { return activeLane * configBuffer[activeLane].rootCapacity; }
fn rasterValue(index : u32) -> u32 { return rasterResult[rasterBase() + index]; }
fn building() -> bool { return atomicLoad(&result[resultBase()]) == STATUS_BUILDING; }
fn resolvedChunkMinimumWorldY() -> f32 {
    return bitcast<f32>(rasterValue(15u));
}
fn resolvedChunkMinimumCanonicalY() -> i32 {
    let c = config();
    let finestVoxel = c.voxelSizeMetres / f32(c.canonicalStep);
    return i32(round(resolvedChunkMinimumWorldY() / finestVoxel));
}
fn levelOffset(level : u32) -> u32 {
    let c = config();
    switch level {
        case 0u: { return c.levelDenseOffset0; } case 1u: { return c.levelDenseOffset1; }
        case 2u: { return c.levelDenseOffset2; } case 3u: { return c.levelDenseOffset3; }
        case 4u: { return c.levelDenseOffset4; } case 5u: { return c.levelDenseOffset5; }
        case 6u: { return c.levelDenseOffset6; } default: { return c.levelDenseOffset7; }
    }
}
fn levelCount(level : u32) -> u32 {
    let c = config();
    switch level {
        case 0u: { return c.levelDenseCount0; } case 1u: { return c.levelDenseCount1; }
        case 2u: { return c.levelDenseCount2; } case 3u: { return c.levelDenseCount3; }
        case 4u: { return c.levelDenseCount4; } case 5u: { return c.levelDenseCount5; }
        case 6u: { return c.levelDenseCount6; } default: { return c.levelDenseCount7; }
    }
}
fn axesX(level : u32) -> u32 {
    let c = config();
    switch level {
        case 0u: { return c.levelAxesX0; } case 1u: { return c.levelAxesX1; }
        case 2u: { return c.levelAxesX2; } case 3u: { return c.levelAxesX3; }
        case 4u: { return c.levelAxesX4; } case 5u: { return c.levelAxesX5; }
        case 6u: { return c.levelAxesX6; } default: { return c.levelAxesX7; }
    }
}
fn axesY(level : u32) -> u32 {
    let c = config();
    switch level {
        case 0u: { return c.levelAxesY0; } case 1u: { return c.levelAxesY1; }
        case 2u: { return c.levelAxesY2; } case 3u: { return c.levelAxesY3; }
        case 4u: { return c.levelAxesY4; } case 5u: { return c.levelAxesY5; }
        case 6u: { return c.levelAxesY6; } default: { return c.levelAxesY7; }
    }
}
fn axesZ(level : u32) -> u32 {
    let c = config();
    switch level {
        case 0u: { return c.levelAxesZ0; } case 1u: { return c.levelAxesZ1; }
        case 2u: { return c.levelAxesZ2; } case 3u: { return c.levelAxesZ3; }
        case 4u: { return c.levelAxesZ4; } case 5u: { return c.levelAxesZ5; }
        case 6u: { return c.levelAxesZ6; } default: { return c.levelAxesZ7; }
    }
}
fn denseIndex(p : vec3<u32>, level : u32) -> u32 {
    return p.x * axesY(level) * axesZ(level) + p.y * axesZ(level) + p.z;
}
fn denseCoordinate(index : u32, level : u32) -> vec3<u32> {
    let yz = axesY(level) * axesZ(level);
    let x = index / yz;
    let remainder = index - x * yz;
    let y = remainder / axesZ(level);
    return vec3<u32>(x, y, remainder - y * axesZ(level));
}
fn mapping(level : u32, p : vec3<u32>) -> u32 {
    return scratch[config().mappingBase + levelOffset(level) + denseIndex(p, level)];
}
fn summaryAddress(candidate : u32, word : u32) -> u32 {
    return config().summaryBase + (candidate * 16u + word) * 8u;
}
fn countAddress(candidate : u32) -> u32 { return config().countBase + candidate * 4u; }
fn offsetAddress(candidate : u32) -> u32 { return config().offsetBase + candidate * 4u; }
fn statsAddress(level : u32) -> u32 { return config().levelStatsBase + level * 4u; }
fn heightSampleHalo() -> u32 { return config().reserved & 0xffffu; }
fn heightSampleStride() -> u32 { return config().reserved >> 16u; }
fn heightSampleBase() -> u32 {
    let c = configBuffer[activeLane];
    let halo = c.reserved & 0xffffu;
    let stride = c.reserved >> 16u;
    let sampleZ = c.bricksZ * c.brickSide + 2u * halo + 1u;
    return activeLane * stride * sampleZ;
}
fn heightSampleFlags(sample : HeightSample) -> u32 {
    return sample.flags & 0xffu;
}
fn heightSampleCanonicalUnits(sample : HeightSample) -> f32 {
    let residual = (sample.flags >> 8u) & 16777215u;
    return f32(sample.heightUnits) - f32(residual) / 16777215.0;
}

fn occupiedMaterial(nodeIndex : u32, cell : u32) -> vec2<u32> {
    if (nodeIndex == INVALID_INDEX || nodeIndex >= atomicLoad(&result[resultBase() + 5u])) { return vec2<u32>(0u); }
    let node = nodes[nodeBase() + nodeIndex];
    let word = cell >> 5u;
    let bit = cell & 31u;
    let value = occupancies[nodeBase() + node.occupancyIndex].cells[word];
    if ((value.bits & (1u << bit)) == 0u) { return vec2<u32>(0u); }
    let before = value.bits & select(0u, (1u << bit) - 1u, bit != 0u);
    let address = node.materialBase + value.prefix + countOneBits(before);
    let packed = atomicLoad(&materialWords[materialBase() + (address >> 2u)]);
    return vec2<u32>(1u, (packed >> ((address & 3u) * 8u)) & 0xffu);
}

fn parentCell(level : u32, parent : vec3<u32>, cell : u32) -> vec4<u32> {
    let px = cell & 7u;
    let py = (cell >> 3u) & 7u;
    let pz = cell >> 6u;
    var values : array<u32, 8>;
    var sampleCount = 0u;
    var validCount = 0u;
    for (var sub = 0u; sub < 8u; sub++) {
        let g = vec3<u32>(px * 2u + (sub & 1u), py * 2u + ((sub >> 1u) & 1u), pz * 2u + ((sub >> 2u) & 1u));
        let childCoordinate = parent * 2u + g / 8u;
        if (any(childCoordinate >= vec3<u32>(axesX(level - 1u), axesY(level - 1u), axesZ(level - 1u)))) { continue; }
        validCount++;
        let child = mapping(level - 1u, childCoordinate);
        let sample = occupiedMaterial(child, (g.x & 7u) + 8u * ((g.y & 7u) + 8u * (g.z & 7u)));
        if (sample.x != 0u) { values[sampleCount] = sample.y; sampleCount++; }
    }
    if (sampleCount == 0u) { return vec4<u32>(0u); }
    var selected = 256u;
    var selectedCount = 0u;
    for (var index = 0u; index < sampleCount; index++) {
        var frequency = 0u;
        for (var other = 0u; other < sampleCount; other++) { frequency += select(0u, 1u, values[index] == values[other]); }
        if (frequency > selectedCount || (frequency == selectedCount && values[index] < selected)) {
            selected = values[index]; selectedCount = frequency;
        }
    }
    var materialDiverged = 0u;
    for (var index = 0u; index < sampleCount; index++) {
        materialDiverged |= select(0u, 1u, values[index] != selected);
    }
    return vec4<u32>(
        1u,
        selected,
        select(0u, 1u, sampleCount != validCount),
        materialDiverged);
}

fn representativeCellDeviationMetres(
    level : u32,
    parent : vec3<u32>,
    cell : u32,
    finerVoxelSizeMetres : f32) -> f32 {
    let px = cell & 7u;
    let py = (cell >> 3u) & 7u;
    let pz = cell >> 6u;
    var solid0 = 0u;
    var solid1 = 0u;
    var solid2 = 0u;
    var solid3 = 0u;
    var values : array<u32, 8>;
    var validSamples = 0u;
    var occupiedSamples = 0u;
    var materialCount = 0u;
    for (var sub = 0u; sub < 8u; sub++) {
        let g = vec3<u32>(
            px * 2u + (sub & 1u),
            py * 2u + ((sub >> 1u) & 1u),
            pz * 2u + ((sub >> 2u) & 1u));
        let childCoordinate = parent * 2u + g / 8u;
        if (any(childCoordinate >= vec3<u32>(
            axesX(level - 1u), axesY(level - 1u), axesZ(level - 1u)))) {
            continue;
        }
        validSamples++;
        let child = mapping(level - 1u, childCoordinate);
        let sample = occupiedMaterial(
            child,
            (g.x & 7u) + 8u * ((g.y & 7u) + 8u * (g.z & 7u)));
        if (sample.x == 0u) {
            continue;
        }
        occupiedSamples++;
        values[materialCount] = sample.y;
        materialCount++;
        let subcolumn = (g.x & 1u) + 2u * (g.z & 1u);
        if (subcolumn == 0u) { solid0++; }
        else if (subcolumn == 1u) { solid1++; }
        else if (subcolumn == 2u) { solid2++; }
        else { solid3++; }
    }
    if (occupiedSamples == 0u) {
        return 0.0;
    }
    let lowest = min(min(solid0, solid1), min(solid2, solid3));
    let highest = max(max(solid0, solid1), max(solid2, solid3));
    var deviation = f32(highest - lowest) * finerVoxelSizeMetres;
    if (validSamples != 0u && occupiedSamples != validSamples) {
        deviation = max(deviation, finerVoxelSizeMetres * 0.5);
    }
    if (materialCount > 1u) {
        var selected = values[0];
        var selectedCount = 0u;
        for (var index = 0u; index < materialCount; index++) {
            var frequency = 0u;
            for (var other = 0u; other < materialCount; other++) {
                frequency += select(0u, 1u, values[index] == values[other]);
            }
            if (frequency > selectedCount ||
                (frequency == selectedCount && values[index] < selected)) {
                selected = values[index];
                selectedCount = frequency;
            }
        }
        var materialDiverged = false;
        for (var index = 0u; index < materialCount; index++) {
            materialDiverged = materialDiverged || values[index] != selected;
        }
        if (materialDiverged) {
            deviation = max(deviation, finerVoxelSizeMetres * 0.25);
        }
    }
    return deviation;
}

fn representativeSurfaceDeviationMetres(
    level : u32,
    parent : vec3<u32>,
    cell : u32) -> f32 {
    let c = config();
    let halo = heightSampleHalo();
    let stride = heightSampleStride();
    if (halo == 0u || stride == 0u || !(c.voxelSizeMetres > 0.0)) {
        return 0.0;
    }
    let scale = 1u << level;
    let px = cell & 7u;
    let py = (cell >> 3u) & 7u;
    let pz = cell >> 6u;
    let localX = (parent.x * c.brickSide + px) * scale;
    let localY = (parent.y * c.brickSide + py) * scale;
    let localZ = (parent.z * c.brickSide + pz) * scale;
    if (localX > c.bricksX * c.brickSide ||
        localZ > c.bricksZ * c.brickSide) {
        return 0.0;
    }
    let heightUnitMetres = c.voxelSizeMetres / f32(c.canonicalStep);
    let bottomCanonical = f32(resolvedChunkMinimumCanonicalY()) +
        f32(localY * c.canonicalStep);
    let topCanonical = bottomCanonical + f32(scale * c.canonicalStep);
    let base = heightSampleBase();
    let sampleRows = c.bricksZ * c.brickSide + 2u * halo + 1u;
    var deviation = 0.0;
    var crossed = false;
    for (var iz = 0u; iz < 3u; iz++) {
        let offsetZ = min((iz * scale + 1u) / 2u, scale);
        for (var ix = 0u; ix < 3u; ix++) {
            let offsetX = min((ix * scale + 1u) / 2u, scale);
            let sampleX = localX + offsetX + halo;
            let sampleZ = localZ + offsetZ + halo;
            if (sampleX >= stride || sampleZ >= sampleRows) {
                continue;
            }
            let sample = heightSamples[base + sampleZ * stride + sampleX];
            if ((heightSampleFlags(sample) & 1u) == 0u) {
                continue;
            }
            let heightCanonical = heightSampleCanonicalUnits(sample);
            if (heightCanonical > bottomCanonical && heightCanonical <= topCanonical) {
                crossed = true;
                deviation = max(deviation,
                    abs(topCanonical - heightCanonical) * heightUnitMetres);
            }
        }
    }
    return select(0.0, deviation, crossed);
}

@compute @workgroup_size(1, 1, 1)
fn resetTerrainGPUV2RasterHierarchy(@builtin(workgroup_id) wid : vec3<u32>) {
    activeLane = wid.z;
    let c = config();
    let rb = resultBase();
    for (var index = 0u; index < 16u; index++) { atomicStore(&result[rb + index], 0u); }
    atomicStore(&result[rb], STATUS_BUILDING);
    atomicStore(&result[rb + 3u], c.flags);
    atomicStore(&result[rb + 4u], rasterValue(5u));
    atomicStore(&result[rb + 5u], rasterValue(5u));
    atomicStore(&result[rb + 6u], rasterValue(6u));
    atomicStore(&result[rb + 9u], rasterValue(9u));
    atomicStore(&result[rb + 10u], c.generatorRevision);
    atomicStore(&result[rb + 11u], TERRAIN_V2_RASTER_HIERARCHY_WORKGROUP);
    atomicStore(&result[rb + 12u], c.levelCount);
    atomicStore(&result[rb + 13u], c.scratchWords);
    atomicStore(&result[rb + 15u], 1u);
    let valid = c.levelCount >= 2u && c.levelCount <= 8u && c.brickSide == 8u &&
        c.selectedMode == 1u && c.regionMappingId != 0u && c.canonicalStep != 0u &&
        rasterValue(5u) <= c.leafNodeCapacity && rasterValue(6u) <= c.leafMaterialCapacity;
    if (!valid) { atomicStore(&result[rb], STATUS_INVALID); }
    else if (rasterValue(0u) != RASTER_STATUS_READY || rasterValue(9u) != c.expectedTerrainGeneration) {
        atomicStore(&result[rb], STATUS_STALE_RASTER);
    }
}

@compute @workgroup_size(TERRAIN_V2_RASTER_HIERARCHY_WORKGROUP, 1, 1)
fn clearTerrainGPUV2RasterHierarchyMappings(@builtin(global_invocation_id) gid : vec3<u32>) {
    activeLane = gid.z;
    if (building() && gid.x < config().totalDenseCandidates) { scratch[config().mappingBase + gid.x] = INVALID_INDEX; }
}
@compute @workgroup_size(TERRAIN_V2_RASTER_HIERARCHY_WORKGROUP, 1, 1)
fn clearTerrainGPUV2RasterHierarchyMaterials(@builtin(global_invocation_id) gid : vec3<u32>) {
    activeLane = gid.z;
    if (building() && gid.x < config().materialWordCapacity) { atomicStore(&materialWords[materialBase() + gid.x], 0u); }
}
@compute @workgroup_size(TERRAIN_V2_RASTER_HIERARCHY_WORKGROUP, 1, 1)
fn copyTerrainGPUV2RasterHierarchyLeaves(@builtin(global_invocation_id) gid : vec3<u32>) {
    activeLane = gid.z;
    if (!building() || gid.x >= rasterValue(5u)) { return; }
    let c = config();
    var node = leafNodes[leafNodeBase() + gid.x];
    node.level &= ~FORCE_LEAF_RASTER;
    node.parentIndex = INVALID_INDEX;
    node.parentError = bitcast<f32>(0x7f7fffffu);
    nodes[nodeBase() + gid.x] = node;
    occupancies[nodeBase() + gid.x] = leafOccupancy[leafNodeBase() + node.occupancyIndex];
    let delta = node.canonicalGridOriginScale.xyz - vec3<i32>(
        c.chunkMinimumCanonicalX, resolvedChunkMinimumCanonicalY(),
        c.chunkMinimumCanonicalZ);
    let divisor = i32(c.canonicalStep * 8u);
    if (any(delta < vec3<i32>(0)) || any((delta % vec3<i32>(divisor)) != vec3<i32>(0))) { atomicOr(&result[resultBase() + 2u], 1u); return; }
    let coordinate = vec3<u32>(delta / vec3<i32>(divisor));
    if (any(coordinate >= vec3<u32>(c.bricksX, c.bricksY, c.bricksZ))) { atomicOr(&result[resultBase() + 2u], 1u); return; }
    scratch[c.mappingBase + denseIndex(coordinate, 0u)] = gid.x;
}
@compute @workgroup_size(TERRAIN_V2_RASTER_HIERARCHY_WORKGROUP, 1, 1)
fn copyTerrainGPUV2RasterHierarchyLeafMaterials(@builtin(global_invocation_id) gid : vec3<u32>) {
    activeLane = gid.z;
    if (building() && gid.x < (rasterValue(6u) + 3u) / 4u) { atomicStore(&materialWords[materialBase() + gid.x], leafMaterials[leafMaterialBase() + gid.x]); }
}

fn classifyWord(gid : u32, level : u32) {
    let candidate = gid / 16u;
    let word = gid & 15u;
    if (!building() || level >= config().levelCount || candidate >= levelCount(level)) { return; }
    let parent = denseCoordinate(candidate, level);
    var bits = 0u;
    var count = 0u;
    var mixed = 0u;
    var maximumDeviation = 0.0;
    var minimum = vec3<u32>(7u);
    var maximum = vec3<u32>(0u);
    var coarse = vec2<u32>(0u);
    var anyOccupied = false;
    let scale = 1u << level;
    let finerVoxelSizeMetres = config().voxelSizeMetres * f32(scale >> 1u);
    for (var bit = 0u; bit < 32u; bit++) {
        let cell = word * 32u + bit;
        let value = parentCell(level, parent, cell);
        if (value.x != 0u) {
            bits |= 1u << bit; count++; mixed |= value.z | value.w;
            maximumDeviation = max(maximumDeviation,
                max(representativeCellDeviationMetres(
                        level, parent, cell, finerVoxelSizeMetres),
                    representativeSurfaceDeviationMetres(
                        level, parent, cell)));
            let p = vec3<u32>(cell & 7u, (cell >> 3u) & 7u, cell >> 6u);
            minimum = min(minimum, p); maximum = max(maximum, p); anyOccupied = true;
            let coarseIndex = (p.x >> 1u) + 4u * (p.y >> 1u) + 16u * (p.z >> 1u);
            if (coarseIndex < 32u) { coarse.x |= 1u << coarseIndex; }
            else { coarse.y |= 1u << (coarseIndex - 32u); }
        }
    }
    let address = summaryAddress(candidate, word);
    scratch[address] = bits; scratch[address + 1u] = count;
    scratch[address + 2u] = mixed;
    scratch[address + 3u] = select(0u, minimum.x | (minimum.y << 3u) | (minimum.z << 6u), anyOccupied);
    scratch[address + 4u] = select(0u, maximum.x | (maximum.y << 3u) | (maximum.z << 6u), anyOccupied);
    scratch[address + 5u] = coarse.x; scratch[address + 6u] = coarse.y;
    scratch[address + 7u] = bitcast<u32>(maximumDeviation);
}
fn reduceCandidate(candidate : u32, level : u32) {
    if (!building() || candidate >= levelCount(level)) { return; }
    var materials = 0u;
    var mixed = 0u;
    for (var word = 0u; word < 16u; word++) { let address = summaryAddress(candidate, word); materials += scratch[address + 1u]; mixed |= scratch[address + 2u]; }
    let parent = denseCoordinate(candidate, level);
    var children = 0u;
    for (var octant = 0u; octant < 8u; octant++) {
        let child = parent * 2u + vec3<u32>(octant & 1u, (octant >> 1u) & 1u, (octant >> 2u) & 1u);
        if (all(child < vec3<u32>(axesX(level - 1u), axesY(level - 1u), axesZ(level - 1u))) && mapping(level - 1u, child) != INVALID_INDEX) { children++; }
    }
    let address = countAddress(candidate);
    scratch[address] = select(0u, 1u, materials != 0u);
    scratch[address + 1u] = materials; scratch[address + 2u] = children; scratch[address + 3u] = mixed;
}

#define LEVEL_ENTRY(LEVEL) \
@compute @workgroup_size(TERRAIN_V2_RASTER_HIERARCHY_WORKGROUP, 1, 1) \
fn classifyTerrainGPUV2RasterHierarchyLevel##LEVEL(@builtin(global_invocation_id) gid : vec3<u32>) { activeLane = gid.z; classifyWord(gid.x, LEVEL##u); } \
@compute @workgroup_size(TERRAIN_V2_RASTER_HIERARCHY_WORKGROUP, 1, 1) \
fn reduceTerrainGPUV2RasterHierarchyLevel##LEVEL(@builtin(global_invocation_id) gid : vec3<u32>) { activeLane = gid.z; reduceCandidate(gid.x, LEVEL##u); }
LEVEL_ENTRY(1)
LEVEL_ENTRY(2)
LEVEL_ENTRY(3)
LEVEL_ENTRY(4)
LEVEL_ENTRY(5)
LEVEL_ENTRY(6)
LEVEL_ENTRY(7)

fn scanElementCount(level : u32, scanLevel : u32) -> u32 {
    let count = levelCount(level);
    if (scanLevel == 0u) { return count; }
    let first = (count + 255u) / 256u;
    if (scanLevel == 1u) { return first; }
    return (first + 255u) / 256u;
}
fn scanLoad(index : u32, scanLevel : u32) -> vec4<u32> {
    let c = config();
    var base = c.countBase;
    if (scanLevel == 1u) { base = c.level0SumsBase; }
    if (scanLevel == 2u) { base = c.level1SumsBase; }
    return vec4<u32>(scratch[base + index * 4u], scratch[base + index * 4u + 1u], scratch[base + index * 4u + 2u], scratch[base + index * 4u + 3u]);
}
fn scanStore(index : u32, scanLevel : u32, value : vec4<u32>) {
    let c = config();
    var base = c.offsetBase;
    if (scanLevel == 1u) { base = c.level0OffsetsBase; }
    if (scanLevel == 2u) { base = c.level1OffsetsBase; }
    scratch[base + index * 4u] = value.x; scratch[base + index * 4u + 1u] = value.y;
    scratch[base + index * 4u + 2u] = value.z; scratch[base + index * 4u + 3u] = value.w;
}
fn scanSumStore(index : u32, scanLevel : u32, value : vec4<u32>) {
    let c = config();
    if (scanLevel == 0u) {
        scratch[c.level0SumsBase + index * 4u] = value.x; scratch[c.level0SumsBase + index * 4u + 1u] = value.y;
        scratch[c.level0SumsBase + index * 4u + 2u] = value.z; scratch[c.level0SumsBase + index * 4u + 3u] = value.w;
    } else if (scanLevel == 1u) {
        scratch[c.level1SumsBase + index * 4u] = value.x; scratch[c.level1SumsBase + index * 4u + 1u] = value.y;
        scratch[c.level1SumsBase + index * 4u + 2u] = value.z; scratch[c.level1SumsBase + index * 4u + 3u] = value.w;
    }
}
fn scanBlock(local : u32, group : u32, level : u32, scanLevel : u32) {
    // The chain is persistent and is idled with zero indirect arguments after
    // completion.  Keep this guard as well: a preceding stage can reject the
    // build while the already-recorded scan dispatches are still in flight.
    // The predicate is uniform for the whole workgroup, so returning before
    // any barrier is valid.
    if (!building()) { return; }
    let count = scanElementCount(level, scanLevel);
    let index = group * 256u + local;
    var input = vec4<u32>(0u);
    if (index < count) { input = scanLoad(index, scanLevel); }
    sharedValues[local] = input; workgroupBarrier();
    var stride = 1u;
    while (stride < 256u) {
        let destination = (local + 1u) * stride * 2u - 1u;
        if (destination < 256u) { sharedValues[destination] += sharedValues[destination - stride]; }
        stride *= 2u; workgroupBarrier();
    }
    if (local == 255u) { scanSumStore(group, scanLevel, sharedValues[255u]); sharedValues[255u] = vec4<u32>(0u); }
    workgroupBarrier(); stride = 128u;
    while (stride > 0u) {
        let destination = (local + 1u) * stride * 2u - 1u;
        if (destination < 256u) { let left = sharedValues[destination - stride]; sharedValues[destination - stride] = sharedValues[destination]; sharedValues[destination] += left; }
        stride /= 2u; workgroupBarrier();
    }
    if (index < count) { scanStore(index, scanLevel, sharedValues[local]); }
}
fn addBlocks(gid : u32, level : u32, addLevel : u32) {
    let c = config();
    if (!building()) { return; }
    if (addLevel == 1u) {
        let count = (levelCount(level) + 255u) / 256u;
        if (gid >= count) { return; }
        // The first block has an exclusive upper-level prefix of zero.  This
        // also lets the CPU omit the otherwise expensive one-element S2 scan.
        if (gid < 256u) { return; }
        let base = c.level0OffsetsBase + gid * 4u;
        let upper = c.level1OffsetsBase + (gid / 256u) * 4u;
        for (var lane = 0u; lane < 4u; lane++) { scratch[base + lane] += scratch[upper + lane]; }
    } else {
        if (gid >= levelCount(level)) { return; }
        // S0 already contains the complete prefix for the first 256 elements;
        // no S1 output is required for that block.
        if (gid < 256u) { return; }
        let base = c.offsetBase + gid * 4u;
        let upper = c.level0OffsetsBase + (gid / 256u) * 4u;
        for (var lane = 0u; lane < 4u; lane++) { scratch[base + lane] += scratch[upper + lane]; }
    }
}

#define SCAN_LEVEL_ENTRIES(LEVEL) \
@compute @workgroup_size(256, 1, 1) fn scanTerrainGPUV2RasterHierarchyL##LEVEL##S0(@builtin(local_invocation_id) lid : vec3<u32>, @builtin(workgroup_id) wid : vec3<u32>) { activeLane = wid.z; scanBlock(lid.x, wid.x, LEVEL##u, 0u); } \
@compute @workgroup_size(256, 1, 1) fn scanTerrainGPUV2RasterHierarchyL##LEVEL##S1(@builtin(local_invocation_id) lid : vec3<u32>, @builtin(workgroup_id) wid : vec3<u32>) { activeLane = wid.z; scanBlock(lid.x, wid.x, LEVEL##u, 1u); } \
@compute @workgroup_size(256, 1, 1) fn scanTerrainGPUV2RasterHierarchyL##LEVEL##S2(@builtin(local_invocation_id) lid : vec3<u32>, @builtin(workgroup_id) wid : vec3<u32>) { activeLane = wid.z; scanBlock(lid.x, wid.x, LEVEL##u, 2u); } \
@compute @workgroup_size(256, 1, 1) fn addTerrainGPUV2RasterHierarchyL##LEVEL##S1(@builtin(global_invocation_id) gid : vec3<u32>) { activeLane = gid.z; addBlocks(gid.x, LEVEL##u, 1u); } \
@compute @workgroup_size(256, 1, 1) fn addTerrainGPUV2RasterHierarchyL##LEVEL##S0(@builtin(global_invocation_id) gid : vec3<u32>) { activeLane = gid.z; addBlocks(gid.x, LEVEL##u, 0u); }
SCAN_LEVEL_ENTRIES(1)
SCAN_LEVEL_ENTRIES(2)
SCAN_LEVEL_ENTRIES(3)
SCAN_LEVEL_ENTRIES(4)
SCAN_LEVEL_ENTRIES(5)
SCAN_LEVEL_ENTRIES(6)
SCAN_LEVEL_ENTRIES(7)

fn reserveLevel(level : u32) {
    if (!building()) { return; }
    let c = config();
    let last = levelCount(level) - 1u;
    let count = countAddress(last);
    let offset = offsetAddress(last);
    let totals = vec3<u32>(scratch[offset] + scratch[count], scratch[offset + 1u] + scratch[count + 1u], scratch[offset + 2u] + scratch[count + 2u]);
    let rb = resultBase();
    let bases = vec3<u32>(atomicLoad(&result[rb + 5u]), atomicLoad(&result[rb + 6u]), atomicLoad(&result[rb + 7u]));
    let next = bases + totals;
    if (next.x > c.nodeCapacity || next.y > c.materialCapacity || next.z > c.childCapacity) {
        atomicStore(&result[rb], STATUS_OVERFLOW);
        atomicStore(&result[rb + 1u], select(1u, select(2u, 3u, next.z > c.childCapacity), next.y > c.materialCapacity));
        return;
    }
    let stats = statsAddress(level);
    scratch[stats] = bases.x; scratch[stats + 1u] = bases.y; scratch[stats + 2u] = bases.z; scratch[stats + 3u] = totals.x;
    atomicStore(&result[rb + 5u], next.x); atomicStore(&result[rb + 6u], next.y); atomicStore(&result[rb + 7u], next.z);
}

#define RESERVE_ENTRY(LEVEL) @compute @workgroup_size(1, 1, 1) fn reserveTerrainGPUV2RasterHierarchyLevel##LEVEL(@builtin(workgroup_id) wid : vec3<u32>) { activeLane = wid.z; reserveLevel(LEVEL##u); }
RESERVE_ENTRY(1)
RESERVE_ENTRY(2)
RESERVE_ENTRY(3)
RESERVE_ENTRY(4)
RESERVE_ENTRY(5)
RESERVE_ENTRY(6)
RESERVE_ENTRY(7)

fn emitNode(candidate : u32, level : u32) {
    if (!building() || candidate >= levelCount(level) || scratch[countAddress(candidate)] == 0u) { return; }
    let c = config();
    let stats = statsAddress(level);
    let offsets = offsetAddress(candidate);
    let destination = scratch[stats] + scratch[offsets];
    let materialBase = scratch[stats + 1u] + scratch[offsets + 1u];
    let childBase = scratch[stats + 2u] + scratch[offsets + 2u];
    let coordinate = denseCoordinate(candidate, level);
    let scale = 1u << level;
    var node : SparseVoxelBrickNode;
    node.minimumVoxelSize = vec4<f32>(
        vec3<f32>(c.chunkMinimumWorldX, resolvedChunkMinimumWorldY(),
            c.chunkMinimumWorldZ) +
            vec3<f32>(coordinate * (8u * scale)) * c.voxelSizeMetres,
        c.voxelSizeMetres * f32(scale));
    var minimum = vec3<u32>(7u); var maximum = vec3<u32>(0u); var coarse = vec2<u32>(0u); var any = false;
    for (var word = 0u; word < 16u; word++) {
        let summary = summaryAddress(candidate, word);
        if (scratch[summary + 1u] == 0u) { continue; }
        let packedMinimum = scratch[summary + 3u]; let packedMaximum = scratch[summary + 4u];
        minimum = min(minimum, vec3<u32>(packedMinimum & 7u, (packedMinimum >> 3u) & 7u, (packedMinimum >> 6u) & 7u));
        maximum = max(maximum, vec3<u32>(packedMaximum & 7u, (packedMaximum >> 3u) & 7u, (packedMaximum >> 6u) & 7u));
        coarse |= vec2<u32>(scratch[summary + 5u], scratch[summary + 6u]); any = true;
    }
    node.packedOccupiedBounds = select(0u, minimum.x | (minimum.y << 3u) | (minimum.z << 6u) | (maximum.x << 9u) | (maximum.y << 12u) | (maximum.z << 15u), any);
    node.coarseOccupancyLow = coarse.x; node.coarseOccupancyHigh = coarse.y;
    node.occupancyIndex = destination; node.materialBase = materialBase;
    node.parentIndex = INVALID_INDEX; node.childCount = scratch[countAddress(candidate) + 2u];
    var clipLOD = 0u; var step = c.canonicalStep; while (step > 1u) { step >>= 1u; clipLOD++; }
    node.level = level | (clipLOD << 8u); node.occupiedCount = scratch[countAddress(candidate) + 1u];
    node.firstChild = childBase; node.instanceIndex = INVALID_INDEX; node.paletteBase = c.paletteBase; node.regionMappingId = c.regionMappingId;
    var childError = 0.0; var measuredDeviation = 0.0; let parent = coordinate;
    for (var word = 0u; word < 16u; word++) {
        measuredDeviation = max(
            measuredDeviation,
            bitcast<f32>(scratch[summaryAddress(candidate, word) + 7u]));
    }
    for (var octant = 0u; octant < 8u; octant++) {
        let childCoordinate = parent * 2u + vec3<u32>(octant & 1u, (octant >> 1u) & 1u, (octant >> 2u) & 1u);
        if (all(childCoordinate < vec3<u32>(axesX(level - 1u), axesY(level - 1u), axesZ(level - 1u)))) {
            let child = mapping(level - 1u, childCoordinate); if (child != INVALID_INDEX) { childError = max(childError, nodes[nodeBase() + child].selfError); }
        }
    }
    node.selfError = max(childError, max(node.minimumVoxelSize.w * 0.25, measuredDeviation));
    node.parentError = bitcast<f32>(0x7f7fffffu); node.reservedTailPadding = 0u;
    node.canonicalGridOriginScale = vec4<i32>(
        vec3<i32>(c.chunkMinimumCanonicalX,
            resolvedChunkMinimumCanonicalY(), c.chunkMinimumCanonicalZ) +
            vec3<i32>(coordinate * (8u * scale * c.canonicalStep)),
        i32(scale * c.canonicalStep));
    nodes[nodeBase() + destination] = node;
    scratch[c.mappingBase + levelOffset(level) + candidate] = destination;
}
fn emitWord(gid : u32, level : u32) {
    let candidate = gid / 16u; let word = gid & 15u;
    if (!building() || candidate >= levelCount(level) || scratch[countAddress(candidate)] == 0u) { return; }
    let stats = statsAddress(level); let offsets = offsetAddress(candidate);
    let destination = scratch[stats] + scratch[offsets];
    let materialBase = scratch[stats + 1u] + scratch[offsets + 1u];
    let bits = scratch[summaryAddress(candidate, word)];
    var prefix = 0u; for (var before = 0u; before < word; before++) { prefix += scratch[summaryAddress(candidate, before) + 1u]; }
    occupancies[nodeBase() + destination].cells[word] = SparseVoxelBrickOccupancyCell(bits, prefix);
    let parent = denseCoordinate(candidate, level);
    var local = 0u;
    for (var bit = 0u; bit < 32u; bit++) {
        if ((bits & (1u << bit)) == 0u) { continue; }
        let value = parentCell(level, parent, word * 32u + bit);
        let address = materialBase + prefix + local; let shift = (address & 3u) * 8u;
        atomicOr(&materialWords[materialBase() + (address >> 2u)], (value.y & 0xffu) << shift); local++;
    }
}
fn linkNode(candidate : u32, level : u32) {
    if (!building() || candidate >= levelCount(level) || scratch[countAddress(candidate)] == 0u) { return; }
    let stats = statsAddress(level); let offsets = offsetAddress(candidate);
    let parentIndex = scratch[stats] + scratch[offsets]; let parent = nodes[nodeBase() + parentIndex]; let coordinate = denseCoordinate(candidate, level);
    var childOffset = 0u;
    for (var octant = 0u; octant < 8u; octant++) {
        let childCoordinate = coordinate * 2u + vec3<u32>(octant & 1u, (octant >> 1u) & 1u, (octant >> 2u) & 1u);
        if (any(childCoordinate >= vec3<u32>(axesX(level - 1u), axesY(level - 1u), axesZ(level - 1u)))) { continue; }
        let child = mapping(level - 1u, childCoordinate); if (child == INVALID_INDEX) { continue; }
        childIndices[childBase() + parent.firstChild + childOffset] = child; nodes[nodeBase() + child].parentIndex = parentIndex; nodes[nodeBase() + child].parentError = parent.selfError; childOffset++;
    }
}

#define EMIT_ENTRIES(LEVEL) \
@compute @workgroup_size(TERRAIN_V2_RASTER_HIERARCHY_WORKGROUP, 1, 1) fn emitTerrainGPUV2RasterHierarchyNodesL##LEVEL(@builtin(global_invocation_id) gid : vec3<u32>) { activeLane = gid.z; emitNode(gid.x, LEVEL##u); } \
@compute @workgroup_size(TERRAIN_V2_RASTER_HIERARCHY_WORKGROUP, 1, 1) fn emitTerrainGPUV2RasterHierarchyWordsL##LEVEL(@builtin(global_invocation_id) gid : vec3<u32>) { activeLane = gid.z; emitWord(gid.x, LEVEL##u); } \
@compute @workgroup_size(TERRAIN_V2_RASTER_HIERARCHY_WORKGROUP, 1, 1) fn linkTerrainGPUV2RasterHierarchyL##LEVEL(@builtin(global_invocation_id) gid : vec3<u32>) { activeLane = gid.z; linkNode(gid.x, LEVEL##u); }
EMIT_ENTRIES(1)
EMIT_ENTRIES(2)
EMIT_ENTRIES(3)
EMIT_ENTRIES(4)
EMIT_ENTRIES(5)
EMIT_ENTRIES(6)
EMIT_ENTRIES(7)

@compute @workgroup_size(TERRAIN_V2_RASTER_HIERARCHY_WORKGROUP, 1, 1)
fn emitTerrainGPUV2RasterHierarchyRoots(@builtin(global_invocation_id) gid : vec3<u32>) {
    activeLane = gid.z;
    let level = config().levelCount - 1u;
    if (!building() || gid.x >= levelCount(level) || scratch[countAddress(gid.x)] == 0u) { return; }
    rootIndices[rootBase() + scratch[offsetAddress(gid.x)]] = mapping(level, denseCoordinate(gid.x, level));
}
@compute @workgroup_size(TERRAIN_V2_RASTER_HIERARCHY_WORKGROUP, 1, 1)
fn validateTerrainGPUV2RasterHierarchy(@builtin(global_invocation_id) gid : vec3<u32>) {
    activeLane = gid.z;
    let rb = resultBase();
    if (!building() || gid.x >= atomicLoad(&result[rb + 5u])) { return; }
    let node = nodes[nodeBase() + gid.x]; var prefix = 0u;
    for (var word = 0u; word < 16u; word++) { let value = occupancies[nodeBase() + node.occupancyIndex].cells[word]; if (value.prefix != prefix) { atomicOr(&result[rb + 2u], 1u); } prefix += countOneBits(value.bits); }
    if (prefix != node.occupiedCount || node.materialBase + prefix > atomicLoad(&result[rb + 6u]) || (node.level & FORCE_LEAF_RASTER) != 0u || node.regionMappingId == 0u) { atomicOr(&result[rb + 2u], 1u); }
    if (node.childCount != 0u) {
        if (node.childCount > 8u || node.firstChild + node.childCount > atomicLoad(&result[rb + 7u])) { atomicOr(&result[rb + 2u], 2u); }
        for (var childOffset = 0u; childOffset < node.childCount; childOffset++) {
            let child = childIndices[childBase() + node.firstChild + childOffset];
            if (child >= atomicLoad(&result[rb + 5u]) || nodes[nodeBase() + child].parentIndex != gid.x || nodes[nodeBase() + child].parentError != node.selfError || nodes[nodeBase() + child].selfError > node.selfError || (nodes[nodeBase() + child].level & 0xffu) + 1u != (node.level & 0xffu)) { atomicOr(&result[rb + 2u], 4u); }
        }
    } else if (gid.x >= atomicLoad(&result[rb + 4u])) { atomicOr(&result[rb + 2u], 8u); }
}
@compute @workgroup_size(1, 1, 1)
fn finalizeTerrainGPUV2RasterHierarchy(@builtin(workgroup_id) wid : vec3<u32>) {
    activeLane = wid.z;
    if (!building()) { return; }
    let level = config().levelCount - 1u; let stats = statsAddress(level); let roots = scratch[stats + 3u];
    let rb = resultBase();
    atomicStore(&result[rb + 8u], roots);
    if (roots == 0u || roots > config().rootCapacity || atomicLoad(&result[rb + 2u]) != 0u) { atomicStore(&result[rb], STATUS_VALIDATION_FAILED); return; }
    atomicStore(&result[rb], STATUS_READY);
}
